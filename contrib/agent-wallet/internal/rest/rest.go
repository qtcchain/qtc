// Package rest: the REST API (spec §9), mirroring the MCP tools one-to-one under /v1/ with bearer auth
// and stable error codes. Loopback only.
package rest

import (
	"io"
	"net"
	"net/http"
	"strconv"
	"time"

	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/errs"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/jsonx"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/service"
)

type handler func(s *service.Service, a service.Auth, b map[string]any) (any, error)

func optInt(b map[string]any, k string) (*int64, error) {
	v, ok := b[k]
	if !ok || v == nil {
		return nil, nil
	}
	if !jsonx.IsIntegral(v) {
		return nil, errs.Bad("bad_request", "%s must be an integer in atoms", k)
	}
	i, _ := jsonx.Int(v)
	return &i, nil
}

func reqStr(b map[string]any, k string) (string, error) {
	s, ok := b[k].(string)
	if !ok || s == "" {
		return "", errs.Bad("bad_request", "%s is required", k)
	}
	return s, nil
}

var routes = map[string]handler{
	"/v1/wallet/create": func(s *service.Service, a service.Auth, b map[string]any) (any, error) {
		id, err := reqStr(b, "agent_id")
		if err != nil {
			return nil, err
		}
		var pol map[string]any
		if p, ok := b["policy"].(map[string]any); ok {
			pol = p
		}
		return s.WalletCreate(a, id, int(jsonx.I(b, "tier", 1)), jsonx.Strs(b, "owner_mldsa"), pol, jsonx.I(b, "cooldown_blocks", 1008))
	},
	"/v1/wallet/receive_address": func(s *service.Service, a service.Auth, b map[string]any) (any, error) {
		id, err := reqStr(b, "wallet_id")
		if err != nil {
			return nil, err
		}
		return s.WalletReceiveAddress(a, id, jsonx.S(b, "label"))
	},
	"/v1/wallet/balance": func(s *service.Service, a service.Auth, b map[string]any) (any, error) {
		id, err := reqStr(b, "wallet_id")
		if err != nil {
			return nil, err
		}
		return s.WalletBalance(a, id)
	},
	"/v1/wallet/history": func(s *service.Service, a service.Auth, b map[string]any) (any, error) {
		id, err := reqStr(b, "wallet_id")
		if err != nil {
			return nil, err
		}
		before, err := optInt(b, "before")
		if err != nil {
			return nil, err
		}
		return s.WalletHistory(a, id, before, int(jsonx.I(b, "limit", 50)))
	},
	"/v1/wallet/send": func(s *service.Service, a service.Auth, b map[string]any) (any, error) {
		id, err := reqStr(b, "wallet_id")
		if err != nil {
			return nil, err
		}
		to, err := reqStr(b, "to")
		if err != nil {
			return nil, err
		}
		amount, err := optInt(b, "amount_atoms")
		if err != nil {
			return nil, err
		}
		maxFee, err := optInt(b, "max_fee_rate")
		if err != nil {
			return nil, err
		}
		return s.WalletSend(a, id, to, amount, jsonx.S(b, "purpose"), maxFee, jsonx.B(b, "send_max", false))
	},
	"/v1/wallet/send_status": func(s *service.Service, a service.Auth, b map[string]any) (any, error) {
		id, err := reqStr(b, "wallet_id")
		if err != nil {
			return nil, err
		}
		h, err := reqStr(b, "psbt_sha256")
		if err != nil {
			return nil, err
		}
		return s.WalletSendStatus(a, id, h)
	},
	"/v1/wallet/freeze": func(s *service.Service, a service.Auth, b map[string]any) (any, error) {
		id, err := reqStr(b, "wallet_id")
		if err != nil {
			return nil, err
		}
		return s.WalletFreeze(a, id, jsonx.S(b, "reason"))
	},
	"/v1/policy/describe": func(s *service.Service, a service.Auth, b map[string]any) (any, error) {
		id, err := reqStr(b, "wallet_id")
		if err != nil {
			return nil, err
		}
		return s.PolicyDescribe(a, id)
	},
	"/v1/fees/estimate": func(s *service.Service, a service.Auth, b map[string]any) (any, error) {
		return s.FeesEstimate(a, jsonx.I(b, "target_blocks", 6))
	},
	"/v1/market/reference_price": func(s *service.Service, a service.Auth, b map[string]any) (any, error) {
		q, err := reqStr(b, "quote_asset")
		if err != nil {
			return nil, err
		}
		return s.MarketReferencePrice(a, q)
	},
	"/v1/market/quote": func(s *service.Service, a service.Auth, b map[string]any) (any, error) {
		id, err := reqStr(b, "wallet_id")
		if err != nil {
			return nil, err
		}
		side, err := reqStr(b, "side")
		if err != nil {
			return nil, err
		}
		q, err := reqStr(b, "quote_asset")
		if err != nil {
			return nil, err
		}
		amt, err := optInt(b, "amount_atoms")
		if err != nil || amt == nil {
			return nil, errs.Bad("bad_request", "amount_atoms is required")
		}
		return s.MarketQuote(a, id, side, q, *amt, jsonx.Strs(b, "desks"))
	},
	"/v1/trade/swap": func(s *service.Service, a service.Auth, b map[string]any) (any, error) {
		id, err := reqStr(b, "wallet_id")
		if err != nil {
			return nil, err
		}
		q, err := reqStr(b, "quote_id")
		if err != nil {
			return nil, err
		}
		return s.TradeSwap(a, id, q)
	},
	"/v1/trade/status": func(s *service.Service, a service.Auth, b map[string]any) (any, error) {
		id, err := reqStr(b, "swap_id")
		if err != nil {
			return nil, err
		}
		return s.TradeStatus(a, id)
	},
	"/v1/trade/cancel": func(s *service.Service, a service.Auth, b map[string]any) (any, error) {
		id, err := reqStr(b, "swap_id")
		if err != nil {
			return nil, err
		}
		return s.TradeCancel(a, id)
	},
}

// StatusFor maps a service error to (http status, body).
func StatusFor(err error) (int, map[string]any) {
	e := errs.As(err)
	if e == nil {
		return 500, map[string]any{"code": "internal", "reason": err.Error()}
	}
	body := map[string]any{"code": e.Code, "reason": e.Message}
	switch e.Kind {
	case errs.BadRequest:
		return 400, body
	case errs.Unauthorized:
		return 401, body
	case errs.ScopeDenied:
		return 403, map[string]any{"code": "scope_denied", "scope": e.Message}
	case errs.NotFound:
		return 404, body
	case errs.Conflict:
		return 409, body
	case errs.Frozen:
		return 423, map[string]any{"code": "frozen", "reason": e.Message}
	case errs.NodeUnavailable:
		return 503, map[string]any{"code": e.Code, "reason": e.Message}
	case errs.NodeError:
		return 502, map[string]any{"code": "node_error", "reason": e.Message}
	}
	return 500, body
}

// Handler builds the HTTP handler.
func Handler(svc *service.Service) http.Handler {
	send := func(w http.ResponseWriter, code int, body any) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		w.WriteHeader(code)
		_, _ = w.Write(jsonx.Compact(body))
	}
	auth := func(r *http.Request) *service.Auth {
		h := r.Header.Get("Authorization")
		if len(h) < 8 || h[:7] != "Bearer " {
			return nil
		}
		return svc.AuthFromToken(h[7:])
	}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		host, _, _ := net.SplitHostPort(r.RemoteAddr)
		if host != "127.0.0.1" && host != "::1" {
			send(w, 403, map[string]any{"code": "loopback_only"})
			return
		}
		if r.Method == http.MethodGet {
			switch r.URL.Path {
			case "/v1/status":
				send(w, 200, map[string]any{"service": "qtc-aws", "node": svc.NodeStatus()})
			case "/v1/audit":
				a := auth(r)
				if a == nil {
					send(w, 401, map[string]any{"code": "unauthorized"})
					return
				}
				if !a.Scopes["wallet:admin"] {
					send(w, 403, map[string]any{"code": "scope_denied"})
					return
				}
				since, _ := strconv.ParseInt(r.URL.Query().Get("since"), 10, 64)
				limit, _ := strconv.ParseInt(r.URL.Query().Get("limit"), 10, 64)
				if limit <= 0 {
					limit = 500
				}
				send(w, 200, map[string]any{"entries": svc.Store.AuditExport(since, limit)})
			default:
				send(w, 404, map[string]any{"code": "not_found"})
			}
			return
		}
		if r.Method != http.MethodPost {
			send(w, 404, map[string]any{"code": "not_found"})
			return
		}
		route, ok := routes[r.URL.Path]
		if !ok {
			send(w, 404, map[string]any{"code": "not_found"})
			return
		}
		a := auth(r)
		if a == nil {
			send(w, 401, map[string]any{"code": "unauthorized"})
			return
		}
		data, _ := io.ReadAll(http.MaxBytesReader(w, r.Body, 1<<20))
		body := map[string]any{}
		if len(data) > 0 {
			var v any
			if err := jsonx.Decode(data, &v); err != nil {
				send(w, 400, map[string]any{"code": "bad_request", "reason": "invalid JSON"})
				return
			}
			m, ok := v.(map[string]any)
			if !ok {
				send(w, 400, map[string]any{"code": "bad_request", "reason": "body must be an object"})
				return
			}
			body = m
		}
		out, err := route(svc, *a, body)
		if err != nil {
			code, b := StatusFor(err)
			send(w, code, b)
			return
		}
		code := 200
		var om map[string]any
		_ = jsonx.Decode(jsonx.Compact(out), &om)
		switch jsonx.S(om, "status") {
		case "needs_approval":
			code = 202
		case "denied":
			code = 409
			om["code"] = "policy_denied"
			out = om
		}
		send(w, code, out)
	})
}

// Serve listens on addr (loopback) and serves until the server is shut down.
func Serve(svc *service.Service, addr string) (*http.Server, net.Listener, error) {
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		return nil, nil, err
	}
	srv := &http.Server{Handler: Handler(svc), ReadHeaderTimeout: 10 * time.Second}
	go func() { _ = srv.Serve(ln) }()
	return srv, ln, nil
}
