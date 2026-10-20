// Package store: PostgreSQL persistence for wallets, tokens, spends (rolling limits), approvals,
// denials, swaps and the hash-chained audit log. Each process (service, co-signer) uses its own schema
// in the same database; placeholders are rewritten from ? to $n so the SQL reads like the spec.
package store

import (
	"crypto/sha256"
	"database/sql"
	"encoding/hex"
	"fmt"
	"regexp"
	"sort"
	"strings"
	"sync"
	"time"

	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/stdlib"

	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/jsonx"
)

const schemaSQL = `
CREATE TABLE IF NOT EXISTS wallets (
  wallet_id TEXT PRIMARY KEY, agent_id TEXT NOT NULL, tier INTEGER NOT NULL,
  node_wallet TEXT NOT NULL, key_wallet TEXT, descriptor TEXT, receive_address TEXT,
  frozen INTEGER NOT NULL DEFAULT 0, frozen_reason TEXT, created_at DOUBLE PRECISION NOT NULL, retired INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS tokens (
  token_sha256 TEXT PRIMARY KEY, label TEXT, scopes TEXT NOT NULL, created_at DOUBLE PRECISION NOT NULL, revoked INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS spends (
  id BIGSERIAL PRIMARY KEY, wallet_id TEXT NOT NULL, psbt_sha256 TEXT NOT NULL, txid TEXT,
  amount_atoms BIGINT NOT NULL, fee_atoms BIGINT, destinations TEXT NOT NULL, purpose TEXT, status TEXT NOT NULL,
  created_at DOUBLE PRECISION NOT NULL, updated_at DOUBLE PRECISION NOT NULL);
CREATE INDEX IF NOT EXISTS spends_wallet_time ON spends(wallet_id, created_at);
CREATE TABLE IF NOT EXISTS approvals (
  psbt_sha256 TEXT PRIMARY KEY, wallet_id TEXT NOT NULL, psbt TEXT NOT NULL, decoded TEXT NOT NULL,
  request TEXT NOT NULL, expires_at DOUBLE PRECISION NOT NULL, approved INTEGER NOT NULL DEFAULT 0, approved_at DOUBLE PRECISION, consumed INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS denials (wallet_id TEXT NOT NULL, at DOUBLE PRECISION NOT NULL, reason TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS denials_wallet_time ON denials(wallet_id, at);
CREATE TABLE IF NOT EXISTS swaps (
  swap_id TEXT PRIMARY KEY, wallet_id TEXT NOT NULL, state TEXT NOT NULL, data TEXT NOT NULL, created_at DOUBLE PRECISION NOT NULL, updated_at DOUBLE PRECISION NOT NULL);
CREATE TABLE IF NOT EXISTS audit (
  seq BIGSERIAL PRIMARY KEY, at DOUBLE PRECISION NOT NULL, kind TEXT NOT NULL, wallet_id TEXT, actor TEXT,
  payload TEXT NOT NULL, prev_hash TEXT NOT NULL, hash TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);
`

func now() float64 { return float64(time.Now().UnixNano()) / 1e9 }

type Store struct {
	db *sql.DB
	mu sync.Mutex
}

var qmark = regexp.MustCompile(`\?`)

// pg rewrites ? placeholders to $1, $2, ... (no literal ? appear in our SQL).
func pg(q string) string {
	n := 0
	return qmark.ReplaceAllStringFunc(q, func(string) string { n++; return fmt.Sprintf("$%d", n) })
}

// Open connects to PostgreSQL (DSN: URL or key=value form) and uses the given schema, creating it and
// the tables when missing. Example DSN: postgres://aws@127.0.0.1:5432/qtc_aws?sslmode=disable
func Open(dsn, schema string) (*Store, error) {
	if !regexp.MustCompile(`^[a-z_][a-z0-9_]{0,62}$`).MatchString(schema) {
		return nil, fmt.Errorf("bad schema name %q", schema)
	}
	cfg, err := pgx.ParseConfig(dsn)
	if err != nil {
		return nil, fmt.Errorf("postgres dsn: %w", err)
	}
	if cfg.RuntimeParams == nil {
		cfg.RuntimeParams = map[string]string{}
	}
	cfg.RuntimeParams["search_path"] = schema
	db := stdlib.OpenDB(*cfg)
	db.SetMaxOpenConns(4)
	// DDL under an advisory lock: the service and the co-signer start together and CREATE ... IF NOT EXISTS
	// races in PostgreSQL (duplicate pg_type rows) when two sessions create the same objects at once.
	tx, err := db.Begin()
	if err != nil {
		db.Close()
		return nil, fmt.Errorf("postgres: %w", err)
	}
	if _, err := tx.Exec("SELECT pg_advisory_xact_lock(7207455)"); err != nil {
		tx.Rollback()
		db.Close()
		return nil, fmt.Errorf("postgres: %w", err)
	}
	if _, err := tx.Exec("CREATE SCHEMA IF NOT EXISTS " + schema); err != nil {
		tx.Rollback()
		db.Close()
		return nil, fmt.Errorf("postgres: %w", err)
	}
	for _, stmt := range strings.Split(schemaSQL, ";") {
		if strings.TrimSpace(stmt) == "" {
			continue
		}
		if _, err := tx.Exec(stmt); err != nil {
			tx.Rollback()
			db.Close()
			return nil, fmt.Errorf("postgres schema: %w", err)
		}
	}
	if err := tx.Commit(); err != nil {
		db.Close()
		return nil, fmt.Errorf("postgres: %w", err)
	}
	return &Store{db: db}, nil
}

func (s *Store) Close() error { return s.db.Close() }

// Row is a generic row.
type Row = map[string]any

func (s *Store) query(q string, args ...any) ([]Row, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	rows, err := s.db.Query(pg(q), args...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	cols, _ := rows.Columns()
	var out []Row
	for rows.Next() {
		vals := make([]any, len(cols))
		ptrs := make([]any, len(cols))
		for i := range vals {
			ptrs[i] = &vals[i]
		}
		if err := rows.Scan(ptrs...); err != nil {
			return nil, err
		}
		r := Row{}
		for i, c := range cols {
			switch v := vals[i].(type) {
			case []byte:
				r[c] = string(v)
			default:
				r[c] = v
			}
		}
		out = append(out, r)
	}
	return out, rows.Err()
}

func (s *Store) exec(q string, args ...any) (int64, error) {
	s.mu.Lock()
	defer s.mu.Unlock()
	res, err := s.db.Exec(pg(q), args...)
	if err != nil {
		return 0, err
	}
	n, _ := res.RowsAffected()
	return n, nil
}

func str(r Row, k string) string {
	if r[k] == nil {
		return ""
	}
	return fmt.Sprint(r[k])
}

func i64(r Row, k string) int64 {
	switch v := r[k].(type) {
	case int64:
		return v
	case int32:
		return int64(v)
	case int:
		return int64(v)
	case bool:
		if v {
			return 1
		}
		return 0
	case float64:
		return int64(v)
	case string:
		i, _ := jsonx.Int(v)
		return i
	}
	return 0
}

func f64(r Row, k string) float64 {
	switch v := r[k].(type) {
	case float64:
		return v
	case int64:
		return float64(v)
	}
	return 0
}

// ---- meta
func (s *Store) MetaGet(k string) (string, bool) {
	r, _ := s.query("SELECT v FROM meta WHERE k=?", k)
	if len(r) == 0 {
		return "", false
	}
	return str(r[0], "v"), true
}

func (s *Store) MetaSet(k, v string) error {
	_, err := s.exec("INSERT INTO meta(k,v) VALUES(?,?) ON CONFLICT(k) DO UPDATE SET v=excluded.v", k, v)
	return err
}

// ---- wallets
type Wallet struct {
	WalletID       string  `json:"wallet_id"`
	AgentID        string  `json:"agent_id"`
	Tier           int     `json:"tier"`
	NodeWallet     string  `json:"node_wallet"`
	KeyWallet      string  `json:"key_wallet,omitempty"`
	Descriptor     string  `json:"descriptor,omitempty"`
	ReceiveAddress string  `json:"receive_address"`
	Frozen         bool    `json:"frozen"`
	FrozenReason   string  `json:"frozen_reason,omitempty"`
	Retired        bool    `json:"retired"`
	CreatedAt      float64 `json:"created_at"`
}

func rowWallet(r Row) *Wallet {
	return &Wallet{WalletID: str(r, "wallet_id"), AgentID: str(r, "agent_id"), Tier: int(i64(r, "tier")), NodeWallet: str(r, "node_wallet"),
		KeyWallet: str(r, "key_wallet"), Descriptor: str(r, "descriptor"), ReceiveAddress: str(r, "receive_address"),
		Frozen: i64(r, "frozen") == 1, FrozenReason: str(r, "frozen_reason"), Retired: i64(r, "retired") == 1, CreatedAt: f64(r, "created_at")}
}

func (s *Store) AddWallet(w *Wallet) error {
	var kw, desc any
	if w.KeyWallet != "" {
		kw = w.KeyWallet
	}
	if w.Descriptor != "" {
		desc = w.Descriptor
	}
	_, err := s.exec("INSERT INTO wallets(wallet_id,agent_id,tier,node_wallet,key_wallet,descriptor,receive_address,created_at) VALUES(?,?,?,?,?,?,?,?)",
		w.WalletID, w.AgentID, w.Tier, w.NodeWallet, kw, desc, w.ReceiveAddress, now())
	return err
}

func (s *Store) Wallet(id string) *Wallet {
	r, _ := s.query("SELECT * FROM wallets WHERE wallet_id=?", id)
	if len(r) == 0 {
		return nil
	}
	return rowWallet(r[0])
}

func (s *Store) Wallets() []*Wallet {
	rows, _ := s.query("SELECT * FROM wallets ORDER BY created_at")
	out := make([]*Wallet, 0, len(rows))
	for _, r := range rows {
		out = append(out, rowWallet(r))
	}
	return out
}

func (s *Store) SetFrozen(id string, frozen bool, reason string) error {
	var rs any
	if reason != "" {
		rs = reason
	}
	f := 0
	if frozen {
		f = 1
	}
	_, err := s.exec("UPDATE wallets SET frozen=?, frozen_reason=? WHERE wallet_id=?", f, rs, id)
	return err
}

func (s *Store) SetRetired(id string) error {
	_, err := s.exec("UPDATE wallets SET retired=1, frozen=1, frozen_reason='retired' WHERE wallet_id=?", id)
	return err
}

func (s *Store) SetReceiveAddress(id, addr string) error {
	_, err := s.exec("UPDATE wallets SET receive_address=? WHERE wallet_id=?", addr, id)
	return err
}

// ---- tokens
func TokenHash(tok string) string {
	h := sha256.Sum256([]byte(tok))
	return hex.EncodeToString(h[:])
}

func (s *Store) AddToken(tok, label string, scopes []string) error {
	set := map[string]bool{}
	for _, sc := range scopes {
		set[sc] = true
	}
	list := make([]string, 0, len(set))
	for sc := range set {
		list = append(list, sc)
	}
	sort.Strings(list)
	_, err := s.exec("INSERT INTO tokens(token_sha256,label,scopes,created_at) VALUES(?,?,?,?)", TokenHash(tok), label, strings.Join(list, " "), now())
	return err
}

// TokenScopes returns the scopes of a live token, or nil.
func (s *Store) TokenScopes(tok string) map[string]bool {
	r, _ := s.query("SELECT scopes, revoked FROM tokens WHERE token_sha256=?", TokenHash(tok))
	if len(r) == 0 || i64(r[0], "revoked") == 1 {
		return nil
	}
	out := map[string]bool{}
	for _, sc := range strings.Fields(str(r[0], "scopes")) {
		out[sc] = true
	}
	return out
}

func (s *Store) RevokeToken(label string) int64 {
	n, _ := s.exec("UPDATE tokens SET revoked=1 WHERE label=? AND revoked=0", label)
	return n
}

func (s *Store) Tokens() []Row {
	r, _ := s.query("SELECT label, scopes, created_at, revoked FROM tokens ORDER BY created_at")
	return r
}

// ---- spends and rolling limits
type Spend struct {
	WalletID     string
	PSBTSHA256   string
	TxID         string
	AmountAtoms  int64
	FeeAtoms     *int64
	Destinations []string
	Purpose      string
	Status       string
	CreatedAt    float64
}

func (s *Store) AddSpend(walletID, psbtSHA string, amount int64, dests []string, purpose, status string) error {
	t := now()
	_, err := s.exec("INSERT INTO spends(wallet_id,psbt_sha256,txid,amount_atoms,fee_atoms,destinations,purpose,status,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?)",
		walletID, psbtSHA, nil, amount, nil, jsonx.Canonical(dests), purpose, status, t, t)
	return err
}

func (s *Store) SetSpendStatus(psbtSHA, status string) error {
	_, err := s.exec("UPDATE spends SET status=?, updated_at=? WHERE psbt_sha256=?", status, now(), psbtSHA)
	return err
}

func (s *Store) SetSpendBroadcast(psbtSHA, txid string, fee *int64) error {
	var f any
	if fee != nil {
		f = *fee
	}
	_, err := s.exec("UPDATE spends SET status='broadcast', txid=?, fee_atoms=?, updated_at=? WHERE psbt_sha256=?", txid, f, now(), psbtSHA)
	return err
}

func rowSpend(r Row) *Spend {
	sp := &Spend{WalletID: str(r, "wallet_id"), PSBTSHA256: str(r, "psbt_sha256"), TxID: str(r, "txid"), AmountAtoms: i64(r, "amount_atoms"),
		Purpose: str(r, "purpose"), Status: str(r, "status"), CreatedAt: f64(r, "created_at")}
	if r["fee_atoms"] != nil {
		f := i64(r, "fee_atoms")
		sp.FeeAtoms = &f
	}
	var d []any
	_ = jsonx.Decode([]byte(str(r, "destinations")), &d)
	for _, e := range d {
		if st, ok := e.(string); ok {
			sp.Destinations = append(sp.Destinations, st)
		}
	}
	return sp
}

func (s *Store) Spend(psbtSHA string) *Spend {
	r, _ := s.query("SELECT * FROM spends WHERE psbt_sha256=?", psbtSHA)
	if len(r) == 0 {
		return nil
	}
	return rowSpend(r[0])
}

// Rolling24h returns (sum atoms, count) of spends that reached broadcast/confirmed/pending-approval in the last 24 h.
func (s *Store) Rolling24h(walletID string) (int64, int64) {
	r, _ := s.query("SELECT COALESCE(SUM(amount_atoms),0) AS s, COUNT(*) AS c FROM spends WHERE wallet_id=? AND created_at>=? "+
		"AND status IN ('broadcast','confirmed','needs_approval','approved','signing')", walletID, now()-86400)
	if len(r) == 0 {
		return 0, 0
	}
	return i64(r[0], "s"), i64(r[0], "c")
}

func (s *Store) RecentSpends(walletID string, limit int) []*Spend {
	rows, _ := s.query("SELECT * FROM spends WHERE wallet_id=? ORDER BY created_at DESC LIMIT ?", walletID, limit)
	out := make([]*Spend, 0, len(rows))
	for _, r := range rows {
		out = append(out, rowSpend(r))
	}
	return out
}

// ---- denials / kill switch
func (s *Store) AddDenial(walletID, reason string) int64 {
	t := now()
	_, _ = s.exec("INSERT INTO denials(wallet_id,at,reason) VALUES(?,?,?)", walletID, t, reason)
	r, _ := s.query("SELECT COUNT(*) AS c FROM denials WHERE wallet_id=? AND at>=?", walletID, t-3600)
	if len(r) == 0 {
		return 0
	}
	return i64(r[0], "c")
}

// ---- approvals
type Approval struct {
	PSBTSHA256 string         `json:"psbt_sha256"`
	WalletID   string         `json:"wallet_id"`
	PSBT       string         `json:"-"`
	Decoded    map[string]any `json:"decoded"`
	Request    map[string]any `json:"request"`
	ExpiresAt  float64        `json:"expires_at"`
	Approved   bool           `json:"approved"`
	Consumed   bool           `json:"consumed"`
}

func (s *Store) AddApproval(psbtSHA, walletID, psbt string, decoded, request map[string]any, ttl int64) error {
	_, err := s.exec("INSERT INTO approvals(psbt_sha256,wallet_id,psbt,decoded,request,expires_at) VALUES(?,?,?,?,?,?) "+
		"ON CONFLICT(psbt_sha256) DO UPDATE SET wallet_id=excluded.wallet_id, psbt=excluded.psbt, decoded=excluded.decoded, request=excluded.request, expires_at=excluded.expires_at, approved=0, consumed=0",
		psbtSHA, walletID, psbt, jsonx.Canonical(decoded), jsonx.Canonical(request), now()+float64(ttl))
	return err
}

func rowApproval(r Row) *Approval {
	a := &Approval{PSBTSHA256: str(r, "psbt_sha256"), WalletID: str(r, "wallet_id"), PSBT: str(r, "psbt"), ExpiresAt: f64(r, "expires_at"),
		Approved: i64(r, "approved") == 1, Consumed: i64(r, "consumed") == 1}
	_ = jsonx.Decode([]byte(str(r, "decoded")), &a.Decoded)
	_ = jsonx.Decode([]byte(str(r, "request")), &a.Request)
	return a
}

func (s *Store) Approval(psbtSHA string) *Approval {
	r, _ := s.query("SELECT * FROM approvals WHERE psbt_sha256=?", psbtSHA)
	if len(r) == 0 {
		return nil
	}
	return rowApproval(r[0])
}

func (s *Store) Approve(psbtSHA string) bool {
	t := now()
	n, _ := s.exec("UPDATE approvals SET approved=1, approved_at=? WHERE psbt_sha256=? AND consumed=0 AND expires_at>?", t, psbtSHA, t)
	return n == 1
}

func (s *Store) ConsumeApproval(psbtSHA string) error {
	_, err := s.exec("UPDATE approvals SET consumed=1 WHERE psbt_sha256=?", psbtSHA)
	return err
}

func (s *Store) PendingApprovals(walletID string) []*Approval {
	q := "SELECT * FROM approvals WHERE approved=0 AND consumed=0 AND expires_at>?"
	args := []any{now()}
	if walletID != "" {
		q += " AND wallet_id=?"
		args = append(args, walletID)
	}
	rows, _ := s.query(q, args...)
	out := make([]*Approval, 0, len(rows))
	for _, r := range rows {
		a := rowApproval(r)
		a.PSBT = ""
		out = append(out, a)
	}
	return out
}

// ---- swaps (Phase 2 state machine)
type Swap struct {
	SwapID   string         `json:"swap_id"`
	WalletID string         `json:"wallet_id"`
	State    string         `json:"state"`
	Data     map[string]any `json:"data"`
}

func (s *Store) PutSwap(id, walletID, state string, data map[string]any) error {
	t := now()
	_, err := s.exec("INSERT INTO swaps(swap_id,wallet_id,state,data,created_at,updated_at) VALUES(?,?,?,?,?,?) "+
		"ON CONFLICT(swap_id) DO UPDATE SET state=excluded.state, data=excluded.data, updated_at=excluded.updated_at",
		id, walletID, state, jsonx.Canonical(data), t, t)
	return err
}

func (s *Store) Swap(id string) *Swap {
	r, _ := s.query("SELECT * FROM swaps WHERE swap_id=?", id)
	if len(r) == 0 {
		return nil
	}
	sw := &Swap{SwapID: str(r[0], "swap_id"), WalletID: str(r[0], "wallet_id"), State: str(r[0], "state")}
	_ = jsonx.Decode([]byte(str(r[0], "data")), &sw.Data)
	return sw
}

func (s *Store) OpenSwaps(walletID string) int64 {
	r, _ := s.query("SELECT COUNT(*) AS c FROM swaps WHERE wallet_id=? AND state NOT IN ('claimed','refunded','cancelled')", walletID)
	if len(r) == 0 {
		return 0
	}
	return i64(r[0], "c")
}

// LockedInSwaps sums amount_atoms of open swaps.
func (s *Store) LockedInSwaps(walletID string) int64 {
	rows, _ := s.query("SELECT data FROM swaps WHERE wallet_id=? AND state NOT IN ('claimed','refunded','cancelled')", walletID)
	var total int64
	for _, r := range rows {
		var d map[string]any
		if jsonx.Decode([]byte(str(r, "data")), &d) == nil {
			total += jsonx.I(d, "amount_atoms", 0)
		}
	}
	return total
}

// ---- audit chain (append-only, hash-linked; secrets never enter payloads)
type AuditEntry struct {
	Seq      int64          `json:"seq"`
	At       float64        `json:"at"`
	Kind     string         `json:"kind"`
	WalletID string         `json:"wallet_id,omitempty"`
	Actor    string         `json:"actor,omitempty"`
	Payload  map[string]any `json:"payload"`
	PrevHash string         `json:"prev_hash"`
	Hash     string         `json:"hash"`
}

const zeroHash = "0000000000000000000000000000000000000000000000000000000000000000"

func auditBody(at float64, kind string, walletID, actor any, payload map[string]any, prev string) string {
	return jsonx.Canonical(map[string]any{"at": at, "kind": kind, "wallet_id": walletID, "actor": actor, "payload": payload, "prev": prev})
}

func nilIfEmpty(s string) any {
	if s == "" {
		return nil
	}
	return s
}

// Audit appends an entry and returns its hash.
func (s *Store) Audit(kind string, payload map[string]any, walletID, actor string) string {
	if payload == nil {
		payload = map[string]any{}
	}
	payload = jsonx.Clone(payload).(map[string]any)
	last, _ := s.query("SELECT hash FROM audit ORDER BY seq DESC LIMIT 1")
	prev := zeroHash
	if len(last) > 0 {
		prev = str(last[0], "hash")
	}
	at := now()
	body := auditBody(at, kind, nilIfEmpty(walletID), nilIfEmpty(actor), payload, prev)
	sum := sha256.Sum256([]byte(body))
	h := hex.EncodeToString(sum[:])
	_, _ = s.exec("INSERT INTO audit(at,kind,wallet_id,actor,payload,prev_hash,hash) VALUES(?,?,?,?,?,?,?)",
		at, kind, nilIfEmpty(walletID), nilIfEmpty(actor), jsonx.Canonical(payload), prev, h)
	return h
}

func (s *Store) AuditExport(sinceSeq, limit int64) []*AuditEntry {
	rows, _ := s.query("SELECT * FROM audit WHERE seq>? ORDER BY seq LIMIT ?", sinceSeq, limit)
	out := make([]*AuditEntry, 0, len(rows))
	for _, r := range rows {
		e := &AuditEntry{Seq: i64(r, "seq"), At: f64(r, "at"), Kind: str(r, "kind"), WalletID: str(r, "wallet_id"), Actor: str(r, "actor"),
			PrevHash: str(r, "prev_hash"), Hash: str(r, "hash")}
		_ = jsonx.Decode([]byte(str(r, "payload")), &e.Payload)
		out = append(out, e)
	}
	return out
}

// AuditVerify walks the chain; returns ok and the number of verified entries.
func (s *Store) AuditVerify() (bool, int64) {
	rows, _ := s.query("SELECT * FROM audit ORDER BY seq")
	prev := zeroHash
	var n int64
	for _, r := range rows {
		var payload map[string]any
		_ = jsonx.Decode([]byte(str(r, "payload")), &payload)
		body := auditBody(f64(r, "at"), str(r, "kind"), r["wallet_id"], r["actor"], payload, prev)
		sum := sha256.Sum256([]byte(body))
		if str(r, "prev_hash") != prev || hex.EncodeToString(sum[:]) != str(r, "hash") {
			return false, n
		}
		prev = str(r, "hash")
		n++
	}
	return true, n
}
