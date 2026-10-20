// Package e2e runs the spec §11 scenarios against a throwaway regtest qtcd and a throwaway PostgreSQL
// cluster, with the service and the Tier 2 co-signer in-process (the co-signer over loopback HTTP).
package e2e

import (
	"encoding/json"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"testing"
	"time"

	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/config"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/cosigner"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/jsonx"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/policy"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/rpc"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/service"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/store"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/testpg"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/wallet"
)

const Atoms = wallet.Atoms

func binDir() string {
	if b := os.Getenv("QTC_BIN"); b != "" {
		return b
	}
	here, _ := os.Getwd()
	for _, cand := range []string{filepath.Join(here, "..", "..", "..", "build", "bin"), filepath.Join(os.Getenv("HOME"), "Documents", "qtc", "qtc", "build", "bin")} {
		if _, err := os.Stat(filepath.Join(cand, "qtcd")); err == nil {
			return cand
		}
	}
	return filepath.Join(here, "..", "..", "..", "build", "bin")
}

func freePort() int {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	defer l.Close()
	return l.Addr().(*net.TCPAddr).Port
}

// Node is a regtest qtcd.
type Node struct {
	Dir     string
	RPCPort int
	URL     string
	Cookie  string
	RPC     *rpc.Client
	cmd     *exec.Cmd
}

func StartNode(t testing.TB, dir string, extra ...string) *Node {
	n := &Node{Dir: dir, RPCPort: freePort()}
	n.URL = fmt.Sprintf("http://127.0.0.1:%d", n.RPCPort)
	n.Cookie = filepath.Join(dir, "regtest", ".cookie")
	args := append([]string{"-regtest", "-datadir=" + dir, "-rpcport=" + strconv.Itoa(n.RPCPort), "-port=" + strconv.Itoa(freePort()),
		"-fallbackfee=0.0001", "-listen=0", "-printtoconsole=0", "-server=1"}, extra...)
	if err := os.MkdirAll(dir, 0o700); err != nil { // qtcd refuses a missing datadir
		t.Fatal(err)
	}
	n.cmd = exec.Command(filepath.Join(binDir(), "qtcd"), args...)
	if f, err := os.Create(filepath.Join(dir, "qtcd.stderr")); err == nil {
		n.cmd.Stderr = f
	}
	if err := n.cmd.Start(); err != nil {
		t.Fatalf("qtcd: %v", err)
	}
	n.RPC = rpc.New(n.URL, n.Cookie, "", "")
	for i := 0; i < 240; i++ {
		if _, err := os.Stat(n.Cookie); err == nil {
			if _, err := n.RPC.Int("getblockcount"); err == nil {
				return n
			}
		}
		time.Sleep(250 * time.Millisecond)
	}
	logTail, _ := os.ReadFile(filepath.Join(dir, "regtest", "debug.log"))
	stderr, _ := os.ReadFile(filepath.Join(dir, "qtcd.stderr"))
	logTail = append(stderr, logTail...)
	if len(logTail) > 2000 {
		logTail = logTail[len(logTail)-2000:]
	}
	t.Fatalf("qtcd did not start (bin %s, cookie %s)\n%s", filepath.Join(binDir(), "qtcd"), n.Cookie, logTail)
	return nil
}

func (n *Node) Mine(t testing.TB, count int, addr string) {
	if _, err := n.RPC.Call("generatetoaddress", count, addr); err != nil {
		t.Fatalf("mine: %v", err)
	}
}

func (n *Node) Stop() {
	_, _ = n.RPC.Call("stop")
	done := make(chan struct{})
	go func() { _ = n.cmd.Wait(); close(done) }()
	select {
	case <-done:
	case <-time.After(60 * time.Second):
		_ = n.cmd.Process.Kill()
	}
}

// Env is the shared fixture: node, postgres, funder wallet, service, co-signer.
type Env struct {
	Node    *Node
	PG      *testpg.Cluster
	Funder  *rpc.Client
	FunderA string
	Svc     *service.Service
	Cfg     *config.Config
	Owner   service.Auth
	Agent   service.Auth
	csSrv   interface{ Close() error }
}

var env *Env

func TestMain(m *testing.M) {
	code := func() int {
		dir, err := os.MkdirTemp("", "aws-e2e-")
		if err != nil {
			panic(err)
		}
		defer os.RemoveAll(dir)
		pg, err := testpg.Start(filepath.Join(dir, "pg"))
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		defer pg.Stop()
		t := &fatalT{}
		node := StartNode(t, filepath.Join(dir, "node"))
		defer node.Stop()
		if _, err := node.RPC.Call("createwallet", "funder"); err != nil {
			panic(err)
		}
		funder := node.RPC.Wallet("funder")
		faddr, _ := funder.String("getnewaddress")
		node.Mine(t, 120, faddr)
		data := filepath.Join(dir, "aws")
		csPort := freePort()
		cfg := config.Default(data)
		cfg.PgDSN, cfg.RPCURL, cfg.CookiePath, cfg.ConfirmationDepth = pg.DSN, node.URL, node.Cookie, 2
		cfg.CosignerURL, cfg.CosignerListen = fmt.Sprintf("http://127.0.0.1:%d", csPort), fmt.Sprintf("127.0.0.1:%d", csPort)
		svc, err := service.New(cfg)
		if err != nil {
			panic(err)
		}
		// co-signer in-process with its own keyring/store schema, sharing only the two secrets it needs
		csDir := filepath.Join(data, "cosigner")
		_ = os.MkdirAll(csDir, 0o700)
		kr := config.NewKeyring(filepath.Join(csDir, "keyring.json"))
		for _, n := range []string{"decision_secret", "admin_secret"} {
			v, _ := svc.Keyring.Require(n)
			_ = kr.Set(n, v)
		}
		csStore, err := store.Open(pg.DSN, "cosigner")
		if err != nil {
			panic(err)
		}
		admin, _ := kr.Require("admin_secret")
		cs := &cosigner.Service{Cfg: cfg, RPC: rpc.New(node.URL, node.Cookie, "", ""), Keyring: kr, Store: csStore,
			Policy: policy.NewEngine(cfg.PolicyDir(), admin, csStore), WalletLookup: svc.Store.Wallet}
		csSrv, err := cosigner.Serve(cs, cfg.CosignerListen)
		if err != nil {
			panic(err)
		}
		defer csSrv.Close()
		env = &Env{Node: node, PG: pg, Funder: funder, FunderA: faddr, Svc: svc, Cfg: cfg,
			Owner: service.NewAuth("owner", "wallet:admin"),
			Agent: service.NewAuth("agent", "wallet:read", "wallet:spend", "market:read", "trade:quote", "trade:read"), csSrv: csSrv}
		return m.Run()
	}()
	os.Exit(code)
}

type fatalT struct{ testing.TB }

func (f *fatalT) Fatalf(format string, a ...any) { panic(fmt.Sprintf(format, a...)) }
func (f *fatalT) Fatal(a ...any)                 { panic(fmt.Sprint(a...)) }

func (e *Env) NewAddr(t testing.TB) string {
	a, err := e.Funder.String("getnewaddress")
	if err != nil {
		t.Fatal(err)
	}
	return a
}

func (e *Env) Fund(t testing.TB, addr string, qtc float64) string {
	txid, err := e.Funder.String("sendtoaddress", addr, qtc)
	if err != nil {
		t.Fatalf("fund: %v", err)
	}
	e.Node.Mine(t, 1, e.FunderA)
	return txid
}

// PolicyFor builds a policy allowing the destinations with limit overrides (limits.* keys).
func PolicyFor(agentID string, allow []string, limits map[string]any) map[string]any {
	doc := policy.Default()
	doc["agent_id"] = agentID
	al := make([]any, len(allow))
	for i, a := range allow {
		al[i] = a
	}
	doc["destinations"].(map[string]any)["allow"] = al
	for k, v := range limits {
		doc["limits"].(map[string]any)[k] = v
	}
	return doc
}

func setApproval(doc map[string]any, above int64) {
	doc["approval"].(map[string]any)["require_above_atoms"] = above
}

func i64p(v int64) *int64 { return &v }

func mustJSON(v any) string { b, _ := json.Marshal(v); return string(b) }

var _ = jsonx.Canonical
