// Package testpg starts a throwaway PostgreSQL cluster (initdb + pg_ctl) on a free port for tests and the
// localhost launcher, so neither ever touches an operator's database. Set AWS_PG_DSN to reuse a server.
package testpg

import (
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

type Cluster struct {
	Dir  string
	Port int
	DSN  string
	bin  string
}

func FreePort() int {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		panic(err)
	}
	defer l.Close()
	return l.Addr().(*net.TCPAddr).Port
}

func pgBin(name string) string {
	for _, cand := range []string{"/opt/homebrew/opt/postgresql@16/bin", "/opt/homebrew/bin", "/usr/local/bin", "/usr/lib/postgresql/16/bin", "/usr/bin"} {
		if _, err := os.Stat(filepath.Join(cand, name)); err == nil {
			return filepath.Join(cand, name)
		}
	}
	p, _ := exec.LookPath(name)
	return p
}

// Start creates and starts a cluster under dir. If AWS_PG_DSN is set, no cluster is started and that DSN is returned.
func Start(dir string) (*Cluster, error) {
	if dsn := os.Getenv("AWS_PG_DSN"); dsn != "" {
		return &Cluster{DSN: dsn}, nil
	}
	initdb, pgctl := pgBin("initdb"), pgBin("pg_ctl")
	if initdb == "" || pgctl == "" {
		return nil, fmt.Errorf("initdb/pg_ctl not found; install postgresql@16 or set AWS_PG_DSN")
	}
	data := filepath.Join(dir, "pgdata")
	if _, err := os.Stat(filepath.Join(data, "PG_VERSION")); err != nil {
		cmd := exec.Command(initdb, "-D", data, "-U", "aws", "--auth=trust", "-E", "UTF8", "--no-locale")
		cmd.Env = pgEnv()
		if out, err := cmd.CombinedOutput(); err != nil {
			return nil, fmt.Errorf("initdb: %v\n%s", err, out)
		}
	}
	port := FreePort()
	c := &Cluster{Dir: data, Port: port, bin: pgctl}
	sock := filepath.Join(dir, "pgsock")
	_ = os.MkdirAll(sock, 0o700)
	opts := fmt.Sprintf("-p %d -k %s -c listen_addresses=127.0.0.1 -c fsync=off -c synchronous_commit=off", port, sock)
	cmd := exec.Command(pgctl, "-D", data, "-o", opts, "-l", filepath.Join(dir, "postgres.log"), "-w", "start")
	cmd.Env = pgEnv()
	if out, err := cmd.CombinedOutput(); err != nil {
		return nil, fmt.Errorf("pg_ctl start: %v\n%s", err, out)
	}
	c.DSN = fmt.Sprintf("postgres://aws@127.0.0.1:%d/postgres?sslmode=disable", port)
	// wait for readiness
	for i := 0; i < 50; i++ {
		conn, err := net.DialTimeout("tcp", "127.0.0.1:"+strconv.Itoa(port), 200*time.Millisecond)
		if err == nil {
			conn.Close()
			return c, nil
		}
		time.Sleep(100 * time.Millisecond)
	}
	return c, nil
}

func (c *Cluster) Stop() {
	if c.bin == "" || c.Dir == "" {
		return
	}
	cmd := exec.Command(c.bin, "-D", c.Dir, "-m", "fast", "-w", "stop")
	cmd.Env = pgEnv()
	_ = cmd.Run()
}

// pgEnv pins LC_ALL: on macOS the postmaster aborts with "became multithreaded during startup" when the
// locale environment is unset or invalid.
func pgEnv() []string {
	env := []string{}
	for _, kv := range os.Environ() {
		if !strings.HasPrefix(kv, "LC_ALL=") && !strings.HasPrefix(kv, "LANG=") {
			env = append(env, kv)
		}
	}
	return append(env, "LC_ALL=C", "LANG=C")
}
