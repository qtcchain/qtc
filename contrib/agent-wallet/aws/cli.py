"""Owner CLI: python -m aws.cli <command> --config aws.json

  init                      create data dir, keyring (admin + decision secrets), config
  token issue --label L --scopes s1,s2     print a new bearer token once
  token revoke --label L
  token list
  wallet create --agent-id ID [--tier 2 --owner-mldsa HEX --cooldown N]
  wallet list | balance | backup | unfreeze | retire [--sweep-to ADDR]
  policy show --wallet-id W | policy set --wallet-id W --file policy.json
  approvals list | approve --psbt-sha256 H
  audit export [--since N] | audit verify
  serve-rest                run the REST API (loopback)
  serve-cosigner            run the Tier 2 policy co-signer (loopback)
  mcp --token T             run the MCP stdio server for an agent
  status
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

from .config import Config, Keyring
from .policy import DEFAULT_POLICY, PolicyEngine
from .rpc import Rpc
from .service import AgentWalletService
from .store import Store


def _svc(args) -> AgentWalletService:
    return AgentWalletService(Config.load(args.config))


def _print(obj) -> None:
    print(json.dumps(obj, indent=2, sort_keys=True))


def cmd_init(args) -> None:
    cfg = Config(data_dir=Path(args.data_dir).expanduser(), rpc_url=args.rpc_url, cookie_path=args.cookie, chain=args.chain,
                 listen=args.listen, cosigner_url=f"http://{args.cosigner_listen}", cosigner_listen=args.cosigner_listen,
                 explorer_url=args.explorer_url or "")
    cfg.data_dir.mkdir(parents=True, exist_ok=True)
    kr = Keyring(cfg.keyring_path)
    kr.ensure("admin_secret", 32); kr.ensure("decision_secret", 32); kr.ensure("archive_passphrase", 32)
    if args.rpc_user:
        # rpcauth identity (spec §9): the password is read from stdin and kept in the 0600 keyring, never in argv or the config
        cfg.rpc_user = args.rpc_user
        kr.set("rpc_password", sys.stdin.readline().rstrip("\n"))
    cfg.save(args.config)
    print(f"initialised {cfg.data_dir}; config written to {args.config}; keyring has {len(kr.names())} secrets (0600)")


def cmd_token(args) -> None:
    svc = _svc(args)
    if args.action == "issue":
        tok = svc.issue_token(args.label, [s.strip() for s in args.scopes.split(",") if s.strip()])
        print(tok)  # printed once, never stored in clear
    elif args.action == "revoke":
        print(f"revoked {svc.store.revoke_token(args.label)} token(s)")
    else:
        _print(svc.store.tokens())


def cmd_wallet(args) -> None:
    svc = _svc(args)
    from .service import Auth
    owner = Auth(scopes={"wallet:admin"}, actor="owner")
    if args.action == "create":
        policy = json.loads(Path(args.policy).read_text()) if args.policy else None
        _print(svc.wallet_create(owner, args.agent_id, args.tier, args.owner_mldsa, policy, args.cooldown))
    elif args.action == "list":
        _print([{k: v for k, v in w.items() if k != "descriptor"} for w in svc.store.wallets()])
    elif args.action == "balance":
        _print(svc.wallet_balance(owner, args.wallet_id))
    elif args.action == "backup":
        _print(svc.wallets.backup(args.wallet_id, actor="owner"))
    elif args.action == "unfreeze":
        svc.owner_unfreeze(args.wallet_id); print("unfrozen")
    elif args.action == "retire":
        _print(svc.owner_retire(args.wallet_id, args.sweep_to))


def cmd_policy(args) -> None:
    svc = _svc(args)
    if args.action == "show":
        _print(svc.policy.describe(args.wallet_id))
    elif args.action == "set":
        doc = json.loads(Path(args.file).read_text())
        cur = svc.policy.load(args.wallet_id) if svc.policy.path(args.wallet_id).exists() else {"version": 0}
        if int(doc.get("version", 0)) <= int(cur.get("version", 0)):
            sys.exit(f"policy version must exceed the current {cur.get('version', 0)}")
        svc.policy.write(args.wallet_id, doc)
        svc.store.audit("policy.update", {"version": doc["version"]}, args.wallet_id, "owner")
        print(f"policy v{doc['version']} signed and installed for {args.wallet_id}")
    elif args.action == "template":
        _print(DEFAULT_POLICY)


def cmd_approvals(args) -> None:
    svc = _svc(args)
    if args.action == "list":
        _print(svc.store.pending_approvals())
    else:
        _print(svc.owner_approve(args.psbt_sha256))


def cmd_audit(args) -> None:
    svc = _svc(args)
    if args.action == "verify":
        ok, n = svc.store.audit_verify(); print(f"chain {'OK' if ok else 'BROKEN'} over {n} entries"); sys.exit(0 if ok else 1)
    _print(svc.store.audit_export(args.since, args.limit))


def cmd_serve_rest(args) -> None:
    from .rest import serve
    svc = _svc(args)
    srv = serve(svc, svc.cfg.listen)
    print(f"qtc-aws REST on http://{svc.cfg.listen}/v1/ (loopback only)", flush=True)
    srv.serve_forever()


def cmd_serve_cosigner(args) -> None:
    from .cosigner import CosignerService, serve
    cfg = Config.load(args.config)
    cs_dir = cfg.data_dir / "cosigner"; cs_dir.mkdir(parents=True, exist_ok=True)
    main_kr = Keyring(cfg.keyring_path)
    kr = Keyring(cs_dir / "keyring.json")
    # the co-signer needs the decision secret and the admin secret (to verify policy signatures); nothing else is shared
    for name in ("decision_secret", "admin_secret"):
        if kr.get(name) is None:
            kr.set(name, main_kr.require(name))
    store = Store(cs_dir / "cosigner.sqlite3")
    rpc = Rpc(cfg.rpc_url, cfg.cookie_path or None, cfg.rpc_user, cfg.rpc_password or main_kr.get("rpc_password"))
    policy = PolicyEngine(cfg.policy_dir, kr.require("admin_secret"), store)
    svc = CosignerService(cfg, rpc, kr, store, policy)
    # the co-signer reads wallet rows (receive address = change) from the AWS store, read-only
    svc.store.wallet = Store(cfg.db_path).wallet  # type: ignore[method-assign]
    srv = serve(svc, cfg.cosigner_listen)
    print(f"qtc-aws policy co-signer on http://{cfg.cosigner_listen}/ (loopback only)", flush=True)
    srv.serve_forever()


def cmd_mcp(args) -> None:
    from .mcp_server import main
    main(args.config, args.token)


def cmd_status(args) -> None:
    svc = _svc(args)
    _print({"node": svc.node_status(), "wallets": [w["wallet_id"] for w in svc.store.wallets()],
            "pending_approvals": len(svc.store.pending_approvals()), "audit": dict(zip(("ok", "entries"), svc.store.audit_verify()))})


def main(argv: list[str] | None = None) -> None:
    p = argparse.ArgumentParser(prog="aws", description="QTC Agent Wallet Service")
    p.add_argument("--config", default="aws.json")
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("init"); s.add_argument("--data-dir", required=True); s.add_argument("--rpc-url", default="http://127.0.0.1:28790")
    s.add_argument("--cookie", default=""); s.add_argument("--chain", default="regtest"); s.add_argument("--listen", default="127.0.0.1:8792")
    s.add_argument("--cosigner-listen", default="127.0.0.1:8793"); s.add_argument("--explorer-url", default="")
    s.add_argument("--rpc-user", help="rpcauth user for the node (its password is read from stdin, one line)"); s.set_defaults(fn=cmd_init)
    s = sub.add_parser("token"); s.add_argument("action", choices=["issue", "revoke", "list"]); s.add_argument("--label"); s.add_argument("--scopes", default="wallet:read,wallet:spend,market:read"); s.set_defaults(fn=cmd_token)
    s = sub.add_parser("wallet"); s.add_argument("action", choices=["create", "list", "balance", "backup", "unfreeze", "retire"])
    s.add_argument("--agent-id"); s.add_argument("--tier", type=int, default=1); s.add_argument("--owner-mldsa", action="append", help="repeat twice: two owner ML-DSA-44 public keys (hex)"); s.add_argument("--cooldown", type=int, default=1008)
    s.add_argument("--policy"); s.add_argument("--wallet-id"); s.add_argument("--sweep-to"); s.set_defaults(fn=cmd_wallet)
    s = sub.add_parser("policy"); s.add_argument("action", choices=["show", "set", "template"]); s.add_argument("--wallet-id"); s.add_argument("--file"); s.set_defaults(fn=cmd_policy)
    s = sub.add_parser("approvals"); s.add_argument("action", choices=["list", "approve"]); s.add_argument("--psbt-sha256"); s.set_defaults(fn=cmd_approvals)
    s = sub.add_parser("audit"); s.add_argument("action", choices=["export", "verify"]); s.add_argument("--since", type=int, default=0); s.add_argument("--limit", type=int, default=500); s.set_defaults(fn=cmd_audit)
    sub.add_parser("serve-rest").set_defaults(fn=cmd_serve_rest)
    sub.add_parser("serve-cosigner").set_defaults(fn=cmd_serve_cosigner)
    s = sub.add_parser("mcp"); s.add_argument("--token", required=True); s.set_defaults(fn=cmd_mcp)
    sub.add_parser("status").set_defaults(fn=cmd_status)
    args = p.parse_args(argv)
    args.fn(args)


if __name__ == "__main__":
    main()
