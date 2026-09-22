# QTC Block Explorer Specification

Date: 2026-09-21 · Software baseline: `v0.1.0-rc5` (`qtc-main` `ea0f0173`) · Status: SPECIFICATION, nothing built

This document specifies the public block explorer for the QTC network: what it indexes, what it shows, the JSON
API it exposes, the transaction-broadcast and UTXO endpoints the browser wallet depends on, how it is deployed,
and the acceptance gates before it is announced. It fills the "Explorer: optional, day 2" line in
`doc/launch/launch-build-spec.md` and implements the gateway contract in `doc/browser-wallet-backend.md`.

Everything QTC-specific in this document was read from the tree at the baseline above. Where a value is a
consensus parameter it is quoted from `src/kernel/chainparams.cpp` or `src/consensus/params.h`, not from an
earlier design document.

## 1. Why a stock explorer does not work

QTC diverges from Bitcoin in ways that break every off-the-shelf explorer and every off-the-shelf address indexer:

| Divergence | Where | Effect on stock tools |
|---|---|---|
| 182-byte block header (`nNonce64`, `matmul_digest`, `matmul_dim`, `seed_a`, `seed_b`) | `src/primitives/block.h` | rust-bitcoin, btcd and bitcoinjs header parsers reject the header; electrs, esplora, mempool.space, btcd-based indexers cannot sync |
| Witness v2 P2MR output (`OP_2 <32-byte merkle root>`, Bech32m `qtc1z…`) | `src/addresstype.h`, `doc/qtc-pqc-spec.md` | address decoders show "unknown witness v2"; no address pages |
| No witness discount (weight = 1 × serialized size), sigop pricing 20 B/unit | launch build spec §1 | fee-rate and vsize columns are wrong |
| Empty-block subsidy penalty (inert on mainnet, live on test networks) | `src/validation.cpp` `GetBlockSubsidyForBlock` | "expected reward" columns are wrong on testnet |
| Shielded transaction fields (`shielded`, `note_commitment`, `nullifier`) | `src/core_write.cpp` | test-network transactions render with empty inputs |
| ASERT from genesis, no 2016-block epochs | chainparams | difficulty charts that assume retarget epochs are meaningless |

The `getblock` / `getrawtransaction` JSON already carries all of the extra fields, so an explorer driven by the
node's own RPC needs no parser of its own. What must be built is the address/UTXO index and the presentation.

**Decision:** build `qtc-explorer` as a single Go service in `contrib/explorer/` (indexer + JSON API +
server-rendered HTML) against PostgreSQL 16. Go matches the existing pool code, one binary keeps the
DigitalOcean droplet simple, and server-rendered pages avoid a JavaScript build pipeline. A separate SPA is
not required; the JSON API exists for the browser wallet and third parties.

## 2. Scope

In scope for the first release (Phase 1, launch "day 2"):

- Blocks, transactions, addresses, UTXOs, mempool, on mainnet and testnet.
- MatMul header display and difficulty health.
- Rate-limited anonymous broadcast endpoint and address-UTXO endpoint for the browser wallet.
- Reorg-safe indexing with ZMQ tip notifications.
- Public read-only JSON API with CORS restricted to the deployed website origin.

Out of scope for Phase 1 (see §13 for later phases): shielded-pool state views, rich-list and supply charts,
miner attribution, PSBT decoding, full-text search, mining-pool statistics, websocket live feeds.

## 3. Architecture

```
                 public HTTPS (Cloudflare proxy, TLS)
                              │
                     nginx (rate limits, static assets)
                              │
        ┌─────────────────────┴──────────────────────┐
        │  qtc-explorer (Go, one binary)              │
        │   ├─ indexer   : ZMQ hashblock/sequence     │
        │   │             + RPC getblock verbosity 2  │
        │   ├─ api       : /api/v1/*  (JSON)          │
        │   ├─ gateway   : /api/v1/tx/broadcast       │
        │   └─ web       : server-rendered HTML       │
        └───────────┬───────────────────┬────────────┘
                    │ 127.0.0.1:5432    │ 127.0.0.1:19754 RPC (cookie auth)
              PostgreSQL 16         qtc-node (read-only, no wallet)
                                    -txindex=1 -rest=0 -zmqpub*
```

Three processes on one host, all bound to loopback except nginx. The node has no wallet, no mining, no RPC
credentials other than the cookie file, and its RPC is never reachable from outside the host. The explorer
process runs as an unprivileged user with read access to the cookie only.

### 3.1 Explorer node configuration

```
# /etc/qtc/qtc.conf on the explorer host
server=1
listen=1
maxconnections=32
dbcache=1024
txindex=1
coinstatsindex=1
blockfilterindex=0
rest=0
disablewallet=1
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
rpcthreads=8
rpcworkqueue=64
zmqpubhashblock=tcp://127.0.0.1:28332
zmqpubsequence=tcp://127.0.0.1:28333
zmqpubhashblockhwm=1000
zmqpubsequencehwm=100000
addnode=<NodeC-ip>:19755
```

`txindex` is required for `getrawtransaction` by txid on historical transactions. `coinstatsindex` makes the
supply figure on the home page a single `gettxoutsetinfo muhash` call. Pruning is not permitted on this node.
Mainnet P2P port 19755 and RPC 19754; testnet P2P 29755 and RPC 29754.

## 4. Data model

PostgreSQL schema, one database per network (`qtc_mainnet`, `qtc_testnet`). All hashes are stored as `bytea`
in RPC (big-endian, display) byte order. ZMQ delivers hashes little-endian and the indexer reverses them on
receipt, as `doc/zmq.md` warns.

| Table | Key columns | Notes |
|---|---|---|
| `blocks` | `height` PK, `hash` UNIQUE, `prev_hash`, `time`, `mediantime`, `version`, `bits`, `target`, `difficulty`, `chainwork`, `nonce64`, `matmul_digest`, `matmul_dim`, `seed_a`, `seed_b`, `matrix_c_words`, `size`, `weight`, `n_tx`, `subsidy_expected`, `coinbase_value`, `fees_total`, `orphaned` | one row per block ever seen; `orphaned=true` on reorg, never deleted |
| `txs` | `txid` PK, `block_height` FK nullable, `block_index`, `version`, `locktime`, `size`, `weight`, `fee`, `is_coinbase`, `shielded`, `shielded_input_count`, `shielded_output_count`, `raw` bytea | `block_height NULL` while in mempool |
| `tx_inputs` | (`txid`, `vin`) PK, `prev_txid`, `prev_vout`, `sequence`, `witness` bytea[], `p2mr_leaf_version`, `p2mr_leaf_script`, `p2mr_control_block`, `sig_algorithm` | `sig_algorithm` ∈ {`mldsa44`, `slhdsa`, `multisig`, `ecdsa`, `schnorr`, `unknown`} decoded per §6.3 |
| `tx_outputs` | (`txid`, `vout`) PK, `value`, `script_pubkey` bytea, `script_type`, `address` text nullable, `spent_by_txid`, `spent_by_vin`, `spent_height` | `script_type` is the node's `TxoutType` name, e.g. `witness_v2_p2mr` |
| `address_stats` | `address` PK, `first_seen_height`, `tx_count`, `received`, `sent`, `balance`, `utxo_count` | materialised, updated per block and per mempool delta |
| `mempool_txs` | `txid` PK, `first_seen`, `fee`, `size`, `ancestor_count` | mirrored from `getrawmempool true`; cleared on restart |
| `difficulty_samples` | `height` PK, `time`, `bits`, `difficulty`, `interval_s` | derived, drives the ASERT chart |
| `sync_state` | `network`, `indexed_height`, `indexed_hash`, `zmq_sequence` | single row |

Indexes: `tx_outputs(address, spent_by_txid NULLS FIRST)` for UTXO lookup; `tx_outputs(script_pubkey)` hashed
for script-level search; `txs(block_height, block_index)`; `blocks(time)`.

Address derivation follows the node: `witness_v2_p2mr` → Bech32m with hrp `qtc` (mainnet) or `tqtc` (testnet),
witness version 2; legacy P2PKH/P2SH via Base58 prefixes 58/63 (mainnet) and 111/196 (testnet); Taproot and
segwit v0 via Bech32/Bech32m as Bitcoin. `anchor` and non-standard scripts get no address and are indexed by
script only. The explorer never derives addresses itself from raw scripts; it takes `address` from the node's
`decodescript` / `vout.scriptPubKey.address` and only falls back to a local encoder when the node omits one.

## 5. Indexing

### 5.1 Initial sync

Walk heights 0..tip with `getblockhash` + `getblock <hash> 2`, batching 64 blocks per JSON-RPC batch, 4 workers,
writing each block in one transaction. `getblock` verbosity 2 includes decoded transactions, fees (`fee` per tx
when `txindex` is on) and all QTC header fields, so no second call per transaction is needed. At mainnet's
600 s spacing the first year is ≈ 52,600 blocks; initial sync must finish in under 30 minutes on the target
droplet against a local node.

### 5.2 Steady state

Subscribe to `hashblock` and `sequence` on ZMQ. On `hashblock`, fetch the block, compare `previousblockhash`
with the indexed tip, and either append or run reorg handling. On `sequence` events `A`/`R` (mempool add /
remove) and `C`/`D` (block connect / disconnect), update `mempool_txs` incrementally. Every 30 s, and on any ZMQ
gap detected via the sequence number, reconcile against `getbestblockhash` and `getrawmempool`. ZMQ is an
optimisation, not the source of truth.

### 5.3 Reorgs

On a block whose `previousblockhash` is not the indexed tip: walk back from the indexed tip via
`getblockheader` until a hash that `getblockhash <height>` still returns; mark every block above the fork point
`orphaned=true`, unlink its transactions (`block_height=NULL`, outputs unspent, address stats reversed), then
apply the new branch forward. Mainnet hysteresis depth is 1, so forks deeper than a handful of blocks are a
network incident and the explorer must page the operator (§10) rather than silently rewrite more than 10 blocks.

### 5.4 Subsidy and fees

Per block, compute `subsidy_expected` exactly as `GetBlockSubsidyForBlock` does:

- base subsidy = `nInitialSubsidy` (50 QTC) right-shifted by `height / 210000`;
- on networks where the empty-block penalty is active (`nEmptyBlockSubsidyPenaltyHeight` finite, mainnet sets
  it to `INT32_MAX`) and the block has exactly one transaction, apply the halving / quartering rule from
  `src/validation.cpp:6044`.

Store `coinbase_value` and `fees_total = coinbase_value − subsidy_expected`. Display a warning badge when the
coinbase claims less than expected (miner under-claim) and treat more-than-expected as an indexer bug, since
consensus would have rejected the block.

Per transaction, `fee` comes from the node. Fee rate is `fee / size` in atoms per byte. Because QTC has no
witness discount, `vsize == size`; the UI labels the column "atoms/B" and never shows a separate vsize.

## 6. QTC-specific presentation

### 6.1 Block page

Show all header fields from `getblock` with the legacy fields hidden:

| Field | Source key | Display |
|---|---|---|
| Height, hash, prev, next | `height`, `hash`, `previousblockhash`, `nextblockhash` | links |
| Time, median time | `time`, `mediantime` | UTC + relative; interval vs previous block; vs 600 s target |
| Version | `version` | hex + decimal |
| Bits, target, difficulty, chainwork | `bits`, `target`, `difficulty`, `chainwork` | difficulty to 4 significant figures |
| Nonce | `nonce64` | decimal and hex; **`nonce` and `mixhash` are legacy zero fields and are not shown** |
| MatMul digest | `matmul_digest` | hex, copy button, tooltip "transcript hash z, the proof-of-work hash" |
| Matrix dimension | `matmul_dim` | integer; mainnet 512, testnet 256, regtest 64 |
| Seeds | `seed_a`, `seed_b` | hex, collapsed by default |
| Matrix C words | `matrix_c_words` | integer, collapsed |
| Size / weight | `size`, `weight` | bytes; weight against 24,000,000 max |
| Reward | computed | `subsidy_expected` + fees, penalty badge if applied |

The block hash shown is `hash` from RPC, which is the header hash the node uses for `hashPrevBlock` linkage.
The PoW hash (`matmul_digest`) is a separate field and the page must not conflate the two.

### 6.2 Difficulty panel (home page and `/difficulty`)

Poll `getdifficultyhealth 120` every block and render:

- current and next `bits` / `difficulty` / `target`;
- interval statistics: mean, p50, p90, p99, standard deviation, mean absolute error, largest overshoot and
  undershoot, all in seconds against the 600 s target;
- a 2,016-block chart of `difficulty` and a histogram of intervals from `difficulty_samples`;
- peer health: connected, outbound, synced-outbound counts.

ASERT is continuous from genesis (`nFastMineHeight = 0`, half-life `nMatMulAsertHalfLife`, target timespan
14 days), so the chart has no epoch markers. Show the powLimit floor (`0x1e033333` mainnet, `0x1e011da5`
testnet) as a horizontal line; blocks sitting on the floor mean hash rate is below the floor's design point.

### 6.3 Transaction page

Standard layout plus:

- **Script type badge** per output from `scriptPubKey.type`: `witness_v2_p2mr` renders as "P2MR (post-quantum)".
- **Witness decoding** per input. For a P2MR spend the witness stack is
  `[signature, leaf script, control block]`. Decode the leaf script by its final opcode:
  `OP_CHECKSIG_MLDSA` with a 1,312-byte key → ML-DSA-44 (signature 2,420 bytes);
  `OP_CHECKSIG_SLHDSA` with a 32-byte key → SLH-DSA; `OP_CHECKSIGADD_*` … `OP_NUMEQUAL` → m-of-n PQ multisig;
  leading `OP_CHECKLOCKTIMEVERIFY` / `OP_CHECKSEQUENCEVERIFY` / `OP_CHECKTEMPLATEVERIFY` → timelocked or
  covenant variants. Show the leaf version (`0xc2` masked `0xfe`), the number of sibling hashes in the control
  block, and the 32-byte merkle root recovered from the spent output. Do not attempt to verify signatures.
- **Sigop cost** from `decoderawtransaction` where present; show it beside size since fee policy prices sigops
  at 20 bytes per unit (ML-DSA 50, SLH-DSA 1,000 units).
- **Shielded flag**: if `shielded` is true, show `shielded_input_count` / `shielded_output_count`, each
  `note_commitment` and `nullifier`, and a banner "Shielded transaction (test network)". On mainnet
  `nShieldedSunsetHeight = 0`, so a shielded transaction can never appear; the indexer logs at error level and
  the page shows an "unexpected on mainnet" banner if one ever does.
- **Anti-fee-sniping**: browser-wallet transactions use version 2, sequence `0xfffffffd` and a locktime near the
  tip; render locktime as a height with "(anti-fee-sniping)" when it is within 100 blocks below the including
  block.

### 6.4 Address page

Balance, received, sent, UTXO count, transaction count, paginated transaction list, unspent outputs. For
`qtc1z…` addresses show "P2MR, witness v2, Bech32m" and the 32-byte merkle root. Do not show any key material:
the address commits to a merkle root, and the ML-DSA public key is revealed only when an output is spent.

### 6.5 Units

Amounts in QTC with eight decimals, and in atoms (1 QTC = 100,000,000 atoms) on hover and in the API. Dust
threshold for P2MR outputs is 11,583 atoms; outputs below it are flagged.

### 6.6 Search

One box accepting: block height, block hash, `matmul_digest` (searched in `blocks.matmul_digest`), txid,
address, or raw scriptPubKey hex. Ambiguous 64-hex input is tried as block hash, then txid, then digest.

## 7. JSON API

Base path `/api/v1`, JSON only, `Cache-Control` set per endpoint, all amounts in atoms as integers, all hashes
as big-endian hex. Errors are `{"error": {"code": "...", "message": "..."}}` with an appropriate HTTP status.
Every endpoint is read-only except `POST /tx/broadcast`.

| Method | Path | Returns | Cache |
|---|---|---|---|
| GET | `/chain` | height, tip hash, mediantime, difficulty, next difficulty, supply (`gettxoutsetinfo`), mempool size, indexer lag | 10 s |
| GET | `/blocks?before=<height>&limit=<n≤100>` | block summaries newest first | 10 s |
| GET | `/block/{hash-or-height}` | full block row + txids (paginated `?page=`) | immutable once 6 confirmations |
| GET | `/block/{hash}/header` | the 12 header fields exactly as `getblockheader` returns them plus `hex` of the 182-byte header | immutable |
| GET | `/tx/{txid}` | decoded tx with QTC fields, `confirmations`, `block_height`, fee, fee rate, per-input decoded witness (§6.3) | immutable once 6 confirmations |
| GET | `/tx/{txid}/hex` | raw hex | immutable |
| GET | `/tx/{txid}/status` | `{confirmed, block_height, block_hash, block_time}` | 10 s |
| GET | `/address/{addr}` | `address_stats` row + decoded type | 10 s |
| GET | `/address/{addr}/txs?before=<height>&limit=` | txids and summaries | 10 s |
| GET | `/address/{addr}/utxo` | see §7.2 | 5 s |
| GET | `/mempool` | count, total size, fee histogram | 5 s |
| GET | `/mempool/txids` | txid list | 5 s |
| GET | `/fees` | recommended atoms/B for 1, 3, 6, 12 blocks from `estimatesmartfee` and the mempool histogram | 30 s |
| GET | `/difficulty` | `getdifficultyhealth` passthrough with `difficulty_samples` for the last 2,016 blocks | 60 s |
| GET | `/search?q=` | `{type, id}` or 404 | none |
| POST | `/tx/broadcast` | see §7.1 | none |

Pagination is cursor-based by height, never by offset. Limits are capped server-side.

### 7.1 Broadcast gateway (`POST /api/v1/tx/broadcast`)

This endpoint implements the seven requirements in `doc/browser-wallet-backend.md` verbatim:

1. Body is `{"hex": "<raw transaction hex>"}`; `Content-Type: application/json`; body limit **64 KiB**
   (a single-input ML-DSA-44 P2MR spend is ≈ 3.9 KB; the limit allows a dozen inputs and rejects abuse).
2. Before any RPC call: reject non-JSON, missing or non-string `hex`, odd length, non-hex characters,
   decoded size above the limit, and a txid already seen in the last 10 minutes (in-memory LRU keyed by
   `SHA256d(bytes)`). All of these return HTTP 400 with a stable `code`.
3. Call `testmempoolaccept [hex]`. If `allowed` is false, return HTTP 422 with the node's `reject-reason`
   string only. Never include the RPC URL, host, cookie path, exception text, or stack trace.
4. Only if accepted, call `sendrawtransaction hex`. Return HTTP 200 `{"txid": "..."}`.
5. Upstream concurrency is capped at **4** in-flight broadcasts; excess requests wait up to 5 s then return 503.
   Per-IP rate limit **6 per minute** and per-origin **60 per minute**, enforced in nginx and again in-process.
6. CORS: `Access-Control-Allow-Origin` is exactly the deployed wallet origin from configuration; preflight allows
   `POST` and `Content-Type` only; no credentials.
7. Logs record timestamp, client IP hash (salted, rotated daily), txid, size, outcome and reject reason.
   Request bodies are never logged.

The gateway runs inside `qtc-explorer` but in its own goroutine pool with its own RPC client whose only
permitted methods are `testmempoolaccept` and `sendrawtransaction` (`rpcwhitelist` on the node enforces this
for the gateway's RPC user if a second user is configured; otherwise the client-side allowlist applies).

### 7.2 Address UTXO endpoint (`GET /api/v1/address/{addr}/utxo`)

Returns exactly what the browser wallet needs to build a P2MR spend and nothing it should trust blindly:

```json
{
  "address": "qtc1z…",
  "script_pubkey": "5220…",
  "tip_height": 51234,
  "utxos": [
    {"txid": "…", "vout": 0, "value": 123456789, "height": 51000, "confirmations": 235,
     "script_pubkey": "5220…", "coinbase": false}
  ]
}
```

`script_pubkey` is repeated per output because the browser must verify it against its locally derived script
before signing (the wallet contract says so). Mempool-only outputs are included with `height: null` and
`confirmations: 0`. Immature coinbase outputs (fewer than 100 confirmations) are included with `coinbase: true`
so the wallet can exclude them. The response is capped at 1,000 outputs with a `next` cursor.

## 8. Web front end

Server-rendered Go templates, one CSS file, no framework, no third-party scripts, no analytics beacon by
default. Pages: `/` (tip, last 20 blocks, mempool, difficulty summary), `/blocks`, `/block/{id}`, `/tx/{txid}`,
`/address/{addr}`, `/mempool`, `/difficulty`, `/search`, `/api` (endpoint reference rendered from the table in
§7), `/status` (indexer height vs node height, ZMQ lag, last reorg). Every page works without JavaScript; the
only scripts are a copy-to-clipboard helper and a 10 s refresh on `/` and `/mempool`. Layout must be usable at
375 px width. Testnet and mainnet are separate deployments on separate hostnames, each with a persistent
banner naming the network.

## 9. Security

- RPC bound to loopback, cookie auth, `disablewallet=1`, `rest=0`. The explorer never has a wallet.
- The explorer host is not a miner and not a DNS seed; losing it affects nothing on the network.
- nginx terminates nothing publicly: TLS is at Cloudflare, origin traffic is over Cloudflare's origin
  certificate, and the droplet firewall admits 443 from Cloudflare ranges only, 19755 from anywhere for P2P,
  and 51820 from Node C for the WireGuard hub.
- Global read rate limit 120 req/min per IP; write limits per §7.1.
- All user input reaches PostgreSQL through parameterised queries; hashes are validated as 64 hex before use.
- Secrets: the RPC cookie file and PostgreSQL password live in `/etc/qtc-explorer/env`, mode 0600, owned by the
  service user. Nothing is compiled into the binary or in the repository.
- Logs never contain request bodies, cookies, descriptors or seeds.
- The MatMul service challenge RPC (`getmatmulservicechallenge`) is available as an optional proof-of-work gate
  on the broadcast endpoint in a later phase; it is not enabled in Phase 1.

## 10. Operations

- Runs as a `systemd` unit `qtc-explorer.service` with `Restart=always`, after `qtc-node.service` and
  `postgresql.service`. A Docker Compose alternative is provided using `contrib/docker/Dockerfile` for the node.
- Prometheus metrics on `127.0.0.1:9101/metrics`: indexed height, node height, lag blocks, ZMQ events per
  second, reorg count and depth, API request rate and latency by endpoint, broadcast outcomes by code,
  PostgreSQL pool usage. Node C's Prometheus scrapes it over WireGuard.
- Alerts: lag > 3 blocks for 15 min; reorg depth ≥ 3; broadcast 5xx rate > 1 %; disk > 80 %; node peers < 4.
- Backups: nightly `pg_dump` to the DigitalOcean volume plus weekly droplet snapshot. The index is fully
  reproducible from the node, so backups are for recovery time, not correctness.
- Shutdown order on maintenance: stop `qtc-explorer`, then `qtc-cli stop` and wait for exit, never a kill.

## 11. Performance targets

Measured on the launch droplet (DigitalOcean Basic, 4 vCPU, 8 GB, 100 GB volume) with a synced local node:

| Metric | Target |
|---|---|
| Initial mainnet index, first 100,000 blocks | < 1 h |
| Block ingest latency after ZMQ `hashblock` | p99 < 2 s |
| `/block`, `/tx`, `/address` page render | p95 < 200 ms |
| `/address/{addr}/utxo` for an address with 10,000 outputs | p95 < 500 ms |
| Broadcast round trip (`testmempoolaccept` + `sendrawtransaction`) | p95 < 1.5 s |
| Concurrent readers before p95 degrades 2× | ≥ 200 |
| Disk at 1 year of mainnet | node ≈ 15 GB with `txindex`, PostgreSQL ≈ 10 GB; both far below the 100 GB volume |

## 12. Testing and acceptance gates

Nothing is announced until every row is green.

| Gate | Method |
|---|---|
| Header fidelity | For 1,000 random mainnet and testnet blocks, `/block/{hash}/header.hex` equals `getblockheader <hash> false` and all 12 fields match `getblockheader <hash> true` |
| Address balance | For 200 random addresses, `balance` equals the sum of `gettxout` over the explorer's listed UTXOs, and equals `scantxoutset` for the address descriptor |
| P2MR witness decoding | Vectors from `test/functional/feature_p2mr_end_to_end.py` and `rpc_pq_wallet.py` decode to the expected algorithm, key size, leaf version and sibling count |
| Subsidy on test networks | On regtest with the empty-block penalty active, mine empty and non-empty sequences and confirm `subsidy_expected` matches the coinbase the node accepted |
| Reorg | Regtest: two nodes, partition, mine 5 vs 7 blocks, reconnect; explorer marks 5 orphaned, address balances match `scantxoutset`, no orphaned txid resolves to a block |
| Broadcast negatives | Malformed JSON, non-hex, oversized, duplicate, and each `testmempoolaccept` reject case from `rpc_rawtransaction.py`; every response has the right status, a stable `code`, and no host or path strings |
| Broadcast positive | A browser-built P2MR spend (version 2, sequence `0xfffffffd`, anti-fee-sniping locktime) is accepted and appears on `/tx/{txid}` within 2 s |
| CORS | A request from a foreign origin receives no `Access-Control-Allow-Origin` header |
| Rate limit | 7th broadcast from one IP within a minute returns 429 |
| Shielded on mainnet | Indexer unit test: a mainnet block containing a `shielded=true` tx logs an error and renders the banner |
| No-JS | Every page renders and every link works with JavaScript disabled |
| Mobile | Every page has no horizontal scroll at 375 px |

The functional tests live in `contrib/explorer/test/` and run against regtest in CI on every explorer change.

## 13. Phasing

| Phase | When | Content |
|---|---|---|
| 1 | launch day 2 (see launch spec §3) | everything in §§4–12 on testnet first, then mainnet at T+2 days |
| 2 | first point release | supply and issuance chart, block-interval heat map, `getmatmulchallenge` live panel, miner-address grouping, PSBT decode, websocket tip feed |
| 3 | after shielded activation decision | shielded pool views on test networks (commitment tree depth, nullifier set size, `getshieldedstate`-style RPCs once they exist), unshield velocity cap display |
| 4 | when demand justifies | Electrum-style subscription API for third-party wallets, public read replica |

## 14. Cost

| Item | Monthly |
|---|---|
| DigitalOcean Basic 4 vCPU / 8 GB | ≈ $48 |
| 100 GB volume | ≈ $10 |
| Weekly snapshot | ≈ $2 |
| Cloudflare (free tier, proxy + origin cert) | $0 |
| **Total** | **≈ $60**, matching the "≈ $1,010 with explorer" line in the launch spec |

A second identical host for testnet doubles this; during the burn-in the testnet explorer can share the
mainnet host with a second node and database at the cost of a larger droplet.

## 15. Open decisions

1. **Hostnames.** `explorer.<domain1>` and `testnet-explorer.<domain1>`, or one host with a network switch.
   Recommendation: separate hostnames, one deployment each, so a testnet incident cannot touch mainnet.
2. **Wallet origin for CORS.** Needs the final browser-wallet URL before the gateway can be configured.
3. **Second RPC user.** Whether to give the gateway its own `rpcauth` user with `rpcwhitelist` limited to the
   two broadcast methods (recommended) or rely only on the client-side allowlist.
4. **Proof-of-work gate on broadcast.** Whether to require a `getmatmulservicechallenge` solution in Phase 2.
5. **Public API keys.** Phase 1 is anonymous with IP limits; decide whether third parties get keyed higher
   limits later.
