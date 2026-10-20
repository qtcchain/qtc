package e2e

import (
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"

	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/config"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/errs"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/rpc"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/service"
)

func whitelistLine(t *testing.T) string {
	raw, err := os.ReadFile(filepath.Join("..", "rpcwhitelist.conf"))
	if err != nil {
		t.Fatal(err)
	}
	for _, l := range strings.Split(string(raw), "\n") {
		if strings.HasPrefix(l, "rpcwhitelist=") {
			return strings.TrimPrefix(l, "rpcwhitelist=")
		}
	}
	t.Fatal("no rpcwhitelist line")
	return ""
}

func allowedSet(line string) map[string]bool {
	out := map[string]bool{}
	for _, m := range strings.Split(strings.SplitN(line, ":", 2)[1], ",") {
		out[m] = true
	}
	return out
}

func rpcauth(t *testing.T, user, pw string) string {
	script := filepath.Join(binDir(), "..", "..", "share", "rpcauth", "rpcauth.py")
	if _, err := os.Stat(script); err != nil {
		script = filepath.Join(os.Getenv("HOME"), "Documents", "qtc", "qtc", "share", "rpcauth", "rpcauth.py")
	}
	outb, err := exec.Command("python3", "-I", script, user, pw).Output()
	if err != nil {
		t.Fatalf("rpcauth.py: %v", err)
	}
	m := regexp.MustCompile(`rpcauth=(\S+)`).FindStringSubmatch(string(outb))
	if m == nil {
		t.Fatal("rpcauth output")
	}
	return m[1]
}

func TestRPCWhitelist(t *testing.T) {
	line := whitelistLine(t)
	allowed := allowedSet(line)
	dir := t.TempDir()
	pw := "test-only-password" // regtest, in-process, never reused
	node := StartNode(t, filepath.Join(dir, "node"), "-rpcauth="+rpcauth(t, "aws", pw), "-rpcwhitelistdefault=0", "-rpcwhitelist="+line, "-txindex=1")
	defer node.Stop()
	awsRPC := rpc.New(node.URL, "", "aws", pw)

	t.Run("denies private material", func(t *testing.T) {
		for _, m := range []string{"dumpprivkey", "exportwalletbundle", "gethdkeys", "stop", "generatetoaddress", "sethdseed", "importprivkey", "help", "listdescriptors"} {
			if allowed[m] {
				t.Fatalf("%s must not be whitelisted", m)
			}
			_, err := awsRPC.Call(m)
			if !errs.IsRPC(err, -403) {
				t.Fatalf("%s: expected whitelist denial, got %v", m, err)
			}
		}
		if _, err := awsRPC.Int("getblockcount"); err != nil {
			t.Fatalf("whitelisted call failed: %v", err)
		}
	})

	t.Run("covers every method the service calls", func(t *testing.T) {
		help, err := node.RPC.String("help") // operator identity lists the real RPC names
		if err != nil {
			t.Fatal(err)
		}
		known := map[string]bool{}
		for _, l := range strings.Split(help, "\n") {
			if f := strings.Fields(l); len(f) > 0 && regexp.MustCompile(`^[a-z_]+$`).MatchString(f[0]) {
				known[f[0]] = true
			}
		}
		var src strings.Builder
		_ = filepath.Walk(filepath.Join("..", "internal"), func(p string, info os.FileInfo, err error) error {
			if err == nil && strings.HasSuffix(p, ".go") && !strings.HasSuffix(p, "_test.go") {
				b, _ := os.ReadFile(p)
				src.Write(b)
			}
			return nil
		})
		var missing []string
		for _, m := range regexp.MustCompile(`\("([a-z_]+)"`).FindAllStringSubmatch(src.String(), -1) {
			if known[m[1]] && !allowed[m[1]] {
				missing = append(missing, m[1])
			}
		}
		if len(missing) > 0 {
			t.Fatalf("service calls RPCs the whitelist denies: %v", missing)
		}
	})

	t.Run("tier 1 lifecycle under the whitelist", func(t *testing.T) {
		if _, err := node.RPC.Call("createwallet", "funder-wl"); err != nil {
			t.Fatal(err)
		}
		f := node.RPC.Wallet("funder-wl")
		faddr, _ := f.String("getnewaddress")
		node.Mine(t, 110, faddr)
		cfg := config.Default(filepath.Join(dir, "aws"))
		cfg.PgDSN, cfg.RPCURL, cfg.RPCUser, cfg.RPCPassword, cfg.CosignerURL, cfg.ConfirmationDepth = env.PG.DSN, node.URL, "aws", pw, "", 1
		// separate schema so this service does not share rows with the main fixture
		cfg.Extra = map[string]any{}
		_ = os.MkdirAll(cfg.DataDir, 0o700)
		_ = config.NewKeyring(cfg.KeyringPath()).Set("pg_password", env.PG.Password)
		svc, err := newServiceWithSchema(cfg, "aws_wl")
		if err != nil {
			t.Fatal(err)
		}
		owner := service.NewAuth("owner", "wallet:admin")
		agent := service.NewAuth("agent", "wallet:read", "wallet:spend", "market:read")
		dest, _ := f.String("getnewaddress")
		w, err := svc.WalletCreate(owner, "wl", 1, nil, PolicyFor("wl", []string{dest}, map[string]any{"per_tx_atoms": 2 * Atoms, "per_day_atoms": 10 * Atoms}), 0)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := f.String("sendtoaddress", w.ReceiveAddress, 3.0); err != nil {
			t.Fatal(err)
		}
		node.Mine(t, 1, faddr)
		if b, _ := svc.WalletBalance(agent, w.WalletID); b.ConfirmedAtoms != 3*Atoms {
			t.Fatalf("balance %+v", b)
		}
		r, err := svc.WalletSend(agent, w.WalletID, dest, i64p(1*Atoms), "whitelisted spend", nil, false)
		if err != nil || r.Status != "broadcast" {
			t.Fatalf("send %+v %v", r, err)
		}
		node.Mine(t, 1, faddr)
		if st, _ := svc.WalletSendStatus(agent, w.WalletID, r.PSBTSHA256); st.Status != "confirmed" {
			t.Fatalf("status %+v", st)
		}
		if ok, n := svc.Store.AuditVerify(); !ok || n == 0 {
			t.Fatal("audit")
		}
	})
}
