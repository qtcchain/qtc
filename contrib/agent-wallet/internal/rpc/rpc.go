// Package rpc is a minimal JSON-RPC client for qtcd (cookie or rpcauth), with per-wallet endpoints.
package rpc

import (
	"bytes"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"strings"
	"sync/atomic"
	"time"

	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/errs"
	"github.com/qtcchain/qtc/contrib/agent-wallet/internal/jsonx"
)

// Client talks to one URL (node or /wallet/<name>).
type Client struct {
	URL        string
	CookiePath string
	User, Pass string
	Timeout    time.Duration
	http       *http.Client
	id         *atomic.Int64
}

func New(url, cookiePath, user, pass string) *Client {
	c := &Client{URL: strings.TrimRight(url, "/"), CookiePath: cookiePath, User: user, Pass: pass, Timeout: 120 * time.Second, id: new(atomic.Int64)}
	c.http = &http.Client{Timeout: c.Timeout}
	return c
}

// Wallet returns a client bound to /wallet/<name>.
func (c *Client) Wallet(name string) *Client {
	w := *c
	w.URL = c.URL + "/wallet/" + name
	return &w
}

func (c *Client) authHeader() (string, error) {
	u, p := c.User, c.Pass
	if u == "" || p == "" {
		if c.CookiePath == "" {
			return "", errs.Unavailable("no rpc credentials configured")
		}
		raw, err := os.ReadFile(c.CookiePath)
		if err != nil {
			return "", errs.Unavailable("rpc cookie not found: %s", c.CookiePath)
		}
		parts := strings.SplitN(strings.TrimSpace(string(raw)), ":", 2)
		if len(parts) != 2 {
			return "", errs.Unavailable("malformed rpc cookie")
		}
		u, p = parts[0], parts[1]
	}
	return "Basic " + base64.StdEncoding.EncodeToString([]byte(u+":"+p)), nil
}

// Call performs method(params...) and decodes the result into a JSON-ish value (numbers as json.Number).
func (c *Client) Call(method string, params ...any) (any, error) {
	if params == nil {
		params = []any{}
	}
	body, _ := json.Marshal(map[string]any{"jsonrpc": "2.0", "id": c.id.Add(1), "method": method, "params": params})
	req, err := http.NewRequest("POST", c.URL, bytes.NewReader(body))
	if err != nil {
		return nil, errs.Unavailable("%v", err)
	}
	auth, err := c.authHeader()
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Authorization", auth)
	resp, err := c.http.Do(req)
	if err != nil {
		var ne net.Error
		if errors.As(err, &ne) || true {
			return nil, errs.Unavailable("%v", err)
		}
	}
	defer resp.Body.Close()
	data, _ := io.ReadAll(resp.Body)
	switch resp.StatusCode {
	case http.StatusForbidden: // rpcwhitelist denial: 403 with no JSON body
		return nil, errs.RPC(-403, fmt.Sprintf("method %s is not in this user's rpcwhitelist", method))
	case http.StatusUnauthorized:
		return nil, errs.RPC(-401, "rpc authentication failed")
	}
	var payload struct {
		Result json.RawMessage `json:"result"`
		Error  *struct {
			Code    int    `json:"code"`
			Message string `json:"message"`
		} `json:"error"`
	}
	if err := json.Unmarshal(data, &payload); err != nil {
		return nil, errs.Unavailable("http %d from node", resp.StatusCode)
	}
	if payload.Error != nil {
		return nil, errs.RPC(payload.Error.Code, payload.Error.Message)
	}
	var out any
	if len(payload.Result) > 0 {
		if err := jsonx.Decode(payload.Result, &out); err != nil {
			return nil, errs.Unavailable("bad rpc result: %v", err)
		}
	}
	return out, nil
}

// Map calls and asserts an object result.
func (c *Client) Map(method string, params ...any) (map[string]any, error) {
	r, err := c.Call(method, params...)
	if err != nil {
		return nil, err
	}
	m, ok := r.(map[string]any)
	if !ok {
		return nil, errs.Unavailable("%s: expected object result", method)
	}
	return m, nil
}

// List calls and asserts an array result.
func (c *Client) List(method string, params ...any) ([]any, error) {
	r, err := c.Call(method, params...)
	if err != nil {
		return nil, err
	}
	l, ok := r.([]any)
	if !ok {
		return nil, errs.Unavailable("%s: expected array result", method)
	}
	return l, nil
}

// String calls and asserts a string result.
func (c *Client) String(method string, params ...any) (string, error) {
	r, err := c.Call(method, params...)
	if err != nil {
		return "", err
	}
	s, ok := r.(string)
	if !ok {
		return "", errs.Unavailable("%s: expected string result", method)
	}
	return s, nil
}

// Int calls and asserts an integer result.
func (c *Client) Int(method string, params ...any) (int64, error) {
	r, err := c.Call(method, params...)
	if err != nil {
		return 0, err
	}
	i, ok := jsonx.Int(r)
	if !ok {
		return 0, errs.Unavailable("%s: expected integer result", method)
	}
	return i, nil
}
