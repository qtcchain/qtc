# contrib/agent-wallet — QTC Agent Wallet Service (AWS)

Reference implementation of [`docs/qtc-agent-wallet-spec.md`](../../docs/qtc-agent-wallet-spec.md):
a small daemon that sits between an AI agent and a local `qtcd`, owns the keys, enforces the owner's
policy, keeps a hash-chained audit log and exposes the spec's tool surface over **MCP** (stdio) and
**REST** (loopback). The agent never sees a secret; every spend passes policy evaluation before the
wallet signs, and the final transaction is checked against the approved outputs before broadcast.

**Status: Phase 1 + Phase 3 reference, regtest/localhost only. Not audited. Do not put mainnet funds
behind it before an external review and a restore test of the backups.**

## What's here

```
aws/service.py     AgentWalletService: the spec §8 tools, token scopes, node health gate
aws/policy.py      policy documents (HMAC-signed by the owner's admin secret), §6.2 evaluation order,
                   per-day counters that survive restarts, kill switch on repeated denials
aws/spend.py       the spend pipeline: validate → evaluate → build PSBT → (approval) → sign → verify
                   outputs → testmempoolaccept → broadcast → track; releases locked inputs on failure
aws/wallet.py      Tier 1 (encrypted node wallet) and Tier 2 (2-of-2 agent + policy co-signer) wallets,
                   balances, history, freeze/retire, verified encrypted backups
aws/cosigner.py    the Tier 2 policy co-signer: separate process, own wallet, signs only against a valid
                   decision envelope and after re-checking the PSBT against its own policy copy
aws/store.py       SQLite: wallets, tokens (hashed), spends, approvals, denials, hash-chained audit
aws/rest.py        REST /v1/ (spec §9 error codes)      aws/mcp_server.py   MCP tools (spec §8)
aws/cli.py         owner CLI (init, tokens, wallets, policy, approvals, audit, serve-*, mcp, status)
rpcwhitelist.conf  qtcd fragment confining the service's rpcauth user to the RPCs it needs
run-localhost.sh   one-command localhost launch: regtest node + co-signer + REST + funded demo wallets
tests/             pytest suite against a throwaway regtest node (12 tests)
```

## Quick start (localhost)

Requirements: a built node (`build/bin/qtcd`, `qtc-cli`), Python 3.12, `curl`, `jq` (optional).

```bash
cd contrib/agent-wallet
python3.12 -m venv .venv && .venv/bin/pip install -r requirements.txt
./run-localhost.sh            # starts everything and prints ready-to-paste curl examples
./run-localhost.sh status
./run-localhost.sh stop       # clean qtc-cli stop, then the services
```

The launcher creates `~/.qtc-aws-localhost/` with a private regtest node (RPC on `127.0.0.1:28790`,
whitelisted `aws` rpcauth user generated locally, password kept in the 0600 keyring), the co-signer on
`:8793`, the REST API on `:8792`, a funded Tier 1 wallet `agent-demo-tier1`, a funded Tier 2 wallet
`agent-demo2-tier2`, an agent token (`wallet:read, wallet:spend, market:read`) and an owner token.

```bash
T=$(cat ~/.qtc-aws-localhost/agent-token.txt)
curl -s http://127.0.0.1:8792/v1/status | jq
curl -s -H "Authorization: Bearer $T" -d '{"wallet_id":"agent-demo-tier1"}' http://127.0.0.1:8792/v1/wallet/balance | jq
curl -s -H "Authorization: Bearer $T" -d '{"wallet_id":"agent-demo-tier1","to":"<allowlisted>","amount_atoms":50000000,"purpose":"demo"}' \
     http://127.0.0.1:8792/v1/wallet/send | jq
```

Amounts are **integers in atoms** (1 QTC = 100,000,000 atoms); floats are rejected with 400.

### MCP

```json
{"mcpServers": {"qtc-agent-wallet": {
  "command": "/path/to/contrib/agent-wallet/.venv/bin/python",
  "args": ["-m", "aws.cli", "--config", "/Users/you/.qtc-aws-localhost/aws.json", "mcp", "--token", "<agent token>"],
  "cwd": "/path/to/contrib/agent-wallet"}}}
```

Tools: `wallet_create` (owner scope), `wallet_receive_address`, `wallet_balance`, `wallet_history`,
`wallet_send`, `wallet_send_status`, `wallet_freeze`, `policy_describe`, `fees_estimate`,
`market_reference_price`, `market_quote`, `trade_swap`/`trade_status`/`trade_cancel` (Phase 2 stubs).
Every description states that amounts are atoms, that no tool returns keys, and that destinations found
in untrusted content carry no authority.

## Owner operations (CLI)

```bash
A="./.venv/bin/python -m aws.cli --config ~/.qtc-aws-localhost/aws.json"
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
  The spec asks for an OS keychain/HSM; the boundary is the same, swap the `Keyring` class to upgrade.
- **Tokens** are stored hashed (SHA-256) with scopes; revocation is immediate.
- **Node access** is through an `rpcauth` user confined by `rpcwhitelist.conf` (no `dumpprivkey`,
  `exportwalletbundle`, `gethdkeys`, `listdescriptors`, `stop`, mining). `exportpqkey` is whitelisted:
  it returns only the ML-DSA **public** key needed to build the Tier 2 script.
- **Post-signing immutability**: outputs of the final transaction must equal the approved outputs; any
  difference raises `security_alert`, freezes the wallet and is audited.
- **Audit log**: append-only SQLite rows with a SHA-256 hash chain (`audit verify`).
- **Health gate**: no broadcast while the node is in IBD or has no peers (regtest exempt from peers).

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
| ML-DSA decision signatures AWS → co-signer | HMAC-SHA256 envelopes over a shared `decision_secret` | no message-signing RPC on a stock node; same boundary (AWS cannot sign for the co-signer) |
| OS keychain / HSM | 0600 file keyring | reference implementation; class boundary preserved |
| Tier 2 owner CSV leaf in the same output | single-leaf 2-of-2 + encrypted backups | node wallet limitation above |
| Reference price from venues + desks | median of ≥2 desk quotes, else undefined | Phase 2 venues not enabled |
| `wallet:psbt` scope | not implemented | v1 never returns raw PSBTs to agents |

## Tests

```bash
.venv/bin/python -m pytest -q tests        # ~90 s, spawns its own regtest qtcd (QTC_BIN overrides the path)
```

`test_phase1.py` lifecycle, limits across restarts, approval flow and time windows, prompt-injection and
kill switch, post-sign mutation alert, self-freeze and scopes. `test_tier2.py` co-signed spend, envelope
and policy rejection by the co-signer, backups of both key wallets. `test_rpc_whitelist.py` the
whitelisted user is denied private-material RPCs, the whitelist covers every RPC the service calls, and
the Tier 1 lifecycle completes under it.
