// Package store keeps every explorer record in a single Bleve (scorch) index.
//
// Each record is a flat map with a "type" field that selects the document
// mapping, a handful of indexed fields used for lookups, sorting and prefix
// search, and one stored-but-unindexed "json" field holding the full record.
// Reads always come back through the json field, so numeric precision is
// never subject to Bleve's float64 representation.
package store

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"

	"github.com/blevesearch/bleve/v2"
	"github.com/blevesearch/bleve/v2/analysis/analyzer/keyword"
	"github.com/blevesearch/bleve/v2/index/scorch"
	"github.com/blevesearch/bleve/v2/mapping"
	"github.com/blevesearch/bleve/v2/search/query"
	index "github.com/blevesearch/bleve_index_api"
)

// Document types.
const (
	TypeBlock = "block"
	TypeTx    = "tx"
	TypeUTXO  = "utxo"
	TypeAddr  = "addr"
	TypeState = "state"
)

// ---- records -------------------------------------------------------------

type Block struct {
	Height        int64    `json:"height"`
	Hash          string   `json:"hash"`
	Prev          string   `json:"prev"`
	Time          int64    `json:"time"`
	MedianTime    int64    `json:"mediantime"`
	Interval      int64    `json:"interval"` // seconds since previous block, 0 at genesis
	Version       int64    `json:"version"`
	VersionHex    string   `json:"version_hex"`
	MerkleRoot    string   `json:"merkleroot"`
	Bits          string   `json:"bits"`
	Target        string   `json:"target"`
	Difficulty    float64  `json:"difficulty"`
	ChainWork     string   `json:"chainwork"`
	Nonce64       string   `json:"nonce64"`
	MatmulDigest  string   `json:"matmul_digest"`
	MatmulDim     int64    `json:"matmul_dim"`
	SeedA         string   `json:"seed_a"`
	SeedB         string   `json:"seed_b"`
	MatrixCWords  int64    `json:"matrix_c_words"`
	Size          int64    `json:"size"`
	StrippedSize  int64    `json:"strippedsize"`
	Weight        int64    `json:"weight"`
	NTx           int64    `json:"n_tx"`
	Txids         []string `json:"txids"`
	SubsidyBase   int64    `json:"subsidy_base"`   // atoms, from halving schedule
	CoinbaseValue int64    `json:"coinbase_value"` // atoms
	FeesTotal     int64    `json:"fees_total"`     // atoms, sum of tx fees
	Orphaned      bool     `json:"orphaned"`
}

func (b *Block) SubsidyActual() int64 { return b.CoinbaseValue - b.FeesTotal }
func (b *Block) Penalised() bool      { return b.SubsidyActual() < b.SubsidyBase }

type TxIn struct {
	Coinbase  string   `json:"coinbase,omitempty"`
	Txid      string   `json:"txid,omitempty"`
	Vout      int64    `json:"vout"`
	Value     int64    `json:"value"` // atoms of the spent output (0 for coinbase)
	Address   string   `json:"address,omitempty"`
	Script    string   `json:"script,omitempty"` // spent scriptPubKey hex
	Sequence  int64    `json:"sequence"`
	Witness   []string `json:"witness,omitempty"`
	Nullifier string   `json:"nullifier,omitempty"`
}

type TxOut struct {
	N              int64  `json:"n"`
	Value          int64  `json:"value"`
	Script         string `json:"script"`
	Type           string `json:"type"`
	Address        string `json:"address,omitempty"`
	Asm            string `json:"asm,omitempty"`
	SpentBy        string `json:"spent_by,omitempty"`
	NoteCommitment string `json:"note_commitment,omitempty"`
}

type Tx struct {
	Txid        string   `json:"txid"`
	Hash        string   `json:"hash"`
	Height      int64    `json:"height"`
	BlockHash   string   `json:"blockhash"`
	BlockTime   int64    `json:"blocktime"`
	Index       int64    `json:"index"`
	Version     int64    `json:"version"`
	Locktime    int64    `json:"locktime"`
	Size        int64    `json:"size"`
	Weight      int64    `json:"weight"`
	Fee         int64    `json:"fee"`
	Coinbase    bool     `json:"coinbase"`
	Shielded    bool     `json:"shielded"`
	ShieldedIn  int64    `json:"shielded_in"`
	ShieldedOut int64    `json:"shielded_out"`
	InputValue  int64    `json:"input_value"`
	OutputValue int64    `json:"output_value"`
	Addresses   []string `json:"addresses"`
	Vin         []TxIn   `json:"vin"`
	Vout        []TxOut  `json:"vout"`
	Hex         string   `json:"hex"`
}

type UTXO struct {
	Txid     string `json:"txid"`
	Vout     int64  `json:"vout"`
	Value    int64  `json:"value"`
	Script   string `json:"script"`
	Type     string `json:"type"`
	Address  string `json:"address,omitempty"`
	Height   int64  `json:"height"`
	Coinbase bool   `json:"coinbase"`
	Spent    bool   `json:"spent"`
	SpentBy  string `json:"spent_by,omitempty"`
	SpentAt  int64  `json:"spent_at,omitempty"`
}

type Addr struct {
	Address     string `json:"address"`
	Script      string `json:"script"`
	Type        string `json:"type"`
	FirstHeight int64  `json:"first_height"`
	TxCount     int64  `json:"tx_count"`
	Received    int64  `json:"received"`
	Sent        int64  `json:"sent"`
	UTXOCount   int64  `json:"utxo_count"`
}

func (a *Addr) Balance() int64 { return a.Received - a.Sent }

type State struct {
	Height    int64  `json:"height"` // -1 when nothing indexed
	Hash      string `json:"hash"`
	Chain     string `json:"chain"`
	Reorgs    int64  `json:"reorgs"`
	LastReorg int64  `json:"last_reorg_depth"`
	UpdatedAt int64  `json:"updated_at"`
}

// ---- ids -----------------------------------------------------------------

func BlockID(hash string) string         { return "b:" + hash }
func TxID(txid string) string            { return "t:" + txid }
func UTXOID(txid string, n int64) string { return fmt.Sprintf("u:%s:%d", txid, n) }
func AddrID(addr string) string          { return "a:" + addr }

const StateID = "s:state"

// ---- store ---------------------------------------------------------------

type Store struct {
	idx bleve.Index
}

var ErrNotFound = errors.New("not found")

func keywordField(store bool) *mapping.FieldMapping {
	f := bleve.NewKeywordFieldMapping()
	f.Store = store
	f.IncludeInAll = false
	f.IncludeTermVectors = false
	return f
}

func numField() *mapping.FieldMapping {
	f := bleve.NewNumericFieldMapping()
	f.Store = false
	f.IncludeInAll = false
	return f
}

func boolField() *mapping.FieldMapping {
	f := bleve.NewBooleanFieldMapping()
	f.Store = false
	f.IncludeInAll = false
	return f
}

func jsonField() *mapping.FieldMapping {
	f := bleve.NewTextFieldMapping()
	f.Index = false
	f.Store = true
	f.IncludeInAll = false
	f.IncludeTermVectors = false
	return f
}

func buildMapping() mapping.IndexMapping {
	m := bleve.NewIndexMapping()
	m.TypeField = "type"
	m.DefaultAnalyzer = keyword.Name
	m.DefaultMapping.Dynamic = false
	m.StoreDynamic = false
	m.IndexDynamic = false

	newDoc := func() *mapping.DocumentMapping {
		d := bleve.NewDocumentMapping()
		d.Dynamic = false
		d.AddFieldMappingsAt("type", keywordField(false))
		d.AddFieldMappingsAt("json", jsonField())
		return d
	}

	b := newDoc()
	b.AddFieldMappingsAt("height", numField())
	b.AddFieldMappingsAt("hash", keywordField(false))
	b.AddFieldMappingsAt("digest", keywordField(false))
	b.AddFieldMappingsAt("time", numField())
	b.AddFieldMappingsAt("orphaned", boolField())
	m.AddDocumentMapping(TypeBlock, b)

	t := newDoc()
	t.AddFieldMappingsAt("txid", keywordField(false))
	t.AddFieldMappingsAt("height", numField())
	t.AddFieldMappingsAt("index", numField())
	t.AddFieldMappingsAt("addresses", keywordField(false))
	m.AddDocumentMapping(TypeTx, t)

	u := newDoc()
	u.AddFieldMappingsAt("address", keywordField(false))
	u.AddFieldMappingsAt("script", keywordField(false))
	u.AddFieldMappingsAt("height", numField())
	u.AddFieldMappingsAt("spent", boolField())
	u.AddFieldMappingsAt("value", numField())
	m.AddDocumentMapping(TypeUTXO, u)

	a := newDoc()
	a.AddFieldMappingsAt("address", keywordField(false))
	a.AddFieldMappingsAt("script", keywordField(false))
	m.AddDocumentMapping(TypeAddr, a)

	s := newDoc()
	m.AddDocumentMapping(TypeState, s)
	return m
}

// Open opens or creates the index at path using the scorch backend.
func Open(path string) (*Store, error) {
	var idx bleve.Index
	if _, err := os.Stat(path); err == nil {
		idx, err = bleve.OpenUsing(path, map[string]any{})
		if err != nil {
			return nil, fmt.Errorf("open index: %w", err)
		}
	} else {
		var err error
		idx, err = bleve.NewUsing(path, buildMapping(), scorch.Name, scorch.Name, nil)
		if err != nil {
			return nil, fmt.Errorf("create index: %w", err)
		}
	}
	return &Store{idx: idx}, nil
}

func (s *Store) Close() error { return s.idx.Close() }

func (s *Store) DocCount() (uint64, error) { return s.idx.DocCount() }

// Batch collects writes for one atomic commit (one block).
type Batch struct{ b *bleve.Batch }

func (s *Store) NewBatch() *Batch { return &Batch{b: s.idx.NewBatch()} }

func (s *Store) Commit(b *Batch) error { return s.idx.Batch(b.b) }

func mustJSON(v any) string {
	j, err := json.Marshal(v)
	if err != nil {
		panic(err)
	}
	return string(j)
}

func (b *Batch) PutBlock(blk *Block) {
	_ = b.b.Index(BlockID(blk.Hash), map[string]any{
		"type": TypeBlock, "height": blk.Height, "hash": blk.Hash, "digest": blk.MatmulDigest,
		"time": blk.Time, "orphaned": blk.Orphaned, "json": mustJSON(blk),
	})
}

func (b *Batch) PutTx(tx *Tx) {
	_ = b.b.Index(TxID(tx.Txid), map[string]any{
		"type": TypeTx, "txid": tx.Txid, "height": tx.Height, "index": tx.Index,
		"addresses": tx.Addresses, "json": mustJSON(tx),
	})
}

func (b *Batch) DeleteTx(txid string) { b.b.Delete(TxID(txid)) }

func (b *Batch) PutUTXO(u *UTXO) {
	_ = b.b.Index(UTXOID(u.Txid, u.Vout), map[string]any{
		"type": TypeUTXO, "address": u.Address, "script": u.Script, "height": u.Height,
		"spent": u.Spent, "value": u.Value, "json": mustJSON(u),
	})
}

func (b *Batch) DeleteUTXO(txid string, n int64) { b.b.Delete(UTXOID(txid, n)) }

func (b *Batch) PutAddr(a *Addr) {
	_ = b.b.Index(AddrID(a.Address), map[string]any{
		"type": TypeAddr, "address": a.Address, "script": a.Script, "json": mustJSON(a),
	})
}

func (b *Batch) DeleteAddr(addr string) { b.b.Delete(AddrID(addr)) }

func (b *Batch) PutState(st *State) {
	_ = b.b.Index(StateID, map[string]any{"type": TypeState, "json": mustJSON(st)})
}

// ---- reads ---------------------------------------------------------------

// get loads a document's stored json field by id.
func (s *Store) get(id string, out any) error {
	doc, err := s.idx.Document(id)
	if err != nil {
		return err
	}
	if doc == nil {
		return ErrNotFound
	}
	var js []byte
	doc.VisitFields(func(f index.Field) {
		if f.Name() == "json" {
			js = f.Value()
		}
	})
	if js == nil {
		return ErrNotFound
	}
	return json.Unmarshal(js, out)
}

func (s *Store) GetState() (*State, error) {
	var st State
	if err := s.get(StateID, &st); err != nil {
		if errors.Is(err, ErrNotFound) {
			return &State{Height: -1}, nil
		}
		return nil, err
	}
	return &st, nil
}

func (s *Store) GetBlock(hash string) (*Block, error) {
	var b Block
	return &b, s.get(BlockID(hash), &b)
}

func (s *Store) GetTx(txid string) (*Tx, error) {
	var t Tx
	return &t, s.get(TxID(txid), &t)
}

func (s *Store) GetUTXO(txid string, n int64) (*UTXO, error) {
	var u UTXO
	return &u, s.get(UTXOID(txid, n), &u)
}

func (s *Store) GetAddr(addr string) (*Addr, error) {
	var a Addr
	return &a, s.get(AddrID(addr), &a)
}

func termQ(field, val string) query.Query {
	q := bleve.NewTermQuery(val)
	q.SetField(field)
	return q
}

func typeQ(t string) query.Query { return termQ("type", t) }

func boolQ(field string, v bool) query.Query {
	q := bleve.NewBoolFieldQuery(v)
	q.SetField(field)
	return q
}

func numEqQ(field string, v int64) query.Query {
	f := float64(v)
	inc := true
	q := bleve.NewNumericRangeInclusiveQuery(&f, &f, &inc, &inc)
	q.SetField(field)
	return q
}

func numLtQ(field string, v int64) query.Query {
	f := float64(v)
	inc := false
	q := bleve.NewNumericRangeInclusiveQuery(nil, &f, nil, &inc)
	q.SetField(field)
	return q
}

// search runs q and decodes each hit's json field into a new T.
func search[T any](s *Store, q query.Query, sort []string, size, from int) ([]*T, uint64, error) {
	req := bleve.NewSearchRequestOptions(q, size, from, false)
	req.Fields = []string{"json"}
	if len(sort) > 0 {
		req.SortBy(sort)
	}
	res, err := s.idx.Search(req)
	if err != nil {
		return nil, 0, err
	}
	out := make([]*T, 0, len(res.Hits))
	for _, h := range res.Hits {
		js, ok := h.Fields["json"].(string)
		if !ok {
			continue
		}
		var v T
		if err := json.Unmarshal([]byte(js), &v); err != nil {
			return nil, 0, err
		}
		out = append(out, &v)
	}
	return out, res.Total, nil
}

// GetBlockByHeight returns the active (non-orphaned) block at height.
func (s *Store) GetBlockByHeight(h int64) (*Block, error) {
	q := bleve.NewConjunctionQuery(typeQ(TypeBlock), numEqQ("height", h), boolQ("orphaned", false))
	bs, _, err := search[Block](s, q, nil, 1, 0)
	if err != nil {
		return nil, err
	}
	if len(bs) == 0 {
		return nil, ErrNotFound
	}
	return bs[0], nil
}

// Blocks lists active blocks newest first, strictly below `before` (pass -1 for tip).
func (s *Store) Blocks(before int64, limit int) ([]*Block, error) {
	qs := []query.Query{typeQ(TypeBlock), boolQ("orphaned", false)}
	if before >= 0 {
		qs = append(qs, numLtQ("height", before))
	}
	bs, _, err := search[Block](s, bleve.NewConjunctionQuery(qs...), []string{"-height"}, limit, 0)
	return bs, err
}

// BlockTxs returns the transactions of a block in block order.
func (s *Store) BlockTxs(height int64, from, limit int) ([]*Tx, uint64, error) {
	q := bleve.NewConjunctionQuery(typeQ(TypeTx), numEqQ("height", height))
	return search[Tx](s, q, []string{"index"}, limit, from)
}

// AddrTxs lists a transaction history newest first.
func (s *Store) AddrTxs(addr string, before int64, limit int) ([]*Tx, uint64, error) {
	qs := []query.Query{typeQ(TypeTx), termQ("addresses", addr)}
	if before >= 0 {
		qs = append(qs, numLtQ("height", before))
	}
	return search[Tx](s, bleve.NewConjunctionQuery(qs...), []string{"-height", "-index"}, limit, 0)
}

// AddrUTXOs lists unspent outputs for an address, oldest first.
func (s *Store) AddrUTXOs(addr string, from, limit int) ([]*UTXO, uint64, error) {
	q := bleve.NewConjunctionQuery(typeQ(TypeUTXO), termQ("address", addr), boolQ("spent", false))
	return search[UTXO](s, q, []string{"height", "_id"}, limit, from)
}

// FindByDigest looks a block up by its MatMul digest (the PoW hash).
func (s *Store) FindByDigest(digest string) (*Block, error) {
	q := bleve.NewConjunctionQuery(typeQ(TypeBlock), termQ("digest", digest))
	bs, _, err := search[Block](s, q, nil, 1, 0)
	if err != nil {
		return nil, err
	}
	if len(bs) == 0 {
		return nil, ErrNotFound
	}
	return bs[0], nil
}

// PrefixHit is one candidate from a prefix search.
type PrefixHit struct {
	Type   string `json:"type"`
	ID     string `json:"id"`
	Height int64  `json:"height,omitempty"`
}

// Prefix searches block hashes, digests, txids and addresses by prefix. This
// is the Bleve-specific feature: partial identifiers resolve instantly.
func (s *Store) Prefix(p string, limit int) ([]PrefixHit, error) {
	mk := func(t, field string) query.Query {
		pq := bleve.NewPrefixQuery(p)
		pq.SetField(field)
		return bleve.NewConjunctionQuery(typeQ(t), pq)
	}
	var hits []PrefixHit
	if bs, _, err := search[Block](s, mk(TypeBlock, "hash"), []string{"-height"}, limit, 0); err == nil {
		for _, b := range bs {
			hits = append(hits, PrefixHit{Type: "block", ID: b.Hash, Height: b.Height})
		}
	}
	if bs, _, err := search[Block](s, mk(TypeBlock, "digest"), []string{"-height"}, limit, 0); err == nil {
		for _, b := range bs {
			hits = append(hits, PrefixHit{Type: "digest", ID: b.Hash, Height: b.Height})
		}
	}
	if ts, _, err := search[Tx](s, mk(TypeTx, "txid"), []string{"-height"}, limit, 0); err == nil {
		for _, t := range ts {
			hits = append(hits, PrefixHit{Type: "tx", ID: t.Txid, Height: t.Height})
		}
	}
	if as, _, err := search[Addr](s, mk(TypeAddr, "address"), nil, limit, 0); err == nil {
		for _, a := range as {
			hits = append(hits, PrefixHit{Type: "address", ID: a.Address})
		}
	}
	if len(hits) > limit {
		hits = hits[:limit]
	}
	return hits, nil
}

// RecentBlocks returns up to n active blocks ending at the tip, oldest first (for charts).
func (s *Store) RecentBlocks(n int) ([]*Block, error) {
	bs, err := s.Blocks(-1, n)
	if err != nil {
		return nil, err
	}
	for i, j := 0, len(bs)-1; i < j; i, j = i+1, j-1 {
		bs[i], bs[j] = bs[j], bs[i]
	}
	return bs, nil
}
