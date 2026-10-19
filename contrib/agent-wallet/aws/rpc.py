"""JSON-RPC client for a local qtcd (cookie authentication, loopback only)."""
from __future__ import annotations

import base64
import json
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


class RpcError(Exception):
    def __init__(self, code: int, message: str):
        super().__init__(f"rpc error {code}: {message}")
        self.code = code
        self.message = message


class NodeUnavailable(Exception):
    pass


class Rpc:
    """rpc(method, *params) -> result. Use rpc.wallet(name) for wallet-scoped calls."""

    def __init__(self, url: str, cookie_path: str | None = None, user: str | None = None,
                 password: str | None = None, timeout: float = 120.0):
        self.url = url.rstrip("/")
        self.cookie_path = cookie_path
        self._auth = (user, password) if user and password else None
        self.timeout = timeout
        self._id = 0

    def _auth_header(self) -> str:
        if self._auth:
            u, p = self._auth
        else:
            if not self.cookie_path or not Path(self.cookie_path).exists():
                raise NodeUnavailable("rpc cookie not found: %s" % self.cookie_path)
            u, p = Path(self.cookie_path).read_text().strip().split(":", 1)
        return "Basic " + base64.b64encode(f"{u}:{p}".encode()).decode()

    def wallet(self, name: str) -> "Rpc":
        w = Rpc(f"{self.url}/wallet/{name}", self.cookie_path,
                self._auth[0] if self._auth else None, self._auth[1] if self._auth else None, self.timeout)
        return w

    def __call__(self, method: str, *params: Any) -> Any:
        self._id += 1
        body = json.dumps({"jsonrpc": "2.0", "id": self._id, "method": method, "params": list(params)}).encode()
        req = urllib.request.Request(self.url, data=body, method="POST",
                                     headers={"Content-Type": "application/json", "Authorization": self._auth_header()})
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                payload = json.loads(resp.read().decode())
        except urllib.error.HTTPError as e:
            if e.code == 403:  # rpcwhitelist denial: the node answers 403 with no JSON body
                raise RpcError(-403, f"method {method} is not in this user's rpcwhitelist") from e
            if e.code == 401:
                raise RpcError(-401, "rpc authentication failed") from e
            try:
                payload = json.loads(e.read().decode())
            except Exception:
                raise NodeUnavailable(f"http {e.code} from node") from e
        except (urllib.error.URLError, ConnectionError, TimeoutError) as e:
            raise NodeUnavailable(str(e)) from e
        if payload.get("error"):
            err = payload["error"]
            raise RpcError(int(err.get("code", -1)), str(err.get("message", "")))
        return payload.get("result")
