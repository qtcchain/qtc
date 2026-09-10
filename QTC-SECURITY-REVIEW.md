# QTC security review — v0.0.1 tree (2026-09-09)

Scope: consensus, proof-of-work, difficulty, P2P/DoS, wallet/PQ signature, and launch-configuration
surfaces of the QTC tree at local `qtc-main` (tree identical to `qtcchain/qtc` root commit).
Method: code reading of the verified locations cited below plus arithmetic checks; no fuzzing or
functional-test runs were performed for this review. Severity reflects impact at launch.

Findings are ordered by severity. "Verified" means the code path was read in this review.

---

## HIGH

### H1. Genesis timestamp is fixed at 2026-03-19; launching later mints at floor difficulty
- **Where:** `src/kernel/chainparams.cpp` — `CreateQTCGenesisBlock(1773878400, …)` for main and test.
  ASERT is anchored at genesis (Option B), so `time_diff` is measured from genesis `nTime`.
- **Mechanism:** every second between genesis `nTime` and the real first block is a time surplus.
  ASERT eases 2× per 172,800 s of surplus, clamps at `powLimit`, and (verified: the clamp is not a
  re-anchor) the surplus is only burned by mining blocks faster than 600 s.
- **Numbers as of 2026-09-09** (surplus 174 days = 15,033,600 s):

  | floor block time | blocks at floor | duration | coins minted at floor |
  |---|---|---|---|
  | 360 s | 62,640 | 261 days | 3.13 M |
  | 60 s | 27,840 | 19 days | 1.39 M |

  Meanwhile the chain clock (MTP-relative drift bound, `nMatMulMaxFutureMtpDrift` 43,200 s) can only
  advance ≤ 7,200 s per block, so nothing else limits this.
- **Impact:** whoever has hardware at launch mints a large fraction of the early supply at
  minimum difficulty; the schedule is effectively front-loaded and any "fair launch" claim fails.
- **Fix:** regenerate genesis with `nTime` within a few hours of the real launch (the regen procedure
  is documented in QTC-LAUNCH-SAFETY.md §8) and re-bake all six genesis asserts and the test pins.
  Treat the genesis time as a launch-day artifact, not a repository constant.

### H2. `powLimit` is a placeholder; mis-sizing either kills or floods the chain
- **Where:** `powLimit` = `0x013333·2^216` (compact `0x1e013333`), marked PLACEHOLDER.
- **Impact:** too hard → no block ever found (dead chain; ASERT cannot ease below the floor).
  Too easy → blocks at the pre-hash/hardware minimum rate until ASERT tightens (each 172,800 s of
  deficit halves the target; from a floor that is 2^k too easy the chain over-mints for ~k half-lives).
- **Fix:** measure the launch fleet's MatMul rate at `nMatMulDimension`, set the floor so the expected
  block interval at floor is ≈ 600 s for a *single* mid-range GPU, then rerun
  `contrib/qtc-launch/stall_sim.py --spacing 600 --tau 172800 --drift 43200`.

### H3. Header-only chain work is unverified and, at floor difficulty, free to forge
- **Where (verified):** `src/pow.cpp:2665` `CheckMatMulProofOfWork_Phase1` only checks the miner-supplied
  `matmul_digest ≤ target` (no recomputation). `src/validation.cpp:9991` adds the sigma pre-hash gate,
  `DeriveSigma(header) ≤ target << 18` (`nMatMulPreHashEpsilonBits = 18`). `src/chain.cpp:140`
  `GetBlockProof` credits chain work purely from `nBits`. Real MatMul verification happens only with the
  block payload (`validation.cpp:10137–10167`).
- **Cost to forge a header that passes header acceptance:**

  | difficulty vs floor | expected hashes per forged header |
  |---|---|
  | 1× (launch) | 53 |
  | 1,000× | 5.3 × 10⁴ |
  | 10⁶× | 5.3 × 10⁷ |

- **Impact:** an attacker can push arbitrarily long header chains with full claimed work. Nodes will
  follow them as best-header, request blocks, and only then reject. Mitigations do exist
  (per-address MatMul verification budgets and Phase-2 failure punishment in `net_processing.cpp`
  lines 164–662; invalid-block descendants marked failed), but `nMinimumChainWork = 0` and a
  genesis-only checkpoint table (`chainparams.cpp:326,388`) mean headers-presync accepts anything.
  This is an inherited design property, not a regression; the exposure is largest in the launch
  window when difficulty is near the floor.
- **Fix:** (a) add a functional test that floods a node with Phase-1-valid/Phase-2-invalid header
  chains at floor difficulty and asserts the node stays synced and bans the source; (b) after a
  few thousand blocks, publish `nMinimumChainWork` and a checkpoint in a point release; (c) consider
  raising the epsilon (smaller shift) so header forgery is never cheaper than ~2^20 hashes.

### H4. Transcript compression lets a miner compute the digest with ~10× less work than the matmul
- **Where (verified):** `src/matmul/transcript.cpp` `ComputeProductCommittedDigest` — every 16×16 block of
  C' = A'·B' is compressed to one field element by an inner product with a single σ-derived vector v
  (`DeriveCompressionVector(sigma, b)`), and only those 1,024 elements are hashed. Active from genesis
  (`nMatMulProductDigestHeight = 0`). The transcript path (`CanonicalMatMul`) has the same structure.
- **Mechanism:** the compressed value of block (i, j) is a linear functional of C', so with
  U_i = vᵀ·A'_rows(i) (cost n²·b for all i) every compressed value is Σ_l Σ_m U_i[l,m]·B'[m, jb+l]
  (cost n³/b in total). At n = 512, b = 16: 12.6 M multiply-adds per nonce versus 134 M for the
  honest matmul, i.e. 10.7× cheaper. Verified numerically (random M31 matrices, identical outputs).
  Only the winning nonce needs the real C' for the Freivalds payload: one matmul per block.
- **Impact (corrected after measurement):** the 10.7× is the matmul arithmetic alone. Every nonce still
  needs A and B from the seed oracle (540 k SHA-256 calls, one per element), which the shortcut cannot
  skip, so the wall-clock advantage is bounded by the oracle's share of an attempt: 3.8× measured on an
  M5 CPU (332 ms honest vs 87 ms), an estimated ~1.35× on an A6000-class GPU where the oracle is ~72 %
  of the attempt (assumption, measure before finalising). A shortcut miner still out-earns honest
  miners on equal hardware and the chain's work is not what the parameters imply. The spec's
  §8.3.2/§8.3.5 analysed forgery of the compression, not its cost. The upstream chain has run this
  digest since its height 61,000.
- **Related observation:** one SHA-256 per matrix element makes the PoW oracle-bound on GPUs (≈ 72 %
  of an attempt), contrary to the "matmul is the work" design; it is why the A6000 measured as
  CPU-feed-bound in July. Fix together with H4 (yield 8 elements per hash).
- **Status (2026-09-09): FIXED in code (option O5, commit pending full-suite result)** — v4 digest hashes every
  C' tile in full; oracle v2 yields 8 lanes per hash. GPU kernels gated to CPU until ported.
- **Fix (as modelled and applied):** hash every 1 KiB tile of C' with SHA-256 and bind the root in
  the digest (cost < 1 % CPU, ~2 % GPU, no new primitive), and take 8 field elements per oracle hash.
  Modelled options and numbers: `QTC_H4_Fix_Options_Model_2026-09-09.md` (iCloud QTC/network).

---

## MEDIUM

### M1. 12-block reorg limit from genesis → permanent-split risk at tiny hashrate
- **Where:** `nMaxReorgDepth = 12` (main) with `RecordRejectedReorgDepth` / `RecordDeferredReorgDepth`
  in `validation.cpp:911–960`; no `nMinimumChainWork`, no `defaultAssumeValid`.
- **Impact:** with a handful of miners, a network partition (or an eclipsed miner) longer than
  12 blocks produces two chains that will never reconcile automatically. The "deferred" path
  requires a work margin, which a small honest majority may never supply.
- **Fix:** document the operator procedure (`invalidateblock`/`reconsiderblock`), seed enough public
  nodes before launch (see M3), and ship checkpoints in the first point release. Alternatively
  start with a larger depth (e.g. 100) and reduce it once hashrate is established.

### M2. Timewarp exposure is bounded but nonzero
- **Where (verified):** `MAX_FUTURE_BLOCK_TIME = 7200` (`src/chain.h:30`, enforced at
  `validation.cpp:10054`); MTP-relative drift bound 43,200 s from height 0;
  `nMatMulTimewarpReconcileHeight = 0`.
- **Analysis:** a miner can push each block's time up to 2 h into the future; with the drift bound the
  chain clock still advances at most 7,200 s per block, and ASERT's easing per block from that is
  2^(7200/172800) − 1 ≈ 2.9 %. A sustained majority attacker gains ≈ 2.9 % per block only while
  honest miners keep re-anchoring real time. Acceptable; noted so that nobody "simplifies" the
  drift bound later.

### M3. No DNS seeds, no fixed seeds, empty mining-guard mesh → isolation and eclipse at launch
- **Where:** `vSeeds` empty on all nets; `mining_guard.cpp` default mesh `{}` and guard OFF.
- **Impact:** first nodes depend entirely on `-addnode`/`-connect`; a single well-connected attacker
  node can eclipse new joiners. Combined with M1 this is how a permanent split would start.
- **Fix:** stand up ≥3 public nodes on independent hosts/ASNs, populate `vSeeds` and the fixed-seed
  list, and put the same nodes into the guard mesh before flipping the repo public.

### M4. Shielded pool is "sunset from genesis" but the whole validation surface still compiles in
- **Where (verified):** sunset / recovery-exit gates at `validation.cpp:320–481, 7192, 13243, 13439`;
  the exit path rejects every value-in, rollover, bridge and control op and accepts only exits
  whose transparent outputs are covered by a verified value balance against an empty tree.
- **Analysis:** no way to create a note exists, so no exit can validate; nothing reachable was found.
  The residual risk is complexity: a future "flip four D9 heights to INT32_MAX to enable the pool"
  re-opens the full ZK/MatRiCT+ surface with only unit-test coverage and no external audit.
- **Fix:** keep the pool sunset in v0.x; before any enablement commission the PQC/ZK audit listed in
  QTC-FORK.md.

### M5. PQ signature verification cost is weighted, but the SLH-DSA weight deserves a measurement
- **Where (verified):** `script.h:72,76` — ML-DSA-44 sigop weight 50, SLH-DSA weight 500;
  `MAX_BLOCK_SIGOPS_COST = 480,000` (`consensus.h:23`); counted at `interpreter.cpp:1213,1314,2531–2540`.
- **Analysis:** the cap allows ≤ 960 SLH-DSA or ≤ 9,600 ML-DSA verifications per block. FIPS-205
  SLH-DSA-128s verification is roughly 1–3 ms on a modern core, so a maximal block costs ≈ 1–3 s
  single-threaded, ≈ 25–75 ms at 40× parallel validation. Acceptable, but the 500 weight was set
  upstream without QTC measuring it on target validator hardware.
- **Fix:** benchmark `bench_qtc` PQ verify on the slowest node class you intend to support and
  confirm a full-sigops block validates well inside the 600 s spacing.

---

## LOW / INFORMATIONAL

### L1. Auto-update is off, but the dev-origin escape hatch ships in release binaries
- **Where (verified):** `autoupdate.h` defaults empty; `init.cpp:1189–1193` requires HTTPS unless
  `-autoupdatedevorigin`; `autoupdate.cpp:1129`.
- **Risk:** an operator who sets `-autoupdate=1 -autoupdatedevorigin=1` with an `http://` manifest is
  one MITM away from running attacker code. Consider compiling the dev-origin flag out of release
  builds. Do not enable auto-update until a QTC ML-DSA release key exists and is held offline.

### L2. Regtest-only knobs are correctly scoped
- **Where (verified):** `-regtestmatmul*` and `-test=matmulasert` are read only under
  `case ChainType::REGTEST` (`chainparams.cpp:450–452`). No mainnet consensus value is
  reachable from the command line.

### L3. Cross-chain replay and confusion with the upstream chain
- Distinct genesis, message-start bytes `Q T C`, bech32 HRPs, and `QTC_*` domain-separation tags.
  Transactions, shielded proofs and PQ signatures from the upstream chain cannot validate on QTC.
  No further action.

### L4. Launch placeholders in binaries
- `github.com/qtcchain/qtc` (clientversion), `node.qtc.tools` (RPC help), `qtc.dev` (autoupdate doc).
  Cosmetic, but a stale hostname in help text is a phishing vector once the repo is public. Replace
  or blank them before the first tagged public release.

### L5. Hardening from genesis (D8/D9) verified as intended
- All activation heights 0; empty-block subsidy penalty never; `nSubsidyHalvingInterval 210,000`;
  `nInitialSubsidy 50 COIN`; unit pins in `pow_tests`, `matmul_subsidy_tests`, `validation_tests`
  (`nSum = 2,099,999,997,690,000`). No divergence from the modelled schedule found.

---

## What was NOT covered
- No fuzzing of the MatMul transcript / Freivalds verifier, the SMILE v2 CT prover, or the PQ
  script paths. The upstream chain had an external assessment; QTC's changes to those areas are
  renames and constant changes only, but the audit does not transfer automatically.
- No functional-test run (the functional suite has not been ported; see QTC-FORK.md).
- No review of the miner/pool tooling, which is not in this tree.

## Recommended order of work
1. H4: applied (O5). Remaining: port the Metal/CUDA digest kernels and parity-test on hardware.
2. H1 + H2 together: size `powLimit` from the fleet, regenerate genesis at launch time, rerun the
   stall simulation and the full unit suite.
3. M3: public nodes, seeds, mesh.
4. H3 + M1: header-flood functional test; plan checkpoint / `nMinimumChainWork` point release.
5. M5 benchmark; L1 and L4 cleanups.
6. Testnet burn-in ≥ 2 weeks with ≥ 3 independent miners before mainnet.
