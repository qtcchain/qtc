"""Policy documents (spec §6): loading, owner signature, evaluation rules and the kill switch.

A policy is a JSON document signed by the owner's admin credential. The signature is
HMAC-SHA256(admin_secret, canonical JSON of the document without the "signature" field).
The agent never sees the admin secret; it only receives the document minus secrets via policy.describe.
"""
from __future__ import annotations

import hmac
import hashlib
import json
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from .store import Store, canonical

ALLOW, NEEDS_APPROVAL, DENY = "allow", "needs_approval", "deny"

DEFAULT_POLICY: dict[str, Any] = {
    "version": 1,
    "agent_id": "",
    "tier": 1,
    "limits": {
        "per_tx_atoms": 5_000_000_000,
        "per_day_atoms": 20_000_000_000,
        "per_day_tx_count": 40,
        "max_balance_atoms": 100_000_000_000,
        "max_fee_rate_atoms_per_byte": 20,
        "min_fee_rate_atoms_per_byte": 1,
        "min_confirmations_to_spend": 1,
    },
    "destinations": {"mode": "allowlist", "allow": [], "allow_swap_htlc_addresses": True},
    "approval": {"require_above_atoms": 1_000_000_000, "channel": "webhook", "ttl_seconds": 900},
    "trading": {"venues": [], "counterparties": [], "max_slippage_bps": 50, "max_open_swaps": 3,
                "min_refund_lock_blocks": 40, "max_refund_lock_blocks": 400,
                "reference_price_sources": [], "max_price_deviation_bps": 200},
    "time_windows": [],
    "kill_switch": {"freeze_on_policy_denials_per_hour": 5},
}

DAYS = {"mon": 0, "tue": 1, "wed": 2, "thu": 3, "fri": 4, "sat": 5, "sun": 6}


def sign_policy(doc: dict[str, Any], admin_secret: str) -> dict[str, Any]:
    body = {k: v for k, v in doc.items() if k != "signature"}
    sig = hmac.new(admin_secret.encode(), canonical(body).encode(), hashlib.sha256).hexdigest()
    out = dict(body); out["signature"] = {"alg": "HMAC-SHA256", "value": sig}
    return out


def verify_policy(doc: dict[str, Any], admin_secret: str) -> bool:
    sig = (doc.get("signature") or {}).get("value", "")
    body = {k: v for k, v in doc.items() if k != "signature"}
    want = hmac.new(admin_secret.encode(), canonical(body).encode(), hashlib.sha256).hexdigest()
    return bool(sig) and hmac.compare_digest(sig, want)


def _in_window(windows: list[dict[str, Any]], now: float) -> bool:
    if not windows:
        return True
    t = datetime.fromtimestamp(now, tz=timezone.utc)
    for w in windows:
        days = w.get("days", "mon-sun")
        a, _, b = days.partition("-")
        lo, hi = DAYS[a[:3].lower()], DAYS[(b or a)[:3].lower()]
        day_ok = lo <= t.weekday() <= hi if lo <= hi else (t.weekday() >= lo or t.weekday() <= hi)
        h1, h2 = w.get("utc", "00:00-24:00").split("-")
        m = t.hour * 60 + t.minute
        m1 = int(h1[:2]) * 60 + int(h1[3:5]); m2 = int(h2[:2]) * 60 + int(h2[3:5])
        if day_ok and m1 <= m < m2:
            return True
    return False


@dataclass
class Decision:
    decision: str
    reason: str = ""
    remaining_day_atoms: int = 0
    remaining_day_count: int = 0

    def as_dict(self) -> dict[str, Any]:
        return {"decision": self.decision, "reason": self.reason,
                "remaining_day_atoms": self.remaining_day_atoms, "remaining_day_count": self.remaining_day_count}


class PolicyEngine:
    def __init__(self, policy_dir: Path, admin_secret: str, store: Store):
        self.policy_dir = policy_dir
        self.admin_secret = admin_secret
        self.store = store
        self._cache: dict[str, dict[str, Any]] = {}

    # ---- documents
    def path(self, wallet_id: str) -> Path:
        return self.policy_dir / f"{wallet_id}.json"

    def write(self, wallet_id: str, doc: dict[str, Any]) -> dict[str, Any]:
        self.policy_dir.mkdir(parents=True, exist_ok=True)
        signed = sign_policy(doc, self.admin_secret)
        self.path(wallet_id).write_text(json.dumps(signed, indent=2) + "\n")
        self._cache.pop(wallet_id, None)
        return signed

    def load(self, wallet_id: str) -> dict[str, Any]:
        if wallet_id in self._cache:
            return self._cache[wallet_id]
        p = self.path(wallet_id)
        if not p.exists():
            raise PolicyError("policy_missing", f"no policy document for {wallet_id}")
        doc = json.loads(p.read_text())
        if not verify_policy(doc, self.admin_secret):
            raise PolicyError("policy_signature", "policy signature invalid")
        self._cache[wallet_id] = doc
        return doc

    def describe(self, wallet_id: str) -> dict[str, Any]:
        doc = dict(self.load(wallet_id)); doc.pop("signature", None)
        spent, count = self.store.rolling_24h(wallet_id)
        lim = doc["limits"]
        doc["today"] = {"spent_atoms": spent, "tx_count": count,
                        "remaining_atoms": max(0, lim["per_day_atoms"] - spent),
                        "remaining_tx_count": max(0, lim["per_day_tx_count"] - count)}
        return doc

    # ---- evaluation (spec §6.2): first deny wins, then needs_approval, else allow
    def evaluate(self, wallet: dict[str, Any], amount_atoms: int, destinations: list[str], fee_rate: int,
                 min_input_confs: int | None, now: float | None = None, is_htlc_destination: bool = False,
                 trade: dict[str, Any] | None = None) -> Decision:
        now = now or time.time()
        wid = wallet["wallet_id"]
        doc = self.load(wid)
        lim, dest, appr = doc["limits"], doc["destinations"], doc["approval"]
        spent, count = self.store.rolling_24h(wid)
        rem_atoms = max(0, lim["per_day_atoms"] - spent); rem_count = max(0, lim["per_day_tx_count"] - count)

        def deny(reason: str) -> Decision:
            return Decision(DENY, reason, rem_atoms, rem_count)

        if wallet.get("frozen"):
            return deny("frozen")
        if wallet.get("retired"):
            return deny("retired")
        if not _in_window(doc.get("time_windows", []), now):
            return deny("time_window")
        mode = dest.get("mode", "allowlist")
        for d in destinations:
            if mode == "allowlist":
                ok = d in dest.get("allow", []) or (is_htlc_destination and dest.get("allow_swap_htlc_addresses", False))
                if not ok:
                    return deny("destination_not_allowed")
            elif mode == "denylist" and d in dest.get("deny", []):
                return deny("destination_not_allowed")
        if amount_atoms <= 0:
            return deny("bad_amount")
        if amount_atoms > lim["per_tx_atoms"]:
            return deny("per_tx_limit")
        if spent + amount_atoms > lim["per_day_atoms"]:
            return deny("per_day_limit")
        if count >= lim["per_day_tx_count"]:
            return deny("per_day_count")
        if fee_rate > lim["max_fee_rate_atoms_per_byte"]:
            return deny("fee_rate")
        if min_input_confs is not None and min_input_confs < lim["min_confirmations_to_spend"]:
            return deny("unconfirmed_inputs")
        if trade is not None:
            tr = doc.get("trading", {})
            if trade.get("venue") not in tr.get("venues", []):
                return deny("venue_not_allowed")
            if trade.get("counterparty") and trade["counterparty"] not in tr.get("counterparties", []):
                return deny("counterparty_not_allowed")
            if self.store.open_swaps(wid) >= tr.get("max_open_swaps", 0):
                return deny("max_open_swaps")
            lock = trade.get("refund_lock_blocks")
            if lock is not None and not (tr.get("min_refund_lock_blocks", 0) <= lock <= tr.get("max_refund_lock_blocks", 10**9)):
                return deny("refund_lock_range")
            dev = trade.get("price_deviation_bps")
            if dev is not None and dev > tr.get("max_price_deviation_bps", 0):
                return deny("price_deviation")
        if amount_atoms > appr.get("require_above_atoms", 0):
            return Decision(NEEDS_APPROVAL, "amount_above_threshold", rem_atoms, rem_count)
        return Decision(ALLOW, "", rem_atoms, rem_count)

    def record_denial(self, wallet: dict[str, Any], reason: str) -> bool:
        """Count a denial; returns True if the kill switch threshold was crossed (caller freezes)."""
        n = self.store.add_denial(wallet["wallet_id"], reason)
        limit = self.load(wallet["wallet_id"]).get("kill_switch", {}).get("freeze_on_policy_denials_per_hour", 0)
        return bool(limit) and n >= limit


class PolicyError(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code
