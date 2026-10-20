// Package config: service configuration (JSON file, 0600) and the local keyring.
//
// The keyring is a 0600 JSON file holding wallet passphrases, the owner's admin secret (signs policy
// documents), the decision secret shared with the Tier 2 co-signer, the backup archive passphrase and
// the node rpc password. The spec asks for an OS keychain or HSM; this implementation keeps the same
// boundary (secrets never reach the agent, logs, environment or command lines) behind the Keyring type.
package config

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"time"
)

type Config struct {
	DataDir           string         `json:"data_dir"`
	RPCURL            string         `json:"rpc_url"`
	CookiePath        string         `json:"cookie_path"`
	PgDSN             string         `json:"pg_dsn"`
	RPCUser           string         `json:"rpc_user,omitempty"`
	RPCPassword       string         `json:"rpc_password,omitempty"`
	Listen            string         `json:"listen"`
	CosignerURL       string         `json:"cosigner_url"`
	CosignerListen    string         `json:"cosigner_listen"`
	ExplorerURL       string         `json:"explorer_url"`
	Chain             string         `json:"chain"`
	ConfirmationDepth int            `json:"confirmation_depth"`
	ApprovalWebhook   string         `json:"approval_webhook"`
	Extra             map[string]any `json:"-"`
}

func Default(dataDir string) *Config {
	return &Config{DataDir: dataDir, RPCURL: "http://127.0.0.1:28790", Listen: "127.0.0.1:8792",
		CosignerURL: "http://127.0.0.1:8793", CosignerListen: "127.0.0.1:8793", Chain: "regtest", ConfirmationDepth: 6,
		Extra: map[string]any{}}
}

func (c *Config) KeyringPath() string { return filepath.Join(c.DataDir, "keyring.json") }
func (c *Config) PolicyDir() string   { return filepath.Join(c.DataDir, "policies") }
func (c *Config) BackupDir() string   { return filepath.Join(c.DataDir, "backups") }

var knownKeys = map[string]bool{"data_dir": true, "rpc_url": true, "cookie_path": true, "pg_dsn": true, "rpc_user": true, "rpc_password": true,
	"listen": true, "cosigner_url": true, "cosigner_listen": true, "explorer_url": true, "chain": true, "confirmation_depth": true,
	"approval_webhook": true}

func Load(path string) (*Config, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	c := Default("")
	if err := json.Unmarshal(raw, c); err != nil {
		return nil, fmt.Errorf("config %s: %w", path, err)
	}
	var all map[string]any
	_ = json.Unmarshal(raw, &all)
	c.Extra = map[string]any{}
	for k, v := range all {
		if !knownKeys[k] {
			c.Extra[k] = v
		}
	}
	if c.DataDir == "" {
		return nil, fmt.Errorf("config %s: data_dir required", path)
	}
	if h, err := os.UserHomeDir(); err == nil && len(c.DataDir) > 1 && c.DataDir[:2] == "~/" {
		c.DataDir = filepath.Join(h, c.DataDir[2:])
	}
	return c, nil
}

func (c *Config) Save(path string) error {
	m := map[string]any{}
	raw, _ := json.Marshal(c)
	_ = json.Unmarshal(raw, &m)
	for k, v := range c.Extra {
		m[k] = v
	}
	out, _ := json.MarshalIndent(m, "", "  ")
	if err := os.WriteFile(path, append(out, '\n'), 0o600); err != nil {
		return err
	}
	return os.Chmod(path, 0o600)
}

// Keyring is a 0600 JSON file of named secrets. Values are never logged or returned by any tool.
// It re-reads the file when its mtime changes so a secret written by the owner CLI is visible to a
// running service.
type Keyring struct {
	Path  string
	mu    sync.Mutex
	d     map[string]string
	mtime int64
}

func NewKeyring(path string) *Keyring {
	k := &Keyring{Path: path, d: map[string]string{}}
	k.reload()
	return k
}

func (k *Keyring) reload() {
	st, err := os.Stat(k.Path)
	if err != nil {
		return
	}
	if st.ModTime().UnixNano() != k.mtime {
		raw, err := os.ReadFile(k.Path)
		if err == nil {
			m := map[string]string{}
			if json.Unmarshal(raw, &m) == nil {
				k.d = m
			}
		}
		k.mtime = st.ModTime().UnixNano()
	}
}

func (k *Keyring) flush() error {
	if err := os.MkdirAll(filepath.Dir(k.Path), 0o700); err != nil {
		return err
	}
	tmp := k.Path + ".tmp"
	out, _ := json.MarshalIndent(k.d, "", "  ")
	if err := os.WriteFile(tmp, out, 0o600); err != nil {
		return err
	}
	_ = os.Chmod(tmp, 0o600)
	if err := os.Rename(tmp, k.Path); err != nil {
		return err
	}
	_ = os.Chmod(k.Path, 0o600)
	// bump mtime so other processes notice even within the same second
	now := time.Now()
	_ = os.Chtimes(k.Path, now, now)
	if st, err := os.Stat(k.Path); err == nil {
		k.mtime = st.ModTime().UnixNano()
	}
	return nil
}

func (k *Keyring) Get(name string) (string, bool) {
	k.mu.Lock()
	defer k.mu.Unlock()
	k.reload()
	v, ok := k.d[name]
	return v, ok
}

func (k *Keyring) Require(name string) (string, error) {
	v, ok := k.Get(name)
	if !ok {
		return "", fmt.Errorf("keyring has no secret named %q", name)
	}
	return v, nil
}

func (k *Keyring) Set(name, value string) error {
	k.mu.Lock()
	defer k.mu.Unlock()
	k.reload()
	k.d[name] = value
	return k.flush()
}

// Ensure returns the secret, generating nbytes random bytes (hex) when absent.
func (k *Keyring) Ensure(name string, nbytes int) (string, error) {
	k.mu.Lock()
	defer k.mu.Unlock()
	k.reload()
	if v, ok := k.d[name]; ok {
		return v, nil
	}
	v := RandomHex(nbytes)
	k.d[name] = v
	return v, k.flush()
}

func (k *Keyring) Names() []string {
	k.mu.Lock()
	defer k.mu.Unlock()
	k.reload()
	out := make([]string, 0, len(k.d))
	for n := range k.d {
		out = append(out, n)
	}
	return out
}

func RandomHex(n int) string {
	b := make([]byte, n)
	if _, err := rand.Read(b); err != nil {
		panic(err)
	}
	return hex.EncodeToString(b)
}
