// Package wallet: lifecycle (spec §4): create, fund, backup, balance, history, rotate, freeze, retire.
//
// Node wallets are named agent-<agent_id>-tier1 / -tier2; Tier 2 additionally uses agent-<agent_id>-key
// (the agent's own ML-DSA key wallet) with agent-<agent_id>-tier2 as the watch-only coordinator that
// holds the mr(multi_pq(2,agent,policy)) descriptor. The service never adopts a wallet it did not create.
package wallet

import (
	"fmt"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"time"

	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/config"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/errs"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/jsonx"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/rpc"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/store"
)

const Atoms = 100_000_000

var (
	walletRe = regexp.MustCompile(`^[a-z0-9][a-z0-9_-]{0,31}$`)
	hexRe    = regexp.MustCompile(`^[0-9a-f]+$`)
)

// isMLDSA44Hex: a 1312-byte ML-DSA-44 public key as lowercase hex (Go's regexp caps repeat counts, so check length separately).
func isMLDSA44Hex(k string) bool { return len(k) == 2624 && hexRe.MatchString(k) }

// QtcToAtoms converts a JSON amount (QTC, up to 8 decimals) to atoms.
func QtcToAtoms(v any) int64 {
	f, _ := jsonx.Float(v)
	return int64(math.Round(f * Atoms))
}

// AtomsToQtc formats atoms as a QTC amount string the node parses exactly.
func AtomsToQtc(a int64) string {
	neg := a < 0
	if neg {
		a = -a
	}
	s := fmt.Sprintf("%d.%08d", a/Atoms, a%Atoms)
	if neg {
		s = "-" + s
	}
	return s
}

// QtcNumber returns the QTC amount as a JSON number value for RPC parameters.
func QtcNumber(a int64) float64 {
	f, _ := jsonx.Float(AtomsToQtc(a))
	return f
}

// CosignerPubkeyFn fetches the policy ML-DSA public key (hex) for an agent from the co-signer.
type CosignerPubkeyFn func(agentID string) (string, error)

type Wallets struct {
	Cfg      *config.Config
	RPC      *rpc.Client
	Keyring  *config.Keyring
	Store    *store.Store
	Cosigner CosignerPubkeyFn
}

func New(cfg *config.Config, r *rpc.Client, kr *config.Keyring, st *store.Store, cs CosignerPubkeyFn) *Wallets {
	return &Wallets{Cfg: cfg, RPC: r, Keyring: kr, Store: st, Cosigner: cs}
}

func (ws *Wallets) ensureLoaded(name string) error {
	loaded, err := ws.RPC.List("listwallets")
	if err != nil {
		return err
	}
	for _, l := range loaded {
		if l == name {
			return nil
		}
	}
	if _, err := ws.RPC.Call("loadwallet", name); err != nil {
		if e := errs.As(err); e != nil && strings.Contains(e.Message, "already loaded") {
			return nil
		}
		return err
	}
	return nil
}

// Get returns the wallet row and makes sure its node wallets are loaded.
func (ws *Wallets) Get(walletID string) (*store.Wallet, error) {
	w := ws.Store.Wallet(walletID)
	if w == nil {
		return nil, errs.NotFoundErr("unknown_wallet", "no wallet %s", walletID)
	}
	if err := ws.ensureLoaded(w.NodeWallet); err != nil {
		return nil, err
	}
	if w.KeyWallet != "" {
		if err := ws.ensureLoaded(w.KeyWallet); err != nil {
			return nil, err
		}
	}
	return w, nil
}

func (ws *Wallets) Unlock(nodeWallet string, seconds int) error {
	pw, err := ws.Keyring.Require("wallet:" + nodeWallet)
	if err != nil {
		return errs.New(errs.Internal, "keyring", "%v", err)
	}
	_, err = ws.RPC.Wallet(nodeWallet).Call("walletpassphrase", pw, seconds)
	return err
}

func (ws *Wallets) Lock(nodeWallet string) {
	_, _ = ws.RPC.Wallet(nodeWallet).Call("walletlock")
}

func (ws *Wallets) createEncrypted(name string) error {
	dir, err := ws.RPC.Map("listwalletdir")
	if err != nil {
		return err
	}
	for _, e := range jsonx.L(dir, "wallets") {
		if m, ok := e.(map[string]any); ok && jsonx.S(m, "name") == name {
			return errs.Conflicting("exists", "node wallet %s already exists; the service refuses to adopt it", name)
		}
	}
	if _, err := ws.RPC.Call("createwallet", name); err != nil {
		return err
	}
	pw := config.RandomHex(32)
	if _, err := ws.RPC.Wallet(name).Call("encryptwallet", pw); err != nil {
		return err
	}
	return ws.Keyring.Set("wallet:"+name, pw)
}

// CreateResult is what wallet.create returns to the owner.
type CreateResult struct {
	WalletID       string `json:"wallet_id"`
	ReceiveAddress string `json:"receive_address"`
	Tier           int    `json:"tier"`
	Descriptor     string `json:"descriptor,omitempty"`
}

// Create implements spec §4 Create for Tier 1 and Tier 2.
func (ws *Wallets) Create(agentID string, tier int, ownerMLDSA []string, cooldownBlocks int64, actor string) (*CreateResult, error) {
	if !walletRe.MatchString(agentID) {
		return nil, errs.Bad("bad_request", "agent_id must match [a-z0-9][a-z0-9_-]{0,31}")
	}
	if tier != 1 && tier != 2 {
		return nil, errs.Bad("bad_request", "tier must be 1 or 2")
	}
	walletID := fmt.Sprintf("agent-%s-tier%d", agentID, tier)
	if ws.Store.Wallet(walletID) != nil {
		return nil, errs.Conflicting("exists", "%s already exists", walletID)
	}
	if tier == 1 {
		if err := ws.createEncrypted(walletID); err != nil {
			return nil, err
		}
		addr, err := ws.RPC.Wallet(walletID).String("getnewaddress", "agent-receive", "p2mr")
		if err != nil {
			return nil, err
		}
		if err := ws.Store.AddWallet(&store.Wallet{WalletID: walletID, AgentID: agentID, Tier: 1, NodeWallet: walletID, ReceiveAddress: addr}); err != nil {
			return nil, err
		}
		ws.Store.Audit("wallet.create", map[string]any{"wallet_id": walletID, "tier": 1, "receive_address": addr}, walletID, actor)
		return &CreateResult{WalletID: walletID, ReceiveAddress: addr, Tier: 1}, nil
	}
	// Tier 2. Node limitation (v0.2.x, verified on regtest): the wallet's PSBT updater fills P2MR leaf metadata
	// for SINGLE-LEAF mr() descriptors only, so the output is mr(multi_pq(2,agent,policy)); the co-signature is
	// enforced in script. The spec's owner CSV recovery leaf is recorded as the planned tree for the day the
	// wallet supports multi-leaf trees; until then owner recovery is through the encrypted backups of both wallets.
	for _, k := range ownerMLDSA {
		if !isMLDSA44Hex(k) {
			return nil, errs.Bad("bad_request", "owner_mldsa entries must be 1312-byte ML-DSA-44 public keys as hex")
		}
	}
	if ws.Cosigner == nil {
		return nil, errs.New(errs.NodeUnavailable, "cosigner_unavailable", "no policy co-signer configured")
	}
	// ask the co-signer first: nothing is created on the node unless it answers
	policyPK, err := ws.Cosigner(agentID)
	if err != nil {
		return nil, errs.New(errs.NodeUnavailable, "cosigner_unavailable", "%v", err)
	}
	keyWallet := fmt.Sprintf("agent-%s-key", agentID)
	if err := ws.createEncrypted(keyWallet); err != nil {
		return nil, err
	}
	if err := ws.Unlock(keyWallet, 60); err != nil { // exporting the PQ public key derives from the encrypted seed
		return nil, err
	}
	kaddr, err := ws.RPC.Wallet(keyWallet).String("getnewaddress", "agent-key", "p2mr")
	var agentPK string
	if err == nil {
		var m map[string]any
		m, err = ws.RPC.Wallet(keyWallet).Map("exportpqkey", kaddr)
		agentPK = jsonx.S(m, "key")
	}
	ws.Lock(keyWallet)
	if err != nil {
		return nil, err
	}
	desc := fmt.Sprintf("mr(multi_pq(2,%s,%s))", agentPK, policyPK)
	var planned any
	if len(ownerMLDSA) == 2 {
		planned = fmt.Sprintf("mr(multi_pq(2,%s,%s),{csv_multi_pq(%d,1,%s,%s)})", agentPK, policyPK, cooldownBlocks, ownerMLDSA[0], ownerMLDSA[1])
	}
	info, err := ws.RPC.Map("getdescriptorinfo", desc)
	if err != nil {
		return nil, err
	}
	descCS := desc + "#" + jsonx.S(info, "checksum")
	if _, err := ws.RPC.Call("createwallet", walletID, true, true); err != nil { // disable_private_keys, blank: coordinator
		return nil, err
	}
	if _, err := ws.RPC.Wallet(walletID).Call("importdescriptors", []any{map[string]any{"desc": descCS, "timestamp": "now", "internal": false, "label": "tier2"}}); err != nil {
		return nil, err
	}
	addrs, err := ws.RPC.List("deriveaddresses", descCS)
	if err != nil || len(addrs) == 0 {
		return nil, errs.Unavailable("deriveaddresses returned nothing")
	}
	addr, _ := addrs[0].(string)
	if err := ws.Store.AddWallet(&store.Wallet{WalletID: walletID, AgentID: agentID, Tier: 2, NodeWallet: walletID, KeyWallet: keyWallet, Descriptor: descCS, ReceiveAddress: addr}); err != nil {
		return nil, err
	}
	ws.Store.Audit("wallet.create", map[string]any{"wallet_id": walletID, "tier": 2, "receive_address": addr, "descriptor": descCS,
		"planned_recovery_tree": planned, "cooldown_blocks": cooldownBlocks}, walletID, actor)
	return &CreateResult{WalletID: walletID, ReceiveAddress: addr, Tier: 2, Descriptor: descCS}, nil
}

func (ws *Wallets) ReceiveAddress(walletID, label string) (string, error) {
	w, err := ws.Get(walletID)
	if err != nil {
		return "", err
	}
	if w.Tier == 2 {
		return w.ReceiveAddress, nil // single-address tree until rotation
	}
	if label == "" {
		label = "agent-receive"
	}
	return ws.RPC.Wallet(w.NodeWallet).String("getnewaddress", label, "p2mr")
}

type Balance struct {
	ConfirmedAtoms     int64 `json:"confirmed_atoms"`
	PendingAtoms       int64 `json:"pending_atoms"`
	ImmatureAtoms      int64 `json:"immature_atoms"`
	LockedInSwapsAtoms int64 `json:"locked_in_swaps_atoms"`
}

func (ws *Wallets) Balance(walletID string) (*Balance, error) {
	w, err := ws.Get(walletID)
	if err != nil {
		return nil, err
	}
	b, err := ws.RPC.Wallet(w.NodeWallet).Map("getbalances")
	if err != nil {
		return nil, err
	}
	mine := jsonx.M(b, "mine")
	if w.Tier == 2 { // watch-only coordinator: balances live under "watchonly"
		if wo, ok := b["watchonly"].(map[string]any); ok {
			mine = wo
		}
	}
	return &Balance{ConfirmedAtoms: QtcToAtoms(mine["trusted"]), PendingAtoms: QtcToAtoms(mine["untrusted_pending"]),
		ImmatureAtoms: QtcToAtoms(mine["immature"]), LockedInSwapsAtoms: ws.Store.LockedInSwaps(walletID)}, nil
}

type HistoryEntry struct {
	TxID          string `json:"txid"`
	Direction     string `json:"direction"`
	AmountAtoms   int64  `json:"amount_atoms"`
	FeeAtoms      int64  `json:"fee_atoms"`
	Height        any    `json:"height"`
	Confirmations int64  `json:"confirmations"`
	Purpose       any    `json:"purpose"`
}

func (ws *Wallets) History(walletID string, limit int, before *int64) ([]HistoryEntry, error) {
	w, err := ws.Get(walletID)
	if err != nil {
		return nil, err
	}
	txs, err := ws.RPC.Wallet(w.NodeWallet).List("listtransactions", "*", limit, 0, w.Tier == 2)
	if err != nil {
		return nil, err
	}
	purposes := map[string]string{}
	for _, s := range ws.Store.RecentSpends(walletID, 500) {
		if s.TxID != "" {
			purposes[s.TxID] = s.Purpose
		}
	}
	out := []HistoryEntry{}
	for i := len(txs) - 1; i >= 0; i-- {
		t, _ := txs[i].(map[string]any)
		h := jsonx.I(t, "blockheight", 0)
		if before != nil && h != 0 && h >= *before {
			continue
		}
		dir := "out"
		switch jsonx.S(t, "category") {
		case "receive", "generate", "immature":
			dir = "in"
		}
		e := HistoryEntry{TxID: jsonx.S(t, "txid"), Direction: dir, AmountAtoms: abs(QtcToAtoms(t["amount"])), FeeAtoms: abs(QtcToAtoms(t["fee"])),
			Confirmations: jsonx.I(t, "confirmations", 0)}
		if _, ok := t["blockheight"]; ok {
			e.Height = h
		}
		if p, ok := purposes[e.TxID]; ok {
			e.Purpose = p
		}
		out = append(out, e)
	}
	return out, nil
}

func abs(x int64) int64 {
	if x < 0 {
		return -x
	}
	return x
}

func (ws *Wallets) Freeze(walletID, reason, actor string) error {
	w, err := ws.Get(walletID)
	if err != nil {
		return err
	}
	if err := ws.Store.SetFrozen(walletID, true, reason); err != nil {
		return err
	}
	for _, n := range []string{w.NodeWallet, w.KeyWallet} {
		if n != "" {
			ws.Lock(n)
		}
	}
	ws.Store.Audit("wallet.freeze", map[string]any{"reason": reason}, walletID, actor)
	return nil
}

func (ws *Wallets) Unfreeze(walletID, actor string) error {
	if _, err := ws.Get(walletID); err != nil {
		return err
	}
	if err := ws.Store.SetFrozen(walletID, false, ""); err != nil {
		return err
	}
	ws.Store.Audit("wallet.unfreeze", map[string]any{}, walletID, actor)
	return nil
}

func (ws *Wallets) Retire(walletID, actor string) error {
	w, err := ws.Get(walletID)
	if err != nil {
		return err
	}
	if err := ws.Store.SetRetired(walletID); err != nil {
		return err
	}
	for _, n := range []string{w.NodeWallet, w.KeyWallet} {
		if n != "" {
			_, _ = ws.RPC.Call("unloadwallet", n)
		}
	}
	ws.Store.Audit("wallet.retire", map[string]any{}, walletID, actor)
	return nil
}

// Backup verifies and archives every node wallet behind the agent wallet (owner-side; never exposed to agents).
func (ws *Wallets) Backup(walletID, actor string) ([]string, error) {
	w, err := ws.Get(walletID)
	if err != nil {
		return nil, err
	}
	if err := os.MkdirAll(ws.Cfg.BackupDir(), 0o700); err != nil {
		return nil, err
	}
	archivePW, err := ws.Keyring.Ensure("archive_passphrase", 32)
	if err != nil {
		return nil, err
	}
	var out []string
	for _, name := range []string{w.NodeWallet, w.KeyWallet} {
		if name == "" {
			continue
		}
		wr := ws.RPC.Wallet(name)
		if _, err := wr.Call("z_verifywalletintegrity"); err != nil && !errs.IsRPC(err, -32601) {
			return nil, err
		}
		dest := filepath.Join(ws.Cfg.BackupDir(), fmt.Sprintf("%s-%d.qtcarchive", name, time.Now().Unix()))
		args := []any{dest, archivePW}
		if wpw, ok := ws.Keyring.Get("wallet:" + name); ok {
			args = append(args, wpw)
		}
		if _, err := wr.Call("backupwalletbundlearchive", args...); err != nil {
			if !errs.IsRPC(err, -32601) {
				return nil, err
			}
			if _, err := wr.Call("backupwallet", dest); err != nil {
				return nil, err
			}
		}
		out = append(out, dest)
	}
	ws.Store.Audit("wallet.backup", map[string]any{"archives": out}, walletID, actor)
	return out, nil
}

// Rotate: Tier 1 gets a fresh receive address; Tier 2 rotation needs a new owner-approved tree.
func (ws *Wallets) Rotate(walletID, actor string) (string, error) {
	w, err := ws.Get(walletID)
	if err != nil {
		return "", err
	}
	if w.Tier != 1 {
		return "", errs.Bad("not_supported", "tier 2 rotation requires a new owner-approved tree; create a new wallet and sweep")
	}
	addr, err := ws.RPC.Wallet(w.NodeWallet).String("getnewaddress", "agent-receive", "p2mr")
	if err != nil {
		return "", err
	}
	if err := ws.Store.SetReceiveAddress(walletID, addr); err != nil {
		return "", err
	}
	ws.Store.Audit("wallet.rotate", map[string]any{"receive_address": addr}, walletID, actor)
	return addr, nil
}
