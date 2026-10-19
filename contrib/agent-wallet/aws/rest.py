"""REST API (spec §9): mirrors the MCP tools one-to-one under /v1/ with bearer auth and stable error codes."""
from __future__ import annotations

import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Callable
from urllib.parse import parse_qs, urlparse

from .policy import PolicyError
from .rpc import NodeUnavailable, RpcError
from .service import AgentWalletService, Auth, Frozen, ScopeDenied
from .spend import SpendError
from .wallet import WalletError

Handler = Callable[[AgentWalletService, Auth, dict[str, Any]], dict[str, Any]]

ROUTES: dict[tuple[str, str], Handler] = {
    ("POST", "/v1/wallet/create"): lambda s, a, b: s.wallet_create(a, b["agent_id"], int(b.get("tier", 1)), b.get("owner_mldsa"), b.get("policy"), int(b.get("cooldown_blocks", 1008))),
    ("POST", "/v1/wallet/receive_address"): lambda s, a, b: s.wallet_receive_address(a, b["wallet_id"], b.get("label")),
    ("POST", "/v1/wallet/balance"): lambda s, a, b: s.wallet_balance(a, b["wallet_id"]),
    ("POST", "/v1/wallet/history"): lambda s, a, b: s.wallet_history(a, b["wallet_id"], b.get("before"), int(b.get("limit", 50))),
    ("POST", "/v1/wallet/send"): lambda s, a, b: s.wallet_send(a, b["wallet_id"], b["to"], b.get("amount_atoms"), b.get("purpose", ""), b.get("max_fee_rate"), bool(b.get("send_max", False))),
    ("POST", "/v1/wallet/send_status"): lambda s, a, b: s.wallet_send_status(a, b["wallet_id"], b["psbt_sha256"]),
    ("POST", "/v1/wallet/freeze"): lambda s, a, b: s.wallet_freeze(a, b["wallet_id"], b.get("reason", "")),
    ("POST", "/v1/policy/describe"): lambda s, a, b: s.policy_describe(a, b["wallet_id"]),
    ("POST", "/v1/fees/estimate"): lambda s, a, b: s.fees_estimate(a, int(b.get("target_blocks", 6))),
    ("POST", "/v1/market/reference_price"): lambda s, a, b: s.market_reference_price(a, b["quote_asset"]),
    ("POST", "/v1/market/quote"): lambda s, a, b: s.market_quote(a, b["wallet_id"], b["side"], b["quote_asset"], int(b["amount_atoms"]), b.get("desks")),
    ("POST", "/v1/trade/swap"): lambda s, a, b: s.trade_swap(a, b["wallet_id"], b["quote_id"]),
    ("POST", "/v1/trade/status"): lambda s, a, b: s.trade_status(a, b["swap_id"]),
    ("POST", "/v1/trade/cancel"): lambda s, a, b: s.trade_cancel(a, b["swap_id"]),
}


def serve(svc: AgentWalletService, listen: str) -> ThreadingHTTPServer:
    host, port = listen.rsplit(":", 1)

    class H(BaseHTTPRequestHandler):
        server_version = "qtc-aws/0.1"

        def log_message(self, *a: Any) -> None:
            pass

        def _send(self, code: int, body: dict[str, Any]) -> None:
            data = json.dumps(body).encode()
            self.send_response(code)
            self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store"); self.end_headers(); self.wfile.write(data)

        def _auth(self) -> Auth | None:
            h = self.headers.get("Authorization", "")
            if not h.startswith("Bearer "):
                return None
            return svc.auth_from_token(h[7:].strip())

        def do_GET(self) -> None:  # noqa: N802
            u = urlparse(self.path)
            if u.path == "/v1/status":
                return self._send(200, {"service": "qtc-aws", "node": svc.node_status()})
            if u.path == "/v1/audit":
                auth = self._auth()
                if not auth:
                    return self._send(401, {"code": "unauthorized"})
                if "wallet:admin" not in auth.scopes:
                    return self._send(403, {"code": "scope_denied"})
                q = parse_qs(u.query)
                return self._send(200, {"entries": svc.store.audit_export(int(q.get("since", ["0"])[0]), int(q.get("limit", ["500"])[0]))})
            return self._send(404, {"code": "not_found"})

        def do_POST(self) -> None:  # noqa: N802
            if self.client_address[0] not in ("127.0.0.1", "::1"):
                return self._send(403, {"code": "loopback_only"})
            route = ROUTES.get(("POST", urlparse(self.path).path))
            if not route:
                return self._send(404, {"code": "not_found"})
            auth = self._auth()
            if not auth:
                return self._send(401, {"code": "unauthorized"})
            try:
                n = int(self.headers.get("Content-Length", "0"))
                raw = self.rfile.read(n).decode() if n else "{}"
                body = json.loads(raw)
                if not isinstance(body, dict):
                    raise ValueError("body must be an object")
                for k in ("amount_atoms", "max_fee_rate"):
                    if k in body and body[k] is not None and (isinstance(body[k], float) or isinstance(body[k], bool)):
                        raise ValueError(f"{k} must be an integer in atoms")
                out = route(svc, auth, body)
                code = 200
                if out.get("status") == "needs_approval":
                    code = 202
                elif out.get("status") == "denied":
                    code = 409; out = {"code": "policy_denied", **out}
                return self._send(code, out)
            except ScopeDenied as e:
                return self._send(403, {"code": "scope_denied", "scope": str(e)})
            except Frozen as e:
                return self._send(423, {"code": "frozen", "reason": str(e)})
            except (KeyError, ValueError, TypeError) as e:
                return self._send(400, {"code": "bad_request", "reason": str(e)[:200]})
            except (WalletError, SpendError, PolicyError) as e:
                code = {"unknown_wallet": 404, "security_alert": 409, "mempool_reject": 409, "destination_not_allowed": 409}.get(e.code, 400)
                return self._send(code, {"code": e.code, "reason": str(e)[:300]})
            except NodeUnavailable as e:
                return self._send(503, {"code": "node_unavailable", "reason": str(e)[:200]})
            except RpcError as e:
                return self._send(502, {"code": "node_error", "reason": e.message[:300]})

    return ThreadingHTTPServer((host, int(port)), H)
