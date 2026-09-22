// Package web serves the JSON API and the server-rendered HTML pages.
package web

import (
	"context"
	"embed"
	"encoding/json"
	"errors"
	"fmt"
	"html/template"
	"io/fs"
	"log"
	"net/http"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"

	"qtc-explorer/internal/gateway"
	"qtc-explorer/internal/indexer"
	"qtc-explorer/internal/pq"
	"qtc-explorer/internal/rpc"
	"qtc-explorer/internal/store"
)

//go:embed templates/*.html
var tmplFS embed.FS

//go:embed static/*
var staticFS embed.FS

type Server struct {
	st    *store.Store
	rpc   *rpc.Client
	ix    *indexer.Indexer
	gw    *gateway.Gateway
	chain string
	mux   *http.ServeMux
	tmpl  map[string]*template.Template

	cacheMu sync.Mutex
	cache   map[string]cacheEntry
}

type cacheEntry struct {
	at  time.Time
	val any
}

var (
	hex64    = regexp.MustCompile(`^[0-9a-fA-F]{64}$`)
	hexAny   = regexp.MustCompile(`^[0-9a-fA-F]{6,63}$`)
	addrLike = regexp.MustCompile(`^[a-zA-Z0-9]{6,120}$`)
)

func New(st *store.Store, c *rpc.Client, ix *indexer.Indexer, gw *gateway.Gateway, chain string) *Server {
	s := &Server{st: st, rpc: c, ix: ix, gw: gw, chain: chain, mux: http.NewServeMux(), cache: map[string]cacheEntry{}}
	s.loadTemplates()
	sub, _ := fs.Sub(staticFS, "static")
	s.mux.Handle("GET /static/", http.StripPrefix("/static/", http.FileServer(http.FS(sub))))

	// JSON API
	s.mux.HandleFunc("GET /api/v1/chain", s.apiChain)
	s.mux.HandleFunc("GET /api/v1/status", s.apiStatus)
	s.mux.HandleFunc("GET /api/v1/blocks", s.apiBlocks)
	s.mux.HandleFunc("GET /api/v1/block/{id}", s.apiBlock)
	s.mux.HandleFunc("GET /api/v1/block/{id}/header", s.apiBlockHeader)
	s.mux.HandleFunc("GET /api/v1/tx/{txid}", s.apiTx)
	s.mux.HandleFunc("GET /api/v1/tx/{txid}/hex", s.apiTxHex)
	s.mux.HandleFunc("GET /api/v1/tx/{txid}/status", s.apiTxStatus)
	s.mux.Handle("POST /api/v1/tx/broadcast", gw)
	s.mux.Handle("OPTIONS /api/v1/tx/broadcast", gw)
	s.mux.HandleFunc("GET /api/v1/address/{addr}", s.apiAddr)
	s.mux.HandleFunc("GET /api/v1/address/{addr}/txs", s.apiAddrTxs)
	s.mux.HandleFunc("GET /api/v1/address/{addr}/utxo", s.apiAddrUTXO)
	s.mux.HandleFunc("GET /api/v1/mempool", s.apiMempool)
	s.mux.HandleFunc("GET /api/v1/mempool/txids", s.apiMempoolTxids)
	s.mux.HandleFunc("GET /api/v1/fees", s.apiFees)
	s.mux.HandleFunc("GET /api/v1/difficulty", s.apiDifficulty)
	s.mux.HandleFunc("GET /api/v1/search", s.apiSearch)

	// HTML
	s.mux.HandleFunc("GET /{$}", s.pageHome)
	s.mux.HandleFunc("GET /blocks", s.pageBlocks)
	s.mux.HandleFunc("GET /block/{id}", s.pageBlock)
	s.mux.HandleFunc("GET /tx/{txid}", s.pageTx)
	s.mux.HandleFunc("GET /address/{addr}", s.pageAddr)
	s.mux.HandleFunc("GET /mempool", s.pageMempool)
	s.mux.HandleFunc("GET /difficulty", s.pageDifficulty)
	s.mux.HandleFunc("GET /search", s.pageSearch)
	s.mux.HandleFunc("GET /status", s.pageStatus)
	s.mux.HandleFunc("GET /api", s.pageAPI)
	return s
}

func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("X-Content-Type-Options", "nosniff")
	w.Header().Set("Referrer-Policy", "no-referrer")
	w.Header().Set("Content-Security-Policy", "default-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'")
	s.mux.ServeHTTP(w, r)
}

// ---- helpers -------------------------------------------------------------

func (s *Server) cached(key string, ttl time.Duration, fill func() (any, error)) (any, error) {
	s.cacheMu.Lock()
	if e, ok := s.cache[key]; ok && time.Since(e.at) < ttl {
		s.cacheMu.Unlock()
		return e.val, nil
	}
	s.cacheMu.Unlock()
	v, err := fill()
	if err != nil {
		return nil, err
	}
	s.cacheMu.Lock()
	s.cache[key] = cacheEntry{at: time.Now(), val: v}
	s.cacheMu.Unlock()
	return v, nil
}

func writeJSON(w http.ResponseWriter, status int, v any, cacheSec int) {
	w.Header().Set("Content-Type", "application/json")
	if cacheSec > 0 {
		w.Header().Set("Cache-Control", fmt.Sprintf("public, max-age=%d", cacheSec))
	} else {
		w.Header().Set("Cache-Control", "no-store")
	}
	w.WriteHeader(status)
	enc := json.NewEncoder(w)
	enc.SetIndent("", " ")
	_ = enc.Encode(v)
}

func apiErr(w http.ResponseWriter, status int, code, msg string) {
	writeJSON(w, status, map[string]any{"error": map[string]string{"code": code, "message": msg}}, 0)
}

func limitParam(r *http.Request, def, max int) int {
	n, err := strconv.Atoi(r.URL.Query().Get("limit"))
	if err != nil || n <= 0 {
		return def
	}
	if n > max {
		return max
	}
	return n
}

func int64Param(r *http.Request, key string, def int64) int64 {
	v, err := strconv.ParseInt(r.URL.Query().Get(key), 10, 64)
	if err != nil {
		return def
	}
	return v
}

// resolveBlock accepts a height or a hash.
func (s *Server) resolveBlock(id string) (*store.Block, error) {
	if h, err := strconv.ParseInt(id, 10, 64); err == nil && h >= 0 {
		return s.st.GetBlockByHeight(h)
	}
	if hex64.MatchString(id) {
		return s.st.GetBlock(strings.ToLower(id))
	}
	return nil, store.ErrNotFound
}

// txView is a transaction with per-input witness decoding and confirmation info.
type txView struct {
	*store.Tx
	Confirmations int64       `json:"confirmations"`
	Unconfirmed   bool        `json:"unconfirmed"`
	FeeRate       float64     `json:"fee_rate_atoms_per_byte"`
	Inputs        []inputView `json:"inputs_decoded"`
	AntiFeeSnipe  bool        `json:"anti_fee_sniping"`
}

type inputView struct {
	Index   int        `json:"index"`
	Decoded pq.Decoded `json:"p2mr"`
}

func (s *Server) tipHeight() int64 { return s.ix.Status().IndexedHeight }

func (s *Server) viewTx(tx *store.Tx) *txView {
	v := &txView{Tx: tx}
	tip := s.tipHeight()
	if tx.Height >= 0 && tx.BlockHash != "" {
		v.Confirmations = tip - tx.Height + 1
	} else {
		v.Unconfirmed = true
	}
	if tx.Size > 0 && !tx.Coinbase {
		v.FeeRate = float64(tx.Fee) / float64(tx.Size)
	}
	for i, in := range tx.Vin {
		if len(in.Witness) >= 3 {
			v.Inputs = append(v.Inputs, inputView{Index: i, Decoded: pq.Decode(in.Witness)})
		}
	}
	if !tx.Coinbase && tx.Locktime > 0 && tx.Locktime < 500_000_000 && tx.Height > 0 && tx.Height-tx.Locktime <= 100 && tx.Locktime <= tx.Height {
		v.AntiFeeSnipe = true
	}
	return v
}

// lookupTx checks the index, then the mempool via the node.
func (s *Server) lookupTx(ctx context.Context, txid string) (*store.Tx, error) {
	txid = strings.ToLower(txid)
	if !hex64.MatchString(txid) {
		return nil, store.ErrNotFound
	}
	if tx, err := s.st.GetTx(txid); err == nil {
		return tx, nil
	}
	m, ok := s.ix.MempoolTx(txid)
	if !ok {
		return nil, store.ErrNotFound
	}
	raw, err := s.rpc.GetRawTransaction(ctx, txid)
	if err != nil {
		return nil, err
	}
	tx := &store.Tx{Txid: raw.Txid, Hash: raw.Hash, Height: -1, Version: raw.Version, Locktime: raw.Locktime, Size: raw.Size, Weight: raw.Weight, Fee: m.Fee, Shielded: raw.Shielded, ShieldedIn: raw.ShieldedInputCount, ShieldedOut: raw.ShieldedOutputCount, Hex: raw.Hex, Addresses: []string{}}
	for _, in := range raw.Vin {
		ti := store.TxIn{Coinbase: in.Coinbase, Txid: in.Txid, Vout: in.Vout, Sequence: in.Sequence, Witness: in.Witness}
		if in.Txid != "" {
			if u, err := s.st.GetUTXO(in.Txid, in.Vout); err == nil {
				ti.Value, ti.Address, ti.Script = u.Value, u.Address, u.Script
				tx.InputValue += u.Value
			}
		}
		tx.Vin = append(tx.Vin, ti)
	}
	for _, out := range raw.Vout {
		v := rpc.Atoms(out.Value)
		tx.Vout = append(tx.Vout, store.TxOut{N: out.N, Value: v, Script: out.ScriptPubKey.Hex, Type: out.ScriptPubKey.Type, Address: out.ScriptPubKey.Address, Asm: out.ScriptPubKey.Asm})
		tx.OutputValue += v
	}
	return tx, nil
}

// ---- API handlers --------------------------------------------------------

func (s *Server) apiChain(w http.ResponseWriter, r *http.Request) {
	v, err := s.cached("chain", 5*time.Second, func() (any, error) {
		st := s.ix.Status()
		mi, _ := s.rpc.GetMempoolInfo(r.Context())
		tip, err := s.st.GetBlockByHeight(st.IndexedHeight)
		if err != nil {
			return nil, err
		}
		out := map[string]any{
			"chain": s.chain, "height": tip.Height, "hash": tip.Hash, "time": tip.Time, "mediantime": tip.MedianTime,
			"difficulty": tip.Difficulty, "bits": tip.Bits, "node_height": st.NodeHeight, "indexer_lag": st.Lag,
			"mempool_size": st.MempoolSize,
		}
		if mi != nil {
			out["mempool_bytes"] = mi.Bytes
		}
		return out, nil
	})
	if err != nil {
		apiErr(w, 503, "not_ready", "index is still syncing")
		return
	}
	writeJSON(w, 200, v, 5)
}

func (s *Server) apiStatus(w http.ResponseWriter, r *http.Request) {
	writeJSON(w, 200, s.ix.Status(), 0)
}

func (s *Server) apiBlocks(w http.ResponseWriter, r *http.Request) {
	bs, err := s.st.Blocks(int64Param(r, "before", -1), limitParam(r, 25, 100))
	if err != nil {
		apiErr(w, 500, "internal", err.Error())
		return
	}
	writeJSON(w, 200, map[string]any{"blocks": bs}, 5)
}

func (s *Server) apiBlock(w http.ResponseWriter, r *http.Request) {
	b, err := s.resolveBlock(r.PathValue("id"))
	if err != nil {
		apiErr(w, 404, "not_found", "block not found")
		return
	}
	conf := s.tipHeight() - b.Height + 1
	cache := 0
	if conf >= 6 {
		cache = 86400
	}
	writeJSON(w, 200, map[string]any{"block": b, "confirmations": conf, "subsidy_actual": b.SubsidyActual(), "penalised": b.Penalised()}, cache)
}

func (s *Server) apiBlockHeader(w http.ResponseWriter, r *http.Request) {
	b, err := s.resolveBlock(r.PathValue("id"))
	if err != nil {
		apiErr(w, 404, "not_found", "block not found")
		return
	}
	writeJSON(w, 200, map[string]any{
		"hash": b.Hash, "version": b.Version, "previousblockhash": b.Prev, "merkleroot": b.MerkleRoot, "time": b.Time,
		"bits": b.Bits, "nonce64": b.Nonce64, "matmul_digest": b.MatmulDigest, "matmul_dim": b.MatmulDim,
		"seed_a": b.SeedA, "seed_b": b.SeedB, "header_bytes": 182,
	}, 86400)
}

func (s *Server) apiTx(w http.ResponseWriter, r *http.Request) {
	tx, err := s.lookupTx(r.Context(), r.PathValue("txid"))
	if err != nil {
		apiErr(w, 404, "not_found", "transaction not found")
		return
	}
	v := s.viewTx(tx)
	cache := 0
	if v.Confirmations >= 6 {
		cache = 86400
	}
	writeJSON(w, 200, v, cache)
}

func (s *Server) apiTxHex(w http.ResponseWriter, r *http.Request) {
	tx, err := s.lookupTx(r.Context(), r.PathValue("txid"))
	if err != nil {
		apiErr(w, 404, "not_found", "transaction not found")
		return
	}
	w.Header().Set("Content-Type", "text/plain")
	_, _ = w.Write([]byte(tx.Hex))
}

func (s *Server) apiTxStatus(w http.ResponseWriter, r *http.Request) {
	tx, err := s.lookupTx(r.Context(), r.PathValue("txid"))
	if err != nil {
		apiErr(w, 404, "not_found", "transaction not found")
		return
	}
	v := s.viewTx(tx)
	writeJSON(w, 200, map[string]any{"confirmed": !v.Unconfirmed, "block_height": tx.Height, "block_hash": tx.BlockHash, "block_time": tx.BlockTime, "confirmations": v.Confirmations}, 5)
}

// addrOrEmpty returns the indexed record, or an all-zero record for an address
// that looks valid but has never appeared on chain.
func (s *Server) addrOrEmpty(addr string) (*store.Addr, error) {
	a, err := s.st.GetAddr(addr)
	if err == nil {
		return a, nil
	}
	if !errors.Is(err, store.ErrNotFound) || !addrLike.MatchString(addr) {
		return nil, store.ErrNotFound
	}
	return &store.Addr{Address: addr, FirstHeight: -1}, nil
}

func (s *Server) apiAddr(w http.ResponseWriter, r *http.Request) {
	a, err := s.addrOrEmpty(r.PathValue("addr"))
	if err != nil {
		apiErr(w, 404, "not_found", "not an address")
		return
	}
	writeJSON(w, 200, map[string]any{"address": a, "balance": a.Balance()}, 5)
}

func (s *Server) apiAddrTxs(w http.ResponseWriter, r *http.Request) {
	txs, total, err := s.st.AddrTxs(r.PathValue("addr"), int64Param(r, "before", -1), limitParam(r, 25, 100))
	if err != nil {
		apiErr(w, 500, "internal", err.Error())
		return
	}
	writeJSON(w, 200, map[string]any{"total": total, "txs": txs}, 5)
}

func (s *Server) apiAddrUTXO(w http.ResponseWriter, r *http.Request) {
	addr := r.PathValue("addr")
	a, err := s.addrOrEmpty(addr)
	if err != nil {
		apiErr(w, 404, "not_found", "not an address")
		return
	}
	from := int(int64Param(r, "from", 0))
	us, total, err := s.st.AddrUTXOs(addr, from, limitParam(r, 1000, 1000))
	if err != nil {
		apiErr(w, 500, "internal", err.Error())
		return
	}
	tip := s.tipHeight()
	type u struct {
		Txid          string `json:"txid"`
		Vout          int64  `json:"vout"`
		Value         int64  `json:"value"`
		Height        int64  `json:"height"`
		Confirmations int64  `json:"confirmations"`
		ScriptPubKey  string `json:"script_pubkey"`
		Coinbase      bool   `json:"coinbase"`
	}
	out := make([]u, 0, len(us))
	for _, x := range us {
		out = append(out, u{x.Txid, x.Vout, x.Value, x.Height, tip - x.Height + 1, x.Script, x.Coinbase})
	}
	resp := map[string]any{"address": addr, "script_pubkey": a.Script, "tip_height": tip, "total": total, "utxos": out}
	if from+len(out) < int(total) {
		resp["next"] = from + len(out)
	}
	writeJSON(w, 200, resp, 5)
}

func (s *Server) apiMempool(w http.ResponseWriter, r *http.Request) {
	m := s.ix.Mempool()
	var bytes, fees int64
	hist := map[string]int{}
	for _, t := range m {
		bytes += t.Size
		fees += t.Fee
		rate := 0.0
		if t.Size > 0 {
			rate = float64(t.Fee) / float64(t.Size)
		}
		hist[bucket(rate)]++
	}
	writeJSON(w, 200, map[string]any{"count": len(m), "bytes": bytes, "total_fee": fees, "fee_rate_histogram": hist}, 5)
}

func bucket(rate float64) string {
	switch {
	case rate < 1:
		return "<1"
	case rate < 2:
		return "1-2"
	case rate < 5:
		return "2-5"
	case rate < 10:
		return "5-10"
	default:
		return "10+"
	}
}

func (s *Server) apiMempoolTxids(w http.ResponseWriter, r *http.Request) {
	m := s.ix.Mempool()
	ids := make([]string, 0, len(m))
	for _, t := range m {
		ids = append(ids, t.Txid)
	}
	sort.Strings(ids)
	writeJSON(w, 200, map[string]any{"txids": ids}, 5)
}

func (s *Server) apiFees(w http.ResponseWriter, r *http.Request) {
	v, err := s.cached("fees", 30*time.Second, func() (any, error) {
		out := map[string]any{"unit": "atoms_per_byte"}
		for _, t := range []int{1, 3, 6, 12} {
			sf, err := s.rpc.EstimateSmartFee(r.Context(), t)
			if err == nil && sf.FeeRate > 0 {
				out[strconv.Itoa(t)] = rpc.Atoms(sf.FeeRate) / 1000 // QTC/kB -> atoms/B
			} else {
				out[strconv.Itoa(t)] = nil
			}
		}
		return out, nil
	})
	if err != nil {
		apiErr(w, 500, "internal", err.Error())
		return
	}
	writeJSON(w, 200, v, 30)
}

func (s *Server) difficulty(ctx context.Context) (map[string]any, error) {
	v, err := s.cached("difficulty", 10*time.Second, func() (any, error) {
		raw, err := s.rpc.GetDifficultyHealth(ctx, 120)
		if err != nil {
			return nil, err
		}
		var health map[string]any
		_ = json.Unmarshal(raw, &health)
		recent, _ := s.st.RecentBlocks(120)
		type sample struct {
			Height     int64   `json:"height"`
			Time       int64   `json:"time"`
			Difficulty float64 `json:"difficulty"`
			Interval   int64   `json:"interval_s"`
		}
		samples := make([]sample, 0, len(recent))
		for _, b := range recent {
			samples = append(samples, sample{b.Height, b.Time, b.Difficulty, b.Interval})
		}
		return map[string]any{"health": health, "samples": samples}, nil
	})
	if err != nil {
		return nil, err
	}
	return v.(map[string]any), nil
}

func (s *Server) apiDifficulty(w http.ResponseWriter, r *http.Request) {
	d, err := s.difficulty(r.Context())
	if err != nil {
		apiErr(w, 502, "node_unavailable", "node did not answer")
		return
	}
	writeJSON(w, 200, d, 10)
}

type searchResult struct {
	Type string            `json:"type"`
	ID   string            `json:"id"`
	Hits []store.PrefixHit `json:"hits,omitempty"`
}

// search resolves free text to one target or a list of prefix hits.
func (s *Server) search(ctx context.Context, q string) (*searchResult, error) {
	q = strings.TrimSpace(q)
	if q == "" {
		return nil, store.ErrNotFound
	}
	if h, err := strconv.ParseInt(q, 10, 64); err == nil && h >= 0 {
		if b, err := s.st.GetBlockByHeight(h); err == nil {
			return &searchResult{Type: "block", ID: b.Hash}, nil
		}
	}
	lq := strings.ToLower(q)
	if hex64.MatchString(lq) {
		if _, err := s.st.GetBlock(lq); err == nil {
			return &searchResult{Type: "block", ID: lq}, nil
		}
		if _, err := s.lookupTx(ctx, lq); err == nil {
			return &searchResult{Type: "tx", ID: lq}, nil
		}
		if b, err := s.st.FindByDigest(lq); err == nil {
			return &searchResult{Type: "block", ID: b.Hash}, nil
		}
		// A raw 32-byte script is unlikely; fall through to prefix search.
	}
	if _, err := s.st.GetAddr(q); err == nil {
		return &searchResult{Type: "address", ID: q}, nil
	}
	if hexAny.MatchString(lq) || addrLike.MatchString(q) {
		hits, err := s.st.Prefix(lq, 20)
		if err == nil && len(hits) == 1 {
			return &searchResult{Type: hits[0].Type, ID: hits[0].ID}, nil
		}
		if err == nil && len(hits) > 1 {
			return &searchResult{Type: "multiple", Hits: hits}, nil
		}
		if !hexAny.MatchString(lq) {
			hits, _ = s.st.Prefix(q, 20)
			if len(hits) == 1 {
				return &searchResult{Type: hits[0].Type, ID: hits[0].ID}, nil
			}
			if len(hits) > 1 {
				return &searchResult{Type: "multiple", Hits: hits}, nil
			}
		}
	}
	return nil, store.ErrNotFound
}

func (s *Server) apiSearch(w http.ResponseWriter, r *http.Request) {
	res, err := s.search(r.Context(), r.URL.Query().Get("q"))
	if err != nil {
		apiErr(w, 404, "not_found", "nothing matched")
		return
	}
	writeJSON(w, 200, res, 0)
}

// ---- HTML pages ----------------------------------------------------------

func (s *Server) loadTemplates() {
	funcs := template.FuncMap{
		"qtc":   fmtQTC,
		"atoms": func(v int64) string { return fmtInt(v) },
		"int":   fmtInt,
		"utc":   func(t int64) string { return time.Unix(t, 0).UTC().Format("2006-01-02 15:04:05 UTC") },
		"ago":   ago,
		"short": func(h string) string {
			if len(h) > 16 {
				return h[:8] + "…" + h[len(h)-8:]
			}
			return h
		},
		"rate":     func(f float64) string { return strconv.FormatFloat(f, 'f', 2, 64) },
		"diff":     func(f float64) string { return strconv.FormatFloat(f, 'g', 6, 64) },
		"sub":      func(a, b int64) int64 { return a - b },
		"add":      func(a, b int64) int64 { return a + b },
		"typeName": typeName,
		"json":     func(v any) string { b, _ := json.MarshalIndent(v, "", " "); return string(b) },
		"seq": func(n int) []int {
			r := make([]int, n)
			for i := range r {
				r[i] = i
			}
			return r
		},
		"divf": func(a, b int64) float64 {
			if b == 0 {
				return 0
			}
			return float64(a) / float64(b)
		},
		"mul": func(a, b int64) int64 { return a * b },
		"int64": func(v any) int64 {
			switch x := v.(type) {
			case int:
				return int64(x)
			case int64:
				return x
			case uint64:
				return int64(x)
			case float64:
				return int64(x)
			}
			return 0
		},
		"chart":  intervalChart,
		"dchart": difficultyChart,
	}
	s.tmpl = map[string]*template.Template{}
	pages := []string{"home", "blocks", "block", "tx", "address", "mempool", "difficulty", "search", "status", "api", "error"}
	for _, p := range pages {
		t := template.Must(template.New("layout.html").Funcs(funcs).ParseFS(tmplFS, "templates/layout.html", "templates/"+p+".html"))
		s.tmpl[p] = t
	}
}

func fmtQTC(atoms int64) string {
	neg := atoms < 0
	if neg {
		atoms = -atoms
	}
	whole, frac := atoms/1e8, atoms%1e8
	s := fmt.Sprintf("%s.%08d", fmtInt(whole), frac)
	if neg {
		s = "-" + s
	}
	return s
}

func fmtInt(v int64) string {
	s := strconv.FormatInt(v, 10)
	neg := strings.HasPrefix(s, "-")
	if neg {
		s = s[1:]
	}
	var out []byte
	for i, c := range []byte(s) {
		if i > 0 && (len(s)-i)%3 == 0 {
			out = append(out, ',')
		}
		out = append(out, c)
	}
	if neg {
		return "-" + string(out)
	}
	return string(out)
}

func ago(t int64) string {
	d := time.Since(time.Unix(t, 0))
	switch {
	case d < time.Minute:
		return fmt.Sprintf("%ds ago", int(d.Seconds()))
	case d < time.Hour:
		return fmt.Sprintf("%dm ago", int(d.Minutes()))
	case d < 48*time.Hour:
		return fmt.Sprintf("%dh ago", int(d.Hours()))
	default:
		return fmt.Sprintf("%dd ago", int(d.Hours()/24))
	}
}

func typeName(t string) string {
	switch t {
	case "witness_v2_p2mr":
		return "P2MR (post-quantum)"
	case "witness_v1_taproot":
		return "Taproot"
	case "witness_v0_keyhash":
		return "P2WPKH"
	case "witness_v0_scripthash":
		return "P2WSH"
	case "pubkeyhash":
		return "P2PKH"
	case "scripthash":
		return "P2SH"
	case "nulldata":
		return "OP_RETURN"
	case "anchor":
		return "Anchor"
	case "":
		return "unknown"
	}
	return t
}

// intervalChart draws block intervals as an inline SVG bar chart.
func intervalChart(bs []*store.Block, target int64) template.HTML {
	if len(bs) == 0 {
		return ""
	}
	const w, h, pad = 720.0, 160.0, 24.0
	maxv := target * 2
	cap := target * 6
	for _, b := range bs {
		if b.Interval > maxv {
			maxv = b.Interval
		}
	}
	if maxv > cap {
		maxv = cap
	}
	bw := (w - 2*pad) / float64(len(bs))
	var sb strings.Builder
	fmt.Fprintf(&sb, `<svg viewBox="0 0 %.0f %.0f" class="chart" role="img" aria-label="block intervals">`, w, h)
	ty := h - pad - (float64(target)/float64(maxv))*(h-2*pad)
	fmt.Fprintf(&sb, `<line x1="%.0f" y1="%.1f" x2="%.0f" y2="%.1f" class="target"/><text x="%.0f" y="%.1f" class="lbl">target %ds</text>`, pad, ty, w-pad, ty, w-pad-90, ty-4, target)
	for i, b := range bs {
		iv := b.Interval
		cls := "bar"
		if iv > maxv {
			iv, cls = maxv, "bar clamped"
		}
		bh := (float64(iv) / float64(maxv)) * (h - 2*pad)
		fmt.Fprintf(&sb, `<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" class="%s"><title>#%d: %ds</title></rect>`, pad+float64(i)*bw, h-pad-bh, bw*0.8, bh, cls, b.Height, b.Interval)
	}
	fmt.Fprintf(&sb, `<text x="%.0f" y="%.0f" class="lbl">#%d</text><text x="%.0f" y="%.0f" class="lbl end">#%d</text></svg>`, pad, h-6, bs[0].Height, w-pad, h-6, bs[len(bs)-1].Height)
	return template.HTML(sb.String())
}

// difficultyChart draws difficulty as a line.
func difficultyChart(bs []*store.Block) template.HTML {
	if len(bs) < 2 {
		return ""
	}
	const w, h, pad = 720.0, 160.0, 24.0
	minv, maxv := bs[0].Difficulty, bs[0].Difficulty
	for _, b := range bs {
		if b.Difficulty < minv {
			minv = b.Difficulty
		}
		if b.Difficulty > maxv {
			maxv = b.Difficulty
		}
	}
	if maxv == minv {
		maxv = minv * 1.1
		minv = minv * 0.9
		if maxv == 0 {
			maxv = 1
		}
	}
	var sb strings.Builder
	fmt.Fprintf(&sb, `<svg viewBox="0 0 %.0f %.0f" class="chart" role="img" aria-label="difficulty">`, w, h)
	sb.WriteString(`<polyline class="line" points="`)
	for i, b := range bs {
		x := pad + float64(i)/float64(len(bs)-1)*(w-2*pad)
		y := h - pad - (b.Difficulty-minv)/(maxv-minv)*(h-2*pad)
		fmt.Fprintf(&sb, "%.1f,%.1f ", x, y)
	}
	fmt.Fprintf(&sb, `"/><text x="%.0f" y="%.0f" class="lbl">%s</text><text x="%.0f" y="%.0f" class="lbl">%s</text></svg>`, pad, pad-6, strconv.FormatFloat(maxv, 'g', 4, 64), pad, h-6, strconv.FormatFloat(minv, 'g', 4, 64))
	return template.HTML(sb.String())
}

type pageData struct {
	Chain  string
	Title  string
	Query  string
	Status indexer.Status
	Data   any
}

func (s *Server) render(w http.ResponseWriter, status int, page, title string, data any) {
	t := s.tmpl[page]
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.WriteHeader(status)
	if err := t.ExecuteTemplate(w, "layout.html", pageData{Chain: s.chain, Title: title, Status: s.ix.Status(), Data: data}); err != nil {
		log.Printf("render %s: %v", page, err)
	}
}

func (s *Server) notFound(w http.ResponseWriter, what string) {
	s.render(w, 404, "error", "Not found", what)
}

func (s *Server) pageHome(w http.ResponseWriter, r *http.Request) {
	bs, _ := s.st.Blocks(-1, 20)
	m := s.ix.Mempool()
	sort.Slice(m, func(i, j int) bool { return m[i].FirstSeen > m[j].FirstSeen })
	if len(m) > 10 {
		m = m[:10]
	}
	recent, _ := s.st.RecentBlocks(60)
	var health map[string]any
	if d, err := s.difficulty(r.Context()); err == nil {
		health, _ = d["health"].(map[string]any)
	}
	s.render(w, 200, "home", "QTC Explorer", map[string]any{"Blocks": bs, "Mempool": m, "Recent": recent, "Health": health, "Target": targetSpacing(health)})
}

func targetSpacing(h map[string]any) int64 {
	if v, ok := h["target_spacing_s"].(float64); ok {
		return int64(v)
	}
	return 600
}

func (s *Server) pageBlocks(w http.ResponseWriter, r *http.Request) {
	before := int64Param(r, "before", -1)
	bs, _ := s.st.Blocks(before, 50)
	var next int64 = -1
	if len(bs) == 50 {
		next = bs[len(bs)-1].Height
	}
	s.render(w, 200, "blocks", "Blocks", map[string]any{"Blocks": bs, "Next": next})
}

func (s *Server) pageBlock(w http.ResponseWriter, r *http.Request) {
	b, err := s.resolveBlock(r.PathValue("id"))
	if err != nil {
		s.notFound(w, "No block matches "+r.PathValue("id"))
		return
	}
	page := int(int64Param(r, "page", 0))
	const per = 50
	txs, total, _ := s.st.BlockTxs(b.Height, page*per, per)
	if b.Orphaned {
		txs, total = nil, 0
	}
	var next *store.Block
	if nb, err := s.st.GetBlockByHeight(b.Height + 1); err == nil {
		next = nb
	}
	s.render(w, 200, "block", fmt.Sprintf("Block %d", b.Height), map[string]any{
		"B": b, "Txs": txs, "Total": total, "Page": page, "Per": per, "Next": next,
		"Confirmations": s.tipHeight() - b.Height + 1,
	})
}

func (s *Server) pageTx(w http.ResponseWriter, r *http.Request) {
	tx, err := s.lookupTx(r.Context(), r.PathValue("txid"))
	if err != nil {
		s.notFound(w, "No transaction matches "+r.PathValue("txid"))
		return
	}
	v := s.viewTx(tx)
	dec := map[int]pq.Decoded{}
	for _, in := range v.Inputs {
		dec[in.Index] = in.Decoded
	}
	s.render(w, 200, "tx", "Transaction "+tx.Txid[:12], map[string]any{"T": v, "Dec": dec, "Chain": s.chain})
}

func (s *Server) pageAddr(w http.ResponseWriter, r *http.Request) {
	addr := r.PathValue("addr")
	a, err := s.addrOrEmpty(addr)
	if err != nil {
		s.notFound(w, "Not an address: "+addr)
		return
	}
	before := int64Param(r, "before", -1)
	txs, total, _ := s.st.AddrTxs(addr, before, 25)
	us, utotal, _ := s.st.AddrUTXOs(addr, 0, 50)
	var next int64 = -1
	if len(txs) == 25 {
		next = txs[len(txs)-1].Height
	}
	s.render(w, 200, "address", "Address "+addr[:12], map[string]any{"A": a, "Txs": txs, "Total": total, "UTXOs": us, "UTotal": utotal, "Next": next, "Tip": s.tipHeight()})
}

func (s *Server) pageMempool(w http.ResponseWriter, r *http.Request) {
	m := s.ix.Mempool()
	sort.Slice(m, func(i, j int) bool { return m[i].FirstSeen > m[j].FirstSeen })
	var bytes, fees int64
	for _, t := range m {
		bytes += t.Size
		fees += t.Fee
	}
	s.render(w, 200, "mempool", "Mempool", map[string]any{"Txs": m, "Bytes": bytes, "Fees": fees})
}

func (s *Server) pageDifficulty(w http.ResponseWriter, r *http.Request) {
	d, err := s.difficulty(r.Context())
	var health map[string]any
	if err == nil {
		health, _ = d["health"].(map[string]any)
	}
	recent, _ := s.st.RecentBlocks(120)
	s.render(w, 200, "difficulty", "Difficulty", map[string]any{"Health": health, "Recent": recent, "Target": targetSpacing(health), "Err": err != nil})
}

func (s *Server) pageSearch(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query().Get("q")
	res, err := s.search(r.Context(), q)
	if err != nil {
		s.render(w, 404, "search", "Search", map[string]any{"Q": q})
		return
	}
	switch res.Type {
	case "block":
		http.Redirect(w, r, "/block/"+res.ID, http.StatusFound)
	case "tx":
		http.Redirect(w, r, "/tx/"+res.ID, http.StatusFound)
	case "address":
		http.Redirect(w, r, "/address/"+res.ID, http.StatusFound)
	default:
		s.render(w, 200, "search", "Search", map[string]any{"Q": q, "Hits": res.Hits})
	}
}

func (s *Server) pageStatus(w http.ResponseWriter, r *http.Request) {
	s.render(w, 200, "status", "Status", s.ix.Status())
}

func (s *Server) pageAPI(w http.ResponseWriter, r *http.Request) {
	s.render(w, 200, "api", "API", nil)
}
