"""Wallet lifecycle (spec §4): create, fund, backup, balance, history, rotate, freeze, retire.

Node wallets are named `agent-<agent_id>-<tier>`; Tier 2 additionally uses `agent-<agent_id>-key`
(the agent's own ML-DSA key) and `agent-<agent_id>-tier2` as a watch-only coordinator wallet that
holds the `mr(multi_pq(2,agent,policy),{csv_multi_pq(cooldown,1,owner)})` descriptor. The AWS never
loads a wallet it did not create unless the owner imports it explicitly.
"""
from __future__ import annotations

import re
import secrets
import time
from typing import Any

from .config import Config, Keyring
from .rpc import Rpc, RpcError
from .store import Store

ATOMS = 100_000_000
WALLET_RE = re.compile(r"^[a-z0-9][a-z0-9_-]{0,31}$")


def atoms_to_qtc(a: int) -> float:
    return a / ATOMS


def qtc_to_atoms(q: Any) -> int:
    return int(round(float(q) * ATOMS))


class WalletError(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


class Wallets:
    def __init__(self, cfg: Config, rpc: Rpc, keyring: Keyring, store: Store, cosigner_pubkey_fn=None):
        self.cfg, self.rpc, self.keyring, self.store = cfg, rpc, keyring, store
        self._cosigner_pubkey = cosigner_pubkey_fn  # callable(agent_id) -> policy ML-DSA pubkey hex (Tier 2)

    # ---- helpers
    def _node_wallet(self, w: dict[str, Any]) -> Rpc:
        return self.rpc.wallet(w["node_wallet"])

    def _ensure_loaded(self, name: str) -> None:
        if name in self.rpc("listwallets"):
            return
        try:
            self.rpc("loadwallet", name)
        except RpcError as e:
            if "already loaded" not in e.message:
                raise

    def get(self, wallet_id: str) -> dict[str, Any]:
        w = self.store.wallet(wallet_id)
        if not w:
            raise WalletError("unknown_wallet", f"no wallet {wallet_id}")
        self._ensure_loaded(w["node_wallet"])
        if w.get("key_wallet"):
            self._ensure_loaded(w["key_wallet"])
        return w

    def _passphrase(self, node_wallet: str) -> str:
        return self.keyring.require(f"wallet:{node_wallet}")

    def unlock(self, node_wallet: str, seconds: int = 60) -> None:
        self.rpc.wallet(node_wallet)("walletpassphrase", self._passphrase(node_wallet), seconds)

    def lock(self, node_wallet: str) -> None:
        try:
            self.rpc.wallet(node_wallet)("walletlock")
        except RpcError:
            pass

    def _create_encrypted(self, name: str) -> None:
        if name in self.rpc("listwalletdir")["wallets"] and any(w["name"] == name for w in self.rpc("listwalletdir")["wallets"]):
            raise WalletError("exists", f"node wallet {name} already exists; the AWS refuses to adopt it")
        self.rpc("createwallet", name)
        pw = secrets.token_urlsafe(32)
        self.rpc.wallet(name)("encryptwallet", pw)
        self.keyring.set(f"wallet:{name}", pw)

    # ---- create (spec §4 Create)
    def create(self, agent_id: str, tier: int, owner_mldsa: str | None = None, cooldown_blocks: int = 1008,
               actor: str | None = None) -> dict[str, Any]:
        if not WALLET_RE.match(agent_id):
            raise WalletError("bad_request", "agent_id must match [a-z0-9][a-z0-9_-]{0,31}")
        if tier not in (1, 2):
            raise WalletError("bad_request", "tier must be 1 or 2")
        wallet_id = f"agent-{agent_id}-{'tier1' if tier == 1 else 'tier2'}"
        if self.store.wallet(wallet_id):
            raise WalletError("exists", f"{wallet_id} already exists")
        if tier == 1:
            node_wallet = wallet_id
            self._create_encrypted(node_wallet)
            addr = self.rpc.wallet(node_wallet)("getnewaddress", "agent-receive", "p2mr")
            self.store.add_wallet(wallet_id=wallet_id, agent_id=agent_id, tier=1, node_wallet=node_wallet,
                                  key_wallet=None, descriptor=None, receive_address=addr)
            self.store.audit("wallet.create", {"wallet_id": wallet_id, "tier": 1, "receive_address": addr}, wallet_id, actor)
            return {"wallet_id": wallet_id, "receive_address": addr, "tier": 1}
        # Tier 2: agent key wallet + policy co-signer key + owner recovery key in one mr() tree
        # Node limitation (10 Oct 2026 dev build, verified on regtest): the wallet's PSBT updater fills P2MR
        # leaf metadata for SINGLE-LEAF mr() descriptors only; a tree with a backup leaf gets no leaf data and
        # cannot be signed through walletprocesspsbt. So the Tier 2 output is the single leaf
        # mr(multi_pq(2,agent,policy)): the co-signature is still enforced in script. The owner recovery leaf
        # from the spec is recorded here for the day the wallet supports multi-leaf trees; until then owner
        # recovery is through the encrypted backup archives of both key wallets.
        keys = owner_mldsa if isinstance(owner_mldsa, list) else ([owner_mldsa] if owner_mldsa else [])
        if any(not re.fullmatch(r"[0-9a-f]{2624}", k) for k in keys):
            raise WalletError("bad_request", "owner_mldsa entries must be 1312-byte ML-DSA-44 public keys as hex")
        if not self._cosigner_pubkey:
            raise WalletError("cosigner_unavailable", "no policy co-signer configured")
        key_wallet = f"agent-{agent_id}-key"
        self._create_encrypted(key_wallet)
        self.unlock(key_wallet, 60)  # exporting the PQ public key derives from the encrypted seed
        try:
            kaddr = self.rpc.wallet(key_wallet)("getnewaddress", "agent-key", "p2mr")
            agent_pk = self.rpc.wallet(key_wallet)("exportpqkey", kaddr)["key"]
        finally:
            self.lock(key_wallet)
        policy_pk = self._cosigner_pubkey(agent_id)
        desc = f"mr(multi_pq(2,{agent_pk},{policy_pk}))"
        planned_tree = (f"mr(multi_pq(2,{agent_pk},{policy_pk}),{{csv_multi_pq({int(cooldown_blocks)},1,{keys[0]},{keys[1]})}})"
                        if len(keys) == 2 else None)
        info = self.rpc("getdescriptorinfo", desc)
        desc_cs = f"{desc}#{info['checksum']}"
        node_wallet = wallet_id
        self.rpc("createwallet", node_wallet, True, True)  # disable_private_keys, blank: coordinator / watch-only
        self.rpc.wallet(node_wallet)("importdescriptors", [{"desc": desc_cs, "timestamp": "now", "internal": False, "label": "tier2"}])
        addr = self.rpc("deriveaddresses", desc_cs)[0]
        self.store.add_wallet(wallet_id=wallet_id, agent_id=agent_id, tier=2, node_wallet=node_wallet,
                              key_wallet=key_wallet, descriptor=desc_cs, receive_address=addr)
        self.store.audit("wallet.create", {"wallet_id": wallet_id, "tier": 2, "receive_address": addr, "descriptor": desc_cs,
                                           "planned_recovery_tree": planned_tree, "cooldown_blocks": cooldown_blocks}, wallet_id, actor)
        return {"wallet_id": wallet_id, "receive_address": addr, "tier": 2, "descriptor": desc_cs}

    # ---- fund / read
    def receive_address(self, wallet_id: str, label: str | None = None) -> str:
        w = self.get(wallet_id)
        if w["tier"] == 2:
            return w["receive_address"]  # single-address tree until rotation
        return self._node_wallet(w)("getnewaddress", label or "agent-receive", "p2mr")

    def balance(self, wallet_id: str) -> dict[str, int]:
        w = self.get(wallet_id)
        b = self._node_wallet(w)("getbalances")
        mine = b.get("mine", {})
        if w["tier"] == 2:  # watch-only coordinator: balances live under "watchonly"
            mine = b.get("watchonly", mine)
        locked = 0
        for s in self.store._q("SELECT data FROM swaps WHERE wallet_id=? AND state NOT IN ('claimed','refunded','cancelled')", wallet_id):
            try:
                import json as _j
                locked += int(_j.loads(s["data"]).get("amount_atoms", 0))
            except Exception:
                pass
        return {"confirmed_atoms": qtc_to_atoms(mine.get("trusted", 0)),
                "pending_atoms": qtc_to_atoms(mine.get("untrusted_pending", 0)),
                "immature_atoms": qtc_to_atoms(mine.get("immature", 0)),
                "locked_in_swaps_atoms": locked}

    def history(self, wallet_id: str, limit: int = 50, before: int | None = None) -> list[dict[str, Any]]:
        w = self.get(wallet_id)
        txs = self._node_wallet(w)("listtransactions", "*", limit, 0, w["tier"] == 2)
        purposes = {s["txid"]: s["purpose"] for s in self.store.recent_spends(wallet_id, 500) if s.get("txid")}
        out = []
        for t in reversed(txs):
            if before is not None and t.get("blockheight", 0) and t["blockheight"] >= before:
                continue
            out.append({"txid": t["txid"], "direction": "in" if t["category"] in ("receive", "generate", "immature") else "out",
                        "amount_atoms": abs(qtc_to_atoms(t["amount"])), "fee_atoms": abs(qtc_to_atoms(t.get("fee", 0))),
                        "height": t.get("blockheight"), "confirmations": t.get("confirmations", 0),
                        "purpose": purposes.get(t["txid"])})
        return out

    # ---- freeze / unfreeze / retire
    def freeze(self, wallet_id: str, reason: str, actor: str | None = None) -> None:
        w = self.get(wallet_id)
        self.store.set_wallet(wallet_id, frozen=1, frozen_reason=reason)
        for name in filter(None, [w["node_wallet"], w.get("key_wallet")]):
            self.lock(name)
        self.store.audit("wallet.freeze", {"reason": reason}, wallet_id, actor)

    def unfreeze(self, wallet_id: str, actor: str | None = None) -> None:
        self.get(wallet_id)
        self.store.set_wallet(wallet_id, frozen=0, frozen_reason=None)
        self.store.audit("wallet.unfreeze", {}, wallet_id, actor)

    def retire(self, wallet_id: str, actor: str | None = None) -> None:
        w = self.get(wallet_id)
        self.store.set_wallet(wallet_id, retired=1, frozen=1, frozen_reason="retired")
        for name in filter(None, [w["node_wallet"], w.get("key_wallet")]):
            try:
                self.rpc("unloadwallet", name)
            except RpcError:
                pass
        self.store.audit("wallet.retire", {}, wallet_id, actor)

    # ---- backup (owner-side; never exposed to agents)
    def backup(self, wallet_id: str, actor: str | None = None) -> dict[str, Any]:
        w = self.get(wallet_id)
        self.cfg.backup_dir.mkdir(parents=True, exist_ok=True)
        out = []
        archive_pw = self.keyring.ensure("archive_passphrase", 32)
        for name in filter(None, [w["node_wallet"], w.get("key_wallet")]):
            wr = self.rpc.wallet(name)
            try:
                wr("z_verifywalletintegrity")
            except RpcError as e:
                if e.code != -32601:
                    raise
            dest = str(self.cfg.backup_dir / f"{name}-{int(time.time())}.qtcarchive")
            wpw = self.keyring.get(f"wallet:{name}")
            args = [dest, archive_pw] + ([wpw] if wpw else [])
            try:
                wr("backupwalletbundlearchive", *args)
            except RpcError as e:
                if e.code != -32601:
                    raise
                wr("backupwallet", dest)
            out.append(dest)
        self.store.audit("wallet.backup", {"archives": out}, wallet_id, actor)
        return {"archives": out}

    # ---- rotate agent key (Tier 1: fresh receive descriptor; Tier 2: new key + new tree)
    def rotate(self, wallet_id: str, actor: str | None = None) -> dict[str, Any]:
        w = self.get(wallet_id)
        if w["tier"] == 1:
            addr = self._node_wallet(w)("getnewaddress", "agent-receive", "p2mr")
            self.store.set_wallet(wallet_id, receive_address=addr)
            self.store.audit("wallet.rotate", {"receive_address": addr}, wallet_id, actor)
            return {"receive_address": addr}
        raise WalletError("not_supported", "tier 2 rotation requires a new owner-approved tree; create a new wallet and sweep")
