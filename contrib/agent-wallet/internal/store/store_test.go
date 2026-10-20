package store

import (
	"fmt"
	"os"
	"sync"
	"sync/atomic"
	"testing"

	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/testpg"
)

func TestStoreRoundTrip(t *testing.T) {
	dir := t.TempDir()
	pg, err := testpg.Start(dir)
	if err != nil {
		t.Fatal(err)
	}
	defer pg.Stop()
	s, err := Open(pg.DSN, "aws_test", pg.Password)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	if err := s.AddWallet(&Wallet{WalletID: "agent-a-tier1", AgentID: "a", Tier: 1, NodeWallet: "agent-a-tier1", ReceiveAddress: "qtcrt1z"}); err != nil {
		t.Fatal(err)
	}
	if w := s.Wallet("agent-a-tier1"); w == nil || w.Tier != 1 || w.Frozen {
		t.Fatalf("wallet row: %+v", w)
	}
	_ = s.SetFrozen("agent-a-tier1", true, "test")
	if !s.Wallet("agent-a-tier1").Frozen {
		t.Fatal("frozen flag")
	}
	_ = s.AddToken("tok", "l", []string{"wallet:read", "wallet:spend", "wallet:read"})
	if sc := s.TokenScopes("tok"); len(sc) != 2 || !sc["wallet:spend"] {
		t.Fatalf("scopes %v", sc)
	}
	if s.RevokeToken("l") != 1 || s.TokenScopes("tok") != nil {
		t.Fatal("revoke")
	}
	_ = s.AddSpend("agent-a-tier1", "h1", 100, []string{"d"}, "p", "signing")
	_ = s.AddSpend("agent-a-tier1", "h2", 50, []string{"d"}, "p", "denied")
	if sum, n := s.Rolling24h("agent-a-tier1"); sum != 100 || n != 1 {
		t.Fatalf("rolling %d %d", sum, n)
	}
	fee := int64(7)
	_ = s.SetSpendBroadcast("h1", "tx", &fee)
	if sp := s.Spend("h1"); sp.TxID != "tx" || *sp.FeeAtoms != 7 || sp.Destinations[0] != "d" {
		t.Fatalf("spend %+v", sp)
	}
	_ = s.AddApproval("h3", "agent-a-tier1", "cHNidP8=", map[string]any{"outputs": []any{}}, map[string]any{"to": "d"}, 60)
	if len(s.PendingApprovals("")) != 1 || !s.Approve("h3") || s.Approve("zz") {
		t.Fatal("approvals")
	}
	if n := s.AddDenial("agent-a-tier1", "x"); n != 1 {
		t.Fatal("denial count")
	}
	for i := 0; i < 3; i++ {
		s.Audit("k", map[string]any{"i": i, "s": "é<>"}, "agent-a-tier1", "t")
	}
	ok, n := s.AuditVerify()
	if !ok || n != 3 {
		t.Fatalf("audit %v %d", ok, n)
	}
	if _, err := s.exec("UPDATE audit SET payload='{}' WHERE seq=2"); err != nil {
		t.Fatal(err)
	}
	if ok, n := s.AuditVerify(); ok || n != 1 {
		t.Fatalf("tamper not detected: %v %d", ok, n)
	}
	_ = os.Remove
}

func TestAuditChainConcurrencyAndTruncation(t *testing.T) {
	pg, err := testpg.Start(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer pg.Stop()
	s, err := Open(pg.DSN, "aws_audit", pg.Password)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	s.SetAuditKey("k1")
	var wg sync.WaitGroup
	for i := 0; i < 40; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			s.Audit("burst", map[string]any{"i": i}, "w", "t")
		}(i)
	}
	wg.Wait()
	if ok, n := s.AuditVerify(); !ok || n != 40 {
		t.Fatalf("concurrent appends forked the chain: ok=%v n=%d", ok, n)
	}
	// a second store handle with the same key verifies; a different key does not (DB-only attacker cannot forge)
	other, _ := Open(pg.DSN, "aws_audit", pg.Password)
	other.SetAuditKey("k1")
	if ok, _ := other.AuditVerify(); !ok {
		t.Fatal("same key must verify")
	}
	other.SetAuditKey("wrong")
	if ok, _ := other.AuditVerify(); ok {
		t.Fatal("wrong key must not verify")
	}
	// truncating the tail is detected through the persisted tip
	if _, err := s.exec("DELETE FROM audit WHERE seq=(SELECT MAX(seq) FROM audit)"); err != nil {
		t.Fatal(err)
	}
	if ok, _ := s.AuditVerify(); ok {
		t.Fatal("truncation not detected")
	}
}

// Two store handles on the same schema (two processes: daemon + owner CLI) appending concurrently.
func TestAuditChainAcrossHandles(t *testing.T) {
	pg, err := testpg.Start(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer pg.Stop()
	a, _ := Open(pg.DSN, "aws_multi", pg.Password)
	b, _ := Open(pg.DSN, "aws_multi", pg.Password)
	a.SetAuditKey("k")
	b.SetAuditKey("k")
	var wg sync.WaitGroup
	for i := 0; i < 20; i++ {
		wg.Add(2)
		go func(i int) { defer wg.Done(); a.Audit("a", map[string]any{"i": i}, "w", "daemon") }(i)
		go func(i int) { defer wg.Done(); b.Audit("b", map[string]any{"i": i}, "", "owner") }(i)
	}
	wg.Wait()
	if ok, n, why := a.AuditVerifyDetail(); !ok || n != 40 {
		t.Fatalf("ok=%v n=%d why=%s", ok, n, why)
	}
	if ok, _, why := b.AuditVerifyDetail(); !ok {
		t.Fatal(why)
	}
}

func TestAddSpendCheckedEnforcesDailyLimit(t *testing.T) {
	pg, err := testpg.Start(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	defer pg.Stop()
	s, err := Open(pg.DSN, "aws_limit", pg.Password)
	if err != nil {
		t.Fatal(err)
	}
	defer s.Close()
	var wg sync.WaitGroup
	var okCount atomic.Int64
	for i := 0; i < 30; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			if err := s.AddSpendChecked("w", fmt.Sprintf("h%d", i), 90, []string{"d"}, "p", "signing", 1000, 100); err == nil {
				okCount.Add(1)
			} else if err != ErrLimit {
				t.Error(err)
			}
		}(i)
	}
	wg.Wait()
	if okCount.Load() != 11 { // 11*90 = 990 <= 1000; the 12th would be 1080
		t.Fatalf("expected exactly 11 spends within the cap, got %d", okCount.Load())
	}
}
