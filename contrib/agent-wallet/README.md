# contrib/agent-wallet — QTC Agent Wallet Service (AWS)

Reference implementation of [`docs/qtc-agent-wallet-spec.md`](../../docs/qtc-agent-wallet-spec.md) in Go:
one static binary, `qtc-aws`, that sits between an AI agent and a local `qtcd`, owns the keys, enforces
the owner's policy, keeps a hash-chained audit log in PostgreSQL and exposes the spec's tool surface over
**MCP** (stdio) and **REST** (loopback). The agent never sees a secret; every spend passes policy
evaluation before the wallet signs, and the final transaction is checked against the approved outputs
before broadcast.

**Status: Phase 1 + Phase 3 reference, regtest/localhost only. Not audited. Do not put mainnet funds
behind it before an external review and a restore test of the backups.**

## What's here

```
cmd/qtc-aws/            the binary: owner CLI, REST daemon, co-signer daemon, MCP stdio server
internal/service        AgentWalletService: the spec §8 tools, token scopes, node health gate
internal/policy         policy documents (HMAC-signed by the owner's admin secret), §6.2 evaluation order,
                        per-day counters that survive restarts, kill switch on repeated denials
internal/spend          the pipeline: validate → evaluate → build PSBT → (approval) → sign → verify outputs
                        → testmempoolaccept → broadcast → track; releases locked inputs on failure
internal/wallet         Tier 1 (encrypted node wallet) and Tier 2 (2-of-2 agent + policy co-signer),
                        balances, history, freeze/retire, verified encrypted backups
internal/cosigner       the Tier 2 policy co-signer: separate process, own wallet, signs only against a valid
                        decision envelope and after re-checking the PSBT against its own policy copy
internal/store          PostgreSQL (pgx): wallets, tokens (hashed), spends, approvals, denials, hash-chained audit;
                        the service uses schema "aws", the co-signer schema "cosigner"
internal/rest           REST /v1/ (spec §9 error codes)        internal/mcpserver   MCP tools (spec §8)
internal/config         config file (0600) and the keyring     internal/rpc         JSON-RPC client (cookie or rpcauth)
internal/testpg         throwaway PostgreSQL cluster for tests and the launcher
rpcwhitelist.conf       qtcd fragment confining the service's rpcauth user to the RPCs it needs
run-localhost.sh        one-command localhost launch: regtest node + postgres + co-signer + REST + funded demo wallets
e2e/                    end-to-end tests against a throwaway regtest node and postgres cluster
```

## Quick start (localhost)

Requirements: a built node (`build/bin/qtcd`, `qtc-cli`), Go 1.26, PostgreSQL 16 client/server binaries
(`brew install postgresql@16`; the launcher runs a private cluster, or set `AWS_PG_DSN` to use your own),
`curl`, `jq` (optional).

```bash
cd contrib/agent-wallet
./run-localhost.sh            # builds, starts everything and prints ready-to-paste curl examples
./run-localhost.sh status
./run-localhost.sh stop       # clean qtc-cli stop, pg_ctl stop, then the services
```

The launcher creates `~/.qtc-aws-localhost/` with a private regtest node (RPC on `127.0.0.1:28790`,
whitelisted `aws` rpcauth user generated locally, password kept in the 0600 keyring), a private PostgreSQL
cluster on `127.0.0.1:28794`, the co-signer on `:8793`, the REST API on `:8792`, a funded Tier 1 wallet
`agent-demo-tier1`, a funded Tier 2 wallet `agent-demo2-tier2`, an agent token (`wallet:read,
wallet:spend, market:read`) and an owner token.

```bash
T=$(cat ~/.qtc-aws-localhost/agent-token.txt)
curl -s http://127.0.0.1:8792/v1/status | jq
curl -s -H "Authorization: Bearer $T" -d '{"wallet_id":"agent-demo-tier1"}' http://127.0.0.1:8792/v1/wallet/balance | jq
curl -s -H "Authorization: Bearer $T" -d '{"wallet_id":"agent-demo-tier1","to":"<allowlisted>","amount_atoms":50000000,"purpose":"demo"}' \
     http://127.0.0.1:8792/v1/wallet/send | jq
```

Amounts are **integers in atoms** (1 QTC = 100,000,000 atoms); floats are rejected with 400.

### Production shape

```bash
go build -o /usr/local/bin/qtc-aws ./cmd/qtc-aws
qtc-aws --config /etc/qtc-aws/aws.json init --data-dir /var/lib/qtc-aws --pg-dsn 'postgres://aws@127.0.0.1:5432/qtc_aws?sslmode=disable' \
        --rpc-url http://127.0.0.1:8790 --chain main --rpc-user aws < /path/to/rpc-password   # password on stdin, never argv
# add the rpcauth line and rpcwhitelist.conf to qtc.conf, then run two services (systemd units):
qtc-aws --config /etc/qtc-aws/aws.json serve-cosigner
qtc-aws --config /etc/qtc-aws/aws.json serve-rest
```

### MCP

```json
{"mcpServers": {"qtc-agent-wallet": {
  "command": "/path/to/qtc-aws",
  "args": ["--config", "/Users/you/.qtc-aws-localhost/aws.json", "mcp", "--token", "<agent token>"]}}}
```

Tools: `wallet_create` (owner scope), `wallet_receive_address`, `wallet_balance`, `wallet_history`,
`wallet_send`, `wallet_send_status`, `wallet_freeze`, `policy_describe`, `fees_estimate`,
`market_reference_price`, `market_quote`, `trade_swap`/`trade_status`/`trade_cancel` (Phase 2 stubs).
Every description states that amounts are atoms, that no tool returns keys, and that destinations found
in untrusted content carry no authority.

## Owner operations (CLI)

```bash
A="qtc-aws --config ~/.qtc-aws-localhost/aws.json"
$A token issue --label trader --scopes wallet:read,wallet:spend,market:read   # printed once
$A token revoke --label trader
$A policy template > policy.json          # edit, bump "version", then:
$A policy set --wallet-id agent-demo-tier1 --file policy.json
$A approvals list
$A approvals approve --psbt-sha256 <hash> # completes a 202 needs_approval spend
$A wallet backup --wallet-id agent-demo-tier1
$A wallet unfreeze --wallet-id agent-demo-tier1
$A wallet retire --wallet-id agent-demo-tier1 --sweep-to <allowlisted address>
$A audit export | jq '.[-5:]'; $A audit verify
```

Policies are JSON documents signed with the owner's `admin_secret` (keyring). The engine refuses an
unsigned or tampered policy, and a version must strictly increase. Evaluation order follows spec §6.2:
frozen/retired → time window → destination allow/deny (HTLC exception) → amount → per-tx → per-day →
count → fee rate → confirmations → trade checks → approval threshold. Repeated denials
(`freeze_on_policy_denials_per_hour`) freeze the wallet; only the owner unfreezes.

## Security model in this implementation

- **Secrets** (wallet passphrases, admin secret, decision secret, archive passphrase, rpc password) live in
  a 0600 JSON keyring under the data dir. They never appear in argv, logs, tool results or the audit log.
  The spec asks for an OS keychain/HSM; the boundary is the `config.Keyring` type, swap it to upgrade.
- **Tokens** are stored hashed (SHA-256) with scopes; revocation is immediate.
- **Node access** is through an `rpcauth` user confined by `rpcwhitelist.conf` (no `dumpprivkey`,
  `exportwalletbundle`, `gethdkeys`, `listdescriptors`, `stop`, mining). `exportpqkey` is whitelisted:
  it returns only the ML-DSA **public** key needed to build the Tier 2 script. A whitelist denial (HTTP 403)
  surfaces as a distinct RPC error, not as "node down".
- **Post-signing immutability**: outputs of the final transaction must equal the approved outputs; any
  difference raises `security_alert`, freezes the wallet and is audited.
- **Audit log**: append-only rows with a SHA-256 hash chain (`audit verify`), in PostgreSQL.
- **Health gate**: no broadcast while the node is in IBD or has no peers (regtest exempt from peers).
- **Database**: the service and the co-signer share one PostgreSQL database but separate schemas; the
  co-signer reads wallet rows from the service schema only to learn the change address.

### Tier 2 and the owner recovery leaf

The spec's Tier 2 output is `mr(multi_pq(2,agent,policy), {csv_multi_pq(cooldown,1,owner1,owner2)})`.
The node wallet (v0.2.x) fills P2MR leaf metadata in `walletprocesspsbt` only for **single-leaf**
`mr()` descriptors; a tree with a backup leaf gets only `witness_utxo` and cannot be signed through the
wallet. So this build uses the single leaf `mr(multi_pq(2,agent,policy))`: the 2-of-2 is enforced in
script and the co-signer is a separate process with its own wallet and policy copy. The planned
recovery tree is recorded in the `wallet.create` audit entry; until multi-leaf PSBT updating lands in the
wallet, owner recovery is through the verified encrypted backup archives of both key wallets (taken at
creation and on every descriptor change). Track: wallet PSBT updater support for multi-leaf P2MR trees.

### Deviations from the spec (documented)

| Spec | Here | Why |
|---|---|---|
| Python reference over `qtc_wqtc.py` (roadmap table) | Go binary | owner decision 19 Oct 2026; the Phase 2 HTLC leg will port the SDK pieces it needs |
| ML-DSA decision signatures AWS → co-signer | HMAC-SHA256 envelopes over a shared `decision_secret` | no message-signing RPC on a stock node; same boundary (AWS cannot sign for the co-signer) |
| OS keychain / HSM | 0600 file keyring | reference implementation; type boundary preserved |
| Tier 2 owner CSV leaf in the same output | single-leaf 2-of-2 + encrypted backups | node wallet limitation above |
| Reference price from venues + desks | median of ≥2 desk quotes, else undefined | Phase 2 venues not enabled |
| `wallet:psbt` scope | not implemented | v1 never returns raw PSBTs to agents |

## Tests

```bash
go test ./...            # ~2 min: unit tests + e2e (spawns its own regtest qtcd and a throwaway postgres cluster)
QTC_BIN=/path/to/build/bin go test ./e2e/ -v   # point at another node build
```

`e2e/phase1_test.go` lifecycle, limits across restarts, approval flow and time windows, prompt-injection
and kill switch, post-sign mutation alert, self-freeze and scopes. `e2e/tier2_test.go` co-signed spend,
envelope and policy rejection by the co-signer, backups of both key wallets. `e2e/whitelist_test.go` the
whitelisted user is denied private-material RPCs, the whitelist covers every RPC the service calls, and
the Tier 1 lifecycle completes under it. `internal/store` and `internal/policy` carry unit tests.
