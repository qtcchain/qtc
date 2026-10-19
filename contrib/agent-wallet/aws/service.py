"""The Agent Wallet Service facade: one object that implements the spec's tool surface (§8).

Every tool takes an `auth` context ({"scopes": set, "actor": label}) and raises ScopeDenied when the
token lacks the scope. Amounts are integers in atoms. No tool returns keys, seeds, passphrases,
private descriptors or raw PSBTs (the `wallet:psbt` scope is intentionally not implemented in v1).
"""
from __future__ import annotations

import json
import secrets
import statistics
import time
import urllib.request
from dataclasses import dataclass, field
from typing import Any

from .config import Config, Keyring
from .cosigner import CosignerClient
from .policy import DEFAULT_POLICY, PolicyEngine, PolicyError
from .rpc import NodeUnavailable, Rpc, RpcError
from .spend import SpendError, SpendPipeline
from .store import Store
from .wallet import WalletError, Wallets

SCOPES = {"wallet:admin", "wallet:read", "wallet:spend", "market:read", "trade:quote", "trade:execute", "trade:read"}


class ScopeDenied(Exception):
    pass


class Frozen(Exception):
    pass


@dataclass
class Auth:
    scopes: set[str] = field(default_factory=set)
    actor: str = "agent"

    def need(self, scope: str) -> None:
        if scope not in self.scopes and "wallet:admin" not in self.scopes:
            raise ScopeDenied(scope)


class AgentWalletService:
    def __init__(self, cfg: Config):
        self.cfg = cfg
        cfg.data_dir.mkdir(parents=True, exist_ok=True)
        self.keyring = Keyring(cfg.keyring_path)
        self.store = Store(cfg.db_path)
        self.rpc = Rpc(cfg.rpc_url, cfg.cookie_path or None, cfg.rpc_user, cfg.rpc_password or self.keyring.get("rpc_password"))
        self.policy = PolicyEngine(cfg.policy_dir, self.keyring.ensure("admin_secret", 32), self.store)
        self.cosigner = CosignerClient(cfg.cosigner_url, self.keyring) if cfg.cosigner_url else None
        self.keyring.ensure("decision_secret", 32)
        self.wallets = Wallets(cfg, self.rpc, self.keyring, self.store, self.cosigner.pubkey if self.cosigner else None)
        self.spend = SpendPipeline(cfg, self.rpc, self.wallets, self.policy, self.store, self.cosigner)

    # ---- tokens
    def issue_token(self, label: str, scopes: list[str]) -> str:
        bad = set(scopes) - SCOPES
        if bad:
            raise ValueError(f"unknown scopes: {sorted(bad)}")
        tok = "aws_" + secrets.token_urlsafe(32)
        self.store.add_token(tok, label, scopes)
        self.store.audit("token.issue", {"label": label, "scopes": sorted(scopes)}, None, "owner")
        return tok

    def auth_from_token(self, token: str | None) -> Auth | None:
        if not token:
            return None
        scopes = self.store.token_scopes(token)
        if scopes is None:
            return None
        return Auth(scopes=scopes, actor="token:" + Store.token_hash(token)[:8])

    # ---- node health
    def node_status(self) -> dict[str, Any]:
        try:
            bc = self.rpc("getblockchaininfo"); ni = self.rpc("getnetworkinfo")
        except (NodeUnavailable, RpcError) as e:
            return {"ok": False, "error": str(e)}
        ok = (not bc.get("initialblockdownload", False)) and (int(ni.get("connections", 0)) >= 1 or bc.get("chain") == "regtest")
        return {"ok": ok, "chain": bc.get("chain"), "height": bc.get("blocks"), "ibd": bc.get("initialblockdownload"),
                "connections": ni.get("connections"), "version": ni.get("subversion")}

    # ---- tools (spec §8)
    def wallet_create(self, auth: Auth, agent_id: str, tier: int = 1, owner_mldsa: str | list[str] | None = None, policy: dict[str, Any] | None = None,
                      cooldown_blocks: int = 1008) -> dict[str, Any]:
        auth.need("wallet:admin")
        out = self.wallets.create(agent_id, tier, owner_mldsa, cooldown_blocks, actor=auth.actor)
        doc = json.loads(json.dumps(policy or DEFAULT_POLICY)); doc["agent_id"] = agent_id; doc["tier"] = tier
        doc.setdefault("version", 1)
        self.policy.write(out["wallet_id"], doc)
        self.wallets.backup(out["wallet_id"], actor=auth.actor)
        return out

    def wallet_receive_address(self, auth: Auth, wallet_id: str, label: str | None = None) -> dict[str, Any]:
        auth.need("wallet:read")
        return {"address": self.wallets.receive_address(wallet_id, label)}

    def wallet_balance(self, auth: Auth, wallet_id: str) -> dict[str, Any]:
        auth.need("wallet:read")
        return self.wallets.balance(wallet_id)

    def wallet_history(self, auth: Auth, wallet_id: str, before: int | None = None, limit: int = 50) -> dict[str, Any]:
        auth.need("wallet:read")
        return {"entries": self.wallets.history(wallet_id, min(int(limit), 200), before)}

    def wallet_send(self, auth: Auth, wallet_id: str, to: str, amount_atoms: int | None = None, purpose: str = "",
                    max_fee_rate: int | None = None, send_max: bool = False) -> dict[str, Any]:
        auth.need("wallet:spend")
        w = self.wallets.get(wallet_id)
        if w.get("frozen"):
            raise Frozen(w.get("frozen_reason") or "frozen")
        if not self.node_status()["ok"]:
            raise NodeUnavailable("node not synced or no peers")
        return self.spend.send(wallet_id, to, amount_atoms, purpose, max_fee_rate, send_max, actor=auth.actor)

    def wallet_send_status(self, auth: Auth, wallet_id: str, psbt_sha256: str) -> dict[str, Any]:
        auth.need("wallet:read")
        return self.spend.status(wallet_id, psbt_sha256)

    def wallet_freeze(self, auth: Auth, wallet_id: str, reason: str) -> dict[str, Any]:
        auth.need("wallet:read")  # an agent may self-freeze; only the owner unfreezes
        self.wallets.freeze(wallet_id, reason, actor=auth.actor)
        return {"frozen": True}

    def policy_describe(self, auth: Auth, wallet_id: str) -> dict[str, Any]:
        auth.need("wallet:read")
        self.wallets.get(wallet_id)
        return self.policy.describe(wallet_id)

    def fees_estimate(self, auth: Auth, target_blocks: int = 6) -> dict[str, Any]:
        auth.need("market:read")
        try:
            est = self.rpc("estimatesmartfee", int(target_blocks))
        except RpcError as e:
            raise NodeUnavailable(e.message) from e
        rate = None
        if est.get("feerate"):
            rate = int(round(float(est["feerate"]) * 100_000))  # QTC/kB -> atoms/byte
        relay = int(round(float(self.rpc("getmempoolinfo")["minrelaytxfee"]) * 100_000))
        return {"atoms_per_byte": rate if rate is not None else relay, "source": "estimatesmartfee" if rate is not None else "minrelaytxfee",
                "min_relay_atoms_per_byte": relay}

    # ---- market / trade (Phase 2 scaffolding: desk quotes are the only reference; venues enabled per policy)
    def market_reference_price(self, auth: Auth, quote_asset: str) -> dict[str, Any]:
        auth.need("market:read")
        quotes = self._desk_quotes(quote_asset)
        if len(quotes) < 2:
            return {"price": None, "sources": [q["desk"] for q in quotes], "as_of": int(time.time()),
                    "note": "fewer than two independent desk quotes; reference undefined, trades need approval"}
        return {"price": statistics.median(q["price"] for q in quotes), "sources": [q["desk"] for q in quotes], "as_of": int(time.time())}

    def _desk_quotes(self, quote_asset: str) -> list[dict[str, Any]]:
        out = []
        for desk in self.cfg.extra.get("desks", []):
            try:
                req = urllib.request.Request(desk["url"].rstrip("/") + "/v1/reference", method="GET")
                with urllib.request.urlopen(req, timeout=5) as r:
                    d = json.loads(r.read().decode())
                if d.get("quote") == quote_asset and d.get("price"):
                    out.append({"desk": desk["name"], "price": float(d["price"])})
            except Exception:
                continue
        return out

    def market_quote(self, auth: Auth, wallet_id: str, side: str, quote_asset: str, amount_atoms: int, desks: list[str] | None = None) -> dict[str, Any]:
        auth.need("trade:quote")
        self.wallets.get(wallet_id)
        doc = self.policy.load(wallet_id)
        allowed = set(doc.get("trading", {}).get("counterparties", []))
        quotes = []
        for desk in self.cfg.extra.get("desks", []):
            if desk["name"] not in allowed or (desks and desk["name"] not in desks):
                continue
            try:
                body = json.dumps({"side": side, "base": "QTC", "quote": quote_asset, "amount_atoms": int(amount_atoms)}).encode()
                req = urllib.request.Request(desk["url"].rstrip("/") + "/v1/quote", data=body, headers={"Content-Type": "application/json"}, method="POST")
                with urllib.request.urlopen(req, timeout=10) as r:
                    q = json.loads(r.read().decode()); q["desk"] = desk["name"]; quotes.append(q)
            except Exception as e:  # noqa: BLE001
                quotes.append({"desk": desk["name"], "error": str(e)[:120]})
        return {"quotes": quotes}

    def trade_swap(self, auth: Auth, wallet_id: str, quote_id: str) -> dict[str, Any]:
        auth.need("trade:execute")
        return {"error": "not_enabled", "reason": "Venue A/B settlement (HTLC swaps) is Phase 2; enable per policy once an EVM leg is configured"}

    def trade_status(self, auth: Auth, swap_id: str) -> dict[str, Any]:
        auth.need("trade:read")
        s = self.store.swap(swap_id)
        if not s:
            return {"error": "unknown_swap"}
        return {"state": s["state"], **{k: v for k, v in s["data"].items() if k in ("qtc_txids", "evm_txs", "deadlines")}}

    def trade_cancel(self, auth: Auth, swap_id: str) -> dict[str, Any]:
        auth.need("trade:execute")
        s = self.store.swap(swap_id)
        if not s:
            return {"error": "unknown_swap"}
        return {"state": s["state"], "note": "refund path runs automatically once the locktime passes"}

    # ---- owner-only operations (CLI, never through agent tokens)
    def owner_approve(self, psbt_sha256: str) -> dict[str, Any]:
        if not self.store.approve(psbt_sha256):
            raise SpendError("approval_failed", "no pending, unexpired approval with that hash")
        self.store.audit("approval.granted", {"psbt_sha256": psbt_sha256}, None, "owner")
        return self.spend.complete_approved(psbt_sha256, actor="owner")

    def owner_unfreeze(self, wallet_id: str) -> None:
        self.wallets.unfreeze(wallet_id, actor="owner")

    def owner_retire(self, wallet_id: str, sweep_to: str | None = None) -> dict[str, Any]:
        out: dict[str, Any] = {}
        if sweep_to:
            doc = self.policy.load(wallet_id)
            if sweep_to not in doc["destinations"].get("allow", []):
                raise SpendError("destination_not_allowed", "sweep destination must be on the policy allowlist")
            out = self.spend.send(wallet_id, sweep_to, None, "retire-sweep", send_max=True, actor="owner")
        self.wallets.retire(wallet_id, actor="owner")
        return {"retired": True, **out}
