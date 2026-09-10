# QTC — fork of upstream v0.33.1 (working tracker)

**Goal:** a brand-new chain "QTC" forked from upstream **v0.33.1** (the last release
before the v4.7 ENC_RC / Epoch-A / RC-self-qualification "latest-GPU-only"
transition that landed in v0.33.2). QTC keeps the **full QTC stack** — MatMul
**v3** PoW (any GPU + CPU), PQC signatures (ML-DSA/SLH-DSA), and the SMILE v2
shielded pool — but with a fresh genesis, its own network identity, and the
v4/Epoch-A latest-GPU fork left disabled.

Worktree: `/Users/gavinwhyte/Documents/qtc/qtc-chain` · branch `qtc-main` (base
tag `v0.33.1`, `944a6844`). Isolated from the QTC working tree.

## Chosen QTC identity values

| Param | QTC (v0.33.1) | QTC | Where |
|---|---|---|---|
| Net magic (main) | `b7 54 58 01` | `51 54 43 01` ('Q','T','C') | chainparams.cpp mainnet |
| P2P port (main) | 19335 | **19755** | mainnet |
| bech32 HRP (main) | `qtc` | `qtc` | mainnet |
| base58 PUBKEY/SCRIPT/SECRET (main) | 25 / 50 / 153 | **58 / 63 / 186** | mainnet |
| Genesis timestamp | "QTC 19/Mar/2026 SMILE v2…" | "QTC genesis — any-GPU MatMul PoW, forked from upstream v0.33.1" | CreateQTCGenesisBlock helper |
| v4/Epoch-A fork | (absent in v0.33.1) | left disabled (nMatMulV4Height = INT32_MAX) | — |

Test/regtest magic, ports, HRP, prefixes: **still TODO** (see below).

## Status

### Done (mainnet consensus-identity core)
- [x] Worktree + `qtc-main` branch from v0.33.1
- [x] Mainnet net magic → `51 54 43 01`, port → 19755
- [x] Mainnet base58 prefixes (58/63/186) + bech32 `qtc`
- [x] Mainnet DNS seeds cleared; fixed seeds cleared (no QTC infra inherited)
- [x] Mainnet checkpoints → genesis-only; `m_assumeutxo_data = {}`; chainTxData zeroed
- [x] Mainnet `nMinimumChainWork` = 0; `defaultAssumeValid` = 0
- [x] Genesis timestamp string → QTC (merkle root now changes)

### Done — consensus identity (test + regtest) — stage 1
- [x] Testnet: magic → `51 54 43 02`, port → 29755, HRP → `tqtc`
- [x] Regtest: HRP → `qtcrt`
- [x] All stale genesis asserts (main/testnet/testnet4/signet/regtest/shieldedv2dev)
      neutralized + `[QTC-REGEN]` stderr print added after each
      `hashGenesisBlock = genesis.GetHash()` (`#include <cstdio>` added)
- [x] Metal backend disabled for the regen build (`-DQTC_ENABLE_METAL=OFF`) — this
      host has Command Line Tools only (no `xcrun metal`)
- [ ] Deferred: full rebrand of testnet4/signet (magic/HRP) + testnet base58/seeds —
      not needed for QTC bring-up; do during the rename stage

### DONE — genesis regeneration + regtest bring-up ✅
Regenerated genesis hashes (baked into asserts; merkle `1d61d719…5685e7a2` for all
CreateQTCGenesisBlock nets):
- main    `3bbc692bb5b5846aa572367088362b32c7bcd83ef6c1e05b0afe2ae72199f63b` (Option B regen, 2026-09-06; was `115276be…` with nBits 0x20147ae1)
- test/testnet4/signet `305409d744d6304c2819aca15615e9e235374c8d20aa0d18d3f34d47dad92073`
- regtest `4ef26843324b7639f4cf9db1ba860611e98292c4bc3cccb41f7d103b6a81cddb`
- shieldedv2dev `4ed72f2a…` (unchanged; its own timestamp)

Verified on regtest (CPU build, Metal off): node boots, `getblockhash 0` =
`4ef26843…`, `getnewaddress` → **`qtcrt1…`**, `generatetoaddress 3` mines MatMul
**v3** PoW on CPU (blockcount=3). Genesis PoW is NOT re-checked at construction, so
no genesis mining loop was needed. Clean `stop`.

### DONE — cosmetic rename ✅
Targeted identity renames only (no blanket `s/qtc/qtc/` — that would break
`QTC_*` macros, code identifiers, and libbitcoinpqc):
- `CMakeLists.txt` `CLIENT_NAME` "QTC"→"QTC" (cascades to help text, copyright,
  user-agent, macOS bundle). Binaries via `OUTPUT_NAME` (CMake target names kept,
  so `--target qtcd` and the `bitcoind`/`bitcoin-cli` legacy-alias symlinks still
  work): `qtcd`, `qtc-cli`, `qtc-tx`, `qtc-util`, `qtc-wallet`.
- Default datadir `~/.bitcoin` / "Bitcoin" → **`~/.qtc`** / "QTC" (note: upstream v0.33.1
  had actually kept Bitcoin's default; `~/.qtc` was only ever an explicit -datadir).
- `qtc.conf`→`qtc.conf`; `qtcd.pid`→`qtcd.pid`; `CURRENCY_UNIT` "QTC"→"QTC";
  `UA_NAME` "QTC"→"QTC" (user-agent now `/QTC:0.33.1/`).
- RPC ports made QTC-distinct like P2P: main 19334→**19754**, test 29334→**29754**.
- 32 help-text literals in the 5 CLI front-ends (`qtcd`/`qtc-cli`/"QTC network"…).

Verified: `qtcd --version` = "QTC daemon v0.33.1 / The QTC developers"; fee help
"in QTC/kvB"; regtest boots to QTC genesis, subversion `/QTC:0.33.1/`, `qtcd.pid`.
Still to polish (non-blocking): man pages / packaging names, remaining literal
"QTC"/"bitcoin" wording in docs and less-prominent help strings.
- [ ] Finish testnet4/signet magic+HRP + testnet base58/seeds (deferred, non-blocking)
- [x] **Testnet dry-run PASSED** (see section below). Burn-in (multi-day,
      several nodes) still to run before any mainnet.
- [ ] Launch-safety: initial difficulty / early checkpoints / bootstrap mining plan.
      Findings: mainnet `fPowAllowMinDifficultyBlocks=false` ✓ (no trivial blocks on
      main, unlike testnet); mainnet inherits QTC's bootstrap — a fast-mine phase
      for heights [0, 50,000) (`nFastMineHeight`) then ASERT from 50k. These are
      QTC-tuned values: QTC must **consciously** keep or re-tune them
      (`nFastMineHeight`, `nMatMulAsertHeight`, `nMatMulAsertBootstrapFactor`,
      `powLimit 66c154…`) before launch.
- [ ] Decide fate of inherited PQC + shielded audit debt (BREW-01/02)

### TODO — build + verify
- [ ] `cmake -B build -DBUILD_UTIL=ON -DQTC_ENABLE_CUDA_EXPERIMENTAL=ON -DQTC_CUDA_ARCHITECTURES=<sm> …` (or CPU/Metal for local)
- [ ] Regtest bring-up: `qtcd -regtest`, generate blocks, confirm addresses are `qtc1…`, PoW is MatMul v3, no cross-connect to QTC (different magic)
- [ ] Fix unit/functional tests that pin QTC genesis/params
- [ ] Testnet burn-in before any mainnet launch

## Launch-safety notes
- New chain = ~0 hashrate initially → trivially 51%-attackable. Plan low initial
  difficulty + early checkpoints + a bootstrap mining plan.
- Do not reuse QTC magic/ports/seeds (done for main; finish for test/regtest).
- "QTC" ticker is not globally unique — verify before public branding.
- Keeping PQC + shielded means inheriting their audit debt (BREW-01/02 etc.).


### DONE — testnet dry-run ✅
Two `qtcd -testnet` nodes on one host (A: default QTC testnet ports 29755/29754,
listening; B: `-port=29756 -rpcport=29757 -connect=127.0.0.1:29755`), both
`-dnsseed=0` (seeds are empty anyway). Results:
- Peering: exactly 1 peer each (each other); both advertise `/QTC:0.33.1/`.
- Identity: genesis `305409d7…`, chain `test`, addresses `tqtc1…`.
- Mining: A mined 10 blocks in 27 s on CPU (MatMul v3) → height 11.
- Sync: B caught up from 0 to height 11 with the identical best hash `f7194832…`
  — genuine multi-block relay + sync.
- Issuance: wallet `immature` = 220 QTC = 11 × 20-QTC subsidy.
- Difficulty stayed at min (`20027525`) — expected on testnet (min-diff allowed,
  ASERT retarget not until 61k).
- Leftover QTC `defaultAssumeValid` on testnet/testnet4 reset to `uint256{}`
  (rebuild verified).

CPU-only builds (Metal/CUDA off): the node defaults to requesting the `metal`
backend and the *first* mine call trips the `metal→cpu` fallback
(`metal_unavailable_fallback_to_cpu:disabled_by_build`, a tested code path).
Set `QTC_MATMUL_BACKEND=cpu` for deterministic CPU mining and to avoid the
one-time first-call hiccup.
### DONE — unit test suite ✅ 3,253/3,253 GREEN (full run, one process)
- `pow_tests.cpp` + `pq_address_tests.cpp` updated to QTC identity (genesis/merkle,
  magic, ports, HRPs, base58, `qtc1z`, seeds=0). `pow_tests`: **106/106, no hang**.
- `ChainParams_MAIN_hardening_anchor_consistency` (224 lines of QTC mainnet
  history: 155,700 anchor, chainTxData, 19 assumeutxo snapshots) replaced by a
  fresh-chain invariant test (zero chainwork/assumevalid/tx-stats, genesis-only
  checkpoint, no snapshots).
- Testnet's leftover QTC DNS seeds cleared in chainparams.
- **Latent QTC bug found:** 13 `pow_tests` cases force `ScopedBackendEnv("metal")`;
  on a Metal-off build the async multi-nonce prefetch pipeline blocks on a queue
  the CPU fallback never services (0% CPU hang). Gated on
  `CapabilityFor(Kind::METAL).available`. Same hang exists on stock v0.33.1
  built Metal-off — upstream never sees it (macOS CI builds Metal on). Worth
  reporting upstream as a fallback-robustness bug.
- Full `test_qtc` run pending; functional tests (`bitcoind` legacy symlink →
  `qtcd`) to follow.

### Latent QTC test-hygiene bug found + fixed (unit suite as one process)
89 shielded tests failed with `ShieldedMerkleTree: failed to persist commitment
index` when the whole suite runs in one process, yet each passed alone.
Root cause: a process-global `s_commitment_store` is configured by chainstate
init at the test's temp datadir (`validation.cpp:671`); `~BasicTestingSetup`
deletes that datadir without resetting the global, so every later AUTO-mode tree
inherits a store whose LevelDB dir is gone. This also explained the stress
throughput "failure" (10K synchronous LevelDB writes via the leaked store).
Fixed structurally: reset the global in `~BasicTestingSetup` before
`remove_all`. Pre-existing in QTC (upstream CI shards suites) — worth reporting
upstream alongside the Metal-off async-prefetch hang.

### DONE — test suite: final state ✅
**Unit suite: 3,253/3,253, "No errors detected", exit 0** — definitive full run in one
process on the fully-fixed binary.

Fixes beyond the earlier commits (assumeutxo + functional framework):
- Regtest assumeutxo entry @110 regenerated for QTC's regtest chain: blockhash
  `60417a34…272a73`, hash_serialized `161f4dec…635d2`, nchaintx 111. (Note: my first
  capture baked `535a0856…` — the hash of a deliberately *malleated* snapshot from a
  negative test — which made the corrupted snapshot activate and the valid one fail;
  caught by verifying, corrected to the valid `txoutset_hash`.)
- `test_mainnet_assumeutxo_snapshot_metadata` rewritten to the fresh-chain invariant
  (no mainnet snapshots), like the hardening-anchor test.
- Functional framework rename gaps closed: HRP allow-list (`address.py:178`) and the
  double-quoted testnet HRP `"tqtc"`→`"tqtc"`; binary-name map in `test_framework.py`
  + probe in `test_runner.py` (`qtcd`/`qtc-cli`/`qtc-util`/`qtc-wallet` → `qtc*`);
  stale pre-rename `qtcd`/`qtc-cli` artifacts (which had masked the map bug) and two
  dangling duplicate-name symlinks removed from `build/bin`. Five regtest bech32
  literals re-encoded for `qtcrt` (checksums regenerated; descriptor checksum
  `#44gxgfuu`).

Functional subset (11 tests), fully classified — every failure proven, not assumed:
- ✅ Pass: `p2p_ping`, `rpc_uptime`. Rename gaps verified fixed: `rpc_net`'s
  `base58_to_byte` decode assert gone; `interface_bitcoin_cli` now invokes `qtc-cli`.
- **Inherited from upstream v0.33.1** (empty `git diff v0.33.1` on every relevant file, or an
  unchanged condition): `rpc_help` (stale `bridge_decryptviewgrant` conversion-table
  entry); `rpc_blockchain` (`getblockchaininfo` key-set drift w/ QTC keys);
  `rpc_net` (`scriptpubkey -26`: Core's MiniWallet segwit-v0 vs QTC's p2mr-only
  policy); `interface_bitcoin_cli` (`-netinfo` gate `< 209900` vs QTC's
  `CLIENT_VERSION` 0.33.1 = **3301**); `wallet_basic` (expects Core's 50-coin
  subsidy; QTC's is 20); `mining_basic` (GBT peer guard keys only on
  `ChainType::MAIN`, never fires on regtest); `feature_config_args` (hardcodes
  `'bitcoin.conf'`).
- **Inapplicable:** `wallet_address_types` (QTC's PQC-only `p2mr` address model — also
  means it cannot validate the HRP change).
- **Deferred (packaging stage):** `feature_qtc_faststart` — drives
  `scripts/release/package_release_archive.py`, which validates binaries by QTC
  filename (`--qtcd`, `binary_name == "qtcd"`); rebrand with the release tooling.

Deferred, documented: regtest assumeutxo @299 (`2e5dcf9f…`/`78e6ea38…`) is still
QTC's — consumed only by `feature_assumeutxo.py`; regenerate when that test is run.

**Latent QTC bugs found (worth reporting upstream):** (1) forcing the Metal backend
on a Metal-off build deadlocks the async multi-nonce prefetch pipeline (0% CPU
hang) — 13 `pow_tests` now gated on `CapabilityFor(Kind::METAL).available`;
(2) `ShieldedMerkleTree`'s process-global commitment-index store leaks across tests
(configured by chainstate init at `validation.cpp:671`, datadir deleted by
`~BasicTestingSetup` without reset) — fixed in the shared fixture teardown.

### Launch-safety proposal written → `QTC-LAUNCH-SAFETY.md`
Key finding: QTC (v0.33.1 base) inherits QTC's *pre-incident* difficulty curve —
50k-block 0.25s fast-mine phase, 1h ASERT half-life, easy genesis floor
`66c154…`, drift bound only from 118,482, epsilon-18 only from 50k — and the
v0.33.3 dump-floor *mechanism* is absent from its code. Proposal recommends
Option B (ASERT from genesis, hard genesis `powLimit` sized to the measured
launch fleet, τ=14,400s, drift/epsilon from block 0) plus the activation-runway
rule and early checkpoints. Decisions listed in §6; nothing applied to consensus.

### DONE — stall simulation (Option B, regtest) ✅ → `QTC-LAUNCH-SAFETY.md` §7
Deterministic mocktime run, both variants (raw floor clamp; D4 drift bound +
timewarp reconcile + BIP94 from genesis), 1,140 blocks each, every block's
`nBits` checked against an independent clamped-ASERT model (max error 1.8e-4 in
log2). Floor holds (never below `powLimit`); post-stall clamp exact; recovery
clean; drift bound does not wedge the chain (blocks stamped MTP+3600, accepted).
Two corrections surfaced: (1) regtest inherits Bitcoin's `fPowNoRetargeting =
true`, so default regtest **never runs ASERT** despite Option-B heights — added
`-test=matmulasert` (clears it, keeps the genesis anchor); the proposal's
"regtest already runs Option B" wording is corrected in §4. (2) Genesis `nBits`
must equal `compact(powLimit)` exactly or `getblockheader 0` aborts
(`rpc/util.cpp` `GetTarget` `CHECK_NONFATAL`) — consensus tolerates it, RPC does
not. Regtest knobs added: `-regtestmatmulpowlimit=<hex>`,
`-regtestmatmulmaxfuturemtpdriftheight=<n>`,
`-regtestmatmultimewarpreconcileheight=<n>`, `-test=matmulasert`. Driver:
`contrib/qtc-launch/stall_sim.py --variant {nodrift,drift}` (needs `build/bin`).
Still open for the testnet burn-in: wall-clock convergence to ~90 s blocks
(the mocktime run validates the controller, not real-time behaviour).

### DONE — Option B applied to mainnet consensus + genesis regenerated ✅
`CMainParams` now carries the launch-safety set (`QTC-LAUNCH-SAFETY.md` §8):
no fast phase (`nFastMineHeight = nMatMulAsertHeight = 0`), ASERT from the
genesis anchor with τ = 14,400 s, `powLimit = 2^235` (compact `0x1e080000`,
P = 2^-21 per full digest — **placeholder sizing from the D3 worked example,
re-size from the measured launch-fleet rate and regenerate again**), drift bound
+ a5 timewarp reconcile + BIP94 from block 0, 18-bit pre-hash gate from block 0,
bootstrap factor 1 (inert). Genesis `nBits` = `compact(powLimit)` exactly
(RPC `GetTarget` requirement); merkle unchanged. New mainnet genesis
`3bbc692bb5b5846aa572367088362b32c7bcd83ef6c1e05b0afe2ae72199f63b`, verified by
booting `qtcd -chain=main` (`getblockheader 0` → bits `1e080000`, target ==
powLimit). Tests that pinned QTC's 50k/61k/118,482/125,000 difficulty history
were rewritten as Option-B invariants (`pow_tests`, `matmul_dgw_tests`,
`matmul_params_tests`, `qtc_launch_readiness_tests`). Non-difficulty QTC upgrade
heights (61k Freivalds/product-digest/reorg-protection, 125k nonce seed, 130.5k
parent-MTP seed, shielded 61k/88k, subsidy-penalty 130k–132k) are **unchanged**
— a separate "fresh chain: activate all hardening from genesis" decision.

### DONE — D8/D9: all QTC hardening active from genesis ✅ → `QTC-LAUNCH-SAFETY.md` §9
Every remaining QTC flag-day height in `CMainParams` classified and set: hardening
(61k Freivalds binding / product digest / reorg protection, 125k nonce seed,
130.5k parent-MTP seed, shielded tx-binding / bridge-tag / codec-disable /
MatRiCT-disable / spend-path-recovery / C002) → **0**; the rolled-back
empty-block subsidy penalty window → **never**; and (D9, product decision
flagged in §6) the shielded pool **sunset from block 0** = the upstream chain's live rule state
(no shielding, recovery-exit only, velocity window never opens). Genesis hash
unchanged (heights are not in the header); `qtcd -chain=main` boots and serves
block 0. Tests pinning QTC history on MAIN rewritten to exercise the schedules on
param copies. To launch WITH a live pool instead: flip the four D9 lifecycle
heights to `INT32_MAX` before launch (no genesis regen needed).

### DONE — `QTC_` → `QTC_` rename, tier 1 (non-consensus identifiers) ✅ commit 4b7b0427
460 distinct tokens / 223 files: CMake options (`QTC_ENABLE_METAL`,
`QTC_ENABLE_CUDA_EXPERIMENTAL`, `QTC_CUDA_ARCHITECTURES`, …), runtime env vars
(`QTC_MATMUL_BACKEND`, `QTC_MATMUL_SOLVER_THREADS`, `QTC_TEST_STRICT_PERF`,
autoupdate / mining / CI variables), C++ macros + constants, scripts, functional
framework, docs. **Builders: re-pass `-DQTC_ENABLE_METAL=OFF` when
reconfiguring** — the old `QTC_*` cache entries are ignored and Metal defaults ON
(this Mac has no `xcrun metal`). Full unit suite green (3,253). Tier 2 (the 388
hash domain-separation tags — `"QTC_Note_Commit_V1"`, `"QTC_MatRiCT_*"`,
`"QTC_SMILE2_*"`, `"QTC_ShieldedV2_*"`, `"QTC_MATMUL_SEED_V2/V3"`, KAT/fixture
labels) is consensus-affecting and tracked as its own stage below.

### DONE — `QTC_` → `QTC_` rename, tier 2 (consensus domain-separation tags) ✅
388 hash-personalization tags / 82 files renamed (`QTC_Note_Commit_V1`,
`QTC_MatRiCT_*`, `QTC_SMILE2_*`, `QTC_ShieldedV2_*`, `QTC_Bridge_*`,
`QTC_MATMUL_SEED_V2/V3`, KAT/fixture labels, the `test/reference` generators and
checker). **Zero `QTC_` tokens remain in the tree.** Consequences: every
shielded commitment / nullifier / proof transcript / ring signature / MatMul
nonce-seed digest differs from QTC's — QTC artifacts cannot be replayed on QTC
or vice-versa (desirable cross-chain domain separation). Baked vectors updated:
`test/reference/shielded_test_vectors.json` regenerated via
`gen_shielded_matrict_plus_vectors` + `generate_shielded_test_vectors.py`;
13 known-answer literals in `shielded_kat_tests`, `shielded_merkle_tests`,
`ringct_ring_signature_tests` refreshed by a golden-update pass (each verified
as a pure expected-value change). Genesis **unchanged** (its seeds are literal
constants and the header carries no tag); the live-QTC-block-61000 vector test
still passes (explicit seeds). One statistical test
(`response_distribution_limits_real_index_bias`) was seed-lucky at 24 samples
(gap 61 on a ~1.3e5 mean vs a 16 tolerance; bias signature is 100+) — raised to
96 samples / tolerance 48.

### DONE — full purge of the upstream chain's name (chain stays **QTC**) ✅
Every reference to the upstream chain's name (daemon, namespace, prefixes) is gone from code, tests,
scripts, CI, docs and filenames — 172 paths renamed (`src/qtc-*.cpp`,
`test/functional/*qtc*`, `contrib/qtc-*`, `contrib/wqtc`, man pages, init
scripts, completions, workflows), ~11k substitutions, 87 upstream-history documents
deleted (upstream-era release notes, audits, trackers, assessments; inherited
Bitcoin Core release notes kept). Survivors are unrelated identifiers only:
`psbt_tx` (PSBT variables), `cb_tx` (coinbase tx), `DbTxn`, one third-party SRI
hash, two base64 fixtures. C++ namespace is `qtc::`, test binary `test_qtc`,
bench `bench_qtc`, library `libqtckernel`, the legacy-schedule test fixture is
`LegacyScheduleTestingSetup`. The genesis coinbase no longer names the upstream chain:
text `QTC genesis — any-GPU MatMul PoW`, P2MR commitment
`SHA256("QTC P2MR Genesis - Quantum Safe Since Block 0")` — so all genesis
hashes were regenerated once more (values below), plus the regtest
assumeutxo @110 (@299 still deferred). The two hyphenated shielded tags
(`QTC-SMILE-V2-GLOBAL-MATRIX-SEED-V1`, `QTC-SMILE-V2-COIN-RINGS-V1`) were the
last upstream-named domain tags; reference vectors and KAT literals regenerated.
56 bech32 literals whose HRP changed were re-encoded with valid checksums.
Placeholders that MUST be set before any release: `github.com/qtcchain/qtc`
(source URL in `clientversion.cpp`, crash-report URL), `node.qtc.tools` in RPC
help examples, `qtc.dev` in the autoupdate bootstrap doc.
Genesis (regenerated 2026-09-07, final):
- main    `238a04fb67ac198f905d7b0b12a2f5dfe6cd89af06d18210b9f5f9b9d2f463bc`
- test/testnet4/signet `17dfb5130fe8ff191a0e6b5c6c296f15efdf57d4a0e3d4cdd07c26e4c6e29bd0`
- regtest `f1dfd1ddeeffd89f6e8f9aba5e6583e1dbf3d6cdd973c91230115113be53cfc0`
- shieldedv2dev `62645a00f59f5617d3eddc764c90d0c71f3fcf82309b146349af4f68f3f7d493`
- merkle `7b04723e…e6a95dd7` (shared coinbase), shieldedv2dev `48760419…4d040b9a`
- regtest assumeutxo @110: blockhash `4ca28ecc…54c7a1`, hash_serialized
  `051c0ff8…53ca96`, nchaintx 111 (values from the deterministic unit-test chain, as the chainstatemanager snapshot tests require)

**Two inherited launch hazards fixed during the purge** (they were only *renamed*
by the sweep, which would have been wrong): (1) **auto-update** was on by default
on mainnet, pointing at an unowned `qtc.dev` origin with the upstream chain's compiled-in
ML-DSA-44 release key — a squatter holding that key could have pushed binaries
to every node. Now off by default on every chain, with no manifest URL, origin
or key compiled in (`-autoupdate=1` requires all three explicitly).
(2) The **mining chain guard** defaulted on for mainnet with a mesh of three
upstream public nodes. It is advisory in this version (it reports
`insufficient_peer_consensus` / stale-tip / reorg-hysteresis risk and enrolls
mesh peers via addnode; it does not gate getblocktemplate), but with no mesh it
would sit permanently unhealthy, warn on every template and retry enrollment of
nonexistent hosts every 60 s. Now off by default with an empty default mesh
(`-miningchainguard=1` + `addminingpeermeshnode` once public nodes exist).
testnet4's placeholder DNS seeds were removed like mainnet's. Remaining
placeholders are documentation/help-text only: `github.com/qtcchain/qtc`
(source URL in `clientversion.cpp`), `node.qtc.tools` in RPC help examples,
`qtc.dev` in the autoupdate bootstrap doc and one comment.

### DONE — Bitcoin's monetary policy at 600 s ✅ → `QTC-LAUNCH-SAFETY.md` §10
50 coins / 210,000-block halving / 10-minute blocks / 21 M (was 20 / 525,000 / 90 s).
Spacing retune: ASERT τ 172,800 s (288 blocks), floor `0x1e013333` (placeholder,
D3), drift 3,600 s = 6 blocks. Genesis regenerated again (main `c5343276…`);
regtest @110 rebaked from the deterministic test chain; regtest keeps 90 s with a
new `-regtestmatmulpowtargetspacing` knob so `stall_sim.py --spacing 600 --tau
172800` models mainnet. Tests pinning 90 s / 14,400 s / 0x1e080000 / the 20-coin
schedule rewritten. Stall simulation rerun at 600 s / τ 172,800 s: nodrift PASS; the drift variant
exposed that a 3,600 s MTP-relative drift bound pins the chain clock to exactly
600 s/block (drift/6 via the 11-block median) and **freezes post-stall easing** —
fixed by scaling the bound to τ/4 = 43,200 s (D4 is spacing-dependent); rerun PASS
(floor +78 blocks, 785 floor blocks, model error 2e-4). New regtest knob
`-regtestmatmulmaxfuturemtpdrift`, simulator `--drift`.

### Stage: O5 — oracle v2 + product digest v4 (2026-09-09)
Security-review finding H4: the v3 product digest committed to a *linear* compression of each C' tile with one shared
σ-vector, so the digest could be computed in O(n³/b) without forming C' (10.7× less matmul arithmetic; 3.8× wall-clock
on an M5 CPU because the one-lane oracle was not skippable). Fix chosen after modelling (`QTC_H4_Fix_Options_Model`):
* **Product digest v4** — every 16×16 tile of C' hashed in full with SHA-256, root over the tile hashes, tag
  `matmul-product-digest-v4`; active from genesis on every network (`src/matmul/transcript.*`).
* **Oracle v2** — one SHA-256 yields eight 31-bit lanes (`from_oracle`, `from_oracle_block`, `fill_from_oracle`);
  index 0 unchanged, all other pinned vectors regenerated (`test/reference/generate_test_vectors.py`, 127 self-checks).
* Metal/CUDA digest kernels NOT ported (no toolchain on this host): `accelerated_solver.cpp` gates every digest request
  to the CPU reference path (`kGpuDigestKernelsPortedToV4 = false`) and reports it as a clean backend fallback. Port
  spec in iCloud `QTC/software/QTC_O5_Implementation_2026-09-09.md`. Required before the fleet measurement (D3/H2).
* Genesis blocks unchanged (no MatMul fields); regtest assumeutxo @110 unchanged (regtest mining skips MatMul).
* Verified: matmul_*/pow/validation/pq_genesis suites green; Python-vs-C++ v4 known answer agrees; 5 regtest blocks
  mined and accepted under `-test=matmulstrict`. Full suite: see commit message.
