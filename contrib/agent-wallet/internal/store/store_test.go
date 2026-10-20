package store

import (
	"os"
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
	s, err := Open(pg.DSN, "aws_test")
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
