package e2e

import (
	"sync"
	"testing"
	"time"

	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/jsonx"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/policy"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/service"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/spend"
)

// Finding 1: concurrent sends must not exceed the daily cap; finding 3: the audit chain must stay linear.
func TestDailyLimitHoldsUnderConcurrency(t *testing.T) {
	dest := env.NewAddr(t)
	pol := PolicyFor("race", []string{dest}, map[string]any{"per_tx_atoms": 1 * Atoms, "per_day_atoms": 5 * Atoms, "per_day_tx_count": 100})
	setApproval(pol, 100*Atoms)
	pol["kill_switch"].(map[string]any)["freeze_on_policy_denials_per_hour"] = 1000
	w, err := env.Svc.WalletCreate(env.Owner, "race", 1, nil, pol, 0)
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i < 20; i++ { // twenty 1 QTC UTXOs so UTXO contention does not mask the policy
		if _, err := env.Funder.String("sendtoaddress", w.ReceiveAddress, 1.0); err != nil {
			t.Fatal(err)
		}
	}
	env.Node.Mine(t, 1, env.FunderA)
	var wg sync.WaitGroup
	var mu sync.Mutex
	broadcast, denied := 0, map[string]int{}
	for i := 0; i < 20; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			r, err := env.Svc.WalletSend(env.Agent, w.WalletID, dest, i64p(90_000_000), "burst", nil, false)
			mu.Lock()
			defer mu.Unlock()
			if err != nil {
				denied["error:"+err.Error()]++
				return
			}
			if r.Status == "broadcast" {
				broadcast++
			} else {
				denied[r.Reason]++
			}
		}()
	}
	wg.Wait()
	if broadcast != 5 { // 5 x 0.9 = 4.5 QTC fits the 5 QTC cap; a 6th would be 5.4
		t.Fatalf("broadcast %d (want 5); denials %v", broadcast, denied)
	}
	if denied["per_day_limit"] != 15 {
		t.Fatalf("denials %v", denied)
	}
	if spent, _ := env.Svc.Store.Rolling24h(w.WalletID); spent > 5*Atoms {
		t.Fatalf("rolling total %d exceeds the cap", spent)
	}
	if ok, _ := env.Svc.Store.AuditVerify(); !ok {
		t.Fatal("audit chain broke under concurrent writes")
	}
}

// Finding 2: a policy written by another process (the owner CLI) takes effect without a restart.
func TestPolicyChangeFromAnotherProcessTakesEffect(t *testing.T) {
	dest := env.NewAddr(t)
	pol := PolicyFor("live", []string{dest}, nil)
	w, err := env.Svc.WalletCreate(env.Owner, "live", 1, nil, pol, 0)
	if err != nil {
		t.Fatal(err)
	}
	env.Fund(t, w.ReceiveAddress, 2.0)
	if r, err := env.Svc.WalletSend(env.Agent, w.WalletID, dest, i64p(1*Atoms), "before", nil, false); err != nil || r.Status != "broadcast" {
		t.Fatalf("before: %+v %v", r, err)
	}
	time.Sleep(20 * time.Millisecond)
	other := policy.NewEngine(env.Svc.Policy.Dir, env.Svc.Policy.AdminSecret, env.Svc.Store) // a second process
	pol2 := PolicyFor("live", []string{}, nil)                                               // allowlist emptied
	pol2["version"] = 2
	if _, err := other.Write(w.WalletID, pol2); err != nil {
		t.Fatal(err)
	}
	r, err := env.Svc.WalletSend(env.Agent, w.WalletID, dest, i64p(50_000_000), "after", nil, false)
	if err != nil || r.Reason != "destination_not_allowed" {
		t.Fatalf("tightened policy ignored: %+v %v", r, err)
	}
	if d, _ := env.Svc.Policy.Describe(w.WalletID); jsonx.I(d, "version", 0) != 2 {
		t.Fatalf("describe shows stale version %v", d["version"])
	}
}

// Finding 5: inputs locked for an approval are recovered and released after a service restart.
func TestLockedInputsRecoveredAfterRestart(t *testing.T) {
	dest := env.NewAddr(t)
	pol := PolicyFor("lock", []string{dest}, map[string]any{"per_tx_atoms": 10 * Atoms, "per_day_atoms": 50 * Atoms})
	setApproval(pol, 1*Atoms)
	pol["approval"].(map[string]any)["ttl_seconds"] = 1
	w, err := env.Svc.WalletCreate(env.Owner, "lock", 1, nil, pol, 0)
	if err != nil {
		t.Fatal(err)
	}
	env.Fund(t, w.ReceiveAddress, 3.0)
	r, err := env.Svc.WalletSend(env.Agent, w.WalletID, dest, i64p(2*Atoms), "park", nil, false)
	if err != nil || r.Status != "needs_approval" {
		t.Fatalf("%+v %v", r, err)
	}
	locked := func() int {
		l, err := env.Svc.RPC.Wallet(w.WalletID).List("listlockunspent") // Tier 1: node wallet name == wallet id
		if err != nil {
			t.Fatal(err)
		}
		return len(l)
	}
	if locked() == 0 {
		t.Fatal("expected locked inputs")
	}
	time.Sleep(1500 * time.Millisecond) // approval expires while "the service is down"
	again, err := service.New(env.Cfg)  // restart: Recover() rebuilds the lock map and releases expired approvals
	if err != nil {
		t.Fatal(err)
	}
	if n := locked(); n != 0 {
		t.Fatalf("%d inputs still locked after restart", n)
	}
	if st, _ := again.Spend.Status(w.WalletID, r.PSBTSHA256); st.Status != "expired" {
		t.Fatalf("status %+v", st)
	}
	_ = spend.PSBTSHA256
}
