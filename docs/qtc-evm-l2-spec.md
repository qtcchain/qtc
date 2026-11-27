# QTC-EVM — engineering specification for a QTC-anchored EVM layer 2

Status: draft v0.1, 2026-10-11. Companion to `wQTC.md`, `wqtc-bridge-architecture.md`,
`wqtc-contracts.md`, `qtc-asset-ledger-trust-model-and-roadmap.md` and `qtc-agent-wallet-spec.md`.
Normative words (MUST, SHOULD, MAY) are used in the RFC 2119 sense.

## 1. Scope

QTC-EVM is an EVM execution layer whose only consensus is QTC's MatMul proof of work. Contracts run
in an unmodified EVM; batches of layer-2 blocks are posted to QTC; every QTC-EVM full node derives
the same chain from QTC blocks alone. The gas token is QTC bridged in through the lock-and-mint
machinery already specified for wQTC. Accounts are post-quantum by default through an ML-DSA-44
verification precompile.

In scope: chain parameters, execution engine, sequencer, batcher, anchoring and data availability,
derivation (including L1 reorg handling), bridge, post-quantum accounts, hashing domains, security
model, test plan, rollout.

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
- **Trust is stated, not implied.** At v1 the federation holds the reserve and decides when a
  withdrawal is paid. Until the exit milestones of section 10.5 are met, QTC-EVM is a federated
  bridge with explicit custody and withdrawal-liveness risk and is described as one.
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
| Sequencer | Liveness only | Orders L2 transactions, produces L2 blocks every `L2_BLOCK_TIME`, gives soft confirmations |
| Batcher | Liveness only | Compresses L2 blocks into batches, posts data and anchors to QTC |
| Federation (attestors, M-of-N) | Custody of locked QTC; correctness and liveness of withdrawals until proofs and enforced exits exist (section 10.5) | Signs epoch statements and withdrawal batches with ML-DSA-44; rotates via `BridgeVerifierSetCommitment` |
| Derivation node | None | Reads QTC, reconstructs the L2 chain, drives an execution engine |
| Execution engine | None | Unmodified EVM (reth fork `qtc-reth`) behind the Engine API |
| Data-availability committee (interim only) | Availability only | Stores batch data and signs availability certificates while witness-space DA is not yet used |
| Watcher | None | Independent node that alerts on sequencer equivocation, missing data, anchor gaps, withdrawal-template mismatch |

## 5. Chain parameters

| Parameter | Value | Note |
|---|---|---|
| `CHAIN_ID` | 20820 (0x5154, "QT") mainnet; 20821 testnet; 20822 devnet | Must not collide with registered EVM chain ids; register before public testnet |
| Native token | QTC, 18 decimals | 1 sat = 10^10 wei, exactly as `wQTC.md` §2 |
| `L2_BLOCK_TIME` | 2 s | Soft blocks; batches aggregate ~300 of them per QTC block |
| L2 gas limit | 30,000,000 per block | Raise only after DA throughput is measured on testnet |
| Base fee | EIP-1559, minimum 1 gwei-equivalent | Base fee is burned on L2 (reduces circulating bridged supply); priority fee to sequencer vault |
| EVM version | Cancun without blob transactions (type 3 rejected) | Blob carriage has no QTC analogue; batch data goes to QTC witness space |
| Predeploys | `L2Bridge` 0x...4200, `SequencerFeeVault` 0x...4201, `L1Origin` 0x...4202, `EntryPoint` (ERC-4337 v0.7) 0x...4203, `PQAccountFactory` 0x...4204 | Addresses fixed in genesis |
| Precompile | `MLDSA44_VERIFY` at 0x0000000000000000000000000000000000000100 | Section 10 |
| Genesis state | Predeploys only; zero native balance anywhere | All QTC on L2 originates from bridge deposits |

## 6. Execution engine

- `qtc-reth` is a fork of reth with: the chain spec above, the precompile, blob transactions
  disabled, and nothing else. The fork MUST track upstream and carry its delta as a patch series
  that can be rebased in a day.
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
- Accepts transactions over standard JSON-RPC, builds blocks every `L2_BLOCK_TIME`, and returns
  receipts as soft confirmations. Soft confirmations are non-binding; wallets MUST display them as
  such until the `safe` head passes the block.
- **Forced inclusion.** The L1 inbox (section 8.4) is read at every QTC block. Every inbox item in
  a QTC block that reaches `SAFE_DEPTH` MUST be included in an L2 block whose L1 origin is at most
  `INBOX_DEADLINE` QTC blocks later. A batch that violates this is invalid under derivation, which
  is what makes censorship impossible rather than merely discouraged.
- **L1 origin.** Every L2 block carries the hash and height of the QTC block it was built against.
  `L1Origin` is updated once per L1 block with the QTC block hash, height, time and `nBits`, so
  contracts can read QTC state with the same freshness guarantees as the derivation node.

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
  sequencer_sig  bytes   ML-DSA-44 over SHA256d(QTCEVM/batch/v1 || fields above)
```

`L2Block` = timestamp, L1-origin reference, transaction list (RLP), and nothing else; everything
else is recomputed. A batch MUST contain at least one block and at most `MAX_BLOCKS_PER_BATCH`.

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
7. Executing the blocks through the engine yields `post_state`; otherwise the batch is **invalid**
   and derivation halts (v1) or the batch is skipped (v2, once fraud proofs exist to arbitrate).

### 9.3 Head advancement

- unsafe: sequencer blocks received over the sequencer's block feed, accepted only if they extend
  the current safe head.
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
and is tracked in sections 10.5 and 15. Watchers alert when a finalized batch has withdrawals
and no commitment output appears within `WITHDRAWAL_SLA` blocks. The alert is not a deadline:
nothing on chain follows from it.

### 10.4 Invariants (checked continuously by watchers)

- Σ native L2 supply + pending withdrawals + dust ledger = Σ reserve UTXOs (in wei at 10^10).
- Every `Deposit` event matches exactly one inbox UTXO; every inbox UTXO with `SAFE_DEPTH` has a
  `Deposit` within `INBOX_DEADLINE`.
- Every finalized `Withdrawal` appears in exactly one committed withdrawal batch.

### 10.5 Custody, withdrawal liveness and the exit milestones

Added 2026-11-26 after a review of the trust the withdrawal path places in the federation. It
restates limits that sections 10.3 and 14 already admit, makes the public description of the
system normative, and fixes what has to be shown, publicly, before that description may change.

**What v1 guarantees, mechanism by mechanism.**

| Mechanism | Guarantees | Does not guarantee |
|---|---|---|
| Depositor refund leaf (8.4, 10.2) | A deposit that is never credited returns to the depositor after `DEPOSIT_TIMEOUT`, with nobody's cooperation | Anything once the deposit is credited and swept into the reserve |
| Forced inclusion (7, 9.2 item 6) | An inbox transaction, a call to `L2Bridge.withdraw` included, is in a batch by the deadline or every derivation node rejects the sequencer's batches | That batches are posted at all: a sequencer that stops halts the chain. Any movement of reserve funds on QTC. A forced `withdraw` burns the L2 balance and leaves a claim that only the federation can pay |
| CTV commitment (10.3) | Once a withdrawal batch is committed, anyone can execute it and the federation cannot alter it | That the commitment is ever created |
| `WITHDRAWAL_SLA` watcher (10.3, 13) | A public alert when a finalized batch has withdrawals and no commitment 36 QTC blocks after finality | Any consequence on chain. It is an alarm, not a deadline, and no recovery path opens when it fires |
| Derivation nodes and watchers (9, 10.4) | Detection: an invalid batch, a wrong withdrawal template or a broken invariant stops honest nodes and raises alerts | Prevention: a federation quorum can still sign a release that the L2 state does not justify |
| Registry recovery leaf (8.3, leaf C) | The federation can recover the registry after a timeout | Anything for users |

**Residual trust at v1.** Three things rest on named parties and on nothing else:

1. *Custody.* A quorum of the federation controls the reserve. It can release funds the L2
   state does not justify. The loss is bounded only by the size of the reserve.
2. *Withdrawal liveness.* A quorum can withhold the commitment of section 10.3 indefinitely.
   Nothing at protocol level forces the commitment or replaces the federation in creating it.
3. *Sequencing liveness.* One sequencer posts batches. If it stops, no L2 transaction is
   processed, a `withdraw` included. Forced inclusion makes a censoring batch invalid; it does
   not produce a batch.

**Description (normative).** Until the four exit milestones below are met and their evidence is
published, QTC-EVM is a **federated bridge with explicit custody and withdrawal-liveness risk**.
Specifications, papers, websites, wallet text and announcements produced by the project MUST use
that description or one that names both risks, and MUST NOT call bridged funds trustless,
non-custodial or self-custodial, nor say they can be recovered without the federation or the
operator, nor that users have a unilateral exit. Phases 3 and 4 of section 18 ship under this
description. The architecture is a route to reducing the trust; a roadmap is not the guarantee.

**Exit milestones.** The description may change only when all four hold, each with evidence
anyone can check.

**E1. Enforced unilateral exits.** Roadmap Stage C (`OP_CHECKANCHORPROOF`, section 15) is
active on QTC mainnet, the reserve sits under an exit leaf, and an addendum to this
specification is published with the code. The roadmap describes Stage C for attested ledgers in
general. For QTC-EVM the addendum MUST settle five points that design leaves open:

- *What an exit authenticates against.* Inside `verification_root` an anchor commits two
  roots: `post_state`, an EVM state root that QTC script cannot open, and `withdrawal_root`, a
  plain Merkle tree of the withdrawals recorded in the batch. An exit can therefore cover a
  withdrawal already recorded in an anchored batch. It cannot cover a balance that was never
  withdrawn, and once the sequencer has stopped no new `withdraw` can be recorded. Either the
  anchor gains an exit root over balances, or batches that contain only inbox transactions
  become postable by anyone after a sequencer timeout. The choice defines *eligible funds*.
  Value held inside a contract is eligible only through what that contract lets its users
  withdraw.
- *Who vouches for the root.* After Stage B, QTC validates an anchor's structure and
  continuity, not its contents, and an anchor is posted with the batcher's key alone (8.3,
  leaf B). An exit leaf that trusted any matured anchor would let the holder of that key anchor
  a fabricated root and empty the reserve. Exits must rest on a stated assumption: a federation
  attestation of each anchored root (a later refusal then cannot block exits against roots
  already attested), a challenge window with fraud proofs, or validity proofs (Stage D). The
  addendum names the one chosen and what stays trusted under it.
- *Value limits enforced by consensus.* An exit pays exactly the proven amount and returns the
  rest of the reserve output to the reserve script, and each leaf exits once.
- *Time bounds.* Anchor maturity, the minimum age of the anchor exited against, the challenge
  window, and the worst-case time from a user's first action to funds on QTC.
- *Migration.* Existing reserve outputs move under the exit leaf. That move needs the
  federation once, and it is complete and checkable on chain before E2 is attempted.

Evidence: the activation height, the consensus code and tests in `qtcchain/qtc`, the addendum,
and the reserve outputs' scripts on chain.

**E2. An independently reproduced recovery drill.** On the public testnet first, then on
mainnet with real value. From a published QTC height the sequencer and batcher are stopped and
the federation signs nothing and commits nothing. A person with no affiliation to the project,
using only published software and instructions, recovers eligible funds to a QTC wallet they
control. Evidence: the start height, their transaction ids, the instructions, and a second
unaffiliated party repeating them.

**E3. Independent data recovery.** A fresh derivation node, given this specification, a QTC
node and the published chain parameters, reconstructs the L2 state and the exit proofs with
every project server unreachable. That requires: all batch data in witness-space carriers on
QTC for the whole history (8.2); the parameters (genesis, registry start, key schedule,
descriptors) published with a hash that is itself on QTC; a derivation mode that refuses the
data-availability fallback and reports, per batch, where its data came from; a tool that
outputs the exit proof for an account; and a stated bound on the time a full reconstruction
takes on stated hardware. Evidence: a third party's
run log, a head equal to the other verifiers', and the proofs used in E2. This is the principle
of section 2 ("the posted data is the chain") shown, not asserted.

**E4. Adversarial testing and external review.** Demonstrated rejection of: an invalid
withdrawal or exit claim; a second exit, or an exit and a normal withdrawal, for the same leaf;
an exit against a root that predates the holder's own later spend; exits and federation
withdrawal batches competing for one reserve output; and each of these across QTC reorgs up to
`REORG_WINDOW`. An external review covers the exit opcode, the reserve scripts, `L2Bridge`,
the derivation rules and the federation's signing software. Findings and fixes are published.

**What does not count.** A normal withdrawal demonstration, an audit, and two verifiers with
matching heads are each useful evidence. None of them, alone or together, shows recovery while
the federation refuses.

**Status on 2026-11-26.**

| Milestone | Status |
|---|---|
| E1 | Not started. Stages B and C are roadmap items with no consensus code. The five points are open |
| E2 | Not possible by construction. The only unilateral path exercised is the refund of an uncredited deposit, on the devnet |
| E3 | Not met. The batcher has posted every testnet batch in witness carriers, so the data is on QTC. The verifier has read none of it from there: none of the 5,266 batch derivations in its testnet log took its data from QTC. Carrier assembly rejects a batch that spans more than one carrier, and the fallback store hides the failure. It works on the devnet, where a batch fits one carrier. No fallback-free mode and no exit-proof tool exist, and the time of a full re-derivation is neither bounded nor recently measured |
| E4 | Partly. Double credit, phantom credit and censorship were drilled on the devnet. The rest of section 17 item 4 is not recorded as run, the testnet bridge has carried no deposit, and no external review has been completed |

Two implementation gaps sit under the table of guarantees above. Forced inclusion covers
deposits only: the inbox payload's calldata is ignored, so a `withdraw` cannot be forced yet.
And the witness-carrier defect in the E3 row means that today a derivation node does depend on
the operator's data store, against the principle of section 2.

**Decisions this section needs from the owner**, in addition to section 19:

1. A cap on the reserve at mainnet launch and the rule for raising it. Under the description
   above the cap is the bound on what users can lose (proposed: a cap, raised only on evidence).
2. Publication, before launch, of who the federation is: members, threshold, their
   independence and how keys are held. The stated risk is a risk in those parties.
3. Whether pending withdrawals, their age and every `WITHDRAWAL_SLA` breach are shown on a
   public page, so that withholding is visible to anyone (proposed: yes).
4. Which answers to the first two points of E1 the project commits to, and whether Stage C is
   pursued for QTC-EVM at all. If it is not, the description above is permanent and is said so.

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
  `QTCEVM/attest/v1`. Tags are ASCII, no length prefix, fixed in this document.
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

## 14. Security model and threats

| Threat | Mitigation at v1 | Residual |
|---|---|---|
| Sequencer censors | Forced inclusion; non-compliant batches are invalid under derivation | Chain halts until a compliant batch is posted |
| Sequencer equivocates (two unsafe chains) | Only anchored batches are canonical; registry UTXO chain prevents two anchors | Users who acted on soft confirmations may see them reverted |
| Sequencer posts an invalid state root | Every derivation node re-executes; v1 halts, v2 skips via fraud proof | Liveness loss until the operator or federation posts a valid batch |
| Federation withholds withdrawals | CTV-committed batches are permissionless once committed; watchers alert on SLA | Cannot force commitment before Stage C: users who withdrew hold a claim only the federation can pay (section 10.5) |
| Federation key compromise (classical or quantum) | ML-DSA-44 keys; rotation by `BridgeVerifierSetCommitment`; M-of-N threshold | A quorum compromise can mis-release reserve funds; bounded by reserve size per epoch |
| L1 reorg | Hash-keyed window, rollback to fork point; deposits credited only at `SAFE_DEPTH`, withdrawals at `FINALITY_DEPTH` | Reorgs deeper than `REORG_WINDOW` halt the chain for manual recovery |
| Data withholding (DAC interim) | Nodes refuse to advance without data | Halt, not fork; removed entirely by witness-space carriage |
| Precompile bug | Shared code with QTC core; FIPS 204 KATs; differential test against a second implementation; consensus-critical review | A divergence between L1 and L2 verification would be a chain-splitting bug |
| Cross-chain replay | `chain_id` and domain tags in every signed statement | None if tags are respected |
| Quantum attack on classical EOAs | Flagged or disallowed (owner decision) | Users who keep ECDSA accounts carry the risk knowingly |

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

## 18. Rollout

| Phase | Deliverable | Exit criteria |
|---|---|---|
| 0 | wQTC on an established EVM chain (existing plan) | Contracts audited; federation live |
| 1 | Devnet: `qtc-reth` + precompile, single sequencer, batcher to QTC regtest, predeploys, explorer | Test plan items 1, 2, 6 green |
| 2 | Public testnet on QTC testnet; DAC interim; forced inclusion; CTV-committed withdrawals | Items 3, 4, 5, 7 green; 30 days without derivation divergence between independent nodes |
| 3 | Mainnet as a federated rollup; witness-space carriage; watchers and alerting on the fleet | Reserve invariants hold for 30 days; two independent derivation operators |
| 4 | Fraud proofs; then validity proofs after the Stage D soft fork | Separate specifications |

No phase in this table changes how QTC-EVM is described. Phases 3 and 4 ship as a federated
bridge with explicit custody and withdrawal-liveness risk. That description changes only when
the exit milestones E1 to E4 of section 10.5 are met, which needs the roadmap's Stage B and
Stage C soft forks and is tracked apart from these phases.

## 19. Decisions required from the owner

1. Mainnet account policy: classical EOAs allowed and flagged, or bridged QTC only to PQ accounts.
2. Sequencer operator at launch: the wQTC federation or a separate operator.
3. Data availability at mainnet launch: witness-space carriage from day one, or DAC interim.
4. Chain ids and the public names of the network (proposed: QTC-EVM, chain id 20820).
5. Whether the base fee is burned (default) or routed to the treasury.

## 20. References

- `docs/wQTC.md`, `docs/wqtc-bridge-architecture.md`, `docs/wqtc-contracts.md`
- `docs/qtc-asset-ledger-trust-model-and-roadmap.md` (Stages A–D)
- `docs/qtc-agent-wallet-spec.md` (HTLC venue, `ctv_multi_pq` descriptor)
- `src/shielded/bridge.h` (`BridgeExternalAnchor`, `BridgeBatchLeaf`, `BridgeVerifierSetCommitment`)
- `src/script/script.h`, `src/script/interpreter.h` (P2MR, PQ CHECKSIG opcodes, CTV, CSFS, CAT)
- `src/consensus/consensus.h`, `src/policy/policy.h`, `src/kernel/chainparams.cpp` (limits)
- FIPS 204 (ML-DSA), FIPS 205 (SLH-DSA); ERC-4337 v0.7; EIP-7702; EIP-1559
