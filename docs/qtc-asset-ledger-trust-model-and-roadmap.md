# Tokenized-asset ledger settled into QTC — trust model and enforcement roadmap

Status: design / planning, 2026-09-19. Written against `qtc-main` at `dcf2e607`.
Companion to [`wqtc-bridge-architecture.md`](wqtc-bridge-architecture.md),
[`wqtc-contracts.md`](wqtc-contracts.md) and [`wQTC.md`](wQTC.md).

This document answers one question precisely: **if we run a tokenized-asset ledger off-chain
(EVM or a custom appchain) and settle it into QTC through the bridge envelope, what does each
party actually guarantee, what does QTC L1 enforce, and what would it take to make QTC enforce
more?** It then lays out a staged roadmap from the federation-attested design that can ship
today to consensus-enforced anchors and exits.

---

## 1. Executive summary

- **What ships today is a federation-attested ledger with L1 anchoring, not a rollup.** The
  attestor federation is trusted for ledger correctness, finality, and custody of locked QTC.
- **QTC L1 today provides:** ordering and timestamping of anchors; post-quantum attestations
  that anyone can verify offline; a refund path on every deposit so a stuck mint never traps
  funds; replay-bound statements (chain id, bridge id, epoch, destination, amount).
- **QTC L1 today does not provide:** validation of anchor contents, fraud proofs, validity
  proofs, or forced exits. A holder's exit claim against the recovery root is only enforceable
  if the federation honours it.
- **Why:** on QTC mainnet the shielded pool is sunset from genesis
  (`fShieldedPoolDisabled = true`, `src/kernel/chainparams.cpp:306`). The consensus-side
  settlement-anchor machinery lives inside the shielded bundle, so it is never reached. The
  envelope structs and the `bridge_*` build/decode RPCs still work as an **off-chain canonical
  format**; the chain only timestamps whatever digest we put in an `OP_RETURN`.
- **The roadmap has four enforcement stages.** Stage A needs no fork and already removes
  anchor equivocation. Stages B and C are soft forks that make anchors and exits
  consensus-enforced, using the version fields and `OP_SUCCESS` slots the envelope reserved
  for exactly this. Stage D replaces attestations with validity proofs.

---

## 2. System under discussion

| Component | Role | Status in repo |
|---|---|---|
| Asset ledger (EVM chain or appchain) | Holds asset-token balances; enforces transfer rules via the compliance hook | Not built; token pattern exists in `contrib/wqtc/evm/WQTC.sol` |
| Attestor federation (N operators, threshold M) | Signs epoch statements with ML-DSA-44 / SLH-DSA-128s; custodies locked QTC | Structs + RPCs built (`BridgeVerifierSetCommitment`, `BridgeBatchReceipt`, `bridge_buildverifierset`) |
| Epoch settlement statement | `BridgeBatchStatement` v5 with `BridgeBatchAggregateCommitment` | Built (`src/shielded/bridge.h:1166-1260`) |
| L1 anchor | Statement digest + verification roots in a QTC `OP_RETURN` (1,000-byte payload) | Anchor struct built (`BridgeExternalAnchor`); posting service not built |
| QTC deposit lock | Two-leaf P2MR tree: claim leaf bound to `destination_id` + CLTV refund leaf | Built (`BridgeScriptTree`, `bridge_address`, `bridge_buildrefund`) |
| Cash leg on the ledger | wQTC (lock-and-mint) for delivery-versus-payment | Reference contracts, unaudited, 21 Foundry tests |
| Relayer / indexer / DA publisher | Watches both chains, publishes epoch data, posts anchors | Not built |

### 2.1 The statement that gets anchored

`BridgeBatchStatement` v5 serialises to roughly 393 bytes:

| Field | Bytes | Meaning for an asset ledger |
|---|---|---|
| `version`, `direction` | 2 | v5; direction is reused as ledger-epoch kind |
| `ids.bridge_id`, `ids.operation_id` | 64 | ledger domain id; epoch operation id |
| `entry_count`, `total_amount` | 12 | number of ledger actions; QTC value moved this epoch |
| `batch_root` | 32 | Merkle root of the epoch's transfers |
| `domain_id`, `source_epoch` | 36 | replay binding: which ledger, which epoch |
| `data_root` | 32 | commitment to the published epoch data blob |
| `verifier_set` (`attestor_count`, `required_signers`, `attestor_root`) | 41 | which federation, what threshold |
| `proof_policy` | 41 | which proof descriptors are required |
| `aggregate_commitment.action_root` | 32 | Merkle root of mints / burns / issuer actions |
| `aggregate_commitment.data_availability_root` | 32 | where the epoch data lives and its digest |
| `aggregate_commitment.recovery_or_exit_root` | 32 | Merkle root of `(holder, asset, balance)` leaves at epoch end |
| `aggregate_commitment.extension_flags` | 4 | `FLAG_HAS_RECOVERY_OR_EXIT_ROOT`, `FLAG_HAS_POLICY_COMMITMENT`, custom-root flags |
| `aggregate_commitment.policy_commitment` | 32 | hash of the compliance policy in force this epoch |

Attestors each produce a `BridgeBatchReceipt` (statement + attestor key + signature). ML-DSA-44
signatures are ~2.4 KB and SLH-DSA-128s ~7.8 KB, so **receipts never fit in the anchor**. The
`BridgeVerificationBundle` collapses them to `signed_receipt_root` and `proof_receipt_root`, and
`BridgeExternalAnchor` (101 bytes: version, `domain_id`, `source_epoch`, `data_root`,
`verification_root`) is what goes on chain, with the statement itself optionally alongside it
(total < 600 bytes, inside the 1,000-byte payload cap approved 2026-09-18).

---

## 3. Trust model, guarantee by guarantee

Legend for "enforced by": **F** = federation honesty (M-of-N), **L1** = QTC consensus,
**EVM** = the ledger's own consensus + the bridge contract, **DA** = whoever hosts epoch data,
**none** = no party is bound.

| Guarantee | Enforced by | How, today | Failure mode if the enforcer misbehaves |
|---|---|---|---|
| Asset balances are correct (no double-spend on the ledger) | EVM | Ledger consensus + token contract | Ledger reorg or chain halt; federation must not attest until finality |
| Transfers obey the compliance policy | EVM | `IComplianceHook` on every transfer/mint/burn; hook installable only by Timelock | Governance installs a malicious hook (censorship) or removes it (non-compliance) |
| The epoch statement matches the ledger | **F** | M-of-N attestors independently replay the epoch and sign | Colluding threshold signs a false root; nothing on QTC detects it |
| Only one statement per epoch (no equivocation) | **none today**; L1 after Stage A | Today: convention. Stage A: exclusive spend of a registry UTXO | Federation publishes two anchors for one epoch; verifiers disagree |
| Anchors are ordered and timestamped | L1 | Block inclusion of the `OP_RETURN` tx; 90 s blocks; MatMul PoW | Deep QTC reorg reorders anchors (mitigated by `nShieldedSettlementAnchorMaturity = 6`) |
| Attestations are verifiable by anyone, offline, quantum-safely | L1 + F | ML-DSA / SLH-DSA receipts over a replay-bound statement; `bridge_decode*` RPCs | Verifier set rotation not anchored → stale set accepted (Stage A fixes) |
| Locked QTC is not stolen | **F** | Claim leaf requires federation M-of-N PQ multisig | Key compromise at threshold drains custody (Ronin / Harmony class) |
| Locked QTC is not trapped | L1 | CLTV refund leaf returns the deposit to the depositor after `refund_lock_height` | None: enforced by script regardless of federation |
| wQTC / asset mints are backed 1:1 | EVM + F | EIP-712 attestation, outpoint replay guard, window mint cap, circuit breaker, guardian veto on mints above `optimisticThresholdSat` | Colluding threshold + guardian asleep: unbacked mint up to the window cap |
| Redemptions are honoured | **F** | `fulfillRedeem` is `FEDERATION_ROLE`; `refundRedeem` after `redeemRefundTimeout` is `GOVERNANCE_ROLE` | Federation stalls → holder gets wQTC back, not QTC |
| Epoch data is available | **DA** | `data_availability_root` commits to a blob; hosting is off-chain | Withholding: nobody can rebuild balances or check attestors |
| A holder can prove their balance at epoch N | L1 + DA | Merkle proof against `recovery_or_exit_root` in a matured anchor | Data withheld → holder cannot form the proof |
| A holder can **force** an exit | **none today**; L1 after Stage C | Today: federation processes exit claims voluntarily | Federation refuses; holder has a proof and no venue |
| Ledger transitions are valid (not just attested) | **none today**; L1 after Stage D | Today: attestations only | Same as false-root above |

### 3.1 What "the federation is the trust point" means in practice

Three distinct powers sit with the same M-of-N set, and the roadmap separates them over time:

1. **Attestation power** — declaring what the ledger state is. Removed by Stage D (validity
   proofs); bounded by Stage B (anchors must chain and match a registered verifier set).
2. **Custody power** — spending locked QTC. Bounded by the refund leaf today; bounded further by
   Stage C (holder-initiated exits against the reserve).
3. **Liveness power** — choosing to process mints, redemptions, and exits. Partly bounded today
   by the EVM `redeemRefundTimeout`; bounded on the QTC side by Stage C.

### 3.2 What QTC L1 gives you that an EVM-only design does not

- **Post-quantum attestations.** ML-DSA / SLH-DSA receipts are the authoritative record.
  The EVM verifier is classical ECDSA (`ECDSAMultisigVerifier`), explicitly a v1 stand-in
  because PQ verification is not feasible in EVM gas; the QTC receipts are what an auditor or
  regulator checks.
- **Neutral, PoW-ordered timeline.** Anchor order does not depend on the ledger's own
  sequencer or validator set.
- **Script-enforced refund.** A deposit's refund leaf is consensus-enforced on QTC; no EVM
  contract or federation action can override it.
- **A canonical envelope with reserved upgrade room.** `BridgeBatchStatement` versioning,
  `extension_flags`, `policy_commitment`, and `ProofEnvelope.extension_digest` were designed
  so semantics can tighten without a new outer object. That is what Stages B–D exploit.

### 3.3 What it does not give you, stated plainly

- No node validates the statement, the attestor signatures, or the epoch chain.
- No fraud-proof or validity-proof venue exists; a false root is only caught socially.
- Exit claims are proofs without a court. The recovery root makes the federation's obligation
  auditable, not enforceable.
- For assets that represent off-chain property (equity, GPU capacity notes, energy campus
  interests), even Stage C only forces exit of the **record** (a canonical cap-table snapshot).
  Delivery of the underlying asset is a legal-wrapper obligation, not a chain guarantee.

---

## 4. Threat scenarios and current mitigations

| Scenario | Impact today | Current mitigation | Roadmap fix |
|---|---|---|---|
| Threshold key compromise | Unbacked mints up to window cap; custody drain | Window mint cap, circuit breaker, guardian veto, HSM custody, independent operators | Stage C reserve exits; Stage D removes attestation power |
| Federation goes dark | Mints and redemptions stall; ledger keeps running | EVM `refundRedeem` after timeout; QTC refund leaf for unminted deposits | Stage C forced exits from reserve |
| Federation censors one holder's exit | Holder holds an unenforceable proof | Public recovery root makes refusal provable | Stage C |
| Anchor equivocation (two statements for epoch N) | Verifiers disagree; auditors cannot pick | Convention only | Stage A |
| Verifier-set rotation not anchored | Stale set still "valid" to offline verifiers | `attestor_root` in statement; rotation by governance | Stage A registry UTXO carries the set; Stage B enforces continuity |
| Epoch data withheld | Balances unrecoverable; exit proofs impossible | Multiple DA hosts; `data_root` commits content | Stage B option: mandatory on-L1 data lane for the exit-root leaves |
| Ledger reorg after attestation | Attested state no longer canonical | Attest only after ledger finality; `source_epoch` monotonic | Stage B rejects non-monotonic epochs |
| QTC reorg reorders anchors | Anchor maturity assumptions broken | 6-block maturity (`nShieldedSettlementAnchorMaturity`) | Stage B makes maturity consensus |
| Compliance hook abused for censorship | Holder frozen without cause | Hook is Timelock-gated and opt-in; unset by default | `policy_commitment` lets the anchored policy be audited against the deployed hook |

---

## 5. Enforcement roadmap

Each stage is independently shippable and strictly raises the floor set by the previous one.

### Stage A — Registry-UTXO anchor chain (no consensus change)

**Goal:** make anchors unique, ordered, and federation-only using script that is live today.

- Create one **domain registry UTXO** per ledger domain: a P2MR output whose primary leaf is
  the federation's M-of-N ML-DSA multisig leaf (supported up to `MAX_P2MR_SCRIPT_SIZE`), plus a
  CSV-delayed governance leaf for key rotation / recovery.
- Every epoch, the federation **spends the registry UTXO** in a transaction that (a) recreates
  the registry output (same or rotated multisig), (b) carries the `OP_RETURN` anchor:
  tag ‖ `BridgeExternalAnchor` ‖ statement, and (c) optionally tops up or draws the domain's
  QTC reserve.
- Properties gained: at most one anchor per epoch (a UTXO can be spent once); anchor order is
  the registry spend chain; only the registered set can advance it; verifier-set rotation is
  itself an anchored event.
- Note: CTV is *not* used for the recurring registry because a CTV template commits to all
  outputs including the yet-unknown `OP_RETURN` payload; M-of-N multisig is sufficient.
- Deliverables: registry descriptor (`mr(...)` form), anchor tag spec, epoch batcher service,
  anchor poster, verifier CLI (`bridge_decode*` wrappers), regtest E2E.
- Exit criteria: two independent verifiers reconstruct the same epoch chain from a fresh node;
  equivocation attempt is rejected by the mempool as a double-spend.

### Stage B — Consensus-validated anchors (soft fork)

**Goal:** QTC nodes validate anchor *structure and continuity*, not ledger contents.

- Activation: height-gated like the existing `nShielded*ActivationHeight` params.
- New consensus rule, applied to any transaction spending a registry UTXO (recognised by a
  registry tag committed in the registry leaf):
  1. exactly one tagged anchor `OP_RETURN`; payload decodes as `BridgeExternalAnchor` +
     `BridgeBatchStatement` v5, `IsValid()` true;
  2. `source_epoch == previous anchor epoch + 1`;
  3. `verifier_set.attestor_root` equals the set committed in the spent registry leaf
     (rotation must be announced one epoch ahead via `extension_flags`);
  4. the statement digest is what the M-of-N multisig signed (the leaf script already binds the
     signatures to the transaction; the rule binds the transaction to the anchor);
  5. an anchor is *matured* after `nShieldedSettlementAnchorMaturity` (6) blocks and enters the
     node's confirmed-anchor set (`ConfirmedSettlementAnchorState` already models this:
     `anchor`, `created_height`).
- Optional data lane: require the `recovery_or_exit_root` leaves (holder, asset, balance) to be
  posted as additional `OP_RETURN` chunks across the epoch, so exit proofs never depend on
  off-chain DA. Size the epoch so leaves fit the 24 MB block budget comfortably.
- Implementation surface: `validation.cpp` (reuse `RebuildShieldedSettlementAnchorState`
  shape for a transparent variant), `nullifier.h` anchor set, chainparams activation height,
  policy rules, functional tests mirroring `mempool_datacarrier.py`.
- Exit criteria: a node rejects an anchor with a skipped epoch, a wrong verifier set, or two
  anchors in one block; `assumeutxo` snapshots carry the anchor set.

### Stage C — Consensus-enforced exits (soft fork, new opcode)

**Goal:** a holder can withdraw QTC-denominated value from the domain reserve against a matured
anchor, without federation cooperation.

- New opcode in a reserved P2MR `OP_SUCCESS` slot (the PQC spec keeps several reserved;
  Falcon slots are spoken for, so allocate a fresh one): `OP_CHECKANCHORPROOF
  <domain_id> <leaf> <merkle_path>` → succeeds iff `leaf` authenticates against the
  `recovery_or_exit_root` of the latest matured anchor for `domain_id`.
- Domain reserve UTXO gets a third leaf: `OP_CHECKANCHORPROOF` + holder PQ `CHECKSIG` +
  a CSV challenge window, and a nullifier-style spent-leaf set so each exit leaf is used once.
- The federation's normal-path leaf keeps operating; the exit leaf is the backstop. To prevent
  a stale-anchor race, the exit leaf requires the anchor to be at least K epochs old, giving
  the federation K epochs to publish a newer root that reflects the holder's spends.
- Scope limit: this forces exit of **QTC value** (locked deposits, wQTC backing, QTC-settled
  proceeds). For asset tokens representing off-chain property it forces a canonical,
  consensus-recognised **cap-table snapshot**, which is what a transfer agent or court needs.
- Exit criteria: on regtest, a holder exits with the federation offline; a holder cannot exit
  twice; a holder cannot exit against a root that predates their own transfer out.

### Stage D — Validity proofs (largest effort)

**Goal:** remove attestation power. The anchor carries a succinct proof that the epoch's
transitions are valid under the committed policy.

- `ProofEnvelope.proof_kind` / `settlement_binding_kind` / `extension_digest` were reserved for
  this. `bridge_buildproofpolicy`, `bridge_buildproofadapter`, and the proof-redesign gate
  (`smile2_proof_redesign_framework_tests`) already define how a new proof object is
  benchmarked for correctness, tamper resistance, size, and runtime.
- Needs a PQ-safe proof system with an L1 verifier opcode; sizing must respect the 24 MWU block
  weight and per-element 10,000-byte script caps.
- Until then, the `wqtc-bridge-architecture.md` note stands: replace the EVM-side ECDSA verifier
  with a zk verifier of the M-of-N ML-DSA receipts as an intermediate step.

### 5.1 Stage ordering and dependencies

```
Stage A  ──►  Stage B  ──►  Stage C  ──►  Stage D
(no fork)     (soft fork:    (soft fork:    (soft fork:
              anchor rules)  exit opcode)   proof opcode)
   │              │
   └─ requires: federation ops, DA hosting, audited EVM contracts, legal wrapper
```

- Stage A can ship with the mainnet pilot.
- Stage B needs Stage A's registry convention to be stable for at least one full activation
  cycle on testnet.
- Stage C depends on Stage B's confirmed-anchor set.
- Stage D is independent research; it can proceed in parallel with B and C.

---

## 6. Delivery roadmap (build phases)

| Phase | Scope | Key deliverables | Exit criteria |
|---|---|---|---|
| 0. Specification | Anchor tag, registry descriptor, epoch data format, exit-leaf format, DA policy, operator requirements | Spec docs in `docs/`; test vectors | Owner sign-off; vectors round-trip through `bridge_build*` / `bridge_decode*` |
| 1. Regtest end-to-end | Ledger on a local EVM devnet; 3-of-5 federation; epoch batcher; anchor poster; verifier CLI | `test/functional/bridge_asset_ledger_e2e.py`; Python SDK extensions in `contrib/wqtc/qtc_wqtc.py` | Deposit → mint → asset DvP → burn → release, with anchors verified from a second node |
| 2. Contracts hardening | Asset token contract, compliance hook implementation, bridge parameterisation | Audit scope package; invariant fuzz campaign; `contrib/wqtc/SECURITY.md` update | External audit complete; all findings closed or accepted |
| 3. Testnet federation | 5+ independent operators, HSM custody, monitoring, DA hosts | Operator runbook (pattern: `doc/m15-full-lifecycle-runbook.md`); alerting on anchor gaps and mint-cap hits | 30 days of continuous epochs; one rehearsed key rotation; one rehearsed operator loss |
| 4. Mainnet pilot (Stage A) | Capped value; `optimisticThresholdSat` and window caps set low; guardian on call | Public verifier instructions; anchor explorer | Caps raised only after N clean epochs and an incident-free rotation |
| 5. Stage B soft fork | Consensus anchor rules; activation height | Consensus PR + functional tests; `QTC-LAUNCH-SAFETY.md`-style checklist | Testnet activation, then mainnet activation with miner signalling |
| 6. Stage C soft fork | Exit opcode; reserve leaf; challenge window | Consensus PR; spec addendum to `doc/qtc-pqc-spec.md` opcode table | Forced-exit drill succeeds on testnet with federation offline |
| 7. Stage D research | Proof system selection; verifier opcode prototype | Benchmark report through the proof-redesign gate | Go / no-go on size and verify-time budgets |

Legal, transfer-agent, and disclosure work for the underlying assets runs alongside Phases 0–4
and gates Phase 4, not the engineering.

---

## 7. Decisions needed from the owner

1. **Ledger substrate:** existing EVM L2, or a dedicated appchain the federation also runs?
   (An existing L2 reduces liveness risk; an appchain simplifies finality for attestation.)
2. **Data availability policy:** off-chain hosts only (cheap, withholdable) or mandatory on-L1
   exit-root leaves from Stage B (robust, consumes block space)?
3. **Federation shape:** N, M, operator independence requirements, and who holds the
   `GUARDIAN` and `GOVERNANCE` roles on the EVM side.
4. **Whether to pursue Stage C at all** for assets whose value cannot be forced on-chain, or to
   stop at Stage B plus a strong legal wrapper.
5. **Whether to reopen the shielded pool pre-launch** (flip the four D9 heights) to inherit the
   existing consensus settlement lane instead of building the transparent one in Stage B.
   The launch-safety doc recommends against it; this document assumes D9 stays applied.

---

## 8. References in this repository

- Envelope and receipts: `src/shielded/bridge.h` (`BridgeBatchStatement`,
  `BridgeBatchAggregateCommitment`, `BridgeExternalAnchor`, `BridgeVerifierSetCommitment`,
  `BridgeBatchReceipt`, `BridgeVerificationBundle`, `BridgeScriptTree`)
- Confirmed-anchor state model: `src/shielded/nullifier.h` (`ConfirmedSettlementAnchorState`)
- Upstream anchor rebuild logic to mirror for the transparent lane:
  `src/validation.cpp` (`RebuildShieldedSettlementAnchorState`)
- Pool-disabled flag and D9 lifecycle: `src/consensus/params.h`, `src/kernel/chainparams.cpp`,
  `QTC-LAUNCH-SAFETY.md` §D9
- OP_RETURN 1,000-byte payload: commit `b92c3d15`
- Reserved `OP_SUCCESS` slots and P2MR leaf grammar: `doc/qtc-pqc-spec.md`
- EVM references and threat corpus: `contrib/wqtc/evm/*.sol`, `contrib/wqtc/SECURITY.md`
