// Package gateway implements the anonymous, rate-limited broadcast endpoint
// described in doc/browser-wallet-backend.md and spec §7.1.
package gateway

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"

	"qtc-explorer/internal/rpc"
)

const (
	MaxBodyBytes   = 64 << 10 // 64 KiB
	dedupeWindow   = 10 * time.Minute
	maxInFlight    = 4
	acquireTimeout = 5 * time.Second
)

type Config struct {
	AllowedOrigin string        // exact origin allowed through CORS; "" disables CORS headers
	PerIPPerMin   int           // broadcast attempts per client IP per minute
	Timeout       time.Duration // upstream RPC timeout per call
}

type Gateway struct {
	rpc  *rpc.Client
	cfg  Config
	sem  chan struct{}
	mu   sync.Mutex
	seen map[string]time.Time // sha256d(tx bytes) -> first seen
	ips  map[string][]time.Time
}

func New(c *rpc.Client, cfg Config) *Gateway {
	if cfg.PerIPPerMin <= 0 {
		cfg.PerIPPerMin = 6
	}
	if cfg.Timeout <= 0 {
		cfg.Timeout = 20 * time.Second
	}
	// The gateway's RPC client may only ever call these two methods.
	c.Allow = map[string]bool{"testmempoolaccept": true, "sendrawtransaction": true}
	return &Gateway{rpc: c, cfg: cfg, sem: make(chan struct{}, maxInFlight), seen: map[string]time.Time{}, ips: map[string][]time.Time{}}
}

type errBody struct {
	Error struct {
		Code    string `json:"code"`
		Message string `json:"message"`
	} `json:"error"`
}

func writeErr(w http.ResponseWriter, status int, code, msg string) {
	var e errBody
	e.Error.Code, e.Error.Message = code, msg
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(e)
}

func clientIP(r *http.Request) string {
	// Behind nginx/Cloudflare the operator sets the real IP header; on localhost use RemoteAddr.
	if v := r.Header.Get("X-Real-IP"); v != "" {
		return v
	}
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return r.RemoteAddr
	}
	return host
}

func (g *Gateway) cors(w http.ResponseWriter) {
	if g.cfg.AllowedOrigin == "" {
		return
	}
	w.Header().Set("Access-Control-Allow-Origin", g.cfg.AllowedOrigin)
	w.Header().Set("Vary", "Origin")
	w.Header().Set("Access-Control-Allow-Methods", "POST")
	w.Header().Set("Access-Control-Allow-Headers", "Content-Type")
	w.Header().Set("Access-Control-Max-Age", "600")
}

func (g *Gateway) allowIP(ip string) bool {
	g.mu.Lock()
	defer g.mu.Unlock()
	now := time.Now()
	cut := now.Add(-time.Minute)
	kept := g.ips[ip][:0]
	for _, t := range g.ips[ip] {
		if t.After(cut) {
			kept = append(kept, t)
		}
	}
	if len(kept) >= g.cfg.PerIPPerMin {
		g.ips[ip] = kept
		return false
	}
	g.ips[ip] = append(kept, now)
	return true
}

// dedupe returns false if this exact transaction was submitted within the window.
func (g *Gateway) dedupe(raw []byte) bool {
	h1 := sha256.Sum256(raw)
	h2 := sha256.Sum256(h1[:])
	key := hex.EncodeToString(h2[:])
	g.mu.Lock()
	defer g.mu.Unlock()
	now := time.Now()
	for k, t := range g.seen {
		if now.Sub(t) > dedupeWindow {
			delete(g.seen, k)
		}
	}
	if _, dup := g.seen[key]; dup {
		return false
	}
	g.seen[key] = now
	return true
}

// ServeHTTP handles POST /api/v1/tx/broadcast (and its CORS preflight).
func (g *Gateway) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	g.cors(w)
	switch r.Method {
	case http.MethodOptions:
		w.WriteHeader(http.StatusNoContent)
		return
	case http.MethodPost:
	default:
		writeErr(w, http.StatusMethodNotAllowed, "method_not_allowed", "POST only")
		return
	}
	if g.cfg.AllowedOrigin != "" {
		if o := r.Header.Get("Origin"); o != "" && o != g.cfg.AllowedOrigin {
			writeErr(w, http.StatusForbidden, "origin_not_allowed", "origin not allowed")
			return
		}
	}
	ip := clientIP(r)
	if !g.allowIP(ip) {
		writeErr(w, http.StatusTooManyRequests, "rate_limited", "too many broadcasts from this client")
		return
	}
	if ct := r.Header.Get("Content-Type"); !strings.HasPrefix(ct, "application/json") {
		writeErr(w, http.StatusUnsupportedMediaType, "bad_content_type", "send application/json")
		return
	}
	body, err := io.ReadAll(io.LimitReader(r.Body, MaxBodyBytes+1))
	if err != nil || len(body) > MaxBodyBytes {
		writeErr(w, http.StatusRequestEntityTooLarge, "too_large", "request body exceeds 64 KiB")
		return
	}
	var req struct {
		Hex *string `json:"hex"`
	}
	if err := json.Unmarshal(body, &req); err != nil || req.Hex == nil {
		writeErr(w, http.StatusBadRequest, "bad_json", `body must be {"hex": "<raw transaction>"}`)
		return
	}
	hx := strings.TrimSpace(*req.Hex)
	if len(hx) == 0 || len(hx)%2 != 0 {
		writeErr(w, http.StatusBadRequest, "bad_hex", "hex must be non-empty with even length")
		return
	}
	raw, err := hex.DecodeString(hx)
	if err != nil {
		writeErr(w, http.StatusBadRequest, "bad_hex", "hex contains non-hex characters")
		return
	}
	if !g.dedupe(raw) {
		writeErr(w, http.StatusConflict, "duplicate", "this transaction was already submitted recently")
		return
	}
	select {
	case g.sem <- struct{}{}:
		defer func() { <-g.sem }()
	case <-time.After(acquireTimeout):
		writeErr(w, http.StatusServiceUnavailable, "busy", "gateway busy, retry shortly")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), g.cfg.Timeout)
	defer cancel()

	acc, err := g.rpc.TestMempoolAccept(ctx, hx)
	if err != nil {
		var rerr *rpc.Error
		if errors.As(err, &rerr) {
			// The node could not even decode the transaction; that is a client error.
			log.Printf("gateway: testmempoolaccept rejected size=%d: %s", len(raw), rerr.Message)
			writeErr(w, http.StatusUnprocessableEntity, "rejected", rerr.Message)
			return
		}
		log.Printf("gateway: testmempoolaccept failed for %s: %v", ip, redact(err))
		writeErr(w, http.StatusBadGateway, "node_unavailable", "node did not answer")
		return
	}
	if !acc.Allowed {
		log.Printf("gateway: rejected txid=%s size=%d reason=%q", acc.Txid, len(raw), acc.RejectReason)
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusUnprocessableEntity)
		_ = json.NewEncoder(w).Encode(map[string]any{"error": map[string]string{"code": "rejected", "message": acc.RejectReason}, "txid": acc.Txid})
		return
	}
	txid, err := g.rpc.SendRawTransaction(ctx, hx)
	if err != nil {
		var rerr *rpc.Error
		if errors.As(err, &rerr) {
			log.Printf("gateway: sendrawtransaction rejected txid=%s: %s", acc.Txid, rerr.Message)
			writeErr(w, http.StatusUnprocessableEntity, "rejected", rerr.Message)
			return
		}
		log.Printf("gateway: sendrawtransaction failed: %v", redact(err))
		writeErr(w, http.StatusBadGateway, "node_unavailable", "node did not answer")
		return
	}
	log.Printf("gateway: accepted txid=%s size=%d", txid, len(raw))
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(map[string]string{"txid": txid})
}

// redact keeps host details out of logs that might be shipped elsewhere.
func redact(err error) string {
	s := err.Error()
	if i := strings.Index(s, "http://"); i >= 0 {
		return s[:i] + "<rpc>"
	}
	return s
}
