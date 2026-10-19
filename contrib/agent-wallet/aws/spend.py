"""Spend pipeline (spec §5):

  request -> policy evaluation -> PSBT build -> (approval) -> sign -> verify -> broadcast -> track

Amounts are integers in atoms. The PSBT hash is sha256 of the raw PSBT bytes. The final transaction
is re-decoded and its outputs compared byte for byte with the approved outputs before broadcast.
"""
from __future__ import annotations

import base64
import hashlib
import json
import urllib.request
from typing import Any

from .config import Config
from .policy import ALLOW, DENY, NEEDS_APPROVAL, PolicyEngine
from .rpc import Rpc, RpcError
from .store import Store
from .wallet import ATOMS, WalletError, Wallets, atoms_to_qtc, qtc_to_atoms


class SpendError(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


def psbt_sha256(psbt_b64: str) -> str:
    return hashlib.sha256(base64.b64decode(psbt_b64)).hexdigest()


class SpendPipeline:
    def __init__(self, cfg: Config, rpc: Rpc, wallets: Wallets, policy: PolicyEngine, store: Store, cosigner=None):
        self.cfg, self.rpc, self.wallets, self.policy, self.store = cfg, rpc, wallets, policy, store
        self.cosigner = cosigner  # CosignerClient for Tier 2
        self._locked: dict[str, tuple[str, list[dict[str, Any]]]] = {}  # psbt_sha256 -> (node wallet, inputs locked by the build)

    def _release(self, h: str) -> None:
        """Unlock the inputs a failed or expired spend reserved with lockUnspents."""
        item = self._locked.pop(h, None)
        if not item:
            return
        try:
            self.rpc.wallet(item[0])("lockunspent", True, item[1])
        except RpcError:
            pass

    def release_expired_approvals(self) -> None:
        for h, item in list(self._locked.items()):
            a = self.store.approval(h)
            if a and not a["approved"] and a["expires_at"] < __import__("time").time():
                self._release(h); self.store.set_spend(h, status="expired")

    # ---- fee rate: node estimate -> explorer -> policy floor (never below minrelaytxfee), clamped by ceiling
    def fee_rate(self, wallet_id: str, requested: int | None = None) -> int:
        """Atoms per byte. Order: node estimate -> explorer -> policy floor (never below minrelaytxfee).
        A caller-requested cap lowers the rate; a requested rate above the policy ceiling is returned
        as-is so the policy engine denies it with reason fee_rate."""
        lim = self.policy.load(wallet_id)["limits"]
        floor = int(lim.get("min_fee_rate_atoms_per_byte", 1))
        try:
            relay = int(round(float(self.rpc("getmempoolinfo").get("minrelaytxfee", 0)) * ATOMS / 1000))  # QTC/kB -> atoms/byte
            floor = max(floor, relay)
        except RpcError:
            pass
        base = None
        try:
            est = self.rpc("estimatesmartfee", 6)
            if est.get("feerate"):
                base = int(round(float(est["feerate"]) * ATOMS / 1000))
        except RpcError:
            pass
        if base is None and self.cfg.explorer_url:
            try:
                with urllib.request.urlopen(f"{self.cfg.explorer_url.rstrip('/')}/api/v1/fees", timeout=5) as r:
                    fees = json.loads(r.read().decode())
                v = fees.get("6") or fees.get("3") or fees.get("12")
                if v:
                    base = int(round(float(v)))
            except Exception:
                pass
        if base is None:
            base = floor
        rate = max(base, floor)
        if requested is not None:
            if int(requested) > int(lim["max_fee_rate_atoms_per_byte"]):
                return int(requested)
            rate = max(min(rate, int(requested)), floor)
        return min(rate, int(lim["max_fee_rate_atoms_per_byte"]))

    # ---- request -> evaluation -> build -> (approval) -> sign/verify/broadcast
    def send(self, wallet_id: str, to: str, amount_atoms: int | None, purpose: str, max_fee_rate: int | None = None,
             send_max: bool = False, actor: str | None = None, is_htlc_destination: bool = False,
             trade: dict[str, Any] | None = None) -> dict[str, Any]:
        if not purpose or not isinstance(purpose, str):
            raise SpendError("bad_request", "purpose is required")
        if (amount_atoms is None) == (not send_max):
            raise SpendError("bad_request", "give exactly one of amount_atoms or send_max")
        if amount_atoms is not None and (not isinstance(amount_atoms, int) or isinstance(amount_atoms, bool)):
            raise SpendError("bad_request", "amount_atoms must be an integer (no floats)")
        w = self.wallets.get(wallet_id)
        self.release_expired_approvals()
        if not self.rpc("validateaddress", to).get("isvalid"):
            raise SpendError("bad_request", "invalid destination address")
        rate = self.fee_rate(wallet_id, max_fee_rate)
        if send_max:
            bal = self.wallets.balance(wallet_id)
            amount_atoms = bal["confirmed_atoms"]
        decision = self.policy.evaluate(w, int(amount_atoms), [to], rate, None, is_htlc_destination=is_htlc_destination, trade=trade)
        self.store.audit("policy.decision", {"to": to, "amount_atoms": amount_atoms, "fee_rate": rate, "purpose": purpose,
                                             **decision.as_dict()}, wallet_id, actor)
        if decision.decision == DENY:
            if self.policy.record_denial(w, decision.reason) and not w.get("frozen"):
                self.wallets.freeze(wallet_id, f"kill_switch:denials_per_hour ({decision.reason})", actor="policy")
            return {"status": "denied", "reason": decision.reason}
        psbt, decoded = self._build(w, to, int(amount_atoms), rate, send_max)
        h = psbt_sha256(psbt)
        self._locked[h] = (w["node_wallet"], decoded["vin"])
        self.store.add_spend(wallet_id, h, int(amount_atoms), [to], purpose, decision.decision if decision.decision == NEEDS_APPROVAL else "signing")
        if decision.decision == NEEDS_APPROVAL:
            ttl = int(self.policy.load(wallet_id)["approval"].get("ttl_seconds", 900))
            self.store.add_approval(h, wallet_id, psbt, decoded, {"to": to, "amount_atoms": amount_atoms, "purpose": purpose, "fee_rate": rate}, ttl)
            self._notify_approval(wallet_id, h, decoded, ttl)
            self.store.audit("spend.needs_approval", {"psbt_sha256": h, "outputs": decoded["outputs"], "fee_atoms": decoded["fee_atoms"]}, wallet_id, actor)
            return {"status": "needs_approval", "psbt_sha256": h, "expires_in_seconds": ttl}
        return self._sign_verify_broadcast(w, psbt, h, decoded, actor)

    def complete_approved(self, psbt_sha256_hex: str, actor: str | None = None) -> dict[str, Any]:
        """Owner approved (by PSBT hash): finish the pipeline for that exact PSBT."""
        a = self.store.approval(psbt_sha256_hex)
        if not a:
            raise SpendError("unknown_approval", "no such pending approval")
        if not a["approved"]:
            raise SpendError("not_approved", "approval not granted")
        if a["consumed"]:
            raise SpendError("consumed", "approval already used")
        w = self.wallets.get(a["wallet_id"])
        self.store.consume_approval(psbt_sha256_hex)
        self.store.set_spend(psbt_sha256_hex, status="signing")
        return self._sign_verify_broadcast(w, a["psbt"], psbt_sha256_hex, a["decoded"], actor)

    # ---- internals
    def _build(self, w: dict[str, Any], to: str, amount_atoms: int, rate: int, send_max: bool) -> tuple[str, dict[str, Any]]:
        wr = self.rpc.wallet(w["node_wallet"])
        height = self.rpc("getblockcount")
        opts = {"fee_rate": rate, "lockUnspents": True, "changeAddress": w["receive_address"] if w["tier"] == 2 else wr("getrawchangeaddress", "p2mr"),
                "add_inputs": True, "include_watching": w["tier"] == 2}
        if send_max:
            opts["subtractFeeFromOutputs"] = [0]
        try:
            res = wr("walletcreatefundedpsbt", [], [{to: atoms_to_qtc(amount_atoms)}], max(0, height - 1), opts)
        except RpcError as e:
            raise SpendError("build_failed", e.message) from e
        psbt = res["psbt"]
        decoded = self._decode(psbt)
        return psbt, decoded

    def _decode(self, psbt: str) -> dict[str, Any]:
        d = self.rpc("decodepsbt", psbt)
        tx = d["tx"]
        outs = [{"address": (o["scriptPubKey"].get("address") or o["scriptPubKey"].get("hex")), "atoms": qtc_to_atoms(o["value"])} for o in tx["vout"]]
        fee = qtc_to_atoms(d.get("fee", 0)) if d.get("fee") is not None else None
        return {"outputs": outs, "fee_atoms": fee, "vin": [{"txid": i["txid"], "vout": i["vout"]} for i in tx["vin"]], "locktime": tx.get("locktime")}

    def _sign_verify_broadcast(self, w: dict[str, Any], psbt: str, h: str, approved: dict[str, Any], actor: str | None) -> dict[str, Any]:
        wid = w["wallet_id"]
        try:
            if w["tier"] == 1:
                self.wallets.unlock(w["node_wallet"], 60)
                try:
                    signed = self.rpc.wallet(w["node_wallet"])("walletprocesspsbt", psbt)["psbt"]
                finally:
                    self.wallets.lock(w["node_wallet"])
            else:
                # updater step in the coordinator, agent key signs, policy co-signer signs, combine
                upd = self.rpc.wallet(w["node_wallet"])("walletprocesspsbt", psbt, {"sign": False, "bip32derivs": True, "finalize": False})["psbt"]
                self.wallets.unlock(w["key_wallet"], 60)
                try:
                    part_a = self.rpc.wallet(w["key_wallet"])("walletprocesspsbt", upd, {"finalize": False})["psbt"]
                finally:
                    self.wallets.lock(w["key_wallet"])
                if not self.cosigner:
                    raise SpendError("cosigner_unavailable", "tier 2 spend needs the policy co-signer")
                envelope = self.cosigner.envelope(w, upd, approved, self.policy.load(wid)["version"])
                part_b = self.cosigner.sign(w["agent_id"], upd, envelope)
                signed = self.rpc("combinepsbt", [part_a, part_b])
            fin = self.rpc("finalizepsbt", signed)
            if not fin.get("complete"):
                raise SpendError("sign_incomplete", "transaction did not finalize (missing signatures)")
            rawtx = fin["hex"]
            # verify: outputs must equal the approved outputs byte for byte
            dec = self.rpc("decoderawtransaction", rawtx)
            got = [{"address": (o["scriptPubKey"].get("address") or o["scriptPubKey"].get("hex")), "atoms": qtc_to_atoms(o["value"])} for o in dec["vout"]]
            if got != approved["outputs"]:
                self.store.set_spend(h, status="security_alert")
                self.store.audit("security.post_sign_mutation", {"psbt_sha256": h, "expected": approved["outputs"], "got": got}, wid, actor)
                self.wallets.freeze(wid, "security_alert:post_sign_mutation", actor="pipeline")
                raise SpendError("security_alert", "final transaction outputs differ from the approved outputs")
            acc = self.rpc("testmempoolaccept", [rawtx])[0]
            if not acc.get("allowed"):
                self.store.set_spend(h, status="rejected")
                raise SpendError("mempool_reject", acc.get("reject-reason", "rejected"))
            txid = self.rpc("sendrawtransaction", rawtx)
            self._locked.pop(h, None)  # inputs are spent; the wallet forgets the lock itself
            self.store.set_spend(h, status="broadcast", txid=txid, fee_atoms=approved.get("fee_atoms"))
            self.store.audit("spend.broadcast", {"psbt_sha256": h, "txid": txid, "outputs": approved["outputs"], "fee_atoms": approved.get("fee_atoms")}, wid, actor)
            return {"status": "broadcast", "txid": txid, "psbt_sha256": h, "fee_atoms": approved.get("fee_atoms")}
        except SpendError:
            self._release(h)
            raise
        except RpcError as e:
            self._release(h)
            self.store.set_spend(h, status="failed")
            self.store.audit("spend.failed", {"psbt_sha256": h, "error": e.message}, wid, actor)
            raise SpendError("sign_failed", e.message) from e
        except Exception:
            self._release(h)
            raise

    # ---- track
    def status(self, wallet_id: str, psbt_sha256_hex: str) -> dict[str, Any]:
        s = self.store.spend(psbt_sha256_hex)
        if not s or s["wallet_id"] != wallet_id:
            raise SpendError("unknown", "no such spend")
        out = {"status": s["status"], "txid": s.get("txid"), "psbt_sha256": psbt_sha256_hex, "confirmations": 0}
        if s.get("txid") and s["status"] in ("broadcast", "confirmed"):
            w = self.wallets.get(wallet_id)
            try:
                t = self.rpc.wallet(w["node_wallet"])("gettransaction", s["txid"])
                out["confirmations"] = int(t.get("confirmations", 0))
                if out["confirmations"] < 0:
                    self.store.set_spend(psbt_sha256_hex, status="dropped"); out["status"] = "dropped"
                elif out["confirmations"] >= self.cfg.confirmation_depth and s["status"] != "confirmed":
                    self.store.set_spend(psbt_sha256_hex, status="confirmed"); out["status"] = "confirmed"
                    self.store.audit("spend.confirmed", {"txid": s["txid"], "confirmations": out["confirmations"]}, wallet_id)
            except RpcError:
                pass
        return out

    def _notify_approval(self, wallet_id: str, h: str, decoded: dict[str, Any], ttl: int) -> None:
        if not self.cfg.approval_webhook:
            return
        body = json.dumps({"wallet_id": wallet_id, "psbt_sha256": h, "outputs": decoded["outputs"], "fee_atoms": decoded["fee_atoms"], "ttl_seconds": ttl}).encode()
        try:
            req = urllib.request.Request(self.cfg.approval_webhook, data=body, headers={"Content-Type": "application/json"}, method="POST")
            urllib.request.urlopen(req, timeout=5).read()
        except Exception:
            pass
