// Package service: the Agent Wallet Service facade, one object implementing the spec's tool surface (§8).
//
// Every tool takes an Auth (token scopes + actor label) and returns errs.ScopeDenied when the token lacks
// the scope. Amounts are integers in atoms. No tool returns keys, seeds, passphrases, private descriptors
// or raw PSBTs (the wallet:psbt scope is intentionally not implemented in v1).
package service

import (
	"bytes"
	"crypto/rand"
	"encoding/base64"
	"io"
	"net/http"
	"sort"
	"strings"
	"time"

	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/config"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/cosigner"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/errs"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/jsonx"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/policy"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/rpc"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/spend"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/store"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/wallet"
)

var Scopes = map[string]bool{"wallet:admin": true, "wallet:read": true, "wallet:spend": true, "market:read": true,
	"trade:quote": true, "trade:execute": true, "trade:read": true}

// Auth is the caller's context.
type Auth struct {
	Scopes map[string]bool
	Actor  string
}

func NewAuth(actor string, scopes ...string) Auth {
	a := Auth{Scopes: map[string]bool{}, Actor: actor}
	for _, s := range scopes {
		a.Scopes[s] = true
	}
	return a
}

// Owner is the CLI's identity.
var Owner = NewAuth("owner", "wallet:admin")

func (a Auth) Need(scope string) error {
	if a.Scopes[scope] || a.Scopes["wallet:admin"] {
		return nil
	}
	return errs.Scope(scope)
}

type Service struct {
	Cfg      *config.Config
	Keyring  *config.Keyring
	Store    *store.Store
	RPC      *rpc.Client
	Policy   *policy.Engine
	Cosigner *cosigner.Client
	Wallets  *wallet.Wallets
	Spend    *spend.Pipeline
}

// New opens the store (PostgreSQL schema "aws"), keyring and node connection.
func New(cfg *config.Config) (*Service, error) { return NewWithSchema(cfg, "aws") }

// NewWithSchema is New with an explicit PostgreSQL schema (tests isolate fixtures this way).
func NewWithSchema(cfg *config.Config, schema string) (*Service, error) {
	kr := config.NewKeyring(cfg.KeyringPath())
	if cfg.PgDSN == "" {
		return nil, errs.New(errs.Internal, "config", "pg_dsn is required (PostgreSQL connection string)")
	}
	st, err := store.Open(cfg.PgDSN, schema)
	if err != nil {
		return nil, errs.New(errs.Internal, "store", "%v", err)
	}
	pw := cfg.RPCPassword
	if pw == "" {
		pw, _ = kr.Get("rpc_password")
	}
	r := rpc.New(cfg.RPCURL, cfg.CookiePath, cfg.RPCUser, pw)
	admin, err := kr.Ensure("admin_secret", 32)
	if err != nil {
		return nil, err
	}
	if _, err := kr.Ensure("decision_secret", 32); err != nil {
		return nil, err
	}
	s := &Service{Cfg: cfg, Keyring: kr, Store: st, RPC: r, Policy: policy.NewEngine(cfg.PolicyDir(), admin, st)}
	var pk wallet.CosignerPubkeyFn
	if cfg.CosignerURL != "" {
		s.Cosigner = cosigner.NewClient(cfg.CosignerURL, kr)
		pk = s.Cosigner.Pubkey
	}
	s.Wallets = wallet.New(cfg, r, kr, st, pk)
	var cs spend.Cosigner
	if s.Cosigner != nil {
		cs = s.Cosigner
	}
	s.Spend = spend.New(cfg, r, s.Wallets, s.Policy, st, cs)
	return s, nil
}

// SetCosigner swaps the pipeline's co-signer (nil disables it); used by tests.
func (s *Service) SetCosigner(c *cosigner.Client) {
	if c == nil {
		s.Spend.Cosigner = nil
		return
	}
	s.Spend.Cosigner = c
}

// ---- tokens
func (s *Service) IssueToken(label string, scopes []string) (string, error) {
	var bad []string
	for _, sc := range scopes {
		if !Scopes[sc] {
			bad = append(bad, sc)
		}
	}
	if len(bad) > 0 {
		sort.Strings(bad)
		return "", errs.Bad("bad_request", "unknown scopes: %s", strings.Join(bad, ","))
	}
	buf := make([]byte, 32)
	if _, err := rand.Read(buf); err != nil {
		return "", err
	}
	tok := "aws_" + base64.RawURLEncoding.EncodeToString(buf)
	if err := s.Store.AddToken(tok, label, scopes); err != nil {
		return "", err
	}
	sorted := append([]string(nil), scopes...)
	sort.Strings(sorted)
	s.Store.Audit("token.issue", map[string]any{"label": label, "scopes": sorted}, "", "owner")
	return tok, nil
}

// AuthFromToken resolves a bearer token; nil when unknown or revoked.
func (s *Service) AuthFromToken(token string) *Auth {
	if token == "" {
		return nil
	}
	scopes := s.Store.TokenScopes(token)
	if scopes == nil {
		return nil
	}
	return &Auth{Scopes: scopes, Actor: "token:" + store.TokenHash(token)[:8]}
}

// ---- node health
type NodeStatus struct {
	OK          bool   `json:"ok"`
	Error       string `json:"error,omitempty"`
	Chain       string `json:"chain,omitempty"`
	Height      int64  `json:"height,omitempty"`
	IBD         bool   `json:"ibd"`
	Connections int64  `json:"connections"`
	Version     string `json:"version,omitempty"`
}

func (s *Service) NodeStatus() NodeStatus {
	bc, err := s.RPC.Map("getblockchaininfo")
	if err != nil {
		return NodeStatus{Error: err.Error()}
	}
	ni, err := s.RPC.Map("getnetworkinfo")
	if err != nil {
		return NodeStatus{Error: err.Error()}
	}
	st := NodeStatus{Chain: jsonx.S(bc, "chain"), Height: jsonx.I(bc, "blocks", 0), IBD: jsonx.B(bc, "initialblockdownload", false),
		Connections: jsonx.I(ni, "connections", 0), Version: jsonx.S(ni, "subversion")}
	st.OK = !st.IBD && (st.Connections >= 1 || st.Chain == "regtest")
	return st
}

// ---- tools (spec §8)
func (s *Service) WalletCreate(a Auth, agentID string, tier int, ownerMLDSA []string, pol map[string]any, cooldown int64) (*wallet.CreateResult, error) {
	if err := a.Need("wallet:admin"); err != nil {
		return nil, err
	}
	if cooldown <= 0 {
		cooldown = 1008
	}
	out, err := s.Wallets.Create(agentID, tier, ownerMLDSA, cooldown, a.Actor)
	if err != nil {
		return nil, err
	}
	doc := policy.Default()
	if pol != nil {
		doc = jsonx.Clone(pol).(map[string]any)
	}
	doc["agent_id"], doc["tier"] = agentID, tier
	if _, ok := doc["version"]; !ok {
		doc["version"] = 1
	}
	if _, err := s.Policy.Write(out.WalletID, doc); err != nil {
		return nil, err
	}
	if _, err := s.Wallets.Backup(out.WalletID, a.Actor); err != nil {
		return nil, err
	}
	return out, nil
}

func (s *Service) WalletReceiveAddress(a Auth, walletID, label string) (map[string]any, error) {
	if err := a.Need("wallet:read"); err != nil {
		return nil, err
	}
	addr, err := s.Wallets.ReceiveAddress(walletID, label)
	if err != nil {
		return nil, err
	}
	return map[string]any{"address": addr}, nil
}

func (s *Service) WalletBalance(a Auth, walletID string) (*wallet.Balance, error) {
	if err := a.Need("wallet:read"); err != nil {
		return nil, err
	}
	return s.Wallets.Balance(walletID)
}

func (s *Service) WalletHistory(a Auth, walletID string, before *int64, limit int) (map[string]any, error) {
	if err := a.Need("wallet:read"); err != nil {
		return nil, err
	}
	if limit <= 0 {
		limit = 50
	}
	entries, err := s.Wallets.History(walletID, min(limit, 200), before)
	if err != nil {
		return nil, err
	}
	return map[string]any{"entries": entries}, nil
}

// WalletSend is the agent-facing spend entry point.
func (s *Service) WalletSend(a Auth, walletID, to string, amountAtoms *int64, purpose string, maxFeeRate *int64, sendMax bool) (*spend.Result, error) {
	if err := a.Need("wallet:spend"); err != nil {
		return nil, err
	}
	w, err := s.Wallets.Get(walletID)
	if err != nil {
		return nil, err
	}
	if w.Frozen {
		reason := w.FrozenReason
		if reason == "" {
			reason = "frozen"
		}
		return nil, errs.Froze(reason)
	}
	if ns := s.NodeStatus(); !ns.OK {
		return nil, errs.Unavailable("node not synced or no peers")
	}
	return s.Spend.Send(spend.Request{WalletID: walletID, To: to, AmountAtoms: amountAtoms, Purpose: purpose, MaxFeeRate: maxFeeRate, SendMax: sendMax, Actor: a.Actor})
}

func (s *Service) WalletSendStatus(a Auth, walletID, psbtSHA string) (*spend.Status, error) {
	if err := a.Need("wallet:read"); err != nil {
		return nil, err
	}
	return s.Spend.Status(walletID, psbtSHA)
}

// WalletFreeze: an agent may self-freeze; only the owner unfreezes.
func (s *Service) WalletFreeze(a Auth, walletID, reason string) (map[string]any, error) {
	if err := a.Need("wallet:read"); err != nil {
		return nil, err
	}
	if err := s.Wallets.Freeze(walletID, reason, a.Actor); err != nil {
		return nil, err
	}
	return map[string]any{"frozen": true}, nil
}

func (s *Service) PolicyDescribe(a Auth, walletID string) (map[string]any, error) {
	if err := a.Need("wallet:read"); err != nil {
		return nil, err
	}
	if _, err := s.Wallets.Get(walletID); err != nil {
		return nil, err
	}
	return s.Policy.Describe(walletID)
}

func (s *Service) FeesEstimate(a Auth, target int64) (map[string]any, error) {
	if err := a.Need("market:read"); err != nil {
		return nil, err
	}
	if target <= 0 {
		target = 6
	}
	est, err := s.RPC.Map("estimatesmartfee", target)
	if err != nil {
		return nil, errs.Unavailable("%v", err)
	}
	mi, err := s.RPC.Map("getmempoolinfo")
	if err != nil {
		return nil, errs.Unavailable("%v", err)
	}
	relay := int64(jsonx.F(mi, "minrelaytxfee", 0)*100_000 + 0.5)
	out := map[string]any{"atoms_per_byte": relay, "source": "minrelaytxfee", "min_relay_atoms_per_byte": relay}
	if fr, ok := jsonx.Float(est["feerate"]); ok && fr > 0 {
		out["atoms_per_byte"] = int64(fr*100_000 + 0.5) // QTC/kB -> atoms/byte
		out["source"] = "estimatesmartfee"
	}
	return out, nil
}

// ---- market / trade (Phase 2 scaffolding: desk quotes are the only reference; venues enabled per policy)
type desk struct{ Name, URL string }

func (s *Service) desks() []desk {
	var out []desk
	for _, dv := range jsonx.L(s.Cfg.Extra, "desks") {
		d, _ := dv.(map[string]any)
		if jsonx.S(d, "name") != "" && jsonx.S(d, "url") != "" {
			out = append(out, desk{jsonx.S(d, "name"), strings.TrimRight(jsonx.S(d, "url"), "/")})
		}
	}
	return out
}

func (s *Service) deskQuotes(quoteAsset string) []map[string]any {
	cl := &http.Client{Timeout: 5 * time.Second}
	var out []map[string]any
	for _, d := range s.desks() {
		resp, err := cl.Get(d.URL + "/v1/reference")
		if err != nil {
			continue
		}
		data, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		var q map[string]any
		if jsonx.Decode(data, &q) != nil {
			continue
		}
		if p, ok := jsonx.Float(q["price"]); ok && jsonx.S(q, "quote") == quoteAsset && p > 0 {
			out = append(out, map[string]any{"desk": d.Name, "price": p})
		}
	}
	return out
}

func (s *Service) MarketReferencePrice(a Auth, quoteAsset string) (map[string]any, error) {
	if err := a.Need("market:read"); err != nil {
		return nil, err
	}
	quotes := s.deskQuotes(quoteAsset)
	names := []string{}
	prices := []float64{}
	for _, q := range quotes {
		names = append(names, jsonx.S(q, "desk"))
		p, _ := jsonx.Float(q["price"])
		prices = append(prices, p)
	}
	if len(quotes) < 2 {
		return map[string]any{"price": nil, "sources": names, "as_of": time.Now().Unix(),
			"note": "fewer than two independent desk quotes; reference undefined, trades need approval"}, nil
	}
	sort.Float64s(prices)
	med := prices[len(prices)/2]
	if len(prices)%2 == 0 {
		med = (prices[len(prices)/2-1] + prices[len(prices)/2]) / 2
	}
	return map[string]any{"price": med, "sources": names, "as_of": time.Now().Unix()}, nil
}

func (s *Service) MarketQuote(a Auth, walletID, side, quoteAsset string, amountAtoms int64, desks []string) (map[string]any, error) {
	if err := a.Need("trade:quote"); err != nil {
		return nil, err
	}
	if _, err := s.Wallets.Get(walletID); err != nil {
		return nil, err
	}
	doc, err := s.Policy.Load(walletID)
	if err != nil {
		return nil, err
	}
	allowed := jsonx.Strs(jsonx.M(doc, "trading"), "counterparties")
	cl := &http.Client{Timeout: 10 * time.Second}
	quotes := []map[string]any{}
	for _, d := range s.desks() {
		if !jsonx.Contains(allowed, d.Name) || (len(desks) > 0 && !jsonx.Contains(desks, d.Name)) {
			continue
		}
		body := jsonx.Compact(map[string]any{"side": side, "base": "QTC", "quote": quoteAsset, "amount_atoms": amountAtoms})
		resp, err := cl.Post(d.URL+"/v1/quote", "application/json", bytes.NewReader(body))
		if err != nil {
			quotes = append(quotes, map[string]any{"desk": d.Name, "error": err.Error()[:min(120, len(err.Error()))]})
			continue
		}
		data, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		var q map[string]any
		if jsonx.Decode(data, &q) != nil {
			q = map[string]any{"error": "bad response"}
		}
		q["desk"] = d.Name
		quotes = append(quotes, q)
	}
	return map[string]any{"quotes": quotes}, nil
}

func (s *Service) TradeSwap(a Auth, walletID, quoteID string) (map[string]any, error) {
	if err := a.Need("trade:execute"); err != nil {
		return nil, err
	}
	return map[string]any{"error": "not_enabled", "reason": "Venue A/B settlement (HTLC swaps) is Phase 2; enable per policy once an EVM leg is configured"}, nil
}

func (s *Service) TradeStatus(a Auth, swapID string) (map[string]any, error) {
	if err := a.Need("trade:read"); err != nil {
		return nil, err
	}
	sw := s.Store.Swap(swapID)
	if sw == nil {
		return map[string]any{"error": "unknown_swap"}, nil
	}
	out := map[string]any{"state": sw.State}
	for _, k := range []string{"qtc_txids", "evm_txs", "deadlines"} {
		if v, ok := sw.Data[k]; ok {
			out[k] = v
		}
	}
	return out, nil
}

func (s *Service) TradeCancel(a Auth, swapID string) (map[string]any, error) {
	if err := a.Need("trade:execute"); err != nil {
		return nil, err
	}
	sw := s.Store.Swap(swapID)
	if sw == nil {
		return map[string]any{"error": "unknown_swap"}, nil
	}
	return map[string]any{"state": sw.State, "note": "refund path runs automatically once the locktime passes"}, nil
}

// ---- owner-only operations (CLI, never through agent tokens)
func (s *Service) OwnerApprove(psbtSHA string) (*spend.Result, error) {
	if !s.Store.Approve(psbtSHA) {
		return nil, errs.Bad("approval_failed", "no pending, unexpired approval with that hash")
	}
	s.Store.Audit("approval.granted", map[string]any{"psbt_sha256": psbtSHA}, "", "owner")
	return s.Spend.CompleteApproved(psbtSHA, "owner")
}

func (s *Service) OwnerUnfreeze(walletID string) error { return s.Wallets.Unfreeze(walletID, "owner") }

func (s *Service) OwnerRetire(walletID, sweepTo string) (map[string]any, error) {
	out := map[string]any{"retired": true}
	if sweepTo != "" {
		doc, err := s.Policy.Load(walletID)
		if err != nil {
			return nil, err
		}
		if !jsonx.Contains(jsonx.Strs(jsonx.M(doc, "destinations"), "allow"), sweepTo) {
			return nil, errs.Conflicting("destination_not_allowed", "sweep destination must be on the policy allowlist")
		}
		res, err := s.Spend.Send(spend.Request{WalletID: walletID, To: sweepTo, Purpose: "retire-sweep", SendMax: true, Actor: "owner"})
		if err != nil {
			return nil, err
		}
		out["sweep"] = res
	}
	if err := s.Wallets.Retire(walletID, "owner"); err != nil {
		return nil, err
	}
	return out, nil
}

// PolicySet installs a new policy version from a document (version must increase).
func (s *Service) PolicySet(walletID string, doc map[string]any) error {
	cur := int64(0)
	if s.Policy.Exists(walletID) {
		cur = s.Policy.Version(walletID)
	}
	if jsonx.I(doc, "version", 0) <= cur {
		return errs.Bad("bad_request", "policy version must exceed the current %d", cur)
	}
	if _, err := s.Policy.Write(walletID, doc); err != nil {
		return err
	}
	s.Store.Audit("policy.update", map[string]any{"version": jsonx.I(doc, "version", 0)}, walletID, "owner")
	return nil
}
