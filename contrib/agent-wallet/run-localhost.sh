#!/usr/bin/env bash
# Launch the Agent Wallet Service (Go) on localhost against a private regtest node and a private PostgreSQL cluster.
#
#   ./run-localhost.sh            build, start node + postgres + co-signer + REST, create demo wallets, print how to call it
#   ./run-localhost.sh status     node height, service status, wallets
#   ./run-localhost.sh stop       clean shutdown (qtc-cli stop, pg_ctl stop, then the services)
#   ./run-localhost.sh reset      stop and delete the demo state (asks first)
#
# Everything binds to 127.0.0.1. The service talks to the node as the rpcauth user "aws", confined by
# rpcwhitelist.conf; funding and mining use the operator cookie. No secret appears in argv or in output.
# Set AWS_PG_DSN to use an existing PostgreSQL instead of the private cluster.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
QTC_BIN="${QTC_BIN:-$HERE/../../build/bin}"; [[ -x "$QTC_BIN/qtcd" ]] || QTC_BIN="$HOME/Documents/qtc/qtc/build/bin"
AWS_HOME="${AWS_HOME:-$HOME/.qtc-aws-localhost}"
NODE_DIR="$AWS_HOME/node"; CFG="$AWS_HOME/aws.json"; LOGS="$AWS_HOME/logs"; PIDS="$AWS_HOME/pids"; PGDIR="$AWS_HOME/pg"
RPC_PORT="${RPC_PORT:-28790}"; P2P_PORT="${P2P_PORT:-28791}"; REST="${REST_LISTEN:-127.0.0.1:8792}"; COSIGNER="${COSIGNER_LISTEN:-127.0.0.1:8793}"
PG_PORT="${PG_PORT:-28794}"
BIN="$AWS_HOME/bin/qtc-aws"
RPCAUTH_PY="$QTC_BIN/../../share/rpcauth/rpcauth.py"; [[ -f "$RPCAUTH_PY" ]] || RPCAUTH_PY="$HOME/Documents/qtc/qtc/share/rpcauth/rpcauth.py"
PGBIN=""; for c in /opt/homebrew/opt/postgresql@16/bin /opt/homebrew/bin /usr/local/bin /usr/lib/postgresql/16/bin /usr/bin; do [[ -x "$c/pg_ctl" ]] && PGBIN="$c" && break; done
export LC_ALL=C LANG=C   # macOS: the postmaster aborts when the locale environment is unset
cli() { "$QTC_BIN/qtc-cli" -regtest -datadir="$NODE_DIR" -rpcport="$RPC_PORT" "$@"; }
aws() { "$BIN" --config "$CFG" "$@"; }
log() { printf '\033[1;34m[aws]\033[0m %s\n' "$*"; }

build() {
  mkdir -p "$AWS_HOME/bin"
  (cd "$HERE" && go build -o "$BIN" ./cmd/qtc-aws)
  log "built $BIN"
}

start_pg() {
  if [[ -n "${AWS_PG_DSN:-}" ]]; then log "using external PostgreSQL"; return; fi
  [[ -n "$PGBIN" ]] || { echo "postgresql@16 not found (brew install postgresql@16) or set AWS_PG_DSN"; exit 1; }
  mkdir -p "$PGDIR" "$LOGS"
  if [[ ! -f "$PGDIR/pgdata/PG_VERSION" ]]; then
    # scram-sha-256 for every connection: other local users cannot reach the service's tables
    openssl rand -hex 24 > "$AWS_HOME/.pgpw.tmp"; chmod 600 "$AWS_HOME/.pgpw.tmp"
    "$PGBIN/initdb" -D "$PGDIR/pgdata" -U aws --auth=scram-sha-256 --pwfile="$AWS_HOME/.pgpw.tmp" -E UTF8 --no-locale >"$LOGS/initdb.log" 2>&1
  fi
  mkdir -p "$PGDIR/sock"
  if ! "$PGBIN/pg_ctl" -D "$PGDIR/pgdata" status >/dev/null 2>&1; then
    "$PGBIN/pg_ctl" -D "$PGDIR/pgdata" -o "-p $PG_PORT -k $PGDIR/sock -c listen_addresses=127.0.0.1" -l "$LOGS/postgres.log" -w start >/dev/null
  fi
  AWS_PG_DSN="postgres://aws@127.0.0.1:$PG_PORT/postgres?sslmode=disable"
  log "postgres up at 127.0.0.1:$PG_PORT (private cluster, scram-sha-256, password in the service keyring)"
}

start_node() {
  mkdir -p "$NODE_DIR" "$LOGS" "$PIDS"
  if [[ ! -f "$NODE_DIR/qtc.conf" ]]; then
    local pw; pw="$(openssl rand -hex 24)"
    local line; line="$(python3 -I "$RPCAUTH_PY" aws "$pw" | sed -n 's/^rpcauth=//p')"
    {
      echo "regtest=1"; echo "server=1"; echo "txindex=1"; echo "fallbackfee=0.0001"; echo "listen=0"
      echo "[regtest]"; echo "rpcport=$RPC_PORT"; echo "port=$P2P_PORT"; echo "rpcbind=127.0.0.1"; echo "rpcallowip=127.0.0.1"
      echo "rpcauth=$line"; grep -v '^#' "$HERE/rpcwhitelist.conf"
    } > "$NODE_DIR/qtc.conf"; chmod 600 "$NODE_DIR/qtc.conf"
    printf '%s\n' "$pw" > "$AWS_HOME/.rpcpw.tmp"; chmod 600 "$AWS_HOME/.rpcpw.tmp"
  fi
  if cli getblockcount >/dev/null 2>&1; then log "node already running"; return; fi
  "$QTC_BIN/qtcd" -datadir="$NODE_DIR" -daemonwait -printtoconsole=0 >"$LOGS/qtcd.out" 2>&1
  for _ in $(seq 1 60); do cli getblockcount >/dev/null 2>&1 && break; sleep 0.5; done
  log "regtest node up at 127.0.0.1:$RPC_PORT (height $(cli getblockcount))"
}

init_service() {
  if [[ ! -f "$CFG" ]]; then
    # stdin: line 1 = node rpc password, line 2 = postgres password (both land in the 0600 keyring, never argv)
    cat "$AWS_HOME/.rpcpw.tmp" "${AWS_HOME}/.pgpw.tmp" 2>/dev/null | aws init --data-dir "$AWS_HOME/data" --pg-dsn "$AWS_PG_DSN" \
      --rpc-url "http://127.0.0.1:$RPC_PORT" --chain regtest --listen "$REST" --cosigner-listen "$COSIGNER" --rpc-user aws --pg-password-stdin
    rm -f "${AWS_HOME:?}/.rpcpw.tmp" "${AWS_HOME:?}/.pgpw.tmp"
  fi
}

start_bg() {  # start_bg <name> <cmd...>: detached, stdin from /dev/null, output to logs/<name>.log, pid of the command itself
  local name="$1"; shift
  ( cd "$HERE"; nohup "$@" </dev/null >"$LOGS/$name.log" 2>&1 & echo $! > "$PIDS/$name" )
}

start_services() {
  if ! curl -fs "http://$COSIGNER/" -o /dev/null -X POST 2>/dev/null; then start_bg cosigner "$BIN" --config "$CFG" serve-cosigner; fi
  if ! curl -fs "http://$REST/v1/status" -o /dev/null 2>/dev/null; then start_bg rest "$BIN" --config "$CFG" serve-rest; fi
  for _ in $(seq 1 40); do curl -fs "http://$REST/v1/status" -o /dev/null 2>/dev/null && break; sleep 0.25; done
  log "co-signer on http://$COSIGNER  REST on http://$REST/v1/"
}

demo_wallets() {
  if ! cli listwallets | grep -q '"funder"'; then cli -named createwallet wallet_name=funder >/dev/null 2>&1 || cli loadwallet funder >/dev/null; fi
  local faddr; faddr="$(cli -rpcwallet=funder getnewaddress)"
  if (( $(cli getblockcount) < 110 )); then cli generatetoaddress 110 "$faddr" >/dev/null; log "mined 110 regtest blocks to the funder wallet"; fi
  if ! aws wallet list | grep -q '"agent-demo-tier1"'; then
    local dest; dest="$(cli -rpcwallet=funder getnewaddress)"
    aws policy template | python3 -c "import json,sys; d=json.load(sys.stdin); d['agent_id']='demo'; d['destinations']['allow']=['$dest']; d['limits']['per_tx_atoms']=200000000; d['limits']['per_day_atoms']=1000000000; d['approval']['require_above_atoms']=100000000; json.dump(d, open('$AWS_HOME/demo-policy.json','w'), indent=2)"
    aws wallet create --agent-id demo --tier 1 --policy "$AWS_HOME/demo-policy.json" > "$AWS_HOME/demo-wallet.json"
    local addr; addr="$(python3 -c "import json;print(json.load(open('$AWS_HOME/demo-wallet.json'))['receive_address'])")"
    cli -rpcwallet=funder sendtoaddress "$addr" 5 >/dev/null; cli generatetoaddress 1 "$faddr" >/dev/null
    log "Tier 1 wallet agent-demo-tier1 funded with 5 QTC; allowlisted destination: $dest"
    echo "$dest" > "$AWS_HOME/demo-dest.txt"
  fi
  if ! aws wallet list | grep -q '"agent-demo2-tier2"'; then
    cli -named createwallet wallet_name=owner-cold >/dev/null 2>&1 || cli loadwallet owner-cold >/dev/null 2>&1 || true
    local o1 o2 k1 k2 dest
    o1="$(cli -rpcwallet=owner-cold getnewaddress)"; o2="$(cli -rpcwallet=owner-cold getnewaddress)"
    k1="$(cli -rpcwallet=owner-cold exportpqkey "$o1" | python3 -c "import json,sys;print(json.load(sys.stdin)['key'])")"
    k2="$(cli -rpcwallet=owner-cold exportpqkey "$o2" | python3 -c "import json,sys;print(json.load(sys.stdin)['key'])")"
    dest="$(cat "$AWS_HOME/demo-dest.txt")"
    sed "s/\"agent_id\": \"demo\"/\"agent_id\": \"demo2\"/; s/\"tier\": 1/\"tier\": 2/" "$AWS_HOME/demo-policy.json" > "$AWS_HOME/demo2-policy.json"
    aws wallet create --agent-id demo2 --tier 2 --owner-mldsa "$k1" --owner-mldsa "$k2" --cooldown 1008 --policy "$AWS_HOME/demo2-policy.json" > "$AWS_HOME/demo2-wallet.json"
    local addr; addr="$(python3 -c "import json;print(json.load(open('$AWS_HOME/demo2-wallet.json'))['receive_address'])")"
    cli -rpcwallet=funder sendtoaddress "$addr" 5 >/dev/null; cli generatetoaddress 1 "$faddr" >/dev/null
    log "Tier 2 wallet agent-demo2-tier2 (2-of-2 agent+policy co-signer) funded with 5 QTC"
  fi
  if [[ ! -f "$AWS_HOME/agent-token.txt" ]]; then
    aws token issue --label demo-agent --scopes wallet:read,wallet:spend,market:read > "$AWS_HOME/agent-token.txt"; chmod 600 "$AWS_HOME/agent-token.txt"
    aws token issue --label owner-cli --scopes wallet:admin > "$AWS_HOME/owner-token.txt"; chmod 600 "$AWS_HOME/owner-token.txt"
  fi
}

show_usage() {
  local dest; dest="$(cat "$AWS_HOME/demo-dest.txt")"
  cat <<USAGE

Agent Wallet Service is running on localhost.
  REST       http://$REST/v1/      (agent token: $AWS_HOME/agent-token.txt, owner token: $AWS_HOME/owner-token.txt)
  co-signer  http://$COSIGNER/
  node       regtest, RPC 127.0.0.1:$RPC_PORT, datadir $NODE_DIR
  postgres   $AWS_PG_DSN
  binary     $BIN
  logs       $LOGS/

Try it (amounts are integer atoms, 1 QTC = 100000000):
  T=\$(cat $AWS_HOME/agent-token.txt)
  curl -s http://$REST/v1/status | jq
  curl -s -H "Authorization: Bearer \$T" -d '{"wallet_id":"agent-demo-tier1"}' http://$REST/v1/wallet/balance | jq
  curl -s -H "Authorization: Bearer \$T" -d '{"wallet_id":"agent-demo-tier1"}' http://$REST/v1/policy/describe | jq
  curl -s -H "Authorization: Bearer \$T" -d '{"wallet_id":"agent-demo-tier1","to":"$dest","amount_atoms":50000000,"purpose":"demo"}' http://$REST/v1/wallet/send | jq
  curl -s -H "Authorization: Bearer \$T" -d '{"wallet_id":"agent-demo2-tier2","to":"$dest","amount_atoms":50000000,"purpose":"co-signed"}' http://$REST/v1/wallet/send | jq
  # above the approval threshold (1 QTC) -> 202 needs_approval; the owner approves from the CLI:
  curl -s -H "Authorization: Bearer \$T" -d '{"wallet_id":"agent-demo-tier1","to":"$dest","amount_atoms":150000000,"purpose":"big"}' http://$REST/v1/wallet/send | jq
  $BIN --config $CFG approvals list
  $BIN --config $CFG approvals approve --psbt-sha256 <hash>
  $BIN --config $CFG audit verify
  # mine a block so spends confirm:
  $QTC_BIN/qtc-cli -regtest -datadir=$NODE_DIR -rpcport=$RPC_PORT -rpcwallet=funder -generate 1

MCP (stdio) for an agent runtime, e.g. Claude Desktop / Claude Code:
  {"mcpServers":{"qtc-agent-wallet":{"command":"$BIN","args":["--config","$CFG","mcp","--token","\$(cat $AWS_HOME/agent-token.txt)"]}}}

Stop:  $0 stop
USAGE
}

stop_all() {
  for s in rest cosigner; do
    if [[ -f "$PIDS/$s" ]] && kill -0 "$(cat "$PIDS/$s")" 2>/dev/null; then kill "$(cat "$PIDS/$s")"; log "stopped $s"; fi
    rm -f "${PIDS:?}/${s:?}"
  done
  if cli getblockcount >/dev/null 2>&1; then
    cli stop >/dev/null; log "qtcd stopping cleanly..."
    for _ in $(seq 1 120); do cli getblockcount >/dev/null 2>&1 || break; sleep 0.5; done
    log "node stopped"
  fi
  if [[ -z "${AWS_PG_DSN:-}" && -n "$PGBIN" && -f "$PGDIR/pgdata/PG_VERSION" ]] && "$PGBIN/pg_ctl" -D "$PGDIR/pgdata" status >/dev/null 2>&1; then
    "$PGBIN/pg_ctl" -D "$PGDIR/pgdata" -m fast -w stop >/dev/null; log "postgres stopped"
  fi
}

case "${1:-start}" in
  start) build; start_pg; start_node; init_service; start_services; demo_wallets; show_usage ;;
  status) cli getblockchaininfo | grep -E '"(chain|blocks)"'; curl -s "http://$REST/v1/status"; echo; aws status ;;
  stop) stop_all ;;
  reset) read -r -p "Delete $AWS_HOME (demo regtest chain, postgres cluster, wallets, keyring)? [y/N] " a; [[ "$a" == y ]] || exit 1; stop_all; rm -rf -- "${AWS_HOME:?}"; log "removed $AWS_HOME" ;;
  *) echo "usage: $0 [start|status|stop|reset]"; exit 2 ;;
esac
