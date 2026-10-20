// Package policy: policy documents (spec §6), owner signature, the §6.2 evaluation order and the kill switch.
//
// A policy is a JSON document signed by the owner's admin credential:
// HMAC-SHA256(admin_secret, canonical JSON of the document without "signature"). The agent never sees
// the admin secret; it receives the document minus the signature via policy.describe.
package policy

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/errs"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/jsonx"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/store"
)

const (
	Allow         = "allow"
	NeedsApproval = "needs_approval"
	Deny          = "deny"
)

const defaultJSON = `{
  "version": 1,
  "agent_id": "",
  "tier": 1,
  "limits": {
    "per_tx_atoms": 5000000000,
    "per_day_atoms": 20000000000,
    "per_day_tx_count": 40,
    "max_balance_atoms": 100000000000,
    "max_fee_rate_atoms_per_byte": 20,
    "min_fee_rate_atoms_per_byte": 1,
    "min_confirmations_to_spend": 1
  },
  "destinations": {"mode": "allowlist", "allow": [], "allow_swap_htlc_addresses": true},
  "approval": {"require_above_atoms": 1000000000, "channel": "webhook", "ttl_seconds": 900},
  "trading": {"venues": [], "counterparties": [], "max_slippage_bps": 50, "max_open_swaps": 3,
              "min_refund_lock_blocks": 40, "max_refund_lock_blocks": 400,
              "reference_price_sources": [], "max_price_deviation_bps": 200},
  "time_windows": [],
  "kill_switch": {"freeze_on_policy_denials_per_hour": 5}
}`

// Default returns a fresh copy of the default policy document.
func Default() map[string]any {
	var m map[string]any
	_ = jsonx.Decode([]byte(defaultJSON), &m)
	return m
}

func mac(secret string, doc map[string]any) string {
	body := map[string]any{}
	for k, v := range doc {
		if k != "signature" {
			body[k] = v
		}
	}
	h := hmac.New(sha256.New, []byte(secret))
	h.Write([]byte(jsonx.Canonical(body)))
	return hex.EncodeToString(h.Sum(nil))
}

// Sign returns a copy of doc with the owner signature attached.
func Sign(doc map[string]any, adminSecret string) map[string]any {
	out := jsonx.Clone(doc).(map[string]any)
	delete(out, "signature")
	out["signature"] = map[string]any{"alg": "HMAC-SHA256", "value": mac(adminSecret, out)}
	return out
}

// Verify checks the owner signature.
func Verify(doc map[string]any, adminSecret string) bool {
	sig := jsonx.S(jsonx.M(doc, "signature"), "value")
	return sig != "" && hmac.Equal([]byte(sig), []byte(mac(adminSecret, doc)))
}

var days = map[string]int{"mon": 0, "tue": 1, "wed": 2, "thu": 3, "fri": 4, "sat": 5, "sun": 6}

func hhmm(s string) int {
	if len(s) < 5 {
		return 0
	}
	h, _ := strconv.Atoi(s[:2])
	m, _ := strconv.Atoi(s[3:5])
	return h*60 + m
}

// InWindow reports whether now (unix seconds) falls inside any of the windows ({"days":"mon-fri","utc":"09:00-17:00"}).
func InWindow(windows []any, now float64) bool {
	if len(windows) == 0 {
		return true
	}
	t := time.Unix(int64(now), 0).UTC()
	wd := (int(t.Weekday()) + 6) % 7 // Monday=0
	for _, wv := range windows {
		w, _ := wv.(map[string]any)
		d := jsonx.S(w, "days")
		if d == "" {
			d = "mon-sun"
		}
		a, b, _ := strings.Cut(d, "-")
		if b == "" {
			b = a
		}
		lo, okA := days[strings.ToLower(a[:min(3, len(a))])]
		hi, okB := days[strings.ToLower(b[:min(3, len(b))])]
		if !okA || !okB {
			continue
		}
		dayOK := false
		if lo <= hi {
			dayOK = lo <= wd && wd <= hi
		} else {
			dayOK = wd >= lo || wd <= hi
		}
		utc := jsonx.S(w, "utc")
		if utc == "" {
			utc = "00:00-24:00"
		}
		h1, h2, _ := strings.Cut(utc, "-")
		m := t.Hour()*60 + t.Minute()
		if dayOK && hhmm(h1) <= m && m < hhmm(h2) {
			return true
		}
	}
	return false
}

type Decision struct {
	Decision          string `json:"decision"`
	Reason            string `json:"reason"`
	RemainingDayAtoms int64  `json:"remaining_day_atoms"`
	RemainingDayCount int64  `json:"remaining_day_count"`
}

func (d Decision) Map() map[string]any {
	return map[string]any{"decision": d.Decision, "reason": d.Reason, "remaining_day_atoms": d.RemainingDayAtoms, "remaining_day_count": d.RemainingDayCount}
}

// Trade carries the Phase 2 checks (venue, counterparty, refund lock, price deviation).
type Trade struct {
	Venue             string
	Counterparty      string
	RefundLockBlocks  *int64
	PriceDeviationBps *int64
}

type cached struct {
	doc   map[string]any
	mtime int64
	size  int64
}

type Engine struct {
	Dir         string
	AdminSecret string
	Store       *store.Store
	mu          sync.Mutex
	cache       map[string]cached
}

func NewEngine(dir, adminSecret string, st *store.Store) *Engine {
	return &Engine{Dir: dir, AdminSecret: adminSecret, Store: st, cache: map[string]cached{}}
}

func (e *Engine) Path(walletID string) string { return filepath.Join(e.Dir, walletID+".json") }

func (e *Engine) Exists(walletID string) bool {
	_, err := os.Stat(e.Path(walletID))
	return err == nil
}

// Write signs and installs a policy document.
func (e *Engine) Write(walletID string, doc map[string]any) (map[string]any, error) {
	if err := os.MkdirAll(e.Dir, 0o700); err != nil {
		return nil, err
	}
	signed := Sign(doc, e.AdminSecret)
	if err := os.WriteFile(e.Path(walletID), []byte(jsonx.Pretty(signed)), 0o600); err != nil {
		return nil, err
	}
	e.mu.Lock()
	delete(e.cache, walletID)
	e.mu.Unlock()
	return signed, nil
}

// Load returns the verified policy document for a wallet. The cache is keyed on the file's mtime and size,
// so a `policy set` from the owner CLI (another process) takes effect on the next evaluation.
func (e *Engine) Load(walletID string) (map[string]any, error) {
	st, err := os.Stat(e.Path(walletID))
	if err != nil {
		return nil, errs.New(errs.BadRequest, "policy_missing", "no policy document for %s", walletID)
	}
	e.mu.Lock()
	if c, ok := e.cache[walletID]; ok && c.mtime == st.ModTime().UnixNano() && c.size == st.Size() {
		e.mu.Unlock()
		return c.doc, nil
	}
	e.mu.Unlock()
	raw, err := os.ReadFile(e.Path(walletID))
	if err != nil {
		return nil, errs.New(errs.BadRequest, "policy_missing", "no policy document for %s", walletID)
	}
	var doc map[string]any
	if err := jsonx.Decode(raw, &doc); err != nil {
		return nil, errs.New(errs.BadRequest, "policy_invalid", "policy document is not valid JSON")
	}
	if !Verify(doc, e.AdminSecret) {
		return nil, errs.New(errs.BadRequest, "policy_signature", "policy signature invalid")
	}
	e.mu.Lock()
	e.cache[walletID] = cached{doc: doc, mtime: st.ModTime().UnixNano(), size: st.Size()}
	e.mu.Unlock()
	return doc, nil
}

func (e *Engine) Version(walletID string) int64 {
	doc, err := e.Load(walletID)
	if err != nil {
		return 0
	}
	return jsonx.I(doc, "version", 0)
}

// Describe returns the policy minus the signature plus today's remaining limits (spec policy.describe).
func (e *Engine) Describe(walletID string) (map[string]any, error) {
	doc, err := e.Load(walletID)
	if err != nil {
		return nil, err
	}
	out := jsonx.Clone(doc).(map[string]any)
	delete(out, "signature")
	spent, count := e.Store.Rolling24h(walletID)
	lim := jsonx.M(out, "limits")
	out["today"] = map[string]any{"spent_atoms": spent, "tx_count": count,
		"remaining_atoms":    max(int64(0), jsonx.I(lim, "per_day_atoms", 0)-spent),
		"remaining_tx_count": max(int64(0), jsonx.I(lim, "per_day_tx_count", 0)-count)}
	return out, nil
}

// Evaluate implements spec §6.2: first deny wins, then needs_approval, else allow.
// minInputConfs < 0 means unknown (check skipped).
func (e *Engine) Evaluate(w *store.Wallet, amountAtoms int64, destinations []string, feeRate int64, minInputConfs int64,
	now float64, isHTLCDestination bool, trade *Trade) (Decision, error) {
	if now == 0 {
		now = float64(time.Now().Unix())
	}
	doc, err := e.Load(w.WalletID)
	if err != nil {
		return Decision{}, err
	}
	lim, dest, appr := jsonx.M(doc, "limits"), jsonx.M(doc, "destinations"), jsonx.M(doc, "approval")
	spent, count := e.Store.Rolling24h(w.WalletID)
	remAtoms := max(int64(0), jsonx.I(lim, "per_day_atoms", 0)-spent)
	remCount := max(int64(0), jsonx.I(lim, "per_day_tx_count", 0)-count)
	deny := func(reason string) (Decision, error) {
		return Decision{Deny, reason, remAtoms, remCount}, nil
	}
	switch {
	case w.Frozen:
		return deny("frozen")
	case w.Retired:
		return deny("retired")
	}
	if !InWindow(jsonx.L(doc, "time_windows"), now) {
		return deny("time_window")
	}
	mode := jsonx.S(dest, "mode")
	if mode == "" {
		mode = "allowlist"
	}
	allow, denyList := jsonx.Strs(dest, "allow"), jsonx.Strs(dest, "deny")
	for _, d := range destinations {
		switch mode {
		case "allowlist":
			if !(jsonx.Contains(allow, d) || (isHTLCDestination && jsonx.B(dest, "allow_swap_htlc_addresses", false))) {
				return deny("destination_not_allowed")
			}
		case "denylist":
			if jsonx.Contains(denyList, d) {
				return deny("destination_not_allowed")
			}
		}
	}
	if amountAtoms <= 0 {
		return deny("bad_amount")
	}
	if amountAtoms > jsonx.I(lim, "per_tx_atoms", 0) {
		return deny("per_tx_limit")
	}
	if spent+amountAtoms > jsonx.I(lim, "per_day_atoms", 0) {
		return deny("per_day_limit")
	}
	if count >= jsonx.I(lim, "per_day_tx_count", 0) {
		return deny("per_day_count")
	}
	if feeRate > jsonx.I(lim, "max_fee_rate_atoms_per_byte", 0) {
		return deny("fee_rate")
	}
	if minInputConfs >= 0 && minInputConfs < jsonx.I(lim, "min_confirmations_to_spend", 0) {
		return deny("unconfirmed_inputs")
	}
	if trade != nil {
		tr := jsonx.M(doc, "trading")
		if !jsonx.Contains(jsonx.Strs(tr, "venues"), trade.Venue) {
			return deny("venue_not_allowed")
		}
		if trade.Counterparty != "" && !jsonx.Contains(jsonx.Strs(tr, "counterparties"), trade.Counterparty) {
			return deny("counterparty_not_allowed")
		}
		if e.Store.OpenSwaps(w.WalletID) >= jsonx.I(tr, "max_open_swaps", 0) {
			return deny("max_open_swaps")
		}
		if l := trade.RefundLockBlocks; l != nil && !(jsonx.I(tr, "min_refund_lock_blocks", 0) <= *l && *l <= jsonx.I(tr, "max_refund_lock_blocks", 1_000_000_000)) {
			return deny("refund_lock_range")
		}
		if d := trade.PriceDeviationBps; d != nil && *d > jsonx.I(tr, "max_price_deviation_bps", 0) {
			return deny("price_deviation")
		}
	}
	if amountAtoms > jsonx.I(appr, "require_above_atoms", 0) {
		return Decision{NeedsApproval, "amount_above_threshold", remAtoms, remCount}, nil
	}
	return Decision{Allow, "", remAtoms, remCount}, nil
}

// RecordDenial counts a denial; true when the kill switch threshold was crossed (caller freezes).
func (e *Engine) RecordDenial(w *store.Wallet, reason string) bool {
	n := e.Store.AddDenial(w.WalletID, reason)
	doc, err := e.Load(w.WalletID)
	if err != nil {
		return false
	}
	limit := jsonx.I(jsonx.M(doc, "kill_switch"), "freeze_on_policy_denials_per_hour", 0)
	return limit > 0 && n >= limit
}

// ParseDocument decodes a policy JSON file keeping numbers exact.
func ParseDocument(raw []byte) (map[string]any, error) {
	var doc map[string]any
	if err := jsonx.Decode(raw, &doc); err != nil {
		return nil, err
	}
	return doc, nil
}

var _ = json.Marshal
