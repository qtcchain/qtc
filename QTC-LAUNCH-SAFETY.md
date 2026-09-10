# QTC launch-safety proposal — difficulty curve, floors, and activation discipline

**Status:** Option B **APPLIED to mainnet consensus** on 2026-09-06 (§8) with a
**placeholder `powLimit`** — the floor must be re-sized from the measured launch
fleet (D3) and the genesis regenerated again before any public launch.
**Basis:** QTC's own difficulty history (v0.33.1 → v0.34.5) and the two incidents
it records in-code: the ASERT "dump floor" (shipped at 191,714 after a stall could
unwind difficulty ~2000×) and the network split at 199,295/199,299 (a flag-day
re-anchor shipped after the height was already mined).

## 1. What QTC inherits today (v0.33.1 base) — and why it is the wrong default

| Field (mainnet) | Inherited value | Hazard for a fresh chain |
|---|---|---|
| `nFastMineHeight` / `nPowTargetSpacingFastMs` / `nFastMineDifficultyScale` | 50,000 blocks at **0.25 s**, target **6× eased** | A ~50k-block, hours-long fast-mine window: a few early machines mint a large share of supply, and the chain is trivially 51%-able throughout. This was a QTC *launch distribution* choice, not a safety property. |
| `powLimit` | `66c154…` (compact `0x1f0a3d70`), the **easy bootstrap floor** | The only floor. A hashrate stall lets ASERT ease nBits all the way back to it — the exact QTC incident ("reopen the sibling lottery"). |
| `nMatMulAsertHalfLife` | **3,600 s** (1 h); upgrade height disabled | Short τ = fast unwind: ~11 half-lives (~2000×) in a stall. QTC moved to 14,400 s only after the incident. |
| *dump-floor mechanism* (`powLimitUpgrade`) | **absent** (v0.33.3 addition) | No hard floor exists in the code at all. |
| `nMatMulMaxFutureMtpDriftHeight` | **118,482** (QTC history) | Timestamp-shock/timewarp bound is **inactive for the first 118,482 blocks** of a new chain. |
| `nMatMulPreHashEpsilonBitsUpgradeHeight` | 50,000 → epsilon 18 | Weaker header gate for the first 50k blocks for no reason. |
| `nMatMulAsertBootstrapFactor` | 180 | Tuned for QTC's activation at 50k; revisit once the anchor moves to genesis. |

Principle: **inherit nothing here by accident.** Every value is a decision.

## 2. The central tension

A fast-distribution bootstrap *needs* an easy floor (blocks must come at ~0 hashrate).
Stall-safety *needs* a hard floor. QTC had both because it added the hard floor late
and kept the easy one for historical headers. A fresh chain does not have to carry
that complexity — it must choose.

## 3. Decisions and recommendation

### D1 — Bootstrap window
- **Option B (recommended): no fast-mine phase.** `nFastMineHeight = 0`,
  `nMatMulAsertHeight = 0`: ASERT governs from genesis, anchored at genesis. The
  genesis `powLimit` *is* the floor and is sized to the expected launch fleet (§D3).
  Simplest, no port, no early-supply concentration, and stall-safe from block 0.
- Option A: a short fast phase (order 1,000–2,000 blocks) if a deliberate
  distribution window is wanted. This *requires* porting the v0.33.3 dump-floor
  mechanism (`nMatMulPowLimitUpgradeHeight`, `powLimitUpgrade`,
  `MatMulAsertPowLimitForNextHeight` + the ASERT clamp) so the easy bootstrap
  floor is locked out once the phase ends. More code, more surface, and it is
  the configuration that bit QTC. Choose only with a stated distribution goal.

### D2 — Half-life
`nMatMulAsertHalfLife = 14,400` (4 h) **from genesis** — set the base value
directly; no upgrade height needed. This is the upstream chain's post-incident value. Leave
`nMatMulAsertHalfLifeUpgradeHeight` disabled (it exists; unused).

### D3 — Floor sizing (Option B)
Size the genesis `powLimit` so the *expected launch fleet* produces ~90 s blocks
with headroom, rather than the ~0.25 s bootstrap target.

Method: per-eval success probability `P = target / 2^256`; blocks/s ≈ P × R where
R is the fleet's **full-digest** rate. For 90 s blocks: `P ≈ 1 / (90 × R)`.

Worked example (state your own R): testnet's genesis target (`P ≈ 0.0096`) gave
2.7 s/block on one CPU core of this Mac → R_cpu ≈ 39 dig/s. For a small launch
fleet of ~1,000× that (a few A6000-class GPUs on the *full-digest* rate, which is
the pre-hash-gate candidate rate ÷ 2^epsilon), `P ≈ 1/(90 × 3.9e4) ≈ 2.9e-7`.
Set `powLimit` a few × easier than that for headroom, and **measure the real
fleet's full-digest rate first** (`qtc-matmul-verify` / the stage benchmarks) —
do not launch on the estimate. Keep `fPowAllowMinDifficultyBlocks = false`
(inert for MatMul difficulty — `MatMulAsert` returns before that branch — but
keep it false for hygiene).

**Two hard requirements on the chosen value (found in §7):**
- Make `powLimit` *compact-exact* (a 3-byte mantissa at a byte boundary, e.g.
  `0x03ffff << 232`), as Bitcoin does, and set the **genesis `nBits` to exactly
  `compact(powLimit)`**. A genesis `nBits` looser than `powLimit` is tolerated by
  consensus (`MatMulAsert` clamps the anchor) but **aborts `getblockheader`/
  `getblock` on block 0** with "Internal bug detected: DeriveTarget"
  (`rpc/util.cpp` `GetTarget` `CHECK_NONFATAL`) — every explorer and wallet
  scanning from genesis would die.
- Sizing matters more than the clamp: absolute ASERT does **not** re-anchor at
  the floor. After a stall the accumulated time surplus persists and is burned
  only by fast blocks, so a fleet for which the floor is easy mints
  `≈ (stall − prior deficit) / 90 s` blocks at floor difficulty (§7: ~480 blocks
  for a 24 h stall). Sizing the floor to the launch fleet is what bounds that
  burst.

### D4 — Timestamp/drift protection from genesis
`nMatMulMaxFutureMtpDriftHeight = 0` (keep drift 3,600 s), **plus**
`nMatMulTimewarpReconcileHeight = 0` and `enforce_BIP94 = true` (QTC mainnet has
both; the reconcile is the "a5" liveness fix that keeps the BIP94 lower bound
from crossing the drift upper bound). Inheriting 118,482/125,000 leaves QTC
unprotected for its first ~120k blocks. The drift bound is a hard header rule
(block time ≤ parent MTP + 3,600 s): after a long stall the miner stamps blocks
at that limit and the chain clock catches up ~1 h per block — verified in §7
that this **does not wedge the chain** and that ASERT eases only ≈18 % per
block instead of collapsing to the floor at once.

### D5 — Header gate from genesis
`nMatMulPreHashEpsilonBitsUpgradeHeight = 0`, epsilon 18 from block 0.

### D6 — Activation-runway rule (governance, not a param)
Every future consensus height must satisfy, at release tag time:
`height ≥ current_tip + (2 release cycles of blocks)`, and must **never** be a
height already reachable or mined. Ship the software carrying the height
*before* the height, with a supermajority of reachable peers upgraded. This is
QTC's own §G invariant, and the 199,299 split is what violating it costs.

### D7 — Early-chain reorg insurance
While hashrate is tiny: pin checkpoints frequently (e.g. every ~1,000 blocks for
the first weeks) and raise `nMinimumChainWork` / `defaultAssumeValid` with each
release. These are release-time values, not launch constants.

## 4. Recommended mainnet chainparams (Option B) — applied 2026-09-06, see §8

```
consensus.nFastMineHeight                       = 0;
consensus.nMatMulAsertHeight                    = 0;
consensus.nMatMulAsertHalfLife                  = 172'800;     // 2 days at 600 s (§10)
consensus.nMatMulAsertHalfLifeUpgradeHeight     = INT32_MAX;   // unused
consensus.powLimit                              = <sized per §D3, then measured>;
consensus.fPowAllowMinDifficultyBlocks          = false;
consensus.nMatMulMaxFutureMtpDriftHeight        = 0;
consensus.nMatMulMaxFutureMtpDrift              = 43'200;      // tau/4 at 600 s (§10)
consensus.nMatMulTimewarpReconcileHeight        = 0;           // a5 liveness fix
consensus.enforce_BIP94                         = true;
// genesis nBits MUST equal compact(powLimit) (RPC GetTarget aborts otherwise — §7)
consensus.nMatMulPreHashEpsilonBitsUpgradeHeight= 0;
consensus.nMatMulPreHashEpsilonBitsUpgrade      = 18;
// nMatMulAsertBootstrapFactor: re-evaluate for a genesis anchor (QTC's 180 was
// tuned for activation at 50k); nPowTargetSpacingFastMs/nFastMineDifficultyScale
// become inert with nFastMineHeight = 0.
```
**Viability — verified in code, corrected by simulation.** `ValidateMatMulAsertParams`
imposes no height-0 constraint (it checks only half-life/spacing/factor sanity and
retune ordering), no mainnet construction assert touches these heights beyond the
equality (0 == 0), and mainnet has `fPowNoRetargeting = false`, so
`GetNextWorkRequired` routes straight into `MatMulAsert` from block 1 with the
genesis block as anchor. Option B therefore requires **no consensus code change**.
*Correction to the earlier wording:* regtest carries the Option-B heights
(`nFastMineHeight = 0; nMatMulAsertHeight = 0`) but inherits Bitcoin's
`fPowNoRetargeting = true`, so default regtest **never executes ASERT** — the
dry-run "mined and synced" on constant genesis difficulty. The stall simulation
(§7) added `-test=matmulasert`, which clears only that flag and is the first
run in which the Option-B anchor actually retargeted. Two further requirements
surfaced there: genesis `nBits` **must equal `compact(powLimit)`** (see D3), and
D4 needs the timewarp reconcile height alongside the drift bound.

## 5. Validation plan (before any mainnet genesis)
1. *Done:* regtest carries the Option B heights and passes construction and the
   dry-run; with `-test=matmulasert` it actually retargets from the genesis
   anchor (see the §4 correction).
2. Mine at deliberately low hashrate; confirm block times converge to ~90 s.
   *Controller verified* in §7 (every block matches the ASERT model; exact
   cadence holds the target steady). **Wall-clock convergence still to be
   observed** — fold into the testnet burn-in (step 4).
3. **Simulate a stall** — *DONE, PASS, §7* (both without and with the D4 drift
   bound): difficulty eases toward and never below `powLimit`, clamps exactly,
   recovers cleanly; the drift bound does not wedge the chain.
4. Repeat on **testnet** with several nodes (the burn-in already tracked).
5. Only then set mainnet genesis params; the genesis block is regenerated
   (hashes change with `powLimit`/nBits).

## 6. Decisions required
- [x] D1: Option B (no fast phase) — **applied** (§8)
- [ ] D3: launch-fleet full-digest rate R (measured) → `powLimit` — **placeholder
      applied; must be re-sized + genesis regenerated before launch**
- [x] D2/D4/D5: 14,400 s τ, drift + reconcile + BIP94 from genesis, epsilon-18
      from genesis — **applied** (§8)
- [ ] D6/D7: adopt the runway rule and early-checkpoint cadence as policy
- [x] D8: activate the non-difficulty QTC hardening from genesis (61k Freivalds
      binding / product digest / reorg protection, 125k nonce seed, 130.5k
      parent-MTP seed, shielded 61k/88k/123k, subsidy-penalty window → never)
      — **applied** (§9)
- [ ] D9: shielded-pool lifecycle. **Applied default: sunset from genesis**
      (the upstream chain's live post-125,000 rule state — no shielding, no pool credit,
      recovery-exit path only; velocity window never opens). The alternative
      is a live pool (`nShieldedSunsetHeight`, `…PoolCreditDisableHeight`,
      `…DirectSendPublicFlowDisableHeight`, `…RecoveryExitActivationHeight`
      all `INT32_MAX`). Re-opening later is a normal activation height;
      closing a live pool is the path that cost QTC. Confirm or flip (§9).

## 7. Stall simulation — results (regtest, 2026-09-06) — PASS

**Driver:** `contrib/qtc-launch/stall_sim.py --variant {nodrift,drift}`
(deterministic, `setmocktime`-driven; every block's `nBits` is checked against
an independent Python model of the node's clamped aserti3-2d formula anchored at
genesis, tolerance 0.002 in log2). Evidence: `contrib/qtc-launch/results/`.

**Regtest knobs added (regtest-only, `DEBUG_ONLY`):** `-test=matmulasert`
(clears the inherited `fPowNoRetargeting`, keeps the genesis anchor),
`-regtestmatmulpowlimit=<hex>`, `-regtestmatmulmaxfuturemtpdriftheight=<n>`,
`-regtestmatmultimewarpreconcileheight=<n>`; plus the existing
`-regtestgenesisbits` and `-test=bip94`.

**Setup:** floor `0x03ffff·2^232` (compact `0x2003ffff`; genesis `nBits`
identical), τ = 14,400 s, spacing 90 s, dimension 64, CPU backend. Phases:
30 blocks at exact cadence → 480 blocks with time pinned (≈3 half-lives early,
target 2^−2.99 ≈ 8× harder than the floor) → **24 h stall** (6 half-lives, i.e.
2^3 *below* the floor if unclamped) → 600 blocks mined as fast as the floor
allows → 30 blocks at exact cadence. 1,140 blocks per variant.

| | nodrift (raw clamp) | drift (D4 + reconcile + BIP94 from genesis) |
|---|---|---|
| pre-stall log2(target/floor) | −2.994 | −2.994 |
| carrier block Δt vs parent | 86,320 s (full jump) | **3,599 s** (clamped to MTP + drift) |
| first post-stall target | **floor, clamped at once** | 2^−2.757 (eased ≈18 %) |
| floor reached | +1 block (h 512) | +79 blocks (h 590) |
| blocks held at floor | 479 | 396 |
| lift-off, then tightening resumes | yes (final 2^−0.749) | yes (final 2^−0.751) |
| any block above `powLimit` | none | none |
| max model error (log2) | 1.8e-4 | 1.8e-4 |
| blocks rejected | 0 | 0 |

**Findings.**
1. *The floor holds and the clamp is exact.* No block in either run carried a
   target above `powLimit`; the post-stall target lands on `compact(powLimit)`.
2. *Recovery is clean.* Lift-off occurs exactly when the accumulated surplus is
   burned; tightening resumes at 2^(−90/τ) per early block and an exact 90 s
   cadence holds the target constant. ASERT never fell back to its fail-closed
   path.
3. *Absolute ASERT keeps the surplus — a clamp is not a re-anchor.* The number
   of floor-difficulty blocks after a stall is ≈ (stall − prior deficit)/90 s:
   here (6τ − 3τ)/90 ≈ 480. With a floor that is easy for the fleet these
   arrive in minutes — QTC's "sibling lottery" burst in miniature. **D3 (size
   the floor to the fleet) is what bounds that cost; the clamp only caps it.**
4. *The D4 drift bound does not wedge the chain* after a stall: the miner stamps
   blocks at `MTP + 3,600 s` and validation accepts them; the chain clock
   catches up ~1 h per block, so the unwind is smoothed to ≈18 % per block (79
   blocks to the floor) — but the same total surplus enters eventually.
5. *RPC hazard:* genesis `nBits` looser than `powLimit` aborts `getblockheader 0`
   ("Internal bug detected: DeriveTarget", `rpc/util.cpp` `GetTarget`). Consensus
   tolerates it, so the crash surfaces only in RPC/explorers. Set genesis
   `nBits = compact(powLimit)` and pick a compact-exact `powLimit`.
6. *Regtest correction:* default regtest never ran ASERT (`fPowNoRetargeting =
   true` inherited from Bitcoin); the earlier "regtest already runs Option B"
   statement is corrected in §4. Mainnet has `fPowNoRetargeting = false`.

**Not covered here:** wall-clock convergence to ~90 s blocks (needs real-time
mining; testnet burn-in), and the `nMatMulAsertBootstrapFactor` question is
moot for Option B (`next_height == nMatMulAsertHeight` is never reached at 0).

## 8. Applied to mainnet (2026-09-06)

`CMainParams` in `src/kernel/chainparams.cpp` now carries:

```
consensus.powLimit                              = 0x013333·2^216 ("0000013333…", compact 0x1e013333)  // PLACEHOLDER sizing, re-derived for 600 s in §10
consensus.nFastMineHeight                       = 0;
consensus.nMatMulAsertHeight                    = 0;
consensus.nMatMulAsertHalfLife                  = 172'800;     // 2 days at 600 s (§10)
consensus.nMatMulAsertHalfLifeUpgrade           = 172'800;     // upgrade height stays disabled
consensus.nMatMulAsertBootstrapFactor           = 1;           // inert at height 0; non-zero for the validator
consensus.nMatMulMaxFutureMtpDriftHeight        = 0;           // drift 3'600 unchanged
consensus.nMatMulTimewarpReconcileHeight        = 0;
consensus.enforce_BIP94                         = true;        // unchanged
consensus.nMatMulPreHashEpsilonBits             = 18;
consensus.nMatMulPreHashEpsilonBitsUpgradeHeight= 0;           // upgrade value 18
genesis nBits                                   = 0x1e080000   // == compact(powLimit)
```

**Genesis regenerated:** `3bbc692bb5b5846aa572367088362b32c7bcd83ef6c1e05b0afe2ae72199f63b`
(merkle unchanged `1d61d719…5685e7a2`; timestamp/nonce/version unchanged — only
`nBits` moved). Verified by booting `qtcd -chain=main`: `getblockheader 0`
returns `bits 1e080000`, `target == powLimit` — the §7 RPC hazard is closed.

**Floor sizing is a placeholder.** `2^235` is the D3 worked example (P = 2^-21
≈ 4.8e-7 per full digest): a few A6000-class miners at ~3.9e4 digests/s make
~54 s blocks *at the floor*; one CPU core (~39/s) would take ~15 h. Before
launch: measure the real fleet's full-digest rate, pick a compact-exact
`powLimit` a few × easier than `1/(90·R)`, set genesis `nBits` to its compact,
and regenerate (the hash changes). Everything else in this section is final.

**Tests rewritten as Option-B invariants** (QTC's 50k / 61k / 118,482 / 125,000
difficulty history was pinned in four suites): `pow_tests`
(`ChainParams_MAIN_matmul_activation`, new
`ChainParams_MAIN_option_b_asert_anchored_at_genesis`, genesis header/hash
freezes, `EffectiveTargetSpacingForHeight`), `matmul_dgw_tests`
(`asert_mainnet_option_b_no_fast_phase_retargets_from_genesis`),
`matmul_params_tests`, `qtc_launch_readiness_tests`.

**Deliberately unchanged in this step (done in §9):** the non-difficulty QTC
upgrade heights — Freivalds binding / product digest / reorg protection at
61,000, nonce-seed at 125,000, parent-MTP seed at 130,500, shielded activations
at 61,000/88,000, subsidy-penalty window 130,000–132,000. See D8/D9 in §6.

## 9. D8/D9 applied — all QTC hardening from genesis (2026-09-06)

Principle: a fresh chain runs QTC's **current** rule set from block 0 instead
of replaying QTC's flag days. Every remaining QTC height in `CMainParams` was
classified and set:

| Field | QTC | QTC | Class |
|---|---|---|---|
| `nMatMulFreivaldsBindingHeight` | 61,000 | **0** | hardening |
| `nMatMulProductDigestHeight` | 61,000 | **0** | hardening (payload consensus-required from 0; compact-block serving stays off, as on QTC ≥ 61k) |
| `nReorgProtectionStartHeight` (depth 12) | 61,000 | **0** | hardening |
| `nMatMulNonceSeedHeight` | 125,000 | **0** | hardening |
| `nMatMulParentMtpSeedHeight` | 130,500 | **0** | hardening |
| `nShieldedTxBindingActivationHeight` | 61,000 | **0** | hardening |
| `nShieldedBridgeTagActivationHeight` | 61,000 | **0** | hardening |
| `nShieldedSmileRiceCodecDisableHeight` | 61,000 | **0** | hardening (legacy codec never accepted) |
| `nShieldedMatRiCTDisableHeight` | 61,000 | **0** | hardening (legacy proof never accepted) |
| `nShieldedSpendPathRecoveryActivationHeight` | 88,000 | **0** | hardening |
| `nShieldedC002ActivationHeight` | 123,000 (default) | **0** | hardening |
| `nEmptyBlockSubsidyPenalty{,Strict,End}Height` | 130,000 / 130,500 / 132,000 | **never** (`INT32_MAX`) | rolled back forward-only at 132,000 on QTC; steady state is "no penalty" |
| `nShieldedPoolCreditDisableHeight` | 125,000 | **0** | D9 lifecycle |
| `nShieldedSunsetHeight` | 125,000 | **0** | D9 lifecycle |
| `nShieldedDirectSendPublicFlowDisableHeight` | 128,000 | **0** | D9 lifecycle |
| `nShieldedRecoveryExitActivationHeight` | 125,000 | **0** | D9 lifecycle (≥ sunset; exits prove against the live, empty tree — no frozen root needed) |
| `nShieldedUnshieldVelocity{Activation,End,MinCap}Height`, `MinCap` | 125,000 / 135,000 / 132,000 / 10,000 QTC | **never / 0** | D9: closed recovery window; with an empty pool it never opens |
| `nShieldedPQ128UpgradeHeight`, `nShieldedV2SendZeroOutputExitActivationHeight` | `INT32_MAX` | `INT32_MAX` | never activated on QTC either — not "current rules", left off |
| `nMLDSADisableHeight` | `INT32_MAX` | `INT32_MAX` | unchanged |

**What D9 means in practice:** QTC launches with **no usable shielded pool**
(shield/unshield RPCs reject as post-sunset; only the recovery-exit path is
live, against an empty tree). This is the smallest attack surface and the
state the upstream network is actually in; the shielded/PQC audit debt stays out
of the launch. If a live pool is wanted, flip the four D9 lifecycle heights to
`INT32_MAX` **before** launch — a fresh genesis is not needed (heights do not
enter the genesis hash), but the D9 tests in `pow_tests`/`validation_tests`
must be flipped with it.

**Tests rewritten** (QTC heights were pinned on `MAIN`): `pow_tests`
(`ChainParams_MAIN_matmul_activation`, nonce-seed boundary test now exercises
the 125,000 boundary on a copy), `matmul_params_tests`, `matmul_subsidy_tests`
(penalty schedule exercised on a copy, mainnet asserts "never"),
`validation_tests` (sunset/velocity invariants for D9). Genesis hash unchanged
(activation heights do not enter the header). Verified by booting
`qtcd -chain=main` after the change.

**Transition-schedule coverage kept, moved off MAIN.** Fifteen suites exercise
the upstream flag-day *transitions* through the global `Params()` (shielded
validation / ingress / proof / wire / egress / mempool / bridge / bundle / relay
/ adversarial / send-runtime-report, the SMILE2 redesign framework, Dandelion
activation, Freivalds pre-binding). With every height at 0 the "pre" state they
test no longer exists on MAIN, so they now run on `QtcScheduleTestingSetup`
(`src/test/util/setup_common.h`), which selects **testnet** — still carrying
QTC's 61k/88k/125k/128k/130k–135k schedule — so the code paths stay covered.
If testnet is later aligned with mainnet, point the fixture at REGTEST with the
`-regtest*height` overrides. One genuine consequence of D8 surfaced in
`pq_wallet_tests`: C002 from genesis means the wallet signs the SLH-DSA backup
leaf in **FIPS-205 mode from block 0** (validation adds
`SCRIPT_VERIFY_SLHDSA_FIPS205` from the same gate); the test helper now mirrors
`CWallet::SlhdsaFips205ForNextBlock()` instead of assuming the legacy mode.
Dandelion stem relay is likewise active from block 0 (it keys off the
MatRiCT-disable height).

## 10. Monetary policy and block spacing — Bitcoin's schedule at 600 s (applied 2026-09-07)

**Decision (modelled first, then applied):** QTC adopts Bitcoin's monetary policy
verbatim — **50 coins per block, halving every 210,000 blocks, 10-minute blocks,
21 M total** — replacing the inherited 20 / 525,000 / 90 s schedule (which issued
half the supply in 18 months and ended the subsidy after 46 years).

| | inherited (20 / 525,000 @ 90 s) | **QTC now (50 / 210,000 @ 600 s)** |
|---|---|---|
| coins/day at launch | 19,200 | 7,200 |
| halving every | 1.50 yr | 3.99 yr |
| 50 % / 90 % / 99 % issued | 1.5 / 5.1 / 10.1 yr | 4.0 / 13.6 / 26.8 yr |
| reward < 1 coin / last satoshi | 7.5 yr / 45 yr | 24 yr / 128 yr |

Halving table (block, year, coins/block): 0 → 50; 210,000 → 25 (y4); 420,000 →
12.5 (y8); 630,000 → 6.25 (y12); 840,000 → 3.125 (y16); 1,050,000 → 1.5625 (y20).

**Spacing-dependent parameters retuned with it (mainnet, testnet, testnet4):**
- `nPowTargetSpacing` / `nPowTargetSpacingNormal` 90 → **600**.
- `nMatMulAsertHalfLife` 14,400 → **172,800 s** (2 days = 288 blocks, Bitcoin Cash's
  ASERT tuning; 14,400 s would have been only 24 blocks). Upgrade value mirrored.
- `powLimit` re-derived for the longer spacing with the same 1.7× headroom as the
  90 s placeholder: **`0x013333 · 2^216`, compact `0x1e013333`** (P ≈ 7.2e-8 per
  full digest; the D3 worked-example fleet makes ~360 s blocks at the floor).
  **Still a placeholder** — re-size from the measured fleet (D3).
- **Drift bound scaled to τ/4 = 43,200 s** (not left at 3,600 s). The bound is
  relative to the parent's *median time past*, and the 11-block median lags ~5
  blocks, so the chain clock can advance at most drift/6 per block. At 600 s the
  old 3,600 s bound allowed exactly 600 s per block — precisely one target
  spacing — so after a long stall ASERT saw **zero surplus and never eased**
  (the first 600 s drift run held 2^−2.985 for 1,100 blocks; at 90 s the same
  bound had allowed 510 s of surplus per block, which is why §7 passed). D4 is
  therefore spacing-dependent: keep drift ≈ τ/4 (the 90 s design's ratio), which
  still bounds a timestamp shock to a quarter half-life per block. A new
  regtest knob `-regtestmatmulmaxfuturemtpdrift=<n>` (and `--drift` on the
  simulator) exercises it. Block-denominated windows now read:
  validation window 1,000 blocks ≈ 7 days, 2,016-block window = 14 days,
  mining-guard near-tip window 2 blocks = 20 min. Block space ≈ 864 MB/day at the
  current 24 MB weight.
- Regtest keeps 90 s / 14,400 s (functional tests); a new regtest-only knob
  `-regtestmatmulpowtargetspacing=<n>` lets the stall simulator model 600 s, and
  `stall_sim.py` gained `--spacing/--tau`.

**Genesis regenerated** (the coinbase value is the subsidy, nBits = compact(powLimit)):
- main    `c53432768f1d1898dfa336fde028b527fef96a44319ee3f8b6c483aa1b8af8e3` (bits `1e013333`)
- test/testnet4/signet `ac4518b8cdd24b4f82789b1136d6735ebd5d2d260d40b61abd49ee11f54140e2`
- regtest `25d0b1c272072b56bb0e79aea8566b16378648775e7a517d9d022720f6a1fca6`
- shieldedv2dev `309ae3de50712d4520cec19066979473a70de4a4b73d89b3327a07c845336e1f`
- merkle `68668615…` (shared coinbase)
- regtest assumeutxo @110: blockhash `b6aaee11a59ad7cad06e57eceb2c822f1332826835a389c72b2037cedb2d29dc`, hash_serialized `703ea63ca4ade1424f0ad35590309974e77c6c65b722121fd9e04dbc3174f0b5`, nchaintx 111 (deterministic unit-test chain, as the snapshot tests require)

**Stall simulation at 600 s / τ = 172,800 s** (`contrib/qtc-launch/stall_sim.py
--spacing 600 --tau 172800 --tighten 864 --stall-halflives 6 --post 1100`;
evidence in `contrib/qtc-launch/results-600s/`). 864 pinned blocks tighten
≈3τ (2^−2.999), then a 12-day stall (6τ), then 1,100 blocks mined as fast as the
floor allows.

| | nodrift (raw clamp) | drift 3,600 s (first run) | **drift 43,200 s (τ/4)** |
|---|---|---|---|
| carrier block Δt | 1,036,656 s (full jump) | 3,599 s | 43,199 s |
| first post-stall target | floor, clamped at once | **2^−2.985 — no easing at all** | 2^−2.756 (≈18 % easier) |
| floor reached | +1 block | **never** (1,100 blocks, still 4.3 days behind the clock) | +78 blocks |
| blocks held at floor | 863 (≈3τ/600, as predicted) | 0 | 785 |
| lift-off / model error | yes / 1.9e-4 | — | yes / 1.9e-4 |
| any block above `powLimit` | none | none | none |

The 3,600 s row is the failure mode described under the drift bullet above and
is why the bound was scaled; with τ/4 the 600 s chain recovers the way the 90 s
chain did in §7. Absolute-ASERT surplus behaviour is unchanged: a stall of S
seconds from D seconds of deficit costs ≈ (S − D)/600 floor-difficulty blocks
(here 864 vs 785 with the drift bound smoothing the entry).

## 11. O5 applied — oracle v2 and product digest v4 (2026-09-09)

**Why.** Security review H4 (`QTC-SECURITY-REVIEW.md`): the v3 digest hashed one inner product per C' tile with a
single σ-derived vector. Re-associating Σ v·(A'B')_tile lets a miner compute all tile words in O(n³/b + n²b) without
forming C'. Measured on this M5: honest attempt 332 ms, shortcut 87 ms (3.8×); GPU estimate ~1.35× only because the
one-hash-per-element oracle (540 k SHA-256 per nonce) dominated GPU attempts — itself a design defect.

**What changed (consensus, from genesis, all networks).**
| | before | after |
|---|---|---|
| Oracle | 1 element per SHA-256 (seed‖LE32(index)) | 8 lanes per SHA-256 (seed‖LE32(index>>3)), lane = index&7 |
| Product digest | SHA256d(tag_v3‖σ‖SHA256d(compressed words)‖n‖b) | SHA256d(tag_v4‖σ‖SHA256(tile hashes)‖n‖b), tile hash = SHA-256 of the full 1 KiB tile |
| Attempt cost, n=512 (M5, 1 thread) | 332 ms honest / 87 ms shortcut | ≈ 279 ms, no shortcut |
| Verifier FromSeed per block | 60 ms | 7.5 ms |

**Not changed.** Genesis (no MatMul fields), header layout, C' payload, Freivalds, ASERT, powLimit (still placeholder).

**Open.** GPU digest kernels (Metal, CUDA) are gated to the CPU path until ported and parity-tested on hardware; the
in-kernel "factored compression" path is the H4 shortcut and must be deleted in the port. Spec: iCloud
`QTC/software/QTC_O5_Implementation_2026-09-09.md`. The upstream chain remains on v3 and is exposed to H4.
