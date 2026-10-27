# QTC-EVM — engineering specification for a QTC-anchored EVM layer 2

Status: draft v0.1, 2026-10-11; revised 2026-10-22 to add the sequencer ordering policy
(sections 7.1–7.6) and its hooks in sections 1, 4, 5, 8.1, 9, 12–15 and 17–19, and on
2026-10-26 to add the Ethereum-parity fee floor policy (section 5.1, owner decision). Companion to
`wQTC.md`, `wqtc-bridge-architecture.md`, `wqtc-contracts.md`,
`qtc-asset-ledger-trust-model-and-roadmap.md` and `qtc-agent-wallet-spec.md`.
Normative words (MUST, SHOULD, MAY) are used in the RFC 2119 sense.

## 1. Scope

QTC-EVM is an EVM execution layer whose only consensus is QTC's MatMul proof of work. Contracts run
in an unmodified EVM; batches of layer-2 blocks are posted to QTC; every QTC-EVM full node derives
the same chain from QTC blocks alone. The gas token is QTC bridged in through the lock-and-mint
machinery already specified for wQTC. Accounts are post-quantum by default through an ML-DSA-44
verification precompile.

In scope: chain parameters, execution engine, sequencer (including its ordering policy), batcher,
anchoring and data availability, derivation (including L1 reorg handling), bridge, post-quantum
accounts, hashing domains, security model, test plan, rollout.

Out of scope for v0.1: fraud proofs and validity proofs (section 15 fixes the interfaces they will
plug into), cross-rollup messaging, MEV auctions, shared sequencing.

## 2. Non-goals and principles

- **No change to QTC layer-1 consensus** for the first two releases. Everything in this document
  works with script and policy that are live on mainnet today (v0.2.x). Soft-fork dependent
  hardening is listed separately (section 15) and maps onto the roadmap's Stages B–D.
- **No EVM semantic changes.** No new opcodes, no gas schedule edits, no state-trie changes. The
  only addition is a precompile at an address outside the ranges Ethereum uses.
- **The posted data is the chain.** A node with QTC blocks and this specification MUST reconstruct
  the QTC-EVM chain without talking to the sequencer, the federation or any committee.
- **Fail closed.** When data is missing or an anchor is malformed, derivation halts at the last
  good batch; it never guesses.
- **Reorg handling is keyed by block hash, never by height** (the lesson of the September ASERT
  verifier incident).

## 3. Layer-1 interface: what QTC provides and the numbers that bind

| Property | Value | Consequence for QTC-EVM |
|---|---|---|
| Consensus | MatMul PoW, ASERT, 600 s target spacing | Probabilistic finality; one-block reorgs observed between two miners 9 s apart |
| Block weight | `MAX_BLOCK_WEIGHT` 24,000,000 WU | ≈ 40 KB/s of raw data for the whole network |
| Standard tx weight | `MAX_STANDARD_TX_WEIGHT` 1,200,000 WU | One batch transaction carries ≈ 1.1 MB of witness data; larger batches are chunked |
| OP_RETURN | `nMaxOpReturnBytes` 1,004 (1,000-byte payload) | Holds the anchor commitment, never batch data |
| Outputs | P2MR only (witness v2, 32-byte Merkle root), `WitnessV2P2MR` | Data carriage and covenants live in script leaves |
| Signature opcodes | `OP_CHECKSIG_MLDSA` 0xbb, `OP_CHECKSIG_SLHDSA` 0xbc, `OP_CHECKSIGADD_*` | Federation and user keys are ML-DSA-44 or SLH-DSA-128s |
| Covenants | `OP_CHECKTEMPLATEVERIFY`, `OP_CHECKSIGFROMSTACK`, `OP_CAT` | Committed exits, data-stack signature checks, future fraud-proof games |
| ML-DSA-44 sizes | public key 1,312 B, signature 2,420 B | Each attestation signature costs 2.4 KB on both chains |
| SLH-DSA-128s sizes | public key 32 B, signature 7,856 B | Cold keys only; never on the batch hot path |
| Network | mainnet magic `51 54 43 21`, port 19755, bech32 HRP `qtc` | Chain-id and domain tags derive from the magic |
| Existing bridge structs | `BridgeExternalAnchor` (version, domain_id, source_epoch, data_root, verification_root), `BridgeBatchLeaf`, `BridgeVerifierSetCommitment`, `BridgeVerifierSetProof` | Anchor payload and federation rotation reuse these without new serialisation |

Confirmation-depth parameters (section 13) are chosen against the observed reorg profile and MUST
be revisited when mainnet hashrate changes by an order of magnitude.

## 4. Roles

| Role | Trust at v1 | Responsibility |
|---|---|---|
| Sequencer | Liveness only; ordering accountable under section 7.5 | Orders L2 transactions first-come-first-served with forced transactions first (sections 7.1–7.2), produces L2 blocks every `L2_BLOCK_TIME`, gives signed soft confirmations (section 7.4) |
| Batcher | Liveness only | Compresses L2 blocks into batches, posts data and anchors to QTC |
| Federation (attestors, M-of-N) | Custody of locked QTC; correctness of withdrawals until proofs exist | Signs epoch statements and withdrawal batches with ML-DSA-44; rotates via `BridgeVerifierSetCommitment` |
| Derivation node | None | Reads QTC, reconstructs the L2 chain, drives an execution engine |
| Execution engine | None | Unmodified EVM (reth fork `qtc-reth`) behind the Engine API |
| Data-availability committee (interim only) | Availability only | Stores batch data and signs availability certificates while witness-space DA is not yet used |
| Watcher | None | Independent node that alerts on sequencer equivocation, missing data, anchor gaps, withdrawal-template mismatch, and the ordering, omission and data faults of section 7.5 |

## 5. Chain parameters

| Parameter | Value | Note |
|---|---|---|
| `CHAIN_ID` | 20820 (0x5154, "QT") mainnet; 20821 testnet; 20822 devnet | Must not collide with registered EVM chain ids; register before public testnet |
| Native token | QTC, 18 decimals | 1 sat = 10^10 wei, exactly as `wQTC.md` §2 |
| `L2_BLOCK_TIME` | 2 s | Soft blocks; batches aggregate ~300 of them per QTC block |
| L2 gas limit | 30,000,000 per block | Raise only after DA throughput is measured on testnet |
| Base fee | EIP-1559, minimum 1 gwei-equivalent | Base fee is burned on L2 (reduces circulating bridged supply); priority fee to sequencer vault and has no effect on ordering (section 7.1) |
| EVM version | Cancun without blob transactions (type 3 rejected) | Blob carriage has no QTC analogue; batch data goes to QTC witness space |
| Predeploys | `L2Bridge` 0x...4200, `SequencerFeeVault` 0x...4201, `L1Origin` 0x...4202, `EntryPoint` (ERC-4337 v0.7) 0x...4203, `PQAccountFactory` 0x...4204 | Addresses fixed in genesis |
| Precompile | `MLDSA44_VERIFY` at 0x0000000000000000000000000000000000000100 | Section 10 |
| Genesis state | Predeploys only; zero native balance anywhere | All QTC on L2 originates from bridge deposits |

### 5.1 Fee floor policy: Ethereum parity (owner decision 26 October 2026; not yet implemented)

The owner has decided that QTC-EVM charges for smart-contract execution what Ethereum mainnet
charges. The base fee therefore follows EIP-1559 with a **floor** that tracks the current Ethereum
fee per gas, converted into QTC. `FEE_FLOOR_POLICY` = `eth-parity-v1`. Until the oracle path below
is implemented, the floor is the constant `BASE_FEE_FLOOR_STATIC`, set from the parity computation
at the time of the release and re-set at each release (section 5.1.4).

#### 5.1.1 Reference price

For each QTC block (L1 origin) the batcher computes and posts in the batch header a signed
`parity_quote`:

```
ParityQuote v1
  l1_origin         bytes32  QTC block hash the quote is for
  eth_fee_gwei_e6   u64      Ethereum mainnet median (base fee + median priority tip) over the last
                             ETH_FEE_WINDOW blocks, in micro-gwei
  eth_usd_e6        u64      ETH/USD, median of at least PRICE_MIN_SOURCES independent sources
  qtc_usd_e6        u64      QTC/USD, median of at least PRICE_MIN_SOURCES independent sources, or
                             the published qtc.markets formula price while no market exists
  observed_at       u64      unix seconds
  sig               bytes    ML-DSA-44 by the batcher key over SHA256d(QTCEVM/parity/v1 || fields)
```

`floor_wei_per_gas = eth_fee_gwei_e6 × 10^3 × eth_usd_e6 / qtc_usd_e6`
(gwei of QTC = gwei of ETH × ETH/USD ÷ QTC/USD; a gwei is 10⁹ wei).

Worked example, 26 October 2026 23:21 UTC: Ethereum base fee 0.045 gwei plus median tip 0.012 gwei
= 0.057 gwei; ETH/USD 2,309; QTC/USD 31.30 (qtc.markets adjusted price for 26 October 2026, subject
to the latest market price). Floor = 0.057 × 2,309 / 31.30 = **4.2 gwei of QTC** per gas. An ERC-20
transfer (65,000 gas) then costs 0.000273 QTC ≈ US$0.0086 on both chains. The QTC side of every
figure in this section moves with the QTC price; the dollar side is Ethereum's.

#### 5.1.2 Bounds every verifier checks (section 9.2 item 8)

The quote is an oracle input, so verifiers do not recompute it; they bound it:

- `observed_at` within `QUOTE_MAX_AGE` of the L1 origin's block time (from the header).
- Each of the three fields moved by at most `QUOTE_STEP_BPS` from the previous accepted quote.
- `floor_wei_per_gas` within [`BASE_FEE_FLOOR_MIN`, `BASE_FEE_FLOOR_MAX`].
- The resulting floor applied to every L2 block built on that L1 origin: `base_fee ≥ floor`.

A batch whose quote violates any bound is **invalid**; derivation halts and pages the operator, who
must re-post with a compliant quote (the fail-closed rule of section 2). When no fresh quote can be
produced (sources down), the batcher MUST repeat the previous quote with its original `observed_at`;
the age bound then halts the chain after `QUOTE_MAX_AGE`, which is the intended behaviour: the
operator fixes the oracle rather than the chain guessing a price.

#### 5.1.3 Base fee rule and smoothing

`next_base_fee = max(eip1559_next_base_fee(parent), floor_wei_per_gas(L1 origin))`, with the floor
itself smoothed: it may move at most `FLOOR_STEP_BPS` per L1 block, so Ethereum's intra-day fee
spikes reach QTC-EVM as a slope over several QTC blocks rather than a step. Everything else in
EIP-1559 is unchanged, including the burn (or treasury routing, section 19 item 5, which this
decision makes material: at parity the base fee is a significant flow and the owner should decide
its destination before mainnet).

#### 5.1.4 Static interim

Until `ParityQuote` is implemented in the batcher, the engine and the verifier, the floor is
`BASE_FEE_FLOOR_STATIC`, a chain parameter in the verifier bundle, computed with the formula of
5.1.1 at release time and stated in the release notes with its inputs and timestamp. Changing it is
a planned upgrade at an L1 height (`FEE_FLOOR_ACTIVATION_HEIGHT`), testnet first. Value proposed for
the first activation: 4.2 gwei of QTC (inputs above), to be recomputed from the latest market price
on the day the activation height is set.

#### 5.1.5 What this policy does and does not do

It makes a given contract call cost, in dollars, what the same call costs on Ethereum mainnet at
that moment, smoothed. It does not cap fees: above the floor the base fee still rises with QTC-EVM's
own demand. It does not track Ethereum's layer-2 networks, which charge far less; parity is with
mainnet by the owner's decision. It does not change the ordering policy: the priority fee still buys
no position (section 7.1). The explorer MUST show the current floor, the quote behind it and its
sources.

## 6. Execution engine

- `qtc-reth` is a fork of reth with: the chain spec above, the precompile, blob transactions
  disabled, the sequencer's pool ordering of section 7.6, and nothing else. The fork MUST track
  upstream and carry its delta as a patch series that can be rebased in a day.
- The engine is driven exclusively through the Engine API (`engine_newPayloadV3`,
  `engine_forkchoiceUpdatedV3`) by the derivation node. It never gossips L2 blocks on its own.
- Three heads are maintained, named as in Ethereum rollups and labelled in RPC (`eth_getBlockByNumber`
  tags `latest`, `safe`, `finalized`):
  - **unsafe**: produced by the sequencer, not yet anchored;
  - **safe**: derived from an anchor in a QTC block with ≥ `SAFE_DEPTH` confirmations;
  - **finalized**: derived from an anchor with ≥ `FINALITY_DEPTH` confirmations.

## 7. Sequencer

- Single operator at v1, keyed with ML-DSA-44. Rotation to the federation or a separate operator is
  a configuration change, not a protocol change, because the derivation rules (section 9) make the
  posted anchors canonical regardless of who sequences.
- Accepts transactions over standard JSON-RPC through the ingress gateway of section 7.3, builds
  blocks every `L2_BLOCK_TIME` in the order of section 7.1, and returns receipts as soft
  confirmations with the semantics of section 7.4; wallets MUST display them as soft until the
  `safe` head passes the block.
- **Forced inclusion.** The L1 inbox (section 8.4) is read at every QTC block. Every inbox item in
  a QTC block that reaches `SAFE_DEPTH` MUST be included in an L2 block whose L1 origin is at most
  `INBOX_DEADLINE` QTC blocks later. A batch that violates this is invalid under derivation, which
  is what makes censorship impossible rather than merely discouraged.
- **L1 origin.** Every L2 block carries the hash and height of the QTC block it was built against.
  `L1Origin` is updated once per L1 block with the QTC block hash, height, time and `nBits`, so
  contracts can read QTC state with the same freshness guarantees as the derivation node.

### 7.1 Ordering policy: first come, first served

The sequencer orders transactions by arrival, not by fee. `ORDERING_POLICY` = `fcfs-v1`.

- **Arrival** is the moment the ingress gateway (section 7.3) accepts a transaction and assigns it a
  sequence number `seq`: a u64, strictly increasing over the sequencer's lifetime, persisted before
  the acceptance is acknowledged, never reused across restarts or key rotations. A rejected
  submission receives no `seq` and is not an arrival.
- Each L2 block is sealed at the `L2_BLOCK_TIME` tick. Its transaction list is the forced prefix
  (section 7.2) followed by every accepted transaction with `seq` ≤ the block's watermark
  `seq_high`, in increasing `seq`, subject only to the rules below.
- A transaction that does not fit the remaining gas of the block being built MAY be passed over by
  later-arrived transactions that fit. It keeps its position and MUST be included within
  `DEFER_LIMIT` further blocks or dropped with a published reason (section 7.5).
- A transaction that is not yet valid at build time (nonce gap, balance below
  `gas_limit × max_fee`, fee cap below the base fee) is deferred, not dropped, until it becomes
  valid or `DEFER_LIMIT` passes. A nonce gap is filled by the same sender's later arrival in `seq`
  order; the gap-filling transaction does not inherit the earlier `seq`.
- A replacement (same sender and nonce, EIP-1559 bump rules) is a new arrival with its own `seq`;
  the replaced hash is logged `dropped: replaced`.
- The priority fee is accepted and paid to `SequencerFeeVault` as section 5 says, and has **no
  effect on position**. `MIN_PRIORITY_FEE` is 0. The base fee alone prices block space.
- The sequencer MUST NOT reorder within a block, seal a transaction that has no `seq`, or seal a
  block whose non-forced transactions are not in increasing `seq`. Before announcing a block it
  MUST check the engine's payload against its own log; a payload that violates this section is
  discarded and block production halts and pages the operator. Fail closed, as section 2 requires.

### 7.2 Forced transactions first

- A **forced transaction** is an inbox item (section 8.4) whose QTC block has reached
  `SAFE_DEPTH`. The deposit feeder builds its L2 transaction as the executor and submits it
  through the ingress gateway with `source = inbox`; it takes a `seq` like any other arrival.
- In every L2 block the forced transactions form a prefix: they precede every `source = rpc`
  transaction and appear in L1 order (QTC block height, then transaction index within that block).
  The prefix is a derivation validity rule (section 9.2 item 7): a batch that places a forced
  transaction after an RPC transaction, or out of L1 order, is invalid for every node, not merely
  a policy breach.
- Forced transactions are never deferred for gas. The sequencer reserves `FORCED_GAS_RESERVE` of
  the block gas limit for the prefix and MAY let the prefix fill the whole block; a backlog larger
  than one block spills into the following blocks, still in L1 order and still first.
- `INBOX_DEADLINE` (section 9.2 item 6) remains the censorship backstop. This section is the
  stronger promise: a safe inbox item lands in the next L2 block the sequencer builds.

### 7.3 One public ingress, no private order flow

- The sequencer has exactly two transaction sources: the public ingress gateway and the inbox.
  Nothing else can place a transaction into a block.
- The ingress gateway is a single JSON-RPC endpoint run by `qtc-evm-node` in front of `qtc-reth`,
  whose own RPC is bound to loopback. The gateway accepts `eth_sendRawTransaction`, assigns `seq`,
  signs the receipt (section 7.5), persists the entry, and only then forwards the transaction to the
  engine's pool through a single writer in `seq` order. `qtc-reth` runs with discovery and peer
  gossip disabled, so the pool's insertion order is the gateway's order.
- The operator MUST NOT offer a second endpoint with a different queue, bundles, backrun or
  priority auctions, preconfirmation agreements, or any insertion path that bypasses `seq`. The
  operator's own transactions, including fee-vault sweeps and the deposit feeder's, go through the
  gateway and take sequence numbers like everyone else's.
- Pool and pending-block contents (`txpool_*`, `eth_getBlockByNumber("pending")`) are served to
  everyone or to no one, never to selected parties. Under first-come-first-served, seeing the pool
  buys no position, so the default is to serve them.
- Rate limits are per sender address and per source IP, uniform, and published with the policy.
  A rejection carries its reason and is not an arrival.

### 7.4 Soft confirmation semantics

- The sequencer announces every sealed L2 block on its block feed signed with its ML-DSA-44 key:
  `block_sig` over `SHA256d(QTCEVM/block/v1 || chain_id || block_number || block_hash ||
  parent_hash || ordering_root)`. A **soft confirmation** for a transaction is the signed block
  that contains it, together with the transaction's ingress receipt (section 7.5).
- A soft confirmation is a promise by the operator, not a consensus fact. It states that the block
  is sealed, that it will be posted in a batch unchanged and in order, and that no later block from
  this sequencer will contradict it. A block MUST be durably stored before it is announced, and a
  restarted sequencer MUST resume from its last announced block, never rebuild it.
- Wallets, the explorer and the agent wallet service MUST label a transaction *soft* until the
  `safe` head passes its block and *safe* until `finalized`, and MUST NOT release value against a
  soft confirmation beyond what they would risk on the operator's reputation.
- The only ways a soft-confirmed transaction legitimately fails to become safe:
  1. an L1 reorg below its batch's anchor: the batcher re-posts the same batches on the new active
     chain, content unchanged, so the transaction becomes safe later rather than never;
  2. derivation halts on an invalid batch (section 9.2): the operator MUST re-post the announced
     blocks with a correct `post_state`; it MUST NOT post a batch that drops or reorders them. If
     the announced blocks themselves cannot form a valid batch (they break section 9.2 item 7,
     say), the sequencer has already announced a non-conforming block: the operator posts a
     corrected batch to restore liveness, and the contradiction is an equivocation fault,
     sanctioned under section 7.5;
  3. a reorg deeper than `REORG_WINDOW`, which is a manual recovery event under section 9.1.
- An anchored batch that differs from an announced signed block is an **equivocation fault**,
  provable with the two signatures.

### 7.5 Binding the operator

Arrival order is observed only by the sequencer, so the policy is made checkable by making the
sequencer commit to what it observed and letting anyone compare. None of the artefacts below is a
derivation input: a node that never sees a receipt or a segment derives the same chain.

- **Ingress receipt.** On acceptance the gateway returns, through `qtcevm_sendRawTransaction`
  (and later on request through `qtcevm_getIngressReceipt(tx_hash)`),
  `{tx_hash, seq, received_at, source, sig}` with `sig` over
  `SHA256d(QTCEVM/receipt/v1 || chain_id || tx_hash || seq || received_at || source)`.
  A receipt with sequence `n` proves the sequencer accepted the transaction and issued every
  sequence below `n` earlier.
- **Ingress segment.** For each L2 block the sequencer publishes:

  ```
  IngressSegment v1
    block_number  u64
    seq_high      u64      highest seq accepted before the block was sealed
    entries       [](seq u64, tx_hash bytes32, disposition u8, reason u8)
  ```

  with one entry per accepted transaction the block consumed or decided about: `included` (in
  block order), `deferred` (reason: gas, not-yet-valid, build-cutoff) or `dropped` (reason:
  deferred past `DEFER_LIMIT`, replaced, permanently invalid). `ordering_root` =
  `SHA256d(QTCEVM/ingress/v1 || IngressSegment)` is carried in the `L2Block` (section 8.1), so it is
  signed in the batch and anchored on QTC. Segments are served by the sequencer, mirrored by
  watchers, and retained for `INGRESS_LOG_RETENTION`.
- **Invariants a watcher checks** for every announced block, from the segment, the block, the
  batch and the receipts users forward to it:
  1. the block's transaction order equals the `included` entries in order: forced prefix first in
     L1 order, then strictly increasing `seq`;
  2. every `seq` ≤ `seq_high` that no earlier segment resolved as `included` or `dropped` appears
     exactly once in this segment, as `included`, `deferred` or `dropped`; a `deferred` entry
     reappears in every later segment until it is resolved, and is resolved within `DEFER_LIMIT`;
  3. no `included` transaction lacks a `seq`, and no `seq` is reused;
  4. `ordering_root` in the anchored batch matches the served segment;
  5. the segment was served within `INGRESS_SEGMENT_SLA` of the block announcement.
- **Faults.** A receipt whose `seq` is absent from the segments after `DEFER_LIMIT` is an
  **omission fault**; an order violating (1) is a **reorder fault**; a missing or mismatched segment
  is a **data fault**; section 7.4 defines the **equivocation fault**. Each is two signed sequencer
  statements that contradict each other, or one signed statement that contradicts posted data, so
  the evidence is self-contained and a watcher publishes it as such.
- **Sanctions.** At v1 the sanction is key rotation: on published fault evidence the federation
  MUST rotate the sequencer key in the schedule that derivation consults (section 9.2 item 4) within
  `SEQUENCER_ROTATION_SLA` QTC blocks, and watchers alert on that SLA as they do for withdrawals.
  At Stage C the operator posts an ordering bond on QTC and a fault proof spends it through the
  `OP_CHECKSIGFROMSTACK` game that section 15 reserves for state fraud.
- **Policy versioning.** `ORDERING_POLICY` names the version in force. A change MUST be published
  in this specification and announced at least `POLICY_NOTICE` QTC blocks before it takes effect;
  the sequencer key schedule entry that activates it names the version.

What this does not bind: the gateway can delay *accepting* a transaction so that another arrives
first. Receipts carry `received_at`, so the delay is measurable by the submitter and comparable
across watchers, but it is not provable. section 14 lists it as the residual.

### 7.6 Implementation delta for v1

| Component | Change |
|---|---|
| `qtc-reth` | Replace the default coinbase-tip pool ordering with a two-class ordering: inbox-derived transactions (sender = executor) rank above RPC transactions; within a class, reth's tie-break on submission id yields arrival order. Verify that tie-break against the pinned reth tag before relying on it. Run with discovery and gossip disabled and the RPC on loopback. |
| `qtc-evm-node` sequencer | Add the ingress gateway (`seq`, receipts, segments, single-writer forward to the engine); sign block announcements; carry `ordering_root` in `L2Block`; check each payload against the log before announcing; give the engine a non-zero build delay so the sealed payload is the pool at the tick (the delay is unset today, and anything the engine had not yet placed is logged `deferred: build-cutoff`). |
| `qtc-evm-node` deposit feeder | Submit through the gateway with `source = inbox` instead of directly to the engine RPC; drop the fixed tip. |
| Derivation | Validity item 7 of section 9.2 (forced prefix in L1 order). |
| Watcher | The five invariant checks and the four fault reports of section 7.5; the rotation SLA alert. |
| Wallets, explorer, agent wallet service | Soft, safe and finalized labels; receipt verification; a view of the fault feed. |

## 8. Batcher, anchoring and data availability

### 8.1 Batch format

```
Batch v1
  version        u8      = 1
  chain_id       u32
  batch_number   u64     strictly increasing
  parent_hash    bytes32 hash of Batch (batch_number - 1), zero for the first
  l1_origin      bytes32 QTC block hash the last L2 block was built against
  blocks         []L2Block (zstd-compressed, framed)
  post_state     bytes32 EVM state root after the last block
  parity_quote   ParityQuote  optional; present once FEE_FLOOR_POLICY is active (section 5.1.1)
  sequencer_sig  bytes   ML-DSA-44 over SHA256d(QTCEVM/batch/v1 || fields above)
```

`L2Block` = timestamp, L1-origin reference, transaction list (RLP), `ordering_root` (section 7.5),
and nothing else; everything else is recomputed. A batch MUST contain at least one block and at
most `MAX_BLOCKS_PER_BATCH`.

### 8.2 Chunking and the data carrier

Batch bytes are split into chunks of at most `CHUNK_BYTES` (default 1,000,000) so that a chunk
transaction stays under `MAX_STANDARD_TX_WEIGHT`. Each chunk is carried in the witness of a
script-path spend of a **data-carrier output**: a P2MR output whose spend leaf is

```
OP_FALSE OP_IF <chunk_index:u16> <chunk_count:u16> <data push(es)> OP_ENDIF <batcher_pk> OP_CHECKSIG_MLDSA
```

The `OP_IF` body is never executed, so consensus treats it as inert data; the trailing signature
check prevents third parties from spending the carrier. The carrier output is created and spent
by the batcher in consecutive transactions so the data is on chain exactly once.

`data_root` = Merkle root over SHA256d of the chunks in order. A derivation node that holds all
chunks for an anchor and whose Merkle root matches MUST accept the batch; otherwise it MUST
halt at the previous batch and alert.

### 8.3 Anchor transaction

One anchor per batch, posted by the batcher:

- **Input 0**: the previous anchor's registry output (the "registry UTXO chain" of roadmap Stage A).
  Spending it proves ordering and uniqueness: two anchors cannot both spend the same registry
  output, so equivocation is impossible on the active chain.
- **Output 0**: the new registry output, same script as before: a P2MR tree with leaf A =
  M-of-N `OP_CHECKSIGADD_MLDSA` federation multisig, leaf B = batcher single-key, leaf C = timeout
  recovery for the federation.
- **Output 1**: `OP_RETURN` carrying a `BridgeExternalAnchor`: `version` 1, `domain_id` =
  SHA256d(`QTCEVM/domain` || chain_id), `source_epoch` = batch_number, `data_root` as above,
  `verification_root` = SHA256d(`QTCEVM/verify/v1` || post_state || parent_hash ||
  withdrawal_root). Total payload well under 1,000 bytes.
- **Inputs 1..k**: the data-carrier spends for this batch, or references to carrier transactions
  mined in earlier blocks of the same epoch (carriers MAY precede the anchor by up to
  `CARRIER_WINDOW` blocks).

`verification_root` is the hook for later proof systems: fraud and validity proofs bind to it
without changing the anchor format.

### 8.4 Inbox (deposits and forced transactions)

The L1 inbox is a well-known P2MR script: leaf 1 = federation M-of-N (to sweep deposits into the
reserve), leaf 2 = `<DEPOSIT_TIMEOUT> OP_CHECKSEQUENCEVERIFY OP_DROP <depositor_pk> OP_CHECKSIG_MLDSA`
(refund if never credited). A deposit transaction pays the inbox and adds an `OP_RETURN`
`QTCEVM/deposit/v1 || chain_id || l2_recipient (20 B) || optional calldata (≤ 900 B)`.
Forced transactions use the same shape with a minimum deposit of `FORCED_TX_BOND` to pay L2 gas.

### 8.5 Data-availability committee (interim)

Until witness-space carriage is used on mainnet, the batcher MAY post only the anchor and hand
chunks to a DAC of the same M-of-N operators as the federation. Each operator returns an
availability certificate (ML-DSA-44 over `data_root`); M certificates are published with the
anchor as additional `OP_RETURN` chunks or in the carrier leaf. Derivation nodes MUST refuse to
advance past a batch whose chunks they cannot fetch from at least one operator. The DAC is a
liveness committee only; it can halt the chain by withholding, it cannot fork it.

## 9. Derivation pipeline

```
QTC node (ZMQ hashblock + RPC) → L1 reader → anchor filter → chunk assembler → batch decoder
→ validity checks → Engine API (newPayload, forkchoiceUpdated) → heads (unsafe/safe/finalized)
```

### 9.1 L1 reader and reorg handling (normative)

- The reader keeps a window of the last `REORG_WINDOW` QTC block headers keyed **by block hash**,
  each with its parent hash, height, and the list of anchors it contains.
- On every new tip it walks back from the tip by `previousblockhash` until it meets a hash already
  in the window. Blocks above that fork point that are no longer on the active chain are marked
  abandoned, together with every anchor and batch derived from them.
- The L2 chain is rolled back to the last batch whose anchor is still on the active chain, by
  issuing `engine_forkchoiceUpdated` with that batch's block as head, safe and finalized as
  appropriate. Derivation then resumes from the fork point.
- Heights are never used as keys. A batch is identified by (anchor txid, QTC block hash).

### 9.2 Validity checks on a batch (all MUST pass)

1. The anchor spends the registry output of the previous accepted anchor.
2. `source_epoch` = previous batch_number + 1; `parent_hash` matches.
3. All chunks present, `data_root` matches, decompression succeeds, sizes within limits.
4. Sequencer signature valid for the sequencer key current at that L1 height.
5. Every L2 block's L1 origin is on the active QTC chain and not later than the anchor's block.
6. Forced-inclusion deadline respected for every inbox item that was due.
7. In every L2 block, the inbox-derived transactions precede all others and appear in L1 order
   (section 7.2).
8. Once `FEE_FLOOR_POLICY` is active: every L2 block's base fee is at least the floor for its L1
   origin (section 5.1); under `eth-parity-v1` the batch's `ParityQuote` is signed by the current
   batcher key, no older than `QUOTE_MAX_AGE`, each field within `QUOTE_STEP_BPS` of the previous
   accepted quote, and the derived floor within [`BASE_FEE_FLOOR_MIN`, `BASE_FEE_FLOOR_MAX`].
9. Executing the blocks through the engine yields `post_state`; otherwise the batch is **invalid**
   and derivation halts (v1) or the batch is skipped (v2, once fraud proofs exist to arbitrate).

### 9.3 Head advancement

- unsafe: sequencer blocks received over the sequencer's block feed, accepted only if they carry a
  valid `block_sig` (section 7.4) and extend the current safe head.
- safe: a batch whose anchor has ≥ `SAFE_DEPTH` confirmations.
- finalized: ≥ `FINALITY_DEPTH` confirmations. Bridge withdrawals (section 10.3) are released only
  from finalized batches.

## 10. Bridge

### 10.1 Reserve and lock outputs

The L1 reserve reuses the lock-and-mint design of `wqtc-bridge-architecture.md` Model A: locked
QTC sits in P2MR outputs whose tree contains the federation M-of-N leaf and the depositor refund
leaf. The L2 side is the `WQTCBridge` contract renamed `L2Bridge`, predeployed, and `WQTC` is not
needed because the bridged asset is the native token: `L2Bridge` mints native balance via a
system transaction rather than an ERC-20 mint.

### 10.2 Deposits

1. User pays the inbox (section 8.4). 2. After the inbox transaction's block reaches `SAFE_DEPTH`,
the sequencer MUST include a deposit system transaction that credits `sats × 10^10` wei to the
recipient and emits `Deposit(l1_txid, vout, recipient, amount)`. 3. If no batch includes it before
`INBOX_DEADLINE`, every derivation node rejects the sequencer's batches, which halts the chain
until a compliant batch appears. 4. If the deposit is never credited and `DEPOSIT_TIMEOUT` passes,
the depositor spends the refund leaf.

### 10.3 Withdrawals

1. User calls `L2Bridge.withdraw(l1_destination, amount)`; the amount is burned and
`Withdrawal(id, destination, amount)` is appended to the batch's withdrawal tree
(`withdrawal_root` in `verification_root`). 2. When the batch is finalized, the federation
constructs the **withdrawal batch transaction**: one input from the reserve, one output per
withdrawal, change to the reserve. 3. Before signing, the federation publishes a **commitment
output**: a P2MR output whose leaf is `OP_CHECKTEMPLATEVERIFY <template_hash>` where the template
is exactly the withdrawal batch transaction. Anyone can then execute the withdrawal batch; the
federation cannot alter it after committing. 4. The release is `round-down` to whole sats per
`wQTC.md` §4; dust below the P2MR dust limit accrues to a dust ledger per §5.

What CTV gives at launch, stated plainly: once the federation has committed a withdrawal batch,
its execution is permissionless and censorship-proof. What it does not give: forcing the
federation to commit in the first place. That requires roadmap Stage C (`OP_CHECKANCHORPROOF`)
and is tracked in section 15. Watchers alert when a finalized batch has withdrawals and no
commitment output appears within `WITHDRAWAL_SLA` blocks.

### 10.4 Invariants (checked continuously by watchers)

- Σ native L2 supply + pending withdrawals + dust ledger = Σ reserve UTXOs (in wei at 10^10).
- Every `Deposit` event matches exactly one inbox UTXO; every inbox UTXO with `SAFE_DEPTH` has a
  `Deposit` within `INBOX_DEADLINE`.
- Every finalized `Withdrawal` appears in exactly one committed withdrawal batch.

## 11. Post-quantum accounts

### 11.1 Precompile `MLDSA44_VERIFY` (address 0x…0100)

```
input  = pubkey (1,312 B) || ctx_len (1 B) || ctx (ctx_len B) || msg_len (4 B BE) || msg || signature (2,420 B)
output = 32 bytes: 0x…01 on success, 0x…00 on failure (never reverts on invalid signature)
gas    = MLDSA44_VERIFY_GAS (provisional 8,000) + 3 per 32-byte word of msg
```

Semantics are FIPS 204 ML-DSA-44 with the context string, pure mode (no pre-hash). The
implementation MUST be the same code QTC core uses (`src/libbitcoinpqc`) compiled into `qtc-reth`,
and MUST pass the FIPS 204 known-answer tests plus QTC's own vectors. Gas is calibrated on testnet
so that one verification costs the same wall-clock as 2–3 `ecrecover` calls; the number is a chain
parameter, not a constant, so it can be re-tuned by a planned upgrade.

### 11.2 Account model

- `PQAccount` is an ERC-4337 v0.7 account whose validator calls the precompile with the owner's
  ML-DSA-44 public key stored in the account. The user operation signature field carries the
  2,420-byte signature; calldata cost (16 gas/byte) dominates, ≈ 39k gas, acceptable at QTC-EVM
  fee levels.
- `PQAccountFactory` creates accounts at counterfactual addresses from the public key hash, so an
  address can receive funds before deployment.
- EIP-7702 delegation is enabled so a classical EOA can delegate to a PQ validator and migrate.
- Classical secp256k1 EOAs remain valid on devnet and testnet. **Mainnet policy is an owner
  decision (section 17)**: either (a) allowed but flagged in explorer and wallets as classical, or
  (b) `L2Bridge` refuses to credit deposits to non-contract recipients, which makes PQ accounts the
  only way to hold bridged QTC.
- Sequencer, batcher and federation keys are ML-DSA-44 on both chains. Federation attestations are
  verified on L2 by `L2Bridge` through the precompile: M of N signatures over the replay-bound
  statement (chain_id, bridge_id, nonce, destination, amount), ≈ M × (8k + 39k) gas.

## 12. Hashing and domain separation

- `SHA256` precompile 0x02 and `RIPEMD160` 0x03 give `HASH160` = RIPEMD160(SHA256(x)) and
  `SHA256d` with no new code.
- Every QTC-EVM hash is domain-tagged: `SHA256d(tag || payload)` with tags `QTCEVM/batch/v1`,
  `QTCEVM/domain`, `QTCEVM/verify/v1`, `QTCEVM/deposit/v1`, `QTCEVM/withdraw/v1`,
  `QTCEVM/attest/v1`, `QTCEVM/block/v1`, `QTCEVM/receipt/v1`, `QTCEVM/ingress/v1`,
  `QTCEVM/parity/v1`. Tags are ASCII, no length prefix, fixed in this document.
- HTLC hashlocks for atomic swaps (`WQTCAtomicSwapHTLC`) use plain `SHA256(preimage)` with a
  32-byte preimage on both chains, exactly as the agent-wallet spec's Venue A, because QTC's HTLC
  leaf expects that function. Mixing in a tag here would break atomicity silently.

## 13. Parameters

| Name | Default | Rationale |
|---|---|---|
| `SAFE_DEPTH` | 6 QTC blocks (≈ 1 h) | Observed reorgs are 1 block; 6 gives margin at current hashrate |
| `FINALITY_DEPTH` | 24 QTC blocks (≈ 4 h) | Withdrawals and large deposits; revisit with hashrate |
| `REORG_WINDOW` | 288 QTC blocks (≈ 2 days) | Deeper reorgs halt derivation and page the operator |
| `INBOX_DEADLINE` | 12 QTC blocks after `SAFE_DEPTH` | Bound on censorship |
| `DEPOSIT_TIMEOUT` | 1,008 QTC blocks (≈ 1 week), CSV | Refund path |
| `WITHDRAWAL_SLA` | 36 QTC blocks after finality | Watcher alert threshold |
| `CHUNK_BYTES` | 1,000,000 | Under `MAX_STANDARD_TX_WEIGHT` with witness discount |
| `CARRIER_WINDOW` | 6 QTC blocks | Carriers may precede their anchor |
| `MAX_BLOCKS_PER_BATCH` | 600 | ≈ 20 min of L2 blocks |
| `FORCED_TX_BOND` | 100,000 sats | Pays L2 gas for a forced transaction |
| `MLDSA44_VERIFY_GAS` | 8,000 (provisional) | Calibrated on testnet |
| `FEE_FLOOR_POLICY` | `eth-parity-v1` (owner decision 26 Oct 2026) | Section 5.1; static interim until the quote path ships |
| `BASE_FEE_FLOOR_STATIC` | 4.2 gwei of QTC (26 Oct 2026 inputs, QTC US$31.30) | Re-set at each release from the parity formula at the latest market price |
| `ETH_FEE_WINDOW` | 100 Ethereum blocks (≈ 20 min) | Median of base fee + median tip |
| `PRICE_MIN_SOURCES` | 2 | Independent ETH/USD and QTC/USD sources |
| `QUOTE_MAX_AGE` | 3,600 s | Older quote halts derivation rather than guessing |
| `QUOTE_STEP_BPS` / `FLOOR_STEP_BPS` | 2,500 / 1,250 | ≤25% per field per quote; floor ≤12.5% per L1 block |
| `BASE_FEE_FLOOR_MIN` / `MAX` | 1 gwei / 10,000 gwei of QTC | Hard clamps |
| `ORDERING_POLICY` | `fcfs-v1` | Section 7.1; a change needs `POLICY_NOTICE` |
| `MIN_PRIORITY_FEE` | 0 | Tips buy nothing under first-come-first-served |
| `DEFER_LIMIT` | 3 L2 blocks | A passed-over transaction is included or dropped with a reason within this bound |
| `FORCED_GAS_RESERVE` | 10,000,000 gas | A third of the block, always available to the forced prefix |
| `INGRESS_SEGMENT_SLA` | 10 s after the block announcement | Watcher alert threshold for a data fault |
| `SEQUENCER_ROTATION_SLA` | 36 QTC blocks after published fault evidence | Mirrors `WITHDRAWAL_SLA` |
| `INGRESS_LOG_RETENTION` | 30 days | Receipts and segments stay fetchable from the sequencer and its mirrors |
| `POLICY_NOTICE` | 144 QTC blocks (≈ 1 day) | Minimum notice before an ordering-policy change |

## 14. Security model and threats

| Threat | Mitigation at v1 | Residual |
|---|---|---|
| Sequencer censors | Forced inclusion; non-compliant batches are invalid under derivation | Chain halts until a compliant batch is posted |
| Sequencer equivocates (two unsafe chains) | Only anchored batches are canonical; registry UTXO chain prevents two anchors | Users who acted on soft confirmations may see them reverted |
| Sequencer posts an invalid state root | Every derivation node re-executes; v1 halts, v2 skips via fraud proof | Liveness loss until the operator or federation posts a valid batch |
| Federation withholds withdrawals | CTV-committed batches are permissionless once committed; watchers alert on SLA | Cannot force commitment before Stage C |
| Federation key compromise (classical or quantum) | ML-DSA-44 keys; rotation by `BridgeVerifierSetCommitment`; M-of-N threshold | A quorum compromise can mis-release reserve funds; bounded by reserve size per epoch |
| L1 reorg | Hash-keyed window, rollback to fork point; deposits credited only at `SAFE_DEPTH`, withdrawals at `FINALITY_DEPTH` | Reorgs deeper than `REORG_WINDOW` halt the chain for manual recovery |
| Data withholding (DAC interim) | Nodes refuse to advance without data | Halt, not fork; removed entirely by witness-space carriage |
| Precompile bug | Shared code with QTC core; FIPS 204 KATs; differential test against a second implementation; consensus-critical review | A divergence between L1 and L2 verification would be a chain-splitting bug |
| Cross-chain replay | `chain_id` and domain tags in every signed statement | None if tags are respected |
| Quantum attack on classical EOAs | Flagged or disallowed (owner decision) | Users who keep ECDSA accounts carry the risk knowingly |
| Sequencer front-runs, reorders or quietly omits | First-come-first-served with signed receipts, signed block announcements, `ordering_root` anchored in every batch, watcher invariants (section 7.5); forced prefix enforced by derivation | At v1 the only sanction is key rotation by the federation; a slashable bond waits for Stage C |
| Sequencer delays acceptance at the gateway so another arrival goes first | `received_at` in every receipt; uniform published rate limits; watchers compare submit-to-receipt latency across sources | Measurable, not provable; persistent skew is a reputational and rotation matter |

## 15. Proof roadmap and the interfaces reserved for it

- `verification_root` already binds `post_state`, `parent_hash` and `withdrawal_root`. A fraud
  proof (roadmap Stage C timeframe) is a QTC transaction that spends a challenge bond and proves,
  with `OP_CAT` to reconstruct the statement and `OP_CHECKSIGFROMSTACK` to verify the sequencer's
  signature over it, that an anchored `post_state` does not follow from the posted data. The
  challenge game and bond sizes are a separate specification.
- A validity proof (Stage D) is carried as an additional carrier chunk; the anchor format does not
  change. Verification on L1 requires the Stage D opcode; until then derivation nodes verify the
  proof off chain and treat an invalid proof as an invalid batch.
- Both paths remove the federation from correctness; only custody of the reserve remains, which
  Stage C's `OP_CHECKANCHORPROOF` exit leaf then removes as well.
- An ordering fault (section 7.5) is already two contradicting signed sequencer statements, or one
  signed statement against posted data, so it is arbitrated by the same `OP_CHECKSIGFROMSTACK`
  game at Stage C against an ordering bond, without any further format change.

## 16. Components, repositories and operations

| Component | Language / base | Repository |
|---|---|---|
| `qtc-reth` | Rust, reth fork with precompile and chain spec | `qtcchain/qtc-reth` |
| `qtc-evm-node` | Go: L1 reader, derivation, sequencer, batcher, watcher subcommands | `qtcchain/qtc-evm-node` |
| Contracts | Solidity, Foundry: `L2Bridge`, `SequencerFeeVault`, `L1Origin`, `PQAccount`, `PQAccountFactory`, reuse of `WQTCAtomicSwapHTLC` and `ECDSAMultisigVerifier` (testnet only) | `qtcchain/qtc-evm-contracts` |
| Explorer | Blockscout with a PQ-account badge and QTC anchor links | `qtcchain/qtc-evm-explorer` |
| Ops | systemd units with the fleet's hardening drop-ins; sequencer and batcher on dedicated droplets; derivation nodes co-located with QTC relays C and D | iCloud `QTC/software/hardening` profiles apply unchanged |

The QTC node used by the batcher and derivation nodes runs with `txindex=1`, ZMQ `hashblock`, and a
wallet that holds only the batcher key. The build box remains the orchestrator for releases and
long-running jobs, per the owner's standing rule.

## 17. Test plan

1. **Precompile**: FIPS 204 KATs; differential fuzzing against QTC core's verifier; gas
   calibration run on the testnet hardware class.
2. **Derivation**: regtest QTC with scripted reorgs (`invalidateblock`, competing miners) of depth
   1, 3, 10 and `REORG_WINDOW`+1; property test that two nodes fed the same QTC chain produce
   identical heads; fuzzing of batch decoding.
3. **Forced inclusion**: sequencer configured to censor; assert chain halts at `INBOX_DEADLINE`
   and resumes when a compliant batch is posted.
4. **Bridge adversarial campaign** (from the bridge architecture doc): double-mint, reorg during
   deposit, refund race, hash-domain confusion, withdrawal template mismatch, dust accumulation.
5. **Data availability load**: sustain 30 KB/s of batch data on QTC testnet for 48 h; measure
   relay and mempool behaviour of 1 MB carrier transactions on the fleet.
6. **Account flows**: PQ account creation, user operations with 2.4 KB signatures, EIP-7702
   migration from a classical EOA, federation attestation verification cost.
7. **Chaos**: sequencer crash mid-batch, batcher double-post, DAC member offline, QTC node restart
   under the hardening sandbox.
8. **Ordering**: submit transactions with tips from 0 to 100× the base fee from several sources and
   assert block order equals receipt order with the forced prefix first in L1 order; feed the
   sequencer an engine payload out of `seq` order and assert it halts rather than announces;
   present a signed receipt whose `seq` the segments omit and assert the watcher reports an
   omission fault; serve a tampered segment and assert the `ordering_root` mismatch is reported;
   crash the sequencer after announcing a block and assert the restart resumes from it.

## 18. Rollout

| Phase | Deliverable | Exit criteria |
|---|---|---|
| 0 | wQTC on an established EVM chain (existing plan) | Contracts audited; federation live |
| 1 | Devnet: `qtc-reth` + precompile, single sequencer, batcher to QTC regtest, predeploys, explorer | Test plan items 1, 2, 6 green |
| 2 | Public testnet on QTC testnet; DAC interim; forced inclusion; ordering policy of sections 7.1–7.5 in force with public receipts and segments; CTV-committed withdrawals | Items 3, 4, 5, 7, 8 green; 30 days without derivation divergence between independent nodes and without an ordering fault |
| 3 | Mainnet as a federated rollup; witness-space carriage; watchers and alerting on the fleet | Reserve invariants hold for 30 days; two independent derivation operators |
| 4 | Fraud proofs; then validity proofs after the Stage D soft fork | Separate specifications |

## 19. Decisions required from the owner

1. Mainnet account policy: classical EOAs allowed and flagged, or bridged QTC only to PQ accounts.
2. Sequencer operator at launch: the wQTC federation or a separate operator.
3. Data availability at mainnet launch: witness-space carriage from day one, or DAC interim.
4. Chain ids and the public names of the network (proposed: QTC-EVM, chain id 20820).
5. Whether the base fee is burned (default) or routed to the treasury.
6. Whether ingress segments are only served by the sequencer and mirrored by watchers (default), or
   also posted to QTC as carrier chunks, at roughly 40 bytes of L1 data per transaction.
7. Whether priority fees are kept as operator revenue under first-come-first-served, or
   `MIN_PRIORITY_FEE` is enforced as a hard zero (tipped transactions rejected) to remove any
   incentive to deviate from the policy.

## 20. References

- `docs/wQTC.md`, `docs/wqtc-bridge-architecture.md`, `docs/wqtc-contracts.md`
- `docs/qtc-asset-ledger-trust-model-and-roadmap.md` (Stages A–D)
- `docs/qtc-agent-wallet-spec.md` (HTLC venue, `ctv_multi_pq` descriptor)
- `src/shielded/bridge.h` (`BridgeExternalAnchor`, `BridgeBatchLeaf`, `BridgeVerifierSetCommitment`)
- `src/script/script.h`, `src/script/interpreter.h` (P2MR, PQ CHECKSIG opcodes, CTV, CSFS, CAT)
- `src/consensus/consensus.h`, `src/policy/policy.h`, `src/kernel/chainparams.cpp` (limits)
- FIPS 204 (ML-DSA), FIPS 205 (SLH-DSA); ERC-4337 v0.7; EIP-7702; EIP-1559
