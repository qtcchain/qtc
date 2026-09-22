# qtc-explorer

Block explorer for QTC, written in Go, with a single [Bleve](https://blevesearch.com) (scorch) index as its only
store. It implements `doc/qtc-block-explorer-spec.md` for a localhost / single-host deployment: blocks,
transactions, addresses, unspent outputs, mempool, difficulty health, prefix search, a JSON API, and the
rate-limited broadcast gateway required by `doc/browser-wallet-backend.md`.

## Why Bleve

Every record (block, transaction, unspent output, address, indexer state) is one Bleve document with a few
indexed fields for lookup and sorting and one stored `json` field holding the full record. That gives:

- one embedded dependency, no database server;
- exact lookups by id, term queries by address or script, numeric range queries by height, boolean filters
  for `spent` / `orphaned`, and sorted paging, all from the same index;
- prefix search across block hashes, MatMul digests, txids and addresses, so a partial identifier resolves
  instantly (the feature a relational store does not give for free);
- atomic per-block commits through `index.Batch`, which is what makes reorg handling simple.

Schema is in `internal/store/store.go`. Field mappings use the keyword analyzer, numeric and boolean
mappings; `json` is stored but not indexed. Nothing is dynamic, so stray fields are never indexed.

## Layout

| Path | Purpose |
|---|---|
| `cmd/qtc-explorer` | entry point and flags |
| `internal/rpc` | JSON-RPC client with cookie auth and a per-client method allowlist |
| `internal/store` | Bleve mapping, records, batch writes, queries |
| `internal/indexer` | polling sync loop, per-block atomic apply, reorg rollback, mempool mirror |
| `internal/pq` | P2MR witness decoding (ML-DSA-44, SLH-DSA, PQ multisig, CLTV/CSV/CTV variants) |
| `internal/gateway` | `POST /api/v1/tx/broadcast` implementing the seven wallet-backend rules |
| `internal/web` | JSON API, server-rendered HTML, inline SVG charts, embedded CSS |

## Run against regtest

```sh
qtcd -regtest -daemon -server=1 -txindex=1 -rpcbind=127.0.0.1 -rpcallowip=127.0.0.1 -fallbackfee=0.0001
qtc-cli -regtest createwallet ex
qtc-cli -regtest generatetoaddress 105 "$(qtc-cli -regtest getnewaddress)"

cd contrib/explorer && go build -o qtc-explorer ./cmd/qtc-explorer
./qtc-explorer -rpc-url http://127.0.0.1:18443 -rpc-cookie ~/.qtc/regtest/.cookie \
  -index ./data/regtest.bleve -listen 127.0.0.1:8080 -cors-origin http://localhost:5173
```

Open <http://127.0.0.1:8080>. Mainnet defaults are `-rpc-url http://127.0.0.1:19754` and the node's
`.cookie`; the node must run with `txindex=1` and must not be pruned.

Flags (all also readable from `QTC_RPC_URL`, `QTC_RPC_COOKIE`, `QTC_RPC_USER`, `QTC_RPC_PASS`,
`QTC_EXPLORER_INDEX`, `QTC_EXPLORER_LISTEN`, `QTC_EXPLORER_CORS_ORIGIN`):

| Flag | Default | Meaning |
|---|---|---|
| `-rpc-url` | `http://127.0.0.1:19754` | node JSON-RPC |
| `-rpc-cookie` | | path to `.cookie` (preferred over user/pass) |
| `-index` | `./data/index.bleve` | Bleve directory, created on first run |
| `-listen` | `127.0.0.1:8080` | HTTP bind |
| `-cors-origin` | | exact origin allowed to call the broadcast endpoint; empty disables CORS |
| `-poll` | `2s` | node polling interval |
| `-halving-interval` | `210000` | subsidy schedule (mainnet, testnet and regtest all use 210,000) |
| `-initial-subsidy` | `5000000000` | atoms |
| `-max-reorg` | `100` | halt on deeper reorgs |

## How indexing works

Each poll compares the indexed tip with `getbestblockhash`. New blocks are fetched with `getblock <hash> 2`
(decoded transactions with fees and the MatMul header fields) and applied one at a time; every block is one
Bleve batch, so a crash mid-block leaves the index at the previous block. Inputs are resolved from the
indexed unspent-output documents (or from outputs created earlier in the same block), which is how input
values, fee rates and address "sent" totals are known without a second RPC call.

If a new block's `previousblockhash` is not the indexed tip, or the node's hash at the indexed height
changed, the indexer walks back until `getblockhash <height>` matches the indexed hash, reversing each
block's effects in reverse transaction order and marking the block record `orphaned`. Orphaned block records
are kept so their pages still render with a badge. Deeper-than-`-max-reorg` reorgs halt the indexer and
show on `/status`.

The mempool is mirrored from `getrawmempool true` on every poll and kept in memory; unconfirmed transactions
are rendered from the node on demand and are not written to the index.

## Broadcast gateway

`POST /api/v1/tx/broadcast` with `{"hex": "<raw tx>"}`:

- body limit 64 KiB, JSON only, hex validated before any RPC;
- duplicate submissions (same bytes within 10 minutes) get `409`;
- `testmempoolaccept` first; a rejection returns `422` with the node's reason string and nothing else;
- `sendrawtransaction` only for accepted transactions;
- 4 in-flight upstream calls, 6 attempts per client IP per minute, CORS restricted to `-cors-origin`;
- the gateway's RPC client can call only those two methods;
- logs carry txid, size and outcome, never bodies.

## Verified on regtest (2026-09-21)

- 107-block initial index in 3 s; address balance, UTXO count and tx count reconcile with the wallet.
- P2MR spends decode as ML-DSA-44 (2,420-byte signature, 1,316-byte leaf, leaf version 0xc2).
- A 3-block `invalidateblock` reorg rolls back and re-applies; orphaned blocks stay queryable by hash.
- Every negative gateway case returns the documented status; a wallet-signed transaction is accepted and
  appears on its page within one poll.

## Not yet done (see the spec)

ZMQ notifications (polling only), PostgreSQL production profile, Prometheus metrics, shielded-pool views,
supply chart, websocket feed, functional tests in CI.
