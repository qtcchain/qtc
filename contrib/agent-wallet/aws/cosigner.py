"""Tier 2 policy co-signer (spec §3 Tier 2, §6.4).

A separate process with its own encrypted node wallet holding `policy_mldsa`. It signs a PSBT only
when it carries a valid signed policy decision whose PSBT hash matches, and after re-checking the
amounts and destinations against its own copy of the policy. Two independent evaluations must agree.

Decision envelopes are authenticated with HMAC-SHA256 over the canonical envelope using the
`decision_secret` shared between the AWS and the co-signer through the keyring. The spec names an
ML-DSA decision key; this reference implementation uses the HMAC envelope so it runs on a stock node
without a message-signing RPC, and the boundary is the same: the AWS cannot sign for the co-signer.
"""
from __future__ import annotations

import hashlib
import hmac
import json
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

from .config import Config, Keyring
from .policy import PolicyEngine
from .rpc import Rpc, RpcError
from .spend import psbt_sha256
from .store import Store, canonical
from .wallet import qtc_to_atoms


def make_envelope(decision_secret: str, psbt_sha: str, policy_version: int, amount_atoms: int, destinations: list[str],
                  ttl: int = 600) -> dict[str, Any]:
    env = {"psbt_sha256": psbt_sha, "policy_version": policy_version, "decision": "allow", "amount_atoms": amount_atoms,
           "destinations": destinations, "expires_at": int(time.time()) + ttl}
    env["mac"] = hmac.new(decision_secret.encode(), canonical({k: v for k, v in env.items() if k != "mac"}).encode(), hashlib.sha256).hexdigest()
    return env


def verify_envelope(decision_secret: str, env: dict[str, Any]) -> bool:
    body = {k: v for k, v in env.items() if k != "mac"}
    want = hmac.new(decision_secret.encode(), canonical(body).encode(), hashlib.sha256).hexdigest()
    return hmac.compare_digest(str(env.get("mac", "")), want) and env.get("decision") == "allow" and int(env.get("expires_at", 0)) > time.time()


class CosignerClient:
    """Used by the AWS: fetch the policy pubkey for an agent and request a partial signature."""

    def __init__(self, url: str, keyring: Keyring):
        self.url = url.rstrip("/")
        self.keyring = keyring

    def _post(self, path: str, body: dict[str, Any]) -> dict[str, Any]:
        req = urllib.request.Request(self.url + path, data=json.dumps(body).encode(), headers={"Content-Type": "application/json"}, method="POST")
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                return json.loads(r.read().decode())
        except urllib.error.HTTPError as e:
            raise RuntimeError(f"cosigner {e.code}: {e.read().decode()[:200]}") from e

    def pubkey(self, agent_id: str) -> str:
        return self._post("/pubkey", {"agent_id": agent_id})["pubkey"]

    def envelope(self, w: dict[str, Any], psbt: str, decoded: dict[str, Any], policy_version: int) -> dict[str, Any]:
        outs = [o for o in decoded["outputs"] if o["address"] != w["receive_address"]]  # change excluded
        return make_envelope(self.keyring.require("decision_secret"), psbt_sha256(psbt), policy_version,
                             sum(o["atoms"] for o in outs), [o["address"] for o in outs])

    def sign(self, agent_id: str, psbt: str, envelope: dict[str, Any]) -> str:
        return self._post("/sign", {"agent_id": agent_id, "psbt": psbt, "envelope": envelope})["psbt"]


class CosignerService:
    """The co-signer process. Own keyring (wallet passphrases + decision_secret + admin_secret copy for policy verification)."""

    def __init__(self, cfg: Config, rpc: Rpc, keyring: Keyring, store: Store, policy: PolicyEngine):
        self.cfg, self.rpc, self.keyring, self.store, self.policy = cfg, rpc, keyring, store, policy

    def _wallet_name(self, agent_id: str) -> str:
        return f"policy-{agent_id}"

    def pubkey(self, agent_id: str) -> str:
        name = self._wallet_name(agent_id)
        if name not in self.rpc("listwallets"):
            try:
                self.rpc("loadwallet", name)
            except RpcError:
                self.rpc("createwallet", name)
                pw = hashlib.sha256((self.keyring.ensure("cosigner_seed", 32) + name).encode()).hexdigest()
                self.rpc.wallet(name)("encryptwallet", pw)
                self.keyring.set(f"wallet:{name}", pw)
        key = self.store.meta_get(f"policy_pubkey:{agent_id}")
        if key:
            return key
        self.rpc.wallet(name)("walletpassphrase", self.keyring.require(f"wallet:{name}"), 60)
        try:
            addr = self.rpc.wallet(name)("getnewaddress", "policy-key", "p2mr")
            key = self.rpc.wallet(name)("exportpqkey", addr)["key"]
        finally:
            try:
                self.rpc.wallet(name)("walletlock")
            except RpcError:
                pass
        self.store.meta_set(f"policy_pubkey:{agent_id}", key)
        self.store.audit("cosigner.key", {"agent_id": agent_id, "pubkey_prefix": key[:16]}, None, "cosigner")
        return key

    def sign(self, agent_id: str, psbt: str, env: dict[str, Any]) -> str:
        if not verify_envelope(self.keyring.require("decision_secret"), env):
            self.store.audit("cosigner.reject", {"agent_id": agent_id, "reason": "bad_envelope"}, None, "cosigner")
            raise PermissionError("decision envelope invalid or expired")
        h = psbt_sha256(psbt)
        if h != env["psbt_sha256"]:
            self.store.audit("cosigner.reject", {"agent_id": agent_id, "reason": "psbt_hash_mismatch"}, None, "cosigner")
            raise PermissionError("psbt hash does not match the decision")
        wallet_id = f"agent-{agent_id}-tier2"
        doc = self.policy.load(wallet_id)  # own copy of the policy, verified against the admin secret
        if int(doc["version"]) != int(env["policy_version"]):
            raise PermissionError("policy version mismatch")
        # independent re-check of amounts and destinations against the PSBT itself
        d = self.rpc("decodepsbt", psbt)
        outs = {}
        for o in d["tx"]["vout"]:
            a = o["scriptPubKey"].get("address") or o["scriptPubKey"].get("hex")
            outs[a] = outs.get(a, 0) + qtc_to_atoms(o["value"])
        wrow = self.store.wallet(wallet_id) or {}
        change = wrow.get("receive_address")
        ext = {a: v for a, v in outs.items() if a != change}
        if sorted(ext) != sorted(env["destinations"]) or sum(ext.values()) != int(env["amount_atoms"]):
            self.store.audit("cosigner.reject", {"agent_id": agent_id, "reason": "outputs_disagree"}, None, "cosigner")
            raise PermissionError("PSBT outputs disagree with the decision envelope")
        lim, dest = doc["limits"], doc["destinations"]
        if sum(ext.values()) > lim["per_tx_atoms"]:
            raise PermissionError("per_tx_limit (cosigner)")
        if dest.get("mode") == "allowlist" and any(a not in dest.get("allow", []) for a in ext):
            raise PermissionError("destination_not_allowed (cosigner)")
        name = self._wallet_name(agent_id)
        self.pubkey(agent_id)  # ensures loaded
        self.rpc.wallet(name)("walletpassphrase", self.keyring.require(f"wallet:{name}"), 60)
        try:
            out = self.rpc.wallet(name)("walletprocesspsbt", psbt, {"finalize": False})["psbt"]
        finally:
            try:
                self.rpc.wallet(name)("walletlock")
            except RpcError:
                pass
        self.store.audit("cosigner.sign", {"agent_id": agent_id, "psbt_sha256": h, "amount_atoms": env["amount_atoms"]}, wallet_id, "cosigner")
        return out


def serve(svc: CosignerService, listen: str) -> ThreadingHTTPServer:
    host, port = listen.rsplit(":", 1)

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a: Any) -> None:  # quiet
            pass

        def _send(self, code: int, body: dict[str, Any]) -> None:
            data = json.dumps(body).encode()
            self.send_response(code); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(data)))
            self.end_headers(); self.wfile.write(data)

        def do_POST(self) -> None:  # noqa: N802
            if self.client_address[0] not in ("127.0.0.1", "::1"):
                return self._send(403, {"error": "loopback only"})
            n = int(self.headers.get("Content-Length", "0"))
            try:
                body = json.loads(self.rfile.read(n).decode() or "{}")
                if self.path == "/pubkey":
                    return self._send(200, {"pubkey": svc.pubkey(body["agent_id"])})
                if self.path == "/sign":
                    return self._send(200, {"psbt": svc.sign(body["agent_id"], body["psbt"], body["envelope"])})
                return self._send(404, {"error": "not found"})
            except PermissionError as e:
                return self._send(403, {"error": str(e)})
            except Exception as e:  # noqa: BLE001
                return self._send(500, {"error": str(e)[:300]})

    srv = ThreadingHTTPServer((host, int(port)), H)
    return srv
