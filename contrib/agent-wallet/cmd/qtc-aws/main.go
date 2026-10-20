// qtc-aws: owner CLI and daemons for the QTC Agent Wallet Service.
//
//	qtc-aws --config aws.json init --data-dir D --pg-dsn DSN [--rpc-url U] [--cookie P | --rpc-user aws] ...
//	qtc-aws --config aws.json token issue --label L --scopes s1,s2 | token revoke --label L | token list
//	qtc-aws --config aws.json wallet create --agent-id ID [--tier 2 --owner-mldsa HEX --owner-mldsa HEX --cooldown N --policy FILE]
//	qtc-aws --config aws.json wallet list | balance | backup | unfreeze | retire [--sweep-to ADDR] --wallet-id W
//	qtc-aws --config aws.json policy show --wallet-id W | policy set --wallet-id W --file policy.json | policy template
//	qtc-aws --config aws.json approvals list | approvals approve --psbt-sha256 H
//	qtc-aws --config aws.json audit export [--since N --limit N] | audit verify
//	qtc-aws --config aws.json serve-rest | serve-cosigner | mcp --token T | status
package main

import (
	"bufio"
	"context"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/config"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/cosigner"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/jsonx"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/mcpserver"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/policy"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/rest"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/rpc"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/service"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/store"
)

func die(format string, a ...any) {
	fmt.Fprintf(os.Stderr, format+"\n", a...)
	os.Exit(1)
}

func out(v any) { fmt.Print(jsonx.Pretty(v)) }

func svcFor(cfgPath string) (*service.Service, *config.Config) {
	cfg, err := config.Load(cfgPath)
	if err != nil {
		die("%v", err)
	}
	s, err := service.New(cfg)
	if err != nil {
		die("%v", err)
	}
	return s, cfg
}

type multi []string

func (m *multi) String() string     { return strings.Join(*m, ",") }
func (m *multi) Set(v string) error { *m = append(*m, v); return nil }

func main() {
	root := flag.NewFlagSet("qtc-aws", flag.ExitOnError)
	cfgPath := root.String("config", "aws.json", "service config file")
	root.Usage = func() {
		fmt.Fprintln(os.Stderr, "usage: qtc-aws [--config aws.json] <init|token|wallet|policy|approvals|audit|serve-rest|serve-cosigner|mcp|status> ...")
	}
	// allow --config before the subcommand
	args := os.Args[1:]
	if len(args) > 0 && strings.HasPrefix(args[0], "-") {
		_ = root.Parse(args)
		args = root.Args()
	}
	if len(args) == 0 {
		root.Usage()
		os.Exit(2)
	}
	cmd, rest := args[0], args[1:]
	switch cmd {
	case "init":
		cmdInit(*cfgPath, rest)
	case "token":
		cmdToken(*cfgPath, rest)
	case "wallet":
		cmdWallet(*cfgPath, rest)
	case "policy":
		cmdPolicy(*cfgPath, rest)
	case "approvals":
		cmdApprovals(*cfgPath, rest)
	case "audit":
		cmdAudit(*cfgPath, rest)
	case "serve-rest":
		cmdServeRest(*cfgPath)
	case "serve-cosigner":
		cmdServeCosigner(*cfgPath)
	case "mcp":
		cmdMCP(*cfgPath, rest)
	case "status":
		s, _ := svcFor(*cfgPath)
		ids := []string{}
		for _, w := range s.Store.Wallets() {
			ids = append(ids, w.WalletID)
		}
		ok, n := s.Store.AuditVerify()
		out(map[string]any{"node": s.NodeStatus(), "wallets": ids, "pending_approvals": len(s.Store.PendingApprovals("")),
			"audit": map[string]any{"ok": ok, "entries": n}})
	default:
		root.Usage()
		os.Exit(2)
	}
}

func cmdInit(cfgPath string, args []string) {
	fs := flag.NewFlagSet("init", flag.ExitOnError)
	dataDir := fs.String("data-dir", "", "service data directory (keyring, policies, backups)")
	pgDSN := fs.String("pg-dsn", "", "PostgreSQL connection string, e.g. postgres://aws@127.0.0.1:5432/qtc_aws?sslmode=disable")
	rpcURL := fs.String("rpc-url", "http://127.0.0.1:28790", "node RPC URL")
	cookie := fs.String("cookie", "", "node .cookie path (operator identity); prefer --rpc-user")
	rpcUser := fs.String("rpc-user", "", "rpcauth user for the node; its password is read from stdin (one line) into the keyring")
	pgPwStdin := fs.Bool("pg-password-stdin", false, "read the PostgreSQL password from stdin (the line after the rpc password) into the keyring")
	chain := fs.String("chain", "regtest", "chain name")
	listen := fs.String("listen", "127.0.0.1:8792", "REST listen address (loopback)")
	cosignerListen := fs.String("cosigner-listen", "127.0.0.1:8793", "co-signer listen address (loopback)")
	explorer := fs.String("explorer-url", "", "optional public explorer for fee cross-checks")
	_ = fs.Parse(args)
	if *dataDir == "" || *pgDSN == "" {
		die("init: --data-dir and --pg-dsn are required")
	}
	cfg := config.Default(*dataDir)
	cfg.PgDSN, cfg.RPCURL, cfg.CookiePath, cfg.Chain, cfg.Listen, cfg.CosignerListen, cfg.ExplorerURL = *pgDSN, *rpcURL, *cookie, *chain, *listen, *cosignerListen, *explorer
	cfg.CosignerURL = "http://" + *cosignerListen
	if err := os.MkdirAll(cfg.DataDir, 0o700); err != nil {
		die("%v", err)
	}
	kr := config.NewKeyring(cfg.KeyringPath())
	for _, n := range []string{"admin_secret", "decision_secret", "archive_passphrase"} {
		if _, err := kr.Ensure(n, 32); err != nil {
			die("%v", err)
		}
	}
	stdin := bufio.NewReader(os.Stdin)
	if *rpcUser != "" {
		// rpcauth identity (spec §9): the password lives in the 0600 keyring, never in argv or the config
		cfg.RPCUser = *rpcUser
		line, _ := stdin.ReadString('\n')
		pw := strings.TrimRight(line, "\r\n")
		if pw == "" {
			die("init: --rpc-user needs the password on stdin")
		}
		if err := kr.Set("rpc_password", pw); err != nil {
			die("%v", err)
		}
	}
	if *pgPwStdin {
		line, _ := stdin.ReadString('\n')
		pw := strings.TrimRight(line, "\r\n")
		if pw == "" {
			die("init: --pg-password-stdin needs the password on stdin")
		}
		if err := kr.Set("pg_password", pw); err != nil {
			die("%v", err)
		}
	}
	if err := cfg.Save(cfgPath); err != nil {
		die("%v", err)
	}
	fmt.Printf("initialised %s; config written to %s; keyring has %d secrets (0600)\n", cfg.DataDir, cfgPath, len(kr.Names()))
}

func cmdToken(cfgPath string, args []string) {
	if len(args) == 0 {
		die("token: issue|revoke|list")
	}
	fs := flag.NewFlagSet("token", flag.ExitOnError)
	label := fs.String("label", "", "token label")
	scopes := fs.String("scopes", "wallet:read,wallet:spend,market:read", "comma-separated scopes")
	_ = fs.Parse(args[1:])
	s, _ := svcFor(cfgPath)
	switch args[0] {
	case "issue":
		if *label == "" {
			die("token issue: --label required")
		}
		var sc []string
		for _, x := range strings.Split(*scopes, ",") {
			if x = strings.TrimSpace(x); x != "" {
				sc = append(sc, x)
			}
		}
		tok, err := s.IssueToken(*label, sc)
		if err != nil {
			die("%v", err)
		}
		fmt.Println(tok) // printed once, never stored in clear
	case "revoke":
		fmt.Printf("revoked %d token(s)\n", s.Store.RevokeToken(*label))
	default:
		out(s.Store.Tokens())
	}
}

func cmdWallet(cfgPath string, args []string) {
	if len(args) == 0 {
		die("wallet: create|list|balance|backup|unfreeze|retire")
	}
	fs := flag.NewFlagSet("wallet", flag.ExitOnError)
	agentID := fs.String("agent-id", "", "agent id")
	tier := fs.Int("tier", 1, "1 or 2")
	var owners multi
	fs.Var(&owners, "owner-mldsa", "owner ML-DSA-44 public key hex (repeat twice for Tier 2)")
	cooldown := fs.Int64("cooldown", 1008, "owner recovery cooldown in blocks (planned tree)")
	polFile := fs.String("policy", "", "policy JSON file (defaults to the template)")
	walletID := fs.String("wallet-id", "", "wallet id")
	sweepTo := fs.String("sweep-to", "", "sweep destination (must be allowlisted)")
	_ = fs.Parse(args[1:])
	s, _ := svcFor(cfgPath)
	switch args[0] {
	case "create":
		var pol map[string]any
		if *polFile != "" {
			raw, err := os.ReadFile(*polFile)
			if err != nil {
				die("%v", err)
			}
			if pol, err = policy.ParseDocument(raw); err != nil {
				die("policy: %v", err)
			}
		}
		r, err := s.WalletCreate(service.Owner, *agentID, *tier, owners, pol, *cooldown)
		if err != nil {
			die("%v", err)
		}
		out(r)
	case "list":
		type row struct {
			WalletID, AgentID, NodeWallet, KeyWallet, ReceiveAddress, FrozenReason string
			Tier                                                                   int
			Frozen, Retired                                                        bool
		}
		var rows []map[string]any
		for _, w := range s.Store.Wallets() {
			rows = append(rows, map[string]any{"wallet_id": w.WalletID, "agent_id": w.AgentID, "tier": w.Tier, "node_wallet": w.NodeWallet,
				"key_wallet": w.KeyWallet, "receive_address": w.ReceiveAddress, "frozen": w.Frozen, "frozen_reason": w.FrozenReason, "retired": w.Retired})
		}
		if rows == nil {
			rows = []map[string]any{}
		}
		out(rows)
	case "balance":
		b, err := s.WalletBalance(service.Owner, *walletID)
		if err != nil {
			die("%v", err)
		}
		out(b)
	case "backup":
		a, err := s.Wallets.Backup(*walletID, "owner")
		if err != nil {
			die("%v", err)
		}
		out(map[string]any{"archives": a})
	case "unfreeze":
		if err := s.OwnerUnfreeze(*walletID); err != nil {
			die("%v", err)
		}
		fmt.Println("unfrozen")
	case "retire":
		r, err := s.OwnerRetire(*walletID, *sweepTo)
		if err != nil {
			die("%v", err)
		}
		out(r)
	default:
		die("wallet: unknown action %s", args[0])
	}
}

func cmdPolicy(cfgPath string, args []string) {
	if len(args) == 0 {
		die("policy: show|set|template")
	}
	fs := flag.NewFlagSet("policy", flag.ExitOnError)
	walletID := fs.String("wallet-id", "", "wallet id")
	file := fs.String("file", "", "policy JSON file")
	_ = fs.Parse(args[1:])
	if args[0] == "template" {
		out(policy.Default())
		return
	}
	s, _ := svcFor(cfgPath)
	switch args[0] {
	case "show":
		d, err := s.Policy.Describe(*walletID)
		if err != nil {
			die("%v", err)
		}
		out(d)
	case "set":
		raw, err := os.ReadFile(*file)
		if err != nil {
			die("%v", err)
		}
		doc, err := policy.ParseDocument(raw)
		if err != nil {
			die("policy: %v", err)
		}
		if err := s.PolicySet(*walletID, doc); err != nil {
			die("%v", err)
		}
		fmt.Printf("policy v%d signed and installed for %s\n", jsonx.I(doc, "version", 0), *walletID)
	default:
		die("policy: unknown action %s", args[0])
	}
}

func cmdApprovals(cfgPath string, args []string) {
	if len(args) == 0 {
		die("approvals: list|approve")
	}
	fs := flag.NewFlagSet("approvals", flag.ExitOnError)
	h := fs.String("psbt-sha256", "", "PSBT hash to approve")
	_ = fs.Parse(args[1:])
	s, _ := svcFor(cfgPath)
	if args[0] == "approve" {
		r, err := s.OwnerApprove(*h)
		if err != nil {
			die("%v", err)
		}
		out(r)
		return
	}
	out(s.Store.PendingApprovals(""))
}

func cmdAudit(cfgPath string, args []string) {
	if len(args) == 0 {
		die("audit: export|verify")
	}
	fs := flag.NewFlagSet("audit", flag.ExitOnError)
	since := fs.Int64("since", 0, "export entries after this seq")
	limit := fs.Int64("limit", 500, "max entries")
	_ = fs.Parse(args[1:])
	s, _ := svcFor(cfgPath)
	if args[0] == "verify" {
		ok, n, why := s.Store.AuditVerifyDetail()
		state := "OK"
		if !ok {
			state = "BROKEN: " + why
		}
		fmt.Printf("chain %s over %d entries\n", state, n)
		if !ok {
			os.Exit(1)
		}
		return
	}
	out(s.Store.AuditExport(*since, *limit))
}

func waitSignal() {
	ch := make(chan os.Signal, 1)
	signal.Notify(ch, syscall.SIGINT, syscall.SIGTERM)
	<-ch
}

func cmdServeRest(cfgPath string) {
	s, cfg := svcFor(cfgPath)
	srv, _, err := rest.Serve(s, cfg.Listen)
	if err != nil {
		die("%v", err)
	}
	stop := make(chan struct{})
	go s.Spend.Janitor(stop, 30*time.Second)
	fmt.Printf("qtc-aws REST on http://%s/v1/ (loopback only)\n", cfg.Listen)
	waitSignal()
	close(stop)
	_ = srv.Close()
}

func cmdServeCosigner(cfgPath string) {
	cfg, err := config.Load(cfgPath)
	if err != nil {
		die("%v", err)
	}
	csDir := filepath.Join(cfg.DataDir, "cosigner")
	if err := os.MkdirAll(csDir, 0o700); err != nil {
		die("%v", err)
	}
	mainKR := config.NewKeyring(cfg.KeyringPath())
	kr := config.NewKeyring(filepath.Join(csDir, "keyring.json"))
	// the co-signer needs the decision secret, the admin secret (to verify policy signatures) and the database
	// password; nothing else is shared. It keys its own audit chain.
	for _, n := range []string{"decision_secret", "admin_secret", "pg_password"} {
		if n == "pg_password" {
			if _, ok := mainKR.Get(n); !ok {
				continue
			}
		}
		if _, ok := kr.Get(n); !ok {
			v, err := mainKR.Require(n)
			if err != nil {
				die("%v", err)
			}
			if err := kr.Set(n, v); err != nil {
				die("%v", err)
			}
		}
	}
	pgpw, _ := kr.Get("pg_password")
	st, err := store.Open(cfg.PgDSN, "cosigner", pgpw)
	if err != nil {
		die("%v", err)
	}
	auditKey, err := kr.Ensure("audit_key", 32)
	if err != nil {
		die("%v", err)
	}
	st.SetAuditKey(auditKey)
	main, err := store.Open(cfg.PgDSN, "aws", pgpw) // read-only use: wallet rows (receive address = change)
	if err != nil {
		die("%v", err)
	}
	pw := cfg.RPCPassword
	if pw == "" {
		pw, _ = mainKR.Get("rpc_password")
	}
	admin, _ := kr.Require("admin_secret")
	svc := &cosigner.Service{Cfg: cfg, RPC: rpc.New(cfg.RPCURL, cfg.CookiePath, cfg.RPCUser, pw), Keyring: kr, Store: st,
		Policy: policy.NewEngine(cfg.PolicyDir(), admin, st), WalletLookup: main.Wallet}
	srv, err := cosigner.Serve(svc, cfg.CosignerListen)
	if err != nil {
		die("%v", err)
	}
	fmt.Printf("qtc-aws policy co-signer on http://%s/ (loopback only)\n", cfg.CosignerListen)
	waitSignal()
	_ = srv.Close()
}

func cmdMCP(cfgPath string, args []string) {
	fs := flag.NewFlagSet("mcp", flag.ExitOnError)
	token := fs.String("token", "", "agent bearer token")
	_ = fs.Parse(args)
	if *token == "" {
		die("mcp: --token required")
	}
	s, _ := svcFor(cfgPath)
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	if err := mcpserver.RunStdio(ctx, s, *token); err != nil {
		die("%v", err)
	}
}
