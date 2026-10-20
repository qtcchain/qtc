// Package cosigner: the Tier 2 policy co-signer (spec §3 Tier 2, §6.4).
//
// A separate process with its own encrypted node wallet holding policy_mldsa. It signs a PSBT only when
// it carries a valid signed policy decision whose PSBT hash matches, and after re-checking the amounts
// and destinations against its own copy of the policy. Two independent evaluations must agree.
package cosigner

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"time"

	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/config"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/jsonx"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/policy"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/rpc"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/spend"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/store"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/wallet"
)

// Client is used by the service: fetch the policy pubkey for an agent and request a partial signature.
type Client struct {
	URL     string
	Keyring *config.Keyring
	http    *http.Client
}

func NewClient(url string, kr *config.Keyring) *Client {
	return &Client{URL: strings.TrimRight(url, "/"), Keyring: kr, http: &http.Client{Timeout: 120 * time.Second}}
}

// ErrRejected is returned when the co-signer refuses (403).
var ErrRejected = errors.New("cosigner rejected")

func (c *Client) post(path string, body map[string]any) (map[string]any, error) {
	resp, err := c.http.Post(c.URL+path, "application/json", bytes.NewReader(jsonx.Compact(body)))
	if err != nil {
		return nil, fmt.Errorf("cosigner unreachable: %w", err)
	}
	defer resp.Body.Close()
	data, _ := io.ReadAll(resp.Body)
	var out map[string]any
	_ = jsonx.Decode(data, &out)
	if resp.StatusCode != 200 {
		msg := jsonx.S(out, "error")
		if msg == "" {
			msg = string(data)
		}
		if resp.StatusCode == 403 {
			return nil, fmt.Errorf("%w: %s", ErrRejected, msg)
		}
		return nil, fmt.Errorf("cosigner %d: %s", resp.StatusCode, msg)
	}
	return out, nil
}

func (c *Client) Pubkey(agentID string) (string, error) {
	r, err := c.post("/pubkey", map[string]any{"agent_id": agentID})
	if err != nil {
		return "", err
	}
	return jsonx.S(r, "pubkey"), nil
}

// Envelope builds the signed decision for the external outputs of the PSBT (change excluded).
func (c *Client) Envelope(w *store.Wallet, psbt string, approved *spend.Decoded, policyVersion int64) spend.Envelope {
	var amount int64
	dests := []string{}
	for _, o := range approved.Outputs {
		if o.Address != w.ReceiveAddress {
			amount += o.Atoms
			dests = append(dests, o.Address)
		}
	}
	secret, _ := c.Keyring.Require("decision_secret")
	return spend.MakeEnvelope(secret, spend.PSBTSHA256(psbt), policyVersion, amount, dests, 600)
}

func (c *Client) Sign(agentID, psbt string, env spend.Envelope) (string, error) {
	r, err := c.post("/sign", map[string]any{"agent_id": agentID, "psbt": psbt, "envelope": map[string]any(env)})
	if err != nil {
		return "", err
	}
	return jsonx.S(r, "psbt"), nil
}

// Service is the co-signer process: own keyring (wallet passphrases + decision_secret + admin_secret copy
// for policy verification), own store, own node wallets policy-<agent>.
type Service struct {
	Cfg     *config.Config
	RPC     *rpc.Client
	Keyring *config.Keyring
	Store   *store.Store
	Policy  *policy.Engine
	// WalletLookup reads wallet rows (receive address = change) from the service store, read-only.
	WalletLookup func(walletID string) *store.Wallet
}

type permissionError struct{ msg string }

func (e *permissionError) Error() string { return e.msg }

func denied(format string, a ...any) error { return &permissionError{fmt.Sprintf(format, a...)} }

func (s *Service) walletName(agentID string) string { return "policy-" + agentID }

func (s *Service) ensureWallet(name string) error {
	loaded, err := s.RPC.List("listwallets")
	if err != nil {
		return err
	}
	for _, l := range loaded {
		if l == name {
			return nil
		}
	}
	if _, err := s.RPC.Call("loadwallet", name); err == nil {
		return nil
	}
	if _, err := s.RPC.Call("createwallet", name); err != nil {
		return err
	}
	seed, err := s.Keyring.Ensure("cosigner_seed", 32)
	if err != nil {
		return err
	}
	sum := sha256.Sum256([]byte(seed + name))
	pw := hex.EncodeToString(sum[:])
	if _, err := s.RPC.Wallet(name).Call("encryptwallet", pw); err != nil {
		return err
	}
	return s.Keyring.Set("wallet:"+name, pw)
}

// Pubkey returns (creating when needed) the policy ML-DSA public key for an agent.
func (s *Service) Pubkey(agentID string) (string, error) {
	name := s.walletName(agentID)
	if err := s.ensureWallet(name); err != nil {
		return "", err
	}
	if k, ok := s.Store.MetaGet("policy_pubkey:" + agentID); ok {
		return k, nil
	}
	pw, err := s.Keyring.Require("wallet:" + name)
	if err != nil {
		return "", err
	}
	wr := s.RPC.Wallet(name)
	if _, err := wr.Call("walletpassphrase", pw, 60); err != nil {
		return "", err
	}
	defer wr.Call("walletlock")
	addr, err := wr.String("getnewaddress", "policy-key", "p2mr")
	if err != nil {
		return "", err
	}
	m, err := wr.Map("exportpqkey", addr)
	if err != nil {
		return "", err
	}
	key := jsonx.S(m, "key")
	_ = s.Store.MetaSet("policy_pubkey:"+agentID, key)
	s.Store.Audit("cosigner.key", map[string]any{"agent_id": agentID, "pubkey_prefix": key[:min(16, len(key))]}, "", "cosigner")
	return key, nil
}

// Sign verifies the envelope and the PSBT against its own policy copy, then adds the policy signature.
func (s *Service) Sign(agentID, psbt string, env spend.Envelope) (string, error) {
	secret, err := s.Keyring.Require("decision_secret")
	if err != nil {
		return "", err
	}
	if !spend.VerifyEnvelope(secret, env) {
		s.Store.Audit("cosigner.reject", map[string]any{"agent_id": agentID, "reason": "bad_envelope"}, "", "cosigner")
		return "", denied("decision envelope invalid or expired")
	}
	h := spend.PSBTSHA256(psbt)
	if h != jsonx.S(map[string]any(env), "psbt_sha256") {
		s.Store.Audit("cosigner.reject", map[string]any{"agent_id": agentID, "reason": "psbt_hash_mismatch"}, "", "cosigner")
		return "", denied("psbt hash does not match the decision")
	}
	walletID := fmt.Sprintf("agent-%s-tier2", agentID)
	doc, err := s.Policy.Load(walletID) // own copy of the policy, verified against the admin secret
	if err != nil {
		return "", denied("policy unavailable to the co-signer: %v", err)
	}
	if jsonx.I(doc, "version", 0) != jsonx.I(map[string]any(env), "policy_version", -1) {
		return "", denied("policy version mismatch")
	}
	// independent re-check of amounts and destinations against the PSBT itself
	d, err := s.RPC.Map("decodepsbt", psbt)
	if err != nil {
		return "", err
	}
	outs := map[string]int64{}
	for _, ov := range jsonx.L(jsonx.M(d, "tx"), "vout") {
		o, _ := ov.(map[string]any)
		spk := jsonx.M(o, "scriptPubKey")
		a := jsonx.S(spk, "address")
		if a == "" {
			a = jsonx.S(spk, "hex")
		}
		outs[a] += wallet.QtcToAtoms(o["value"])
	}
	change := ""
	if s.WalletLookup != nil {
		if wrow := s.WalletLookup(walletID); wrow != nil {
			change = wrow.ReceiveAddress
		}
	}
	ext := map[string]int64{}
	var total int64
	for a, v := range outs {
		if a != change {
			ext[a] = v
			total += v
		}
	}
	envDests := jsonx.Strs(map[string]any(env), "destinations")
	if len(envDests) != len(ext) || total != jsonx.I(map[string]any(env), "amount_atoms", -1) {
		s.Store.Audit("cosigner.reject", map[string]any{"agent_id": agentID, "reason": "outputs_disagree"}, "", "cosigner")
		return "", denied("PSBT outputs disagree with the decision envelope")
	}
	for _, dd := range envDests {
		if _, ok := ext[dd]; !ok {
			s.Store.Audit("cosigner.reject", map[string]any{"agent_id": agentID, "reason": "outputs_disagree"}, "", "cosigner")
			return "", denied("PSBT outputs disagree with the decision envelope")
		}
	}
	lim, dest := jsonx.M(doc, "limits"), jsonx.M(doc, "destinations")
	if total > jsonx.I(lim, "per_tx_atoms", 0) {
		return "", denied("per_tx_limit (cosigner)")
	}
	if jsonx.S(dest, "mode") == "allowlist" {
		allow := jsonx.Strs(dest, "allow")
		for a := range ext {
			if !jsonx.Contains(allow, a) {
				return "", denied("destination_not_allowed (cosigner)")
			}
		}
	}
	name := s.walletName(agentID)
	if _, err := s.Pubkey(agentID); err != nil { // ensures loaded
		return "", err
	}
	pw, err := s.Keyring.Require("wallet:" + name)
	if err != nil {
		return "", err
	}
	wr := s.RPC.Wallet(name)
	if _, err := wr.Call("walletpassphrase", pw, 60); err != nil {
		return "", err
	}
	defer wr.Call("walletlock")
	out, err := wr.Map("walletprocesspsbt", psbt, map[string]any{"finalize": false})
	if err != nil {
		return "", err
	}
	s.Store.Audit("cosigner.sign", map[string]any{"agent_id": agentID, "psbt_sha256": h, "amount_atoms": env["amount_atoms"]}, walletID, "cosigner")
	return jsonx.S(out, "psbt"), nil
}

// Handler serves /pubkey and /sign on loopback.
func Handler(s *Service) http.Handler {
	mux := http.NewServeMux()
	send := func(w http.ResponseWriter, code int, body map[string]any) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(code)
		_, _ = w.Write(jsonx.Compact(body))
	}
	wrap := func(fn func(body map[string]any) (map[string]any, error)) http.HandlerFunc {
		return func(w http.ResponseWriter, r *http.Request) {
			host, _, _ := net.SplitHostPort(r.RemoteAddr)
			if host != "127.0.0.1" && host != "::1" {
				send(w, 403, map[string]any{"error": "loopback only"})
				return
			}
			if r.Method != http.MethodPost {
				send(w, 404, map[string]any{"error": "not found"})
				return
			}
			data, _ := io.ReadAll(http.MaxBytesReader(w, r.Body, 4<<20))
			var body map[string]any
			if err := jsonx.Decode(data, &body); err != nil {
				send(w, 400, map[string]any{"error": "bad json"})
				return
			}
			out, err := fn(body)
			if err != nil {
				var pe *permissionError
				if errors.As(err, &pe) {
					send(w, 403, map[string]any{"error": err.Error()})
					return
				}
				send(w, 500, map[string]any{"error": err.Error()})
				return
			}
			send(w, 200, out)
		}
	}
	mux.Handle("/pubkey", wrap(func(b map[string]any) (map[string]any, error) {
		k, err := s.Pubkey(jsonx.S(b, "agent_id"))
		return map[string]any{"pubkey": k}, err
	}))
	mux.Handle("/sign", wrap(func(b map[string]any) (map[string]any, error) {
		env, _ := b["envelope"].(map[string]any)
		out, err := s.Sign(jsonx.S(b, "agent_id"), jsonx.S(b, "psbt"), spend.Envelope(env))
		return map[string]any{"psbt": out}, err
	}))
	return mux
}

// Serve starts the co-signer HTTP server on listen (loopback) and returns it.
func Serve(s *Service, listen string) (*http.Server, error) {
	ln, err := net.Listen("tcp", listen)
	if err != nil {
		return nil, err
	}
	srv := &http.Server{Handler: Handler(s), ReadHeaderTimeout: 10 * time.Second}
	go func() { _ = srv.Serve(ln) }()
	return srv, nil
}
