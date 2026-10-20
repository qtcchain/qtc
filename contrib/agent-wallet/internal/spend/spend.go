// Package spend: the spend pipeline (spec §5)
//
//	request -> policy evaluation -> PSBT build -> (approval) -> sign -> verify -> broadcast -> track
//
// Amounts are integers in atoms. The PSBT hash is sha256 of the raw PSBT bytes. The final transaction is
// re-decoded and its outputs compared with the approved outputs before broadcast. Inputs reserved with
// lockUnspents are released on any failure or approval expiry.
package spend

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"io"
	"net/http"
	"strings"
	"sync"
	"time"

	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/config"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/errs"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/jsonx"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/policy"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/rpc"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/store"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/wallet"
)

// PSBTSHA256 hashes the raw PSBT bytes.
func PSBTSHA256(psbtB64 string) string {
	raw, err := base64.StdEncoding.DecodeString(psbtB64)
	if err != nil {
		raw = []byte(psbtB64)
	}
	h := sha256.Sum256(raw)
	return hex.EncodeToString(h[:])
}

// Output is one transaction output as the pipeline sees it.
type Output struct {
	Address string `json:"address"`
	Atoms   int64  `json:"atoms"`
}

// Decoded is the pipeline's view of a PSBT: what gets approved and later compared.
type Decoded struct {
	Outputs  []Output         `json:"outputs"`
	FeeAtoms *int64           `json:"fee_atoms"`
	Vin      []map[string]any `json:"vin"`
	Locktime any              `json:"locktime"`
}

func (d *Decoded) Map() map[string]any {
	var m map[string]any
	_ = jsonx.Decode(jsonx.Compact(d), &m)
	return m
}

func DecodedFromMap(m map[string]any) *Decoded {
	var d Decoded
	_ = jsonx.Decode(jsonx.Compact(m), &d)
	return &d
}

// Envelope is the signed policy decision the service hands the co-signer (HMAC over the canonical body
// with the shared decision secret; the spec names an ML-DSA key, the boundary is the same).
type Envelope map[string]any

func MakeEnvelope(decisionSecret, psbtSHA string, policyVersion, amountAtoms int64, destinations []string, ttl int64) Envelope {
	env := Envelope{"psbt_sha256": psbtSHA, "policy_version": policyVersion, "decision": "allow", "amount_atoms": amountAtoms,
		"destinations": destinations, "expires_at": time.Now().Unix() + ttl}
	env["mac"] = envMAC(decisionSecret, env)
	return env
}

func envMAC(secret string, env Envelope) string {
	body := map[string]any{}
	for k, v := range env {
		if k != "mac" {
			body[k] = v
		}
	}
	h := hmac.New(sha256.New, []byte(secret))
	h.Write([]byte(jsonx.Canonical(body)))
	return hex.EncodeToString(h.Sum(nil))
}

func VerifyEnvelope(decisionSecret string, env Envelope) bool {
	m, _ := env["mac"].(string)
	exp := jsonx.I(map[string]any(env), "expires_at", 0)
	return hmac.Equal([]byte(m), []byte(envMAC(decisionSecret, env))) && jsonx.S(map[string]any(env), "decision") == "allow" && exp > time.Now().Unix()
}

// Cosigner is what the pipeline needs from the Tier 2 policy co-signer.
type Cosigner interface {
	Envelope(w *store.Wallet, psbt string, approved *Decoded, policyVersion int64) Envelope
	Sign(agentID, psbt string, env Envelope) (string, error)
}

type lockedInputs struct {
	nodeWallet string
	vin        []map[string]any
}

type Pipeline struct {
	Cfg      *config.Config
	RPC      *rpc.Client
	Wallets  *wallet.Wallets
	Policy   *policy.Engine
	Store    *store.Store
	Cosigner Cosigner
	// Decode is replaceable for tests (post-sign mutation scenario).
	Decode func(psbt string) (*Decoded, error)

	mu       sync.Mutex
	locked   map[string]lockedInputs // psbt sha256 -> inputs the build locked
	walletMu map[string]*sync.Mutex  // one spend at a time per wallet: evaluate -> build -> record is atomic
}

func New(cfg *config.Config, r *rpc.Client, ws *wallet.Wallets, pe *policy.Engine, st *store.Store, cs Cosigner) *Pipeline {
	p := &Pipeline{Cfg: cfg, RPC: r, Wallets: ws, Policy: pe, Store: st, Cosigner: cs, locked: map[string]lockedInputs{}, walletMu: map[string]*sync.Mutex{}}
	p.Decode = p.decode
	return p
}

// lockWallet serialises spends per wallet within this process; the database-level check in
// Store.AddSpendChecked covers other processes.
func (p *Pipeline) lockWallet(walletID string) func() {
	p.mu.Lock()
	m, ok := p.walletMu[walletID]
	if !ok {
		m = &sync.Mutex{}
		p.walletMu[walletID] = m
	}
	p.mu.Unlock()
	m.Lock()
	return m.Unlock
}

// Recover rebuilds the locked-input map from pending approvals after a restart and releases the inputs of
// approvals that expired while the service was down. Call once at startup.
func (p *Pipeline) Recover() {
	for _, a := range p.Store.AllOpenApprovals() {
		d := DecodedFromMap(a.Decoded)
		w := p.Store.Wallet(a.WalletID)
		if w == nil {
			continue
		}
		p.mu.Lock()
		p.locked[a.PSBTSHA256] = lockedInputs{w.NodeWallet, d.Vin}
		p.mu.Unlock()
	}
	p.ReleaseExpiredApprovals()
}

// Janitor releases expired approvals on a timer (not only on the next Send) until ctx ends.
func (p *Pipeline) Janitor(stop <-chan struct{}, every time.Duration) {
	t := time.NewTicker(every)
	defer t.Stop()
	for {
		select {
		case <-stop:
			return
		case <-t.C:
			p.ReleaseExpiredApprovals()
		}
	}
}

func (p *Pipeline) release(h string) {
	p.mu.Lock()
	item, ok := p.locked[h]
	delete(p.locked, h)
	p.mu.Unlock()
	if !ok {
		return
	}
	vin := make([]any, len(item.vin))
	for i, v := range item.vin {
		vin[i] = v
	}
	_, _ = p.RPC.Wallet(item.nodeWallet).Call("lockunspent", true, vin)
}

// ReleaseExpiredApprovals unlocks inputs of approvals that timed out.
func (p *Pipeline) ReleaseExpiredApprovals() {
	p.mu.Lock()
	keys := make([]string, 0, len(p.locked))
	for h := range p.locked {
		keys = append(keys, h)
	}
	p.mu.Unlock()
	for _, h := range keys {
		a := p.Store.Approval(h)
		switch {
		case a == nil:
			p.release(h)
		case a.Consumed: // completed by another process (owner CLI); the wallet forgot the lock when it spent
			p.mu.Lock()
			delete(p.locked, h)
			p.mu.Unlock()
		case !a.Approved && a.ExpiresAt < float64(time.Now().Unix()):
			p.release(h)
			_ = p.Store.SetSpendStatus(h, "expired")
		}
	}
}

// FeeRate returns atoms per byte. Order: node estimate -> explorer -> policy floor (never below
// minrelaytxfee). A requested cap lowers the rate; a request above the policy ceiling is returned as-is
// so the policy engine denies it with reason fee_rate.
func (p *Pipeline) FeeRate(walletID string, requested *int64) (int64, error) {
	doc, err := p.Policy.Load(walletID)
	if err != nil {
		return 0, err
	}
	lim := jsonx.M(doc, "limits")
	floor := jsonx.I(lim, "min_fee_rate_atoms_per_byte", 1)
	if mi, err := p.RPC.Map("getmempoolinfo"); err == nil {
		relay := int64(jsonx.F(mi, "minrelaytxfee", 0)*wallet.Atoms/1000 + 0.5) // QTC/kB -> atoms/byte
		floor = max(floor, relay)
	}
	var base *int64
	if est, err := p.RPC.Map("estimatesmartfee", 6); err == nil {
		if fr, ok := jsonx.Float(est["feerate"]); ok && fr > 0 {
			v := int64(fr*wallet.Atoms/1000 + 0.5)
			base = &v
		}
	}
	if base == nil && p.Cfg.ExplorerURL != "" {
		cl := &http.Client{Timeout: 5 * time.Second}
		if resp, err := cl.Get(strings.TrimRight(p.Cfg.ExplorerURL, "/") + "/api/v1/fees"); err == nil {
			var fees map[string]any
			data, _ := io.ReadAll(resp.Body)
			resp.Body.Close()
			if jsonx.Decode(data, &fees) == nil {
				for _, k := range []string{"6", "3", "12"} {
					if f, ok := jsonx.Float(fees[k]); ok && f > 0 {
						v := int64(f + 0.5)
						base = &v
						break
					}
				}
			}
		}
	}
	rate := floor
	if base != nil {
		rate = max(*base, floor)
	}
	ceiling := jsonx.I(lim, "max_fee_rate_atoms_per_byte", 0)
	if requested != nil {
		if *requested > ceiling {
			return *requested, nil
		}
		rate = max(min(rate, *requested), floor)
	}
	return min(rate, ceiling), nil
}

// Request is a wallet.send call after transport-level parsing.
type Request struct {
	WalletID          string
	To                string
	AmountAtoms       *int64
	Purpose           string
	MaxFeeRate        *int64
	SendMax           bool
	Actor             string
	IsHTLCDestination bool
	Trade             *policy.Trade
}

// Result is the pipeline outcome for the agent.
type Result struct {
	Status           string `json:"status"`
	Reason           string `json:"reason,omitempty"`
	TxID             string `json:"txid,omitempty"`
	PSBTSHA256       string `json:"psbt_sha256,omitempty"`
	FeeAtoms         *int64 `json:"fee_atoms,omitempty"`
	ExpiresInSeconds int64  `json:"expires_in_seconds,omitempty"`
}

// Send runs request -> evaluation -> build -> (approval) -> sign/verify/broadcast.
func (p *Pipeline) Send(req Request) (*Result, error) {
	if req.Purpose == "" {
		return nil, errs.Bad("bad_request", "purpose is required")
	}
	if (req.AmountAtoms == nil) == !req.SendMax {
		return nil, errs.Bad("bad_request", "give exactly one of amount_atoms or send_max")
	}
	w, err := p.Wallets.Get(req.WalletID)
	if err != nil {
		return nil, err
	}
	defer p.lockWallet(req.WalletID)()
	// re-read under the lock: a concurrent request may have frozen the wallet (kill switch) while we waited
	if w, err = p.Wallets.Get(req.WalletID); err != nil {
		return nil, err
	}
	if w.Frozen {
		reason := w.FrozenReason
		if reason == "" {
			reason = "frozen"
		}
		return nil, errs.Froze(reason)
	}
	p.ReleaseExpiredApprovals()
	va, err := p.RPC.Map("validateaddress", req.To)
	if err != nil {
		return nil, err
	}
	if !jsonx.B(va, "isvalid", false) {
		return nil, errs.Bad("bad_request", "invalid destination address")
	}
	rate, err := p.FeeRate(req.WalletID, req.MaxFeeRate)
	if err != nil {
		return nil, err
	}
	var amount int64
	if req.SendMax {
		bal, err := p.Wallets.Balance(req.WalletID)
		if err != nil {
			return nil, err
		}
		amount = bal.ConfirmedAtoms
	} else {
		amount = *req.AmountAtoms
	}
	decision, err := p.Policy.Evaluate(w, amount, []string{req.To}, rate, -1, 0, req.IsHTLCDestination, req.Trade)
	if err != nil {
		return nil, err
	}
	payload := decision.Map()
	payload["to"], payload["amount_atoms"], payload["fee_rate"], payload["purpose"] = req.To, amount, rate, req.Purpose
	p.Store.Audit("policy.decision", payload, req.WalletID, req.Actor)
	if decision.Decision == policy.Deny {
		if p.Policy.RecordDenial(w, decision.Reason) && !w.Frozen {
			_ = p.Wallets.Freeze(req.WalletID, "kill_switch:denials_per_hour ("+decision.Reason+")", "policy")
		}
		return &Result{Status: "denied", Reason: decision.Reason}, nil
	}
	psbt, decoded, err := p.build(w, req.To, amount, rate, req.SendMax)
	if err != nil {
		return nil, err
	}
	h := PSBTSHA256(psbt)
	p.mu.Lock()
	p.locked[h] = lockedInputs{w.NodeWallet, decoded.Vin}
	p.mu.Unlock()
	status := "signing"
	if decision.Decision == policy.NeedsApproval {
		status = "needs_approval"
	}
	doc, _ := p.Policy.Load(req.WalletID)
	lim := jsonx.M(doc, "limits")
	if err := p.Store.AddSpendChecked(req.WalletID, h, amount, []string{req.To}, req.Purpose, status,
		jsonx.I(lim, "per_day_atoms", 0), jsonx.I(lim, "per_day_tx_count", 0)); err != nil {
		p.release(h)
		if err == store.ErrLimit { // another process recorded a spend between evaluation and here
			reason := "per_day_limit"
			p.Store.Audit("policy.decision", map[string]any{"decision": policy.Deny, "reason": reason, "to": req.To, "amount_atoms": amount, "purpose": req.Purpose}, req.WalletID, req.Actor)
			if p.Policy.RecordDenial(w, reason) && !w.Frozen {
				_ = p.Wallets.Freeze(req.WalletID, "kill_switch:denials_per_hour ("+reason+")", "policy")
			}
			return &Result{Status: "denied", Reason: reason}, nil
		}
		return nil, err
	}
	if decision.Decision == policy.NeedsApproval {
		ttl := jsonx.I(jsonx.M(doc, "approval"), "ttl_seconds", 900)
		request := map[string]any{"to": req.To, "amount_atoms": amount, "purpose": req.Purpose, "fee_rate": rate}
		if err := p.Store.AddApproval(h, req.WalletID, psbt, decoded.Map(), request, ttl); err != nil {
			p.release(h)
			return nil, err
		}
		p.notifyApproval(req.WalletID, h, decoded, ttl)
		p.Store.Audit("spend.needs_approval", map[string]any{"psbt_sha256": h, "outputs": decoded.Outputs, "fee_atoms": decoded.FeeAtoms}, req.WalletID, req.Actor)
		return &Result{Status: "needs_approval", PSBTSHA256: h, ExpiresInSeconds: ttl}, nil
	}
	return p.signVerifyBroadcast(w, psbt, h, decoded, req.Actor)
}

// CompleteApproved finishes the pipeline for a PSBT the owner approved by hash.
func (p *Pipeline) CompleteApproved(psbtSHA, actor string) (*Result, error) {
	a := p.Store.Approval(psbtSHA)
	switch {
	case a == nil:
		return nil, errs.Bad("unknown_approval", "no such pending approval")
	case !a.Approved:
		return nil, errs.Bad("not_approved", "approval not granted")
	case a.Consumed:
		return nil, errs.Bad("consumed", "approval already used")
	}
	w, err := p.Wallets.Get(a.WalletID)
	if err != nil {
		return nil, err
	}
	_ = p.Store.ConsumeApproval(psbtSHA)
	_ = p.Store.SetSpendStatus(psbtSHA, "signing")
	return p.signVerifyBroadcast(w, a.PSBT, psbtSHA, DecodedFromMap(a.Decoded), actor)
}

func (p *Pipeline) build(w *store.Wallet, to string, amount, rate int64, sendMax bool) (string, *Decoded, error) {
	wr := p.RPC.Wallet(w.NodeWallet)
	height, err := p.RPC.Int("getblockcount")
	if err != nil {
		return "", nil, err
	}
	change := w.ReceiveAddress
	if w.Tier != 2 {
		if change, err = wr.String("getrawchangeaddress", "p2mr"); err != nil {
			return "", nil, err
		}
	}
	opts := map[string]any{"fee_rate": rate, "lockUnspents": true, "changeAddress": change, "add_inputs": true, "include_watching": w.Tier == 2}
	if sendMax {
		opts["subtractFeeFromOutputs"] = []any{0}
	}
	res, err := wr.Map("walletcreatefundedpsbt", []any{}, []any{map[string]any{to: json.Number(wallet.AtomsToQtc(amount))}}, max(int64(0), height-1), opts)
	if err != nil {
		if e := errs.As(err); e != nil && e.Kind == errs.NodeError {
			return "", nil, errs.Bad("build_failed", "%s", e.Message)
		}
		return "", nil, err
	}
	psbt := jsonx.S(res, "psbt")
	decoded, err := p.Decode(psbt)
	if err != nil {
		return "", nil, err
	}
	return psbt, decoded, nil
}

func outputsOf(tx map[string]any) []Output {
	var outs []Output
	for _, ov := range jsonx.L(tx, "vout") {
		o, _ := ov.(map[string]any)
		spk := jsonx.M(o, "scriptPubKey")
		addr := jsonx.S(spk, "address")
		if addr == "" {
			addr = jsonx.S(spk, "hex")
		}
		outs = append(outs, Output{Address: addr, Atoms: wallet.QtcToAtoms(o["value"])})
	}
	return outs
}

func (p *Pipeline) decode(psbt string) (*Decoded, error) {
	d, err := p.RPC.Map("decodepsbt", psbt)
	if err != nil {
		return nil, err
	}
	tx := jsonx.M(d, "tx")
	out := &Decoded{Outputs: outputsOf(tx), Locktime: tx["locktime"]}
	if _, ok := d["fee"]; ok {
		f := wallet.QtcToAtoms(d["fee"])
		out.FeeAtoms = &f
	}
	for _, iv := range jsonx.L(tx, "vin") {
		i, _ := iv.(map[string]any)
		out.Vin = append(out.Vin, map[string]any{"txid": i["txid"], "vout": i["vout"]})
	}
	return out, nil
}

func sameOutputs(a, b []Output) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func (p *Pipeline) signVerifyBroadcast(w *store.Wallet, psbt, h string, approved *Decoded, actor string) (*Result, error) {
	res, err := p.signVerifyBroadcastInner(w, psbt, h, approved, actor)
	if err != nil {
		p.release(h)
		if e := errs.As(err); e != nil && e.Kind == errs.NodeError {
			_ = p.Store.SetSpendStatus(h, "failed")
			p.Store.Audit("spend.failed", map[string]any{"psbt_sha256": h, "error": e.Message}, w.WalletID, actor)
			return nil, errs.Bad("sign_failed", "%s", e.Message)
		}
		return nil, err
	}
	return res, nil
}

func (p *Pipeline) signVerifyBroadcastInner(w *store.Wallet, psbt, h string, approved *Decoded, actor string) (*Result, error) {
	wid := w.WalletID
	var signed string
	if w.Tier == 1 {
		if err := p.Wallets.Unlock(w.NodeWallet, 60); err != nil {
			return nil, err
		}
		r, err := p.RPC.Wallet(w.NodeWallet).Map("walletprocesspsbt", psbt)
		p.Wallets.Lock(w.NodeWallet)
		if err != nil {
			return nil, err
		}
		signed = jsonx.S(r, "psbt")
	} else {
		// updater step in the coordinator, agent key signs, policy co-signer signs, combine
		upd, err := p.RPC.Wallet(w.NodeWallet).Map("walletprocesspsbt", psbt, map[string]any{"sign": false, "bip32derivs": true, "finalize": false})
		if err != nil {
			return nil, err
		}
		updPSBT := jsonx.S(upd, "psbt")
		if err := p.Wallets.Unlock(w.KeyWallet, 60); err != nil {
			return nil, err
		}
		pa, err := p.RPC.Wallet(w.KeyWallet).Map("walletprocesspsbt", updPSBT, map[string]any{"finalize": false})
		p.Wallets.Lock(w.KeyWallet)
		if err != nil {
			return nil, err
		}
		if p.Cosigner == nil {
			return nil, errs.New(errs.NodeUnavailable, "cosigner_unavailable", "tier 2 spend needs the policy co-signer")
		}
		env := p.Cosigner.Envelope(w, updPSBT, approved, p.Policy.Version(wid))
		pb, err := p.Cosigner.Sign(w.AgentID, updPSBT, env)
		if err != nil {
			return nil, errs.New(errs.NodeUnavailable, "cosigner_unavailable", "%v", err)
		}
		if signed, err = p.RPC.String("combinepsbt", []any{jsonx.S(pa, "psbt"), pb}); err != nil {
			return nil, err
		}
	}
	fin, err := p.RPC.Map("finalizepsbt", signed)
	if err != nil {
		return nil, err
	}
	if !jsonx.B(fin, "complete", false) {
		return nil, errs.Bad("sign_incomplete", "transaction did not finalize (missing signatures)")
	}
	rawtx := jsonx.S(fin, "hex")
	dec, err := p.RPC.Map("decoderawtransaction", rawtx)
	if err != nil {
		return nil, err
	}
	got := outputsOf(dec)
	if !sameOutputs(got, approved.Outputs) {
		_ = p.Store.SetSpendStatus(h, "security_alert")
		p.Store.Audit("security.post_sign_mutation", map[string]any{"psbt_sha256": h, "expected": approved.Outputs, "got": got}, wid, actor)
		_ = p.Wallets.Freeze(wid, "security_alert:post_sign_mutation", "pipeline")
		return nil, errs.Conflicting("security_alert", "final transaction outputs differ from the approved outputs")
	}
	acc, err := p.RPC.List("testmempoolaccept", []any{rawtx})
	if err != nil {
		return nil, err
	}
	if len(acc) == 0 {
		return nil, errs.Unavailable("testmempoolaccept returned nothing")
	}
	a0, _ := acc[0].(map[string]any)
	if !jsonx.B(a0, "allowed", false) {
		_ = p.Store.SetSpendStatus(h, "rejected")
		reason := jsonx.S(a0, "reject-reason")
		if reason == "" {
			reason = "rejected"
		}
		return nil, errs.Conflicting("mempool_reject", "%s", reason)
	}
	txid, err := p.RPC.String("sendrawtransaction", rawtx)
	if err != nil {
		return nil, err
	}
	p.mu.Lock()
	delete(p.locked, h) // inputs are spent; the wallet forgets the lock itself
	p.mu.Unlock()
	_ = p.Store.SetSpendBroadcast(h, txid, approved.FeeAtoms)
	p.Store.Audit("spend.broadcast", map[string]any{"psbt_sha256": h, "txid": txid, "outputs": approved.Outputs, "fee_atoms": approved.FeeAtoms}, wid, actor)
	return &Result{Status: "broadcast", TxID: txid, PSBTSHA256: h, FeeAtoms: approved.FeeAtoms}, nil
}

// Status tracks a spend by PSBT hash.
type Status struct {
	Status        string `json:"status"`
	TxID          string `json:"txid,omitempty"`
	PSBTSHA256    string `json:"psbt_sha256"`
	Confirmations int64  `json:"confirmations"`
}

func (p *Pipeline) Status(walletID, psbtSHA string) (*Status, error) {
	s := p.Store.Spend(psbtSHA)
	if s == nil || s.WalletID != walletID {
		return nil, errs.NotFoundErr("unknown", "no such spend")
	}
	out := &Status{Status: s.Status, TxID: s.TxID, PSBTSHA256: psbtSHA}
	if s.TxID != "" && (s.Status == "broadcast" || s.Status == "confirmed") {
		w, err := p.Wallets.Get(walletID)
		if err != nil {
			return nil, err
		}
		if t, err := p.RPC.Wallet(w.NodeWallet).Map("gettransaction", s.TxID); err == nil {
			out.Confirmations = jsonx.I(t, "confirmations", 0)
			switch {
			case out.Confirmations < 0:
				_ = p.Store.SetSpendStatus(psbtSHA, "dropped")
				out.Status = "dropped"
			case out.Confirmations >= int64(p.Cfg.ConfirmationDepth) && s.Status != "confirmed":
				_ = p.Store.SetSpendStatus(psbtSHA, "confirmed")
				out.Status = "confirmed"
				p.Store.Audit("spend.confirmed", map[string]any{"txid": s.TxID, "confirmations": out.Confirmations}, walletID, "")
			}
		}
	}
	return out, nil
}

func (p *Pipeline) notifyApproval(walletID, h string, d *Decoded, ttl int64) {
	if p.Cfg.ApprovalWebhook == "" {
		return
	}
	body := jsonx.Compact(map[string]any{"wallet_id": walletID, "psbt_sha256": h, "outputs": d.Outputs, "fee_atoms": d.FeeAtoms, "ttl_seconds": ttl})
	cl := &http.Client{Timeout: 5 * time.Second}
	if resp, err := cl.Post(p.Cfg.ApprovalWebhook, "application/json", bytes.NewReader(body)); err == nil {
		resp.Body.Close()
	}
}
