// Package rpc is a minimal JSON-RPC client for qtcd.
package rpc

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"strings"
	"sync/atomic"
	"time"
)

// Client talks to a single qtcd over HTTP JSON-RPC 1.0.
type Client struct {
	url    string
	user   string
	pass   string
	http   *http.Client
	nextID atomic.Int64
	// Allow restricts the client to the listed methods when non-nil.
	Allow map[string]bool
}

// New creates a client. cookiePath takes precedence over user/pass.
func New(url, cookiePath, user, pass string, timeout time.Duration) (*Client, error) {
	c := &Client{url: url, user: user, pass: pass, http: &http.Client{Timeout: timeout}}
	if cookiePath != "" {
		b, err := os.ReadFile(cookiePath)
		if err != nil {
			return nil, fmt.Errorf("read rpc cookie: %w", err)
		}
		parts := strings.SplitN(strings.TrimSpace(string(b)), ":", 2)
		if len(parts) != 2 {
			return nil, errors.New("malformed rpc cookie")
		}
		c.user, c.pass = parts[0], parts[1]
	}
	return c, nil
}

// Error is a JSON-RPC error returned by the node.
type Error struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

func (e *Error) Error() string { return fmt.Sprintf("rpc error %d: %s", e.Code, e.Message) }

type request struct {
	ID     int64  `json:"id"`
	Method string `json:"method"`
	Params []any  `json:"params"`
}

type response struct {
	ID     int64           `json:"id"`
	Result json.RawMessage `json:"result"`
	Error  *Error          `json:"error"`
}

// Call invokes method and unmarshals the result into out (may be nil).
func (c *Client) Call(ctx context.Context, out any, method string, params ...any) error {
	if c.Allow != nil && !c.Allow[method] {
		return fmt.Errorf("rpc method %q not permitted", method)
	}
	if params == nil {
		params = []any{}
	}
	body, err := json.Marshal(request{ID: c.nextID.Add(1), Method: method, Params: params})
	if err != nil {
		return err
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.url, bytes.NewReader(body))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	req.SetBasicAuth(c.user, c.pass)
	res, err := c.http.Do(req)
	if err != nil {
		return fmt.Errorf("rpc %s: %w", method, err)
	}
	defer res.Body.Close()
	raw, err := io.ReadAll(io.LimitReader(res.Body, 256<<20))
	if err != nil {
		return err
	}
	var r response
	if err := json.Unmarshal(raw, &r); err != nil {
		if res.StatusCode != http.StatusOK {
			return fmt.Errorf("rpc %s: http %d", method, res.StatusCode)
		}
		return fmt.Errorf("rpc %s: decode: %w", method, err)
	}
	if r.Error != nil {
		return r.Error
	}
	if out == nil {
		return nil
	}
	return json.Unmarshal(r.Result, out)
}

// ---- typed helpers -------------------------------------------------------

type ChainInfo struct {
	Chain         string  `json:"chain"`
	Blocks        int64   `json:"blocks"`
	Headers       int64   `json:"headers"`
	BestBlockHash string  `json:"bestblockhash"`
	Difficulty    float64 `json:"difficulty"`
	MedianTime    int64   `json:"mediantime"`
	IBD           bool    `json:"initialblockdownload"`
	Pruned        bool    `json:"pruned"`
}

func (c *Client) GetBlockchainInfo(ctx context.Context) (*ChainInfo, error) {
	var ci ChainInfo
	return &ci, c.Call(ctx, &ci, "getblockchaininfo")
}

func (c *Client) GetBestBlockHash(ctx context.Context) (string, error) {
	var h string
	return h, c.Call(ctx, &h, "getbestblockhash")
}

func (c *Client) GetBlockHash(ctx context.Context, height int64) (string, error) {
	var h string
	return h, c.Call(ctx, &h, "getblockhash", height)
}

// Block is getblock verbosity 2 as qtcd returns it, including the MatMul header fields.
type Block struct {
	Hash              string  `json:"hash"`
	Confirmations     int64   `json:"confirmations"`
	Height            int64   `json:"height"`
	Version           int64   `json:"version"`
	VersionHex        string  `json:"versionHex"`
	MerkleRoot        string  `json:"merkleroot"`
	Time              int64   `json:"time"`
	MedianTime        int64   `json:"mediantime"`
	Bits              string  `json:"bits"`
	Target            string  `json:"target"`
	Difficulty        float64 `json:"difficulty"`
	ChainWork         string  `json:"chainwork"`
	NTx               int64   `json:"nTx"`
	PreviousBlockHash string  `json:"previousblockhash"`
	NextBlockHash     string  `json:"nextblockhash"`
	Nonce64           string  `json:"nonce64"`
	MatmulDigest      string  `json:"matmul_digest"`
	MatmulDim         int64   `json:"matmul_dim"`
	SeedA             string  `json:"seed_a"`
	SeedB             string  `json:"seed_b"`
	MatrixCWords      int64   `json:"matrix_c_words"`
	StrippedSize      int64   `json:"strippedsize"`
	Size              int64   `json:"size"`
	Weight            int64   `json:"weight"`
	Tx                []Tx    `json:"tx"`
}

func (c *Client) GetBlock(ctx context.Context, hash string) (*Block, error) {
	var b Block
	return &b, c.Call(ctx, &b, "getblock", hash, 2)
}

// Tx is a decoded transaction (getblock verbosity 2 / getrawtransaction verbose).
type Tx struct {
	Txid                string  `json:"txid"`
	Hash                string  `json:"hash"`
	Version             int64   `json:"version"`
	Size                int64   `json:"size"`
	VSize               int64   `json:"vsize"`
	Weight              int64   `json:"weight"`
	Locktime            int64   `json:"locktime"`
	Vin                 []Vin   `json:"vin"`
	Vout                []Vout  `json:"vout"`
	Fee                 float64 `json:"fee"`
	Hex                 string  `json:"hex"`
	Shielded            bool    `json:"shielded"`
	ShieldedInputCount  int64   `json:"shielded_input_count"`
	ShieldedOutputCount int64   `json:"shielded_output_count"`
	// Present only on getrawtransaction verbose for confirmed txs.
	BlockHash string `json:"blockhash"`
	BlockTime int64  `json:"blocktime"`
}

type Vin struct {
	Coinbase  string    `json:"coinbase"`
	Txid      string    `json:"txid"`
	Vout      int64     `json:"vout"`
	ScriptSig ScriptSig `json:"scriptSig"`
	Witness   []string  `json:"txinwitness"`
	Sequence  int64     `json:"sequence"`
	Nullifier string    `json:"nullifier"`
	NoteClass string    `json:"note_class"`
}

type ScriptSig struct {
	Asm string `json:"asm"`
	Hex string `json:"hex"`
}

type Vout struct {
	Value          float64      `json:"value"`
	N              int64        `json:"n"`
	ScriptPubKey   ScriptPubKey `json:"scriptPubKey"`
	NoteCommitment string       `json:"note_commitment"`
}

type ScriptPubKey struct {
	Asm     string `json:"asm"`
	Desc    string `json:"desc"`
	Hex     string `json:"hex"`
	Address string `json:"address"`
	Type    string `json:"type"`
}

func (c *Client) GetRawTransaction(ctx context.Context, txid string) (*Tx, error) {
	var t Tx
	return &t, c.Call(ctx, &t, "getrawtransaction", txid, true)
}

// MempoolEntry is one entry of getrawmempool true.
type MempoolEntry struct {
	VSize         int64    `json:"vsize"`
	Weight        int64    `json:"weight"`
	Time          int64    `json:"time"`
	Height        int64    `json:"height"`
	AncestorCount int64    `json:"ancestorcount"`
	Depends       []string `json:"depends"`
	Fees          struct {
		Base float64 `json:"base"`
	} `json:"fees"`
}

func (c *Client) GetRawMempool(ctx context.Context) (map[string]MempoolEntry, error) {
	m := map[string]MempoolEntry{}
	return m, c.Call(ctx, &m, "getrawmempool", true)
}

type MempoolInfo struct {
	Size          int64   `json:"size"`
	Bytes         int64   `json:"bytes"`
	Usage         int64   `json:"usage"`
	TotalFee      float64 `json:"total_fee"`
	MempoolMinFee float64 `json:"mempoolminfee"`
}

func (c *Client) GetMempoolInfo(ctx context.Context) (*MempoolInfo, error) {
	var mi MempoolInfo
	return &mi, c.Call(ctx, &mi, "getmempoolinfo")
}

// GetDifficultyHealth returns the raw JSON of getdifficultyhealth so the API can pass it through.
func (c *Client) GetDifficultyHealth(ctx context.Context, window int) (json.RawMessage, error) {
	var raw json.RawMessage
	return raw, c.Call(ctx, &raw, "getdifficultyhealth", window)
}

type MempoolAccept struct {
	Txid         string `json:"txid"`
	Allowed      bool   `json:"allowed"`
	RejectReason string `json:"reject-reason"`
}

func (c *Client) TestMempoolAccept(ctx context.Context, hex string) (*MempoolAccept, error) {
	var out []MempoolAccept
	if err := c.Call(ctx, &out, "testmempoolaccept", []string{hex}); err != nil {
		return nil, err
	}
	if len(out) != 1 {
		return nil, errors.New("testmempoolaccept: unexpected result shape")
	}
	return &out[0], nil
}

func (c *Client) SendRawTransaction(ctx context.Context, hex string) (string, error) {
	var txid string
	return txid, c.Call(ctx, &txid, "sendrawtransaction", hex)
}

type SmartFee struct {
	FeeRate float64 `json:"feerate"`
	Blocks  int64   `json:"blocks"`
}

func (c *Client) EstimateSmartFee(ctx context.Context, target int) (*SmartFee, error) {
	var sf SmartFee
	return &sf, c.Call(ctx, &sf, "estimatesmartfee", target)
}

// Atoms converts a QTC float amount from RPC to integer atoms (1e-8 QTC).
func Atoms(v float64) int64 {
	if v >= 0 {
		return int64(v*1e8 + 0.5)
	}
	return -int64(-v*1e8 + 0.5)
}
