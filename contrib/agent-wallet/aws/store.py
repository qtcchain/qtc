"""SQLite store: wallets, tokens, spends (for rolling limits), approvals, swaps, and the audit chain."""
from __future__ import annotations

import hashlib
import json
import sqlite3
import threading
import time
from pathlib import Path
from typing import Any, Iterable

SCHEMA = """
CREATE TABLE IF NOT EXISTS wallets (
  wallet_id TEXT PRIMARY KEY, agent_id TEXT NOT NULL, tier INTEGER NOT NULL,
  node_wallet TEXT NOT NULL, key_wallet TEXT, descriptor TEXT, receive_address TEXT,
  frozen INTEGER NOT NULL DEFAULT 0, frozen_reason TEXT, created_at REAL NOT NULL, retired INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS tokens (
  token_sha256 TEXT PRIMARY KEY, label TEXT, scopes TEXT NOT NULL, created_at REAL NOT NULL, revoked INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS spends (
  id INTEGER PRIMARY KEY AUTOINCREMENT, wallet_id TEXT NOT NULL, psbt_sha256 TEXT NOT NULL, txid TEXT,
  amount_atoms INTEGER NOT NULL, fee_atoms INTEGER, destinations TEXT NOT NULL, purpose TEXT, status TEXT NOT NULL,
  created_at REAL NOT NULL, updated_at REAL NOT NULL);
CREATE INDEX IF NOT EXISTS spends_wallet_time ON spends(wallet_id, created_at);
CREATE TABLE IF NOT EXISTS approvals (
  psbt_sha256 TEXT PRIMARY KEY, wallet_id TEXT NOT NULL, psbt TEXT NOT NULL, decoded TEXT NOT NULL,
  request TEXT NOT NULL, expires_at REAL NOT NULL, approved INTEGER NOT NULL DEFAULT 0, approved_at REAL, consumed INTEGER NOT NULL DEFAULT 0);
CREATE TABLE IF NOT EXISTS denials (wallet_id TEXT NOT NULL, at REAL NOT NULL, reason TEXT NOT NULL);
CREATE INDEX IF NOT EXISTS denials_wallet_time ON denials(wallet_id, at);
CREATE TABLE IF NOT EXISTS swaps (
  swap_id TEXT PRIMARY KEY, wallet_id TEXT NOT NULL, state TEXT NOT NULL, data TEXT NOT NULL, created_at REAL NOT NULL, updated_at REAL NOT NULL);
CREATE TABLE IF NOT EXISTS audit (
  seq INTEGER PRIMARY KEY AUTOINCREMENT, at REAL NOT NULL, kind TEXT NOT NULL, wallet_id TEXT, actor TEXT,
  payload TEXT NOT NULL, prev_hash TEXT NOT NULL, hash TEXT NOT NULL);
CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);
"""


def canonical(obj: Any) -> str:
    return json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


class Store:
    def __init__(self, path: Path):
        path.parent.mkdir(parents=True, exist_ok=True)
        self._db = sqlite3.connect(str(path), check_same_thread=False, isolation_level=None)
        self._db.row_factory = sqlite3.Row
        self._lock = threading.RLock()
        with self._lock:
            self._db.executescript("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;" + SCHEMA)

    # ---- generic helpers
    def _q(self, sql: str, *args: Any) -> list[sqlite3.Row]:
        with self._lock:
            return list(self._db.execute(sql, args))

    def _x(self, sql: str, *args: Any) -> None:
        with self._lock:
            self._db.execute(sql, args)

    def meta_get(self, k: str) -> str | None:
        r = self._q("SELECT v FROM meta WHERE k=?", k)
        return r[0]["v"] if r else None

    def meta_set(self, k: str, v: str) -> None:
        self._x("INSERT INTO meta(k,v) VALUES(?,?) ON CONFLICT(k) DO UPDATE SET v=excluded.v", k, v)

    # ---- wallets
    def add_wallet(self, **w: Any) -> None:
        w.setdefault("created_at", time.time())
        cols = ",".join(w); qs = ",".join("?" * len(w))
        self._x(f"INSERT INTO wallets({cols}) VALUES({qs})", *w.values())

    def wallet(self, wallet_id: str) -> dict[str, Any] | None:
        r = self._q("SELECT * FROM wallets WHERE wallet_id=?", wallet_id)
        return dict(r[0]) if r else None

    def wallets(self) -> list[dict[str, Any]]:
        return [dict(r) for r in self._q("SELECT * FROM wallets ORDER BY created_at")]

    def set_wallet(self, wallet_id: str, **fields: Any) -> None:
        sets = ",".join(f"{k}=?" for k in fields)
        self._x(f"UPDATE wallets SET {sets} WHERE wallet_id=?", *fields.values(), wallet_id)

    # ---- tokens
    @staticmethod
    def token_hash(token: str) -> str:
        return hashlib.sha256(token.encode()).hexdigest()

    def add_token(self, token: str, label: str, scopes: Iterable[str]) -> None:
        self._x("INSERT INTO tokens(token_sha256,label,scopes,created_at) VALUES(?,?,?,?)",
                self.token_hash(token), label, " ".join(sorted(set(scopes))), time.time())

    def token_scopes(self, token: str) -> set[str] | None:
        r = self._q("SELECT scopes, revoked FROM tokens WHERE token_sha256=?", self.token_hash(token))
        if not r or r[0]["revoked"]:
            return None
        return set(r[0]["scopes"].split())

    def revoke_token(self, label: str) -> int:
        with self._lock:
            cur = self._db.execute("UPDATE tokens SET revoked=1 WHERE label=? AND revoked=0", (label,))
            return cur.rowcount

    def tokens(self) -> list[dict[str, Any]]:
        return [dict(r) for r in self._q("SELECT label, scopes, created_at, revoked FROM tokens ORDER BY created_at")]

    # ---- spends and rolling limits
    def add_spend(self, wallet_id: str, psbt_sha256: str, amount_atoms: int, destinations: list[str],
                  purpose: str, status: str, fee_atoms: int | None = None, txid: str | None = None) -> int:
        now = time.time()
        with self._lock:
            cur = self._db.execute(
                "INSERT INTO spends(wallet_id,psbt_sha256,txid,amount_atoms,fee_atoms,destinations,purpose,status,created_at,updated_at)"
                " VALUES(?,?,?,?,?,?,?,?,?,?)",
                (wallet_id, psbt_sha256, txid, amount_atoms, fee_atoms, canonical(destinations), purpose, status, now, now))
            return int(cur.lastrowid)

    def set_spend(self, psbt_sha256: str, **fields: Any) -> None:
        fields["updated_at"] = time.time()
        sets = ",".join(f"{k}=?" for k in fields)
        self._x(f"UPDATE spends SET {sets} WHERE psbt_sha256=?", *fields.values(), psbt_sha256)

    def spend(self, psbt_sha256: str) -> dict[str, Any] | None:
        r = self._q("SELECT * FROM spends WHERE psbt_sha256=?", psbt_sha256)
        return dict(r[0]) if r else None

    def rolling_24h(self, wallet_id: str) -> tuple[int, int]:
        """(sum of atoms, count) of spends that reached broadcast/confirmed/pending-approval in the last 24 h."""
        since = time.time() - 86400
        r = self._q("SELECT COALESCE(SUM(amount_atoms),0) AS s, COUNT(*) AS c FROM spends WHERE wallet_id=? AND created_at>=? "
                    "AND status IN ('broadcast','confirmed','needs_approval','approved','signing')", wallet_id, since)
        return int(r[0]["s"]), int(r[0]["c"])

    def recent_spends(self, wallet_id: str, limit: int = 50) -> list[dict[str, Any]]:
        return [dict(r) for r in self._q("SELECT * FROM spends WHERE wallet_id=? ORDER BY created_at DESC LIMIT ?", wallet_id, limit)]

    # ---- denials / kill switch
    def add_denial(self, wallet_id: str, reason: str) -> int:
        now = time.time()
        self._x("INSERT INTO denials(wallet_id,at,reason) VALUES(?,?,?)", wallet_id, now, reason)
        r = self._q("SELECT COUNT(*) AS c FROM denials WHERE wallet_id=? AND at>=?", wallet_id, now - 3600)
        return int(r[0]["c"])

    # ---- approvals
    def add_approval(self, psbt_sha256: str, wallet_id: str, psbt: str, decoded: dict[str, Any],
                     request: dict[str, Any], ttl_seconds: int) -> None:
        self._x("INSERT OR REPLACE INTO approvals(psbt_sha256,wallet_id,psbt,decoded,request,expires_at) VALUES(?,?,?,?,?,?)",
                psbt_sha256, wallet_id, psbt, canonical(decoded), canonical(request), time.time() + ttl_seconds)

    def approval(self, psbt_sha256: str) -> dict[str, Any] | None:
        r = self._q("SELECT * FROM approvals WHERE psbt_sha256=?", psbt_sha256)
        if not r:
            return None
        d = dict(r[0]); d["decoded"] = json.loads(d["decoded"]); d["request"] = json.loads(d["request"])
        return d

    def approve(self, psbt_sha256: str) -> bool:
        with self._lock:
            cur = self._db.execute("UPDATE approvals SET approved=1, approved_at=? WHERE psbt_sha256=? AND consumed=0 AND expires_at>?",
                                   (time.time(), psbt_sha256, time.time()))
            return cur.rowcount == 1

    def consume_approval(self, psbt_sha256: str) -> None:
        self._x("UPDATE approvals SET consumed=1 WHERE psbt_sha256=?", psbt_sha256)

    def pending_approvals(self, wallet_id: str | None = None) -> list[dict[str, Any]]:
        rows = self._q("SELECT * FROM approvals WHERE approved=0 AND consumed=0 AND expires_at>?" + (" AND wallet_id=?" if wallet_id else ""),
                       *( [time.time(), wallet_id] if wallet_id else [time.time()] ))
        out = []
        for r in rows:
            d = dict(r); d["decoded"] = json.loads(d["decoded"]); d["request"] = json.loads(d["request"]); d.pop("psbt", None); out.append(d)
        return out

    # ---- swaps (state machine persisted; Phase 2)
    def put_swap(self, swap_id: str, wallet_id: str, state: str, data: dict[str, Any]) -> None:
        now = time.time()
        self._x("INSERT INTO swaps(swap_id,wallet_id,state,data,created_at,updated_at) VALUES(?,?,?,?,?,?) "
                "ON CONFLICT(swap_id) DO UPDATE SET state=excluded.state, data=excluded.data, updated_at=excluded.updated_at",
                swap_id, wallet_id, state, canonical(data), now, now)

    def swap(self, swap_id: str) -> dict[str, Any] | None:
        r = self._q("SELECT * FROM swaps WHERE swap_id=?", swap_id)
        if not r:
            return None
        d = dict(r[0]); d["data"] = json.loads(d["data"]); return d

    def open_swaps(self, wallet_id: str) -> int:
        r = self._q("SELECT COUNT(*) AS c FROM swaps WHERE wallet_id=? AND state NOT IN ('claimed','refunded','cancelled')", wallet_id)
        return int(r[0]["c"])

    # ---- audit chain (append-only, hash-linked; secrets never enter payloads)
    def audit(self, kind: str, payload: dict[str, Any], wallet_id: str | None = None, actor: str | None = None) -> str:
        with self._lock:
            last = self._q("SELECT hash FROM audit ORDER BY seq DESC LIMIT 1")
            prev = last[0]["hash"] if last else "0" * 64
            at = time.time()
            body = canonical({"at": at, "kind": kind, "wallet_id": wallet_id, "actor": actor, "payload": payload, "prev": prev})
            h = hashlib.sha256(body.encode()).hexdigest()
            self._db.execute("INSERT INTO audit(at,kind,wallet_id,actor,payload,prev_hash,hash) VALUES(?,?,?,?,?,?,?)",
                             (at, kind, wallet_id, actor, canonical(payload), prev, h))
            return h

    def audit_export(self, since_seq: int = 0, limit: int = 1000) -> list[dict[str, Any]]:
        rows = self._q("SELECT * FROM audit WHERE seq>? ORDER BY seq LIMIT ?", since_seq, limit)
        out = []
        for r in rows:
            d = dict(r); d["payload"] = json.loads(d["payload"]); out.append(d)
        return out

    def audit_verify(self) -> tuple[bool, int]:
        prev = "0" * 64
        n = 0
        for r in self._q("SELECT * FROM audit ORDER BY seq"):
            body = canonical({"at": r["at"], "kind": r["kind"], "wallet_id": r["wallet_id"], "actor": r["actor"],
                              "payload": json.loads(r["payload"]), "prev": prev})
            if r["prev_hash"] != prev or hashlib.sha256(body.encode()).hexdigest() != r["hash"]:
                return False, n
            prev = r["hash"]; n += 1
        return True, n
