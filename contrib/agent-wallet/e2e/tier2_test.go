package e2e

import (
	"os"
	"strings"
	"testing"

	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/cosigner"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/jsonx"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/spend"
)

func ownerKeys(t *testing.T) []string {
	if _, err := env.Node.RPC.Call("createwallet", "owner-cold"); err != nil && !strings.Contains(err.Error(), "already") {
		t.Fatal(err)
	}
	w := env.Node.RPC.Wallet("owner-cold")
	var keys []string
	for i := 0; i < 2; i++ {
		a, _ := w.String("getnewaddress")
		k, err := w.Map("exportpqkey", a)
		if err != nil {
			t.Fatal(err)
		}
		keys = append(keys, jsonx.S(k, "key"))
	}
	return keys
}

func TestTier2Cosign(t *testing.T) {
	dest := env.NewAddr(t)
	pol := PolicyFor("t2", []string{dest}, map[string]any{"per_tx_atoms": 5 * Atoms, "per_day_atoms": 50 * Atoms})
	pol["tier"] = 2
	setApproval(pol, 100*Atoms)
	w, err := env.Svc.WalletCreate(env.Owner, "t2", 2, ownerKeys(t), pol, 5)
	if err != nil {
		t.Fatal(err)
	}
	if w.Tier != 2 || !strings.HasPrefix(w.Descriptor, "mr(multi_pq(2,") || strings.Contains(w.Descriptor, "{") {
		t.Fatalf("descriptor %s", w.Descriptor)
	}
	env.Fund(t, w.ReceiveAddress, 4.0)
	if b, _ := env.Svc.WalletBalance(env.Agent, w.WalletID); b.ConfirmedAtoms != 4*Atoms {
		t.Fatalf("balance %+v", b)
	}
	// the agent key alone cannot finalize: the only leaf is the 2-of-2
	saved := env.Svc.Spend.Cosigner
	env.Svc.Spend.Cosigner = nil
	_, err = env.Svc.WalletSend(env.Agent, w.WalletID, dest, i64p(1*Atoms), "no cosigner", nil, false)
	env.Svc.Spend.Cosigner = saved
	if code(err) != "cosigner_unavailable" {
		t.Fatalf("expected cosigner_unavailable, got %v", err)
	}
	r, err := env.Svc.WalletSend(env.Agent, w.WalletID, dest, i64p(1*Atoms), "with cosigner", nil, false)
	if err != nil || r.Status != "broadcast" {
		t.Fatalf("cosigned send %+v %v", r, err)
	}
	env.Node.Mine(t, 1, env.FunderA)
	if b, _ := env.Svc.WalletBalance(env.Agent, w.WalletID); b.ConfirmedAtoms >= 3*Atoms {
		t.Fatalf("balance after spend %+v", b) // 4 - 1 - fee (+ change back to the tree)
	}
}

func TestCosignerRejectsBadEnvelope(t *testing.T) {
	w := env.Svc.Store.Wallet("agent-t2-tier2")
	if w == nil {
		t.Skip("tier2 wallet missing")
	}
	cli := cosigner.NewClient(env.Cfg.CosignerURL, env.Svc.Keyring)
	dest := env.NewAddr(t)
	res, err := env.Svc.RPC.Wallet(w.NodeWallet).Map("walletcreatefundedpsbt", []any{}, []any{map[string]any{dest: 0.5}}, 0,
		map[string]any{"fee_rate": 2, "include_watching": true, "changeAddress": w.ReceiveAddress})
	if err != nil {
		t.Fatal(err)
	}
	psbt := jsonx.S(res, "psbt")
	// wrong secret -> rejected
	bad := spend.MakeEnvelope("not-the-secret", strings.Repeat("00", 32), 1, 50_000_000, []string{dest}, 600)
	if _, err := cli.Sign("t2", psbt, bad); err == nil {
		t.Fatal("bad envelope accepted")
	}
	// right secret but destination not on the allowlist -> rejected by the co-signer's own policy copy
	secret, _ := env.Svc.Keyring.Require("decision_secret")
	env2 := spend.MakeEnvelope(secret, spend.PSBTSHA256(psbt), env.Svc.Policy.Version(w.WalletID), 50_000_000, []string{dest}, 600)
	if _, err := cli.Sign("t2", psbt, env2); err == nil {
		t.Fatal("off-policy destination signed")
	}
}

func TestTier2BackupsCoverBothKeyWallets(t *testing.T) {
	w := env.Svc.Store.Wallet("agent-t2-tier2")
	if w == nil {
		t.Skip("tier2 wallet missing")
	}
	entries, _ := os.ReadDir(env.Cfg.BackupDir())
	node, key := false, false
	for _, e := range entries {
		node = node || strings.HasPrefix(e.Name(), w.NodeWallet)
		key = key || strings.HasPrefix(e.Name(), w.KeyWallet)
	}
	if !node || !key {
		t.Fatal("backups missing for a key wallet")
	}
	for _, e := range env.Svc.Store.AuditExport(0, 10000) {
		if e.Kind == "wallet.create" && e.WalletID == w.WalletID {
			if !strings.HasPrefix(jsonx.S(e.Payload, "planned_recovery_tree"), "mr(multi_pq(2,") {
				t.Fatalf("planned tree: %v", e.Payload["planned_recovery_tree"])
			}
			return
		}
	}
	t.Fatal("wallet.create audit entry missing")
}
