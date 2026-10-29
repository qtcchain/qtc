# QTC base fee with burn — consensus specification

Status: draft v0.1, 2026-10-29. Owner decision of 29 October 2026: QTC layer 1 gets a
protocol-set base fee that rises exponentially while blocks run above a target weight, and the base
portion of every block's fees is **burned**. Companion to `upgrade-policy.md` (section 4 governs the
flag day), `release-process.md` (chain anchor and activation height on every release),
`policy/mempool-limits.md` and `docs/qtc-evm-l2-spec.md` section 5.1 (the layer-2 fee floor, which
is independent of this document). Normative words (MUST, SHOULD, MAY) are used in the RFC 2119 sense.

## 1. Scope and motivation

QTC prices transactions per byte through a first-price auction inherited from Bitcoin. The only
floor is relay policy: 1 000 units per kB (`DEFAULT_MIN_RELAY_TX_FEE`, `src/policy/policy.h`),
that is one unit (10⁻⁸ QTC) per byte. Nothing in consensus raises the price of block space when
blocks fill; the price only rises if users outbid each other.

Two facts make that a weakness on QTC specifically:

- Blocks are 24 000 000 weight units (`MAX_BLOCK_WEIGHT`, `src/consensus/consensus.h`; the
  witness scale factor is 1 so weight equals serialized bytes), of which about 1 MiB is the
  proof-of-work product payload (`matrix_c_data`, 262 144 words). Roughly 23 MB per block is
  transaction space, about 5 900 single-input spends per block or 850 000 per day.
- A single-input pay-to-MatMul-root spend is about 3 900 bytes (an ML-DSA-44 signature of 2 420
  bytes plus a 1 312-byte public key in the witness; block 4 705's anchor transaction is 3 916
  bytes and paid 3 916 units). Chain growth per transaction is 25 times Bitcoin's.

At today's floor, filling a block costs 0.24 QTC (about US$7.80 at the qtc.markets price of
US$32.46) and filling every block for a day costs about US$1 120 for 3.4 GB of permanent chain
growth. Fee revenue is irrelevant today (0.0037 QTC across 48 transactions on 29 October 2026
against 3 850 QTC of subsidy); the point of this change is **spam resistance with a predictable
price**, not revenue.

The mechanism is Ethereum's EIP-1559 idea in the form EIP-4844 uses for blob gas: one integer of
state (the accumulated excess weight above a target), an exponential price in that excess, and the
base portion burned. The arithmetic reuses the fixed-point 2^x approximation QTC already runs in
ASERT difficulty (`CalculateMatMulAsertTarget`, `src/pow.cpp`).

What this does **not** do: it does not raise fees while blocks are below target. On today's load
(fewer than 60 transactions a day) the base rate stays at the floor and nothing changes for any
wallet, miner or explorer. Under sustained load it makes the price compound: 8.5× after 10 full
blocks, 92× after 20, 1 000× after 30 (section 6).

## 2. Design principles

- **Block-level, not transaction-level, enforcement.** Consensus requires each block's total fees
  to cover the burn for the block's transaction weight. It does not require every transaction to
  pay the base rate individually. Rationale in section 4.4: QTC transactions are not independent
  the way Ethereum's are. Child-pays-for-parent, ephemeral-dust packages (`policy/ephemeral_policy`)
  and pre-signed time-locked transactions with fixed fees all rely on one transaction paying for
  another, and a per-transaction rule would strand them when the base rate rose above the fee
  they were signed with. Per-transaction enforcement is mempool policy (section 5), where it
  belongs.
- **Known before the block is built.** The base rate for height *h* depends only on blocks below
  *h*, so a miner, a wallet and a verifier all compute the same number before any transaction is
  selected, exactly as Ethereum's base fee is computed from the parent.
- **The burn is implicit.** In the UTXO model a fee is the gap between inputs and outputs. The
  coinbase may claim only the part above the burn; the rest is never created in any output and
  leaves the money supply. No burn address, no unspendable output, no new script.
- **Nothing changes at the floor.** `BASE_FEE_RATE_FLOOR` equals the existing relay minimum, so a
  chain that never exceeds the target behaves exactly as today. The change is invisible until
  demand exists.
- **Bounded integers.** The excess is clamped so that every quantity in this document fits in an
  int64 and the base rate cannot exceed 2^20 times the floor.
- **Soft fork with a flag day.** Every rule tightens validity. Old nodes accept new blocks; old
  miners produce invalid blocks the moment they claim burned fees. Activation follows
  `upgrade-policy.md` section 4 (design note, pull-request review, release at least 14 days before
  the activation height, height on the release page and on keys.qtc.gold).

## 3. Definitions

| Symbol | Meaning |
|---|---|
| `W(h)` | **Transaction weight** of block *h*: the sum of `GetTransactionWeight(tx)` (`src/consensus/validation.h`) over every transaction in the block **except the coinbase**. The proof-of-work payload and the coinbase are excluded so that a miner cannot move the excess for free by padding either. |
| `E(h)` | **Excess weight** carried into block *h*. `E(ACTIVATION_HEIGHT) = 0`. |
| `R(h)` | **Base fee rate** for block *h*, in units per 1 000 weight units (the `CFeeRate` convention). |
| `B(h)` | **Burn** of block *h*: `ceil(R(h) × W(h) / 1000)`, in units. |
| `F(h)` | Sum of the fees of the block's non-coinbase transactions as consensus already computes them (`txfee` from `Consensus::CheckTxInputs`, transparent inputs minus outputs adjusted by any shielded value balance; `nFees` in `ConnectBlock`). |
| `S(h)` | Block subsidy as consensus computes it today (`GetBlockSubsidyForBlock`: halving schedule, treasury premine at height 1, empty-block penalty where active). |
| `TARGET_WEIGHT` | The transaction weight per block at which the base rate holds steady. |
| `DOUBLING_WEIGHT` | The accumulated excess that doubles the base rate. |
| `EXCESS_MAX` | `20 × DOUBLING_WEIGHT`. Caps the rate at 2^20 × floor. |
| `BASE_FEE_RATE_FLOOR` | The minimum base rate, 1 000 units per kB on every network. |

Parameters per network are in section 7.

## 4. Consensus rules

All rules apply to blocks at height ≥ `ACTIVATION_HEIGHT` of the network. Blocks below it are
unchanged.

### 4.1 Excess update

```
W(h)   = Σ GetTransactionWeight(tx) for tx in block h, tx not coinbase
E(h+1) = clamp(E(h) + W(h) − TARGET_WEIGHT, 0, EXCESS_MAX)
E(ACTIVATION_HEIGHT) = 0
```

`E` is a property of the chain, not of the header. It is derived from the chain of ancestors, like
`nChainWork`, and MUST be identical on every node.

### 4.2 Base fee rate

```
R(h) = min(BASE_FEE_RATE_FLOOR × 2^(E(h) / DOUBLING_WEIGHT), BASE_FEE_RATE_FLOOR × 2^20)
```

evaluated in fixed point as follows, so that every implementation rounds identically:

```
q      = E(h) / DOUBLING_WEIGHT                       (integer division, 0 ≤ q ≤ 20)
frac16 = ((E(h) mod DOUBLING_WEIGHT) << 16) / DOUBLING_WEIGHT        (0 ≤ frac16 < 2^16)
factor = 2^16 + ((C1·frac16 + C2·frac16² + C3·frac16³ + 2^47) >> 48)  (2^16 ≤ factor < 2^17)
R(h)   = (BASE_FEE_RATE_FLOOR × factor) << q  >> 16
```

`C1 = 195766423245049`, `C2 = 971821376`, `C3 = 5127` are the aserti3-2d cubic coefficients
already in `src/pow.cpp` (`ASERT_POLY_COEFF_1..3`, `ASERT_RADIX_BITS = 16`). The implementation
MUST factor that approximation out of `CalculateMatMulAsertTarget` into a shared helper rather than
duplicate it; the unit tests in section 10 pin its outputs. With `E = 0`, `R` is exactly the
floor. With `E = EXCESS_MAX`, `R` is exactly `floor << 20`. The intermediate
`BASE_FEE_RATE_FLOOR × factor << q` is at most 1 000 × 2^17 × 2^20 < 2^48, so int64 suffices.

### 4.3 Burn and coinbase limit

In `ConnectBlock`, where `blockReward = nFees + GetBlockSubsidyForBlock(...)` is computed today
and the coinbase is checked against it (`bad-cb-amount`):

```
B(h)        = ceil(R(h) × W(h) / 1000)
blockReward = S(h) + F(h) − B(h)
```

- If `blockReward < 0` the block is invalid, reject reason `bad-blk-burn-exceeds-reward`. The
  burn may be paid out of fees or out of the subsidy, but not out of thin air.
- If `coinbase.GetValueOut() > blockReward` the block is invalid, reject reason `bad-cb-amount`
  (unchanged string; the limit is what changed).
- The treasury-premine output rule at height 1 and the empty-block subsidy penalty are unaffected:
  both act on `S(h)`, which this rule takes as given.

Nothing in the block names the burned amount. It is `F(h) − (coinbase value − S(h))` for anyone
who wants to display it (section 9).

### 4.4 Why the burn is per block, not per transaction

A per-transaction rule (`fee(tx) ≥ R(h) × weight(tx) / 1000` for every transaction) would be the
literal EIP-1559 translation and is **rejected** for QTC:

1. **Pre-signed transactions.** A time-locked transaction signed today with a fee at the floor
   must remain minable in a year. Under a per-transaction rule a base-rate spike at the moment it
   matures would make it unspendable until the rate decayed, with no way to add fee without a new
   signature from every party. Under the block-level rule a miner can still include it and cover
   its share of the burn from other fees, from a child transaction, or from the subsidy.
2. **Packages.** Child-pays-for-parent, TRUC packages and ephemeral-dust anchors deliberately
   carry zero-fee parents paid for by children. The block-level rule keeps all of them valid.
3. **Equivalent spam economics.** A miner who stuffs its own block pays the burn on that weight
   from its own coinbase, because `blockReward = S + F − B` falls by exactly `R × W / 1000`.
   Self-stuffing to raise the rate for competitors costs the attacker the same as external spam
   and is burned, benefiting no one.
4. **Same price signal.** Every miner who includes a transaction below the base rate loses the
   difference from its own reward, so rational template construction (section 5.2) requires
   packages to pay at least `R(h)`; the per-transaction floor emerges from incentives, enforced by
   policy, without stranding anything.

### 4.5 What old software does after activation

- An un-upgraded **full node** accepts every valid new block (rules only tighten) and follows the
  chain. It will also accept an invalid old-style block from an un-upgraded miner and may follow a
  short invalid fork until the network builds past it; this is the standard soft-fork caveat and
  the reason for the 14-day notice.
- An un-upgraded **miner** produces a valid block only if `F(h) − B(h) ≥ 0` happens to hold and
  the coinbase claims at most `S + F − B`; a block claiming full fees for a non-empty block is
  invalid as soon as `R(h) × W(h) > 0`, that is for every non-empty block at or after activation.
  Un-upgraded miners therefore orphan themselves on their first non-empty block.
- An un-upgraded **wallet** keeps working while the chain is below target. Under load its
  transactions are rejected by upgraded mempools (section 5.1) with the existing
  `mempool min fee not met` reason until the rate decays or the user bumps the fee (RBF is
  unaffected).

## 5. Policy (non-consensus, this implementation)

### 5.1 Mempool acceptance and relay

The relay minimum becomes `max(minRelayTxFee, mempool rolling minimum, R(tip + 1))`, applied
wherever `src/validation.cpp` computes `min_fee_rate` today: single and package acceptance
(`MemPoolAccept::AcceptSingleTransaction` and the package checks built on it) and the mempool
re-evaluation after a tip change (`Chainstate::ConnectTip`, `Chainstate::DisconnectTip`).
`R(tip + 1)` is known from the tip's excess. Package evaluation uses the package fee rate as it does now, so a zero-fee parent with a
paying child is accepted.

On a new tip the mempool is **not** re-evaluated. Transactions below the new base rate stay until
their ordinary expiry (336 hours) and become minable again as the rate decays; they are not
rebroadcast while below the rate.

`-minrelaytxfee` continues to set the floor an operator is willing to relay; it cannot be set below
`BASE_FEE_RATE_FLOOR` on a network where the rule is active (start-up error, like an invalid
`-blockmintxfee` today).

### 5.2 Block template

`BlockAssembler` (`src/node/miner.cpp`) sets `blockMinFeeRate = max(configured, R(h))` for the
template height and skips any package whose fee rate is below it. The coinbase value in the
template is `S(h) + Σ fees − B(h)` with `W(h)` taken over the transactions actually selected.
`getblocktemplate` reports `basefeerate` (units per kB), `txweight`, `burn` and `coinbasevalue`
(already present; its meaning is now net of burn). An external miner that ignores `burn` and
pays itself the gross amount produces an invalid block; external miners that build their own
coinbase MUST read the field.

### 5.3 Fee estimation and wallet

`estimatesmartfee` and `estimaterawfee` return `max(estimator result, R(tip + 1) × (1 + headroom))`
where `headroom` is the largest move the rate can make in one block,
`2^((nMaxBlockWeight − TARGET_WEIGHT) / DOUBLING_WEIGHT) − 1`, about 30 % on mainnet (section 7).
A wallet that needs confirmation within one block pays the headroom; one that can wait does not. `fallbackfee`, `discardfee` and `paytxfee` are clamped up to `R(tip + 1)` at
send time so that no wallet default can produce a transaction the local mempool rejects.

### 5.4 RPC

- `getbasefee` (new): `{ "height": h, "excess": E(h), "basefeerate": R(h), "nextbasefeerate":
  R(h+1) assuming an empty next block, "floor": BASE_FEE_RATE_FLOOR, "target": TARGET_WEIGHT,
  "doubling": DOUBLING_WEIGHT, "active": bool }`.
- `getmininginfo` gains `basefeerate` and `txweightexcess`.
- `getblock` (verbosity ≥ 1) gains `txweight`, `basefeerate` and `burned` for blocks at or after
  activation.
- `getblockchaininfo` gains `basefee: { active, activation_height }`.

## 6. Behaviour under load (mainnet parameters)

With the section 7 mainnet parameters (`TARGET_WEIGHT` 12 000 000, `DOUBLING_WEIGHT` 32 000 000)
and blocks filled to the practical 23 MB of transaction space, the excess grows by 11 MB per full
block. Costs at US$32.46 per QTC:

| Consecutive full blocks | Base rate multiplier | Burn per block | Cumulative burn | Wall clock at 600 s |
|---|---|---|---|---|
| 1 | 1.0× | 0.23 QTC, US$7 | US$7 | 10 min |
| 5 | 2.6× | 0.60 QTC, US$19 | US$64 | 50 min |
| 10 | 8.5× | 1.96 QTC, US$64 | US$273 | 1.7 h |
| 20 | 92× | 21.3 QTC, US$691 | US$3 229 | 3.3 h |
| 30 | 1 002× | 230 QTC, US$7 481 | US$35 259 | 5 h |
| 40 | 10 856× | 2 497 QTC, US$81 051 | US$382 272 | 6.7 h |
| 60 | 2^20 (cap) | 230 000 QTC | US$42.8 million | 10 h |
| 144 (one day) | 2^20 (cap) | 230 000 QTC | US$670 million | 24 h |

Today the same day of full blocks costs about US$1 120.

Decay is symmetric in weight: a block below target reduces the excess by the shortfall, so an empty
block removes 12 MB of excess. Twenty full blocks are undone by nineteen empty ones (about 3 hours),
or by a longer run of lightly used blocks.

A legitimate burst behaves as intended rather than as an attack: an airdrop of 50 000 spends
(200 MB) fills nine blocks, peaks at 8.5× the floor, costs its sender about 0.000 33 QTC
(US$0.011) per transaction at the peak, and decays to the floor nine blocks later. A sustained
load of 11.5 MB per block, half the practical capacity, never moves the rate at all.

Per-transaction cost for the 3 916-byte reference spend: 0.000 039 QTC (US$0.0013) at the floor,
0.000 39 QTC (US$0.013) at 10×, 0.003 9 QTC (US$0.13) at 100×. QTC-EVM anchors are the same size
and are the batcher's cost; at 100× an anchor costs US$0.13, immaterial to the layer 2
(`docs/qtc-evm-l2-spec.md` section 8.3 should cite `getbasefee` for the batcher's fee logic).

## 7. Parameters

| Parameter | mainnet | testnet | regtest | Note |
|---|---|---|---|---|
| `ACTIVATION_HEIGHT` | set at release (section 11) | set at release, before mainnet | 0 by default; `-basefeeactivationheight=<n>` for tests that need a pre-activation stretch | consensus |
| `BASE_FEE_RATE_FLOOR` | 1 000 | 1 000 | 1 000 | units per kB; equals today's relay minimum |
| `TARGET_WEIGHT` | 12 000 000 | 12 000 000 | 20 000 | weight units of non-coinbase transactions per block |
| `DOUBLING_WEIGHT` | 32 000 000 | 32 000 000 | 50 000 | accumulated excess that doubles the rate |
| `EXCESS_MAX` | 640 000 000 | 640 000 000 | 1 000 000 | `20 × DOUBLING_WEIGHT`; cap 2^20 × floor |

Rationale for mainnet: target of half the limit mirrors Ethereum's 2× elasticity; a doubling weight
of 32 MB gives about 10× after ten consecutive full blocks, the response the owner asked for, while
keeping the one-block worst-case move to about +30 % (12 MB of a 32 MB doubling) so a wallet's
one-block headroom (section 5.3) stays small. Ethereum's per-block step of 12.5 % would need 20 blocks for
10× and 40 for 100×, which at 600 s spacing is too slow to deter a spammer who only needs a few
hours. Regtest values let a functional test drive the full range with blocks of a few kilobytes.

The parameters live in `Consensus::Params` (`src/consensus/params.h`) as
`nBaseFeeActivationHeight`, `nBaseFeeRateFloor`, `nBaseFeeTargetWeight`,
`nBaseFeeDoublingWeight`; `EXCESS_MAX` is derived. Like the ASERT schedule, they are immutable
chain parameters and the start-up invariant check in `src/pow.cpp` (the "fail closed to the hardest
target" family) gains the corresponding assertions: floor > 0, target > 0, doubling > 0,
target < `nMaxBlockWeight`.

## 8. Implementation notes

- **State.** Add `nTxWeight` (uint32, non-coinbase transaction weight) to `CBlockIndex` and to its
  disk serialization (`CDiskBlockIndex`, `src/chain.h`), written when a block is first fully
  validated, alongside `nTx`. `E(h)` is **not** persisted: it is recomputed in memory when the block
  index is loaded, in height order from `ACTIVATION_HEIGHT`, the way `nChainWork` is recomputed from
  `nBits`. Entries written by older software lack `nTxWeight`; they are all below the activation
  height at upgrade time, so no reindex is needed, and a node that upgrades after activation
  performs the one-off recomputation from block files for the post-activation range at start-up
  (bounded by the 14-day notice; log a progress line).
- **Headers-first and reorgs.** `E` is defined for a block only once its ancestors' transactions
  are known, so it is attached to the fully validated chain, not to headers. A reorg replaces the
  `E` sequence along with the chain; nothing special is needed, because `E(h)` is a pure function
  of the ancestor blocks. Reorg handling MUST key on block hash, never on height (the September
  ASERT verifier lesson).
- **Assumeutxo.** A snapshot at a height above activation needs the excess at that height to
  validate the next block. `AssumeutxoData` (`src/kernel/chainparams.cpp`) gains
  `m_tx_weight_excess`, hard-coded like `m_chain_tx_count`; `SnapshotMetadata`
  (`src/node/utxo_snapshot.h`) bumps its version and carries the same value, and loading a
  snapshot whose value disagrees with the hard-coded one fails closed. No snapshot above
  activation is published until this is in.
- **Where the checks go.** Weight and fees are both available in `ConnectBlock`; the burn check
  is inserted at the existing `bad-cb-amount` site. `ContextualCheckBlock` cannot compute fees and
  is not touched. The block-weight limit (`bad-blk-weight`) is unchanged.
- **Exponent helper.** Extract the cubic 2^x from `CalculateMatMulAsertTarget` into
  `consensus/fixed_exp2.h` (or similar) with a 64-bit signature `uint64_t Exp2Fixed16(int64_t
  exponent_q16)` used by both ASERT and the base fee; ASERT's outputs MUST be bit-identical before
  and after the refactor (existing ASERT test vectors guard this).
- **Overflow.** `W(h) ≤ nMaxBlockWeight < 2^25`; `E ≤ 20 × DOUBLING_WEIGHT < 2^30`; `R ≤ 1 000 ×
  2^20 < 2^30`; `B ≤ R × W / 1000 < 2^45`; all comfortably int64. `MoneyRange` still bounds `F`.
- **Mining readiness.** `generatetoaddress` (the fleet's mining path) builds through
  `BlockAssembler`, so the pods and node B need no change beyond the release. External miners that
  assemble their own coinbase MUST honour `burn` from `getblocktemplate` (section 5.2).

## 9. Explorer, monitoring and documentation impact

- **Explorer** (`qtc-block-explorer-spec.md` table `blocks`): `fees_total` is currently derived as
  `coinbase_value − subsidy_expected`, which after activation equals fees **minus burn**. Add
  columns `tx_weight`, `base_fee_rate`, `burned` (= Σ input − Σ output over non-coinbase
  transactions, minus `coinbase_value − subsidy_expected`) and a running `burned_total`; show the
  base rate on the block page and the chain page (`/api/v1/chain` gains `basefeerate`,
  `txweightexcess`, `burned_total`). Circulating supply on qtc.markets and qtc.dev is issuance minus
  `burned_total`.
- **Monitoring** (chain-attack exporter and rules): new gauges `qtc_base_fee_rate`,
  `qtc_tx_weight_excess`, counter `qtc_burned_units_total`; alert when `qtc_base_fee_rate` exceeds
  8× the floor for three consecutive blocks (sustained load or attack) and when it is at the cap.
- **Docs**: `qtc-conf.md` (`-minrelaytxfee` clamp), `JSON-RPC-interface.md` (new fields and
  `getbasefee`), `policy/mempool-limits.md` (relay minimum now includes the base rate),
  `release-notes` for the carrying release, `docs/qtc-evm-l2-spec.md` section 8.3 cross-reference.

## 10. Test plan

Unit (`src/test/`):

- `fixed_exp2_tests`: vectors for `Exp2Fixed16` at 0, 2^16 − 1, 2^16, 20·2^16 and the ASERT
  regression vectors; equality of ASERT targets before and after the refactor.
- `base_fee_tests`: `R(E)` at `E = 0`, `DOUBLING_WEIGHT − 1`, `DOUBLING_WEIGHT`, `EXCESS_MAX`;
  monotonic in `E`; `B` rounding (ceil) at boundaries; `E` update clamping at 0 and at
  `EXCESS_MAX`.

Functional (`test/functional/`, regtest parameters of section 7):

- `feature_qtc_base_fee_activation.py`: the block at `ACTIVATION_HEIGHT − 1` may claim full fees;
  the block at `ACTIVATION_HEIGHT` with the same fees and coinbase is rejected `bad-cb-amount`;
  accepted when the coinbase is reduced by the burn; `getbasefee` flips `active`.
- `feature_qtc_base_fee_burn.py`: a block whose fees are below the burn is rejected
  `bad-blk-burn-exceeds-reward` unless the coinbase gives up subsidy to cover it; a coinbase
  claiming one unit too many is rejected; `getblock` reports `burned`; the sum of all UTXO values
  equals issuance minus `burned_total` (gettxoutsetinfo).
- `feature_qtc_base_fee_ramp.py`: mine full blocks and assert `E` and `R` against a Python
  reference implementation of section 4.2 (which the test file carries) for every block up to and
  past the cap, then empty blocks until the floor; include a reorg across the ramp and a restart
  (index reload recomputes `E`).
- `feature_qtc_base_fee_policy.py`: mempool rejects a transaction below `R(tip + 1)` but accepts
  a package whose parent is zero-fee and whose child covers both; template skips below-rate
  packages; `estimatesmartfee` never returns below `R(tip + 1)`; wallet `send` succeeds at the
  floor and under a 10× rate.
- `feature_qtc_base_fee_assumeutxo.py`: load a snapshot above activation with the hard-coded
  excess and validate the next full block; a snapshot with a wrong excess is refused.
- `feature_qtc_base_fee_old_miner.py`: a pre-activation-style block (gross coinbase) from a second
  node after activation is rejected and does not stall the upgraded node.

Testnet soak before mainnet: activate on testnet first (section 11), run the testnet miner pod with
a spam generator at 20 full blocks, confirm the explorer and the alert rule, and confirm QTC-EVM
derivation is unaffected while anchors cost 100×.

## 11. Rollout

1. This document is reviewed in a pull request to `main` (`upgrade-policy.md` section 4 item 1).
2. Implementation lands on `develop` behind `nBaseFeeActivationHeight = INT32_MAX` on mainnet and
   testnet, so merged code is inert until a release sets the heights.
3. The carrying release sets the **testnet** activation height first, roughly 300 blocks after the
   testnet tag (about one week at testnet's current spacing), and ships after the section 10 soak.
4. The **mainnet** activation height is set in the release that ships at least 14 days (at least
   2 016 blocks) before it, stated in the release notes, on the GitHub release page and on
   keys.qtc.gold, with the chain anchor refreshed per `release-process.md`. No P2P message or
   behaviour changes, so the protocol version is not bumped, and no height-triggered peer floor is
   attached to this change (a consensus flag day is sufficient and `upgrade-policy.md` section 4
   forbids combining them).
5. Public notice names the three things an operator must do: upgrade before the height, read `burn`
   if they run an external miner, and update any wallet that hard-codes 1 unit per byte.
6. After activation, the fleet (A2, B, relays C and D, the Mac watch node) is confirmed on the
   same tip, `qtc_base_fee_rate` is at the floor, and the explorer shows `burned = 0` for the
   first blocks.

## 12. Security considerations

- **Griefing miners.** Nobody can raise another miner's burn: `B(h)` is a function of what that
  miner chose to include. A miner can always produce a valid block by including nothing
  (`W = 0`, `B = 0`).
- **Miner stuffing.** Raising the rate by filling one's own blocks burns `R × W / 1000` of the
  stuffer's own reward per block, compounding; it is strictly a loss.
- **Fee sniping and reorgs.** Burned value is not available to a reorging miner either; the
  incentive to reorg for fees is reduced, not increased.
- **Denial of service by cheap bytes.** This is the attack the change addresses; section 6 gives
  the price schedule. The cap of 2^20 × floor (230 000 QTC per full block) is far above any
  rational budget and exists only to bound integers.
- **Security budget.** After the 210 000-block halvings the subsidy shrinks and burned base fees
  are revenue miners no longer receive. Tips (fees above the base) remain theirs. The owner
  accepts this trade; it can be revisited in a later consensus change (for example a treasury
  destination instead of a burn, or a partial burn) under the same flag-day process.
- **Un-upgraded nodes** follow invalid forks of at most a few blocks (section 4.5); the 14-day
  notice and the peer-version metric on relay C limit the population at risk.

## 13. Decisions recorded and decisions still open

Recorded (owner, 29 October 2026):

- The base portion is **burned**, not redirected.
- Enforcement is per block (section 4.4), not per transaction.

Open for the owner before implementation starts:

1. Confirm the mainnet `TARGET_WEIGHT` (12 MB) and `DOUBLING_WEIGHT` (32 MB), or name the
   "full blocks to 10×" figure wanted and the parameters will be solved from it.
2. Whether the carrying release should also lower `-maxmempool` defaults or mempool expiry; this
   document assumes no change.
3. Whether qtc.markets' price model should use issuance net of burn (it does not affect the
   difficulty-driven formula today).

## 14. References

- `src/consensus/consensus.h` (`MAX_BLOCK_WEIGHT`, `WITNESS_SCALE_FACTOR`),
  `src/policy/policy.h` (`DEFAULT_MIN_RELAY_TX_FEE`, `DEFAULT_BLOCK_MIN_TX_FEE`),
  `src/validation.cpp` (`ConnectBlock` fee accumulation and `bad-cb-amount`,
  `GetBlockSubsidyForBlock`), `src/pow.cpp` (`CalculateMatMulAsertTarget`, aserti3-2d
  coefficients), `src/node/miner.cpp` (`BlockAssembler`), `src/consensus/tx_verify.cpp`
  (`CheckTxInputs`, shielded value balance), `src/policy/feerate.cpp` (`CFeeRate::GetFee`).
- EIP-1559 (fee market change), EIP-4844 section "Gas accounting" (excess-based exponential
  pricing and `fake_exponential`), Bitcoin Cash aserti3-2d specification.
- `doc/upgrade-policy.md`, `doc/release-process.md`, `doc/qtc-block-explorer-spec.md`,
  `docs/qtc-evm-l2-spec.md` section 5.1 and 8.3.
