# QTC security review v2 — full-scope (2026-09-09, tree 45a5440c / v0.0.2)

Scope: consensus/validation, proof-of-work and MatMul, P2P/DoS, script and post-quantum signatures, wallet/RPC/init/
auto-update/mining guard, the sunset shielded surface, mempool/policy/miner, build/dependencies, tests. Method: seven
parallel read-only code reviews, one per subsystem, followed by independent verification of every High finding against
the code by the consolidating reviewer. No fuzzing or functional tests were run. "VERIFIED" = the exact code path was
traced (file:line); "PLAUSIBLE" = needs a test. Severity reflects impact on a launched mainnet.

v1 (morning of 2026-09-09) is superseded. v1's H4 was fixed by O5 (`573e4608`). v1's M1 was mis-framed — see C-2.

---

## Corrections to v1

- **"12-block reorg limit" is not a fork-choice rule.** `nMaxReorgDepth` only sizes prune retention
  (`validation.cpp:1732-1762`). Fork choice on mainnet is governed by the default `EMERGENCY` reorg profile
  (`kernel/chainstatemanager_opts.h:118-122,185`): parking disabled, `hysteresis_depth = 0`, `hysteresis_work_margin = 2`.
  The split scenario in v1 M1 is replaced by C-2 below.
- **The functional-test framework exists.** `test/functional/` has 389 scripts and CI runs the 70 QTC ones by default
  (`test_runner.py:549-623`, `ci/test/03_test_script.sh`); the 437 upstream suites are opt-in. v1 said "not ported".
- **Auto-update is not "off on every chain".** The manager auto-enables on mainnet whenever a release key is configured
  (`node/autoupdate.cpp:1125-1137`); only the init-time validation is gated on the explicit flag. See W-2.

---

## HIGH

### H1 (v1) Genesis timestamp fixed at 2026-03-19 — unchanged; regenerate at launch.
### H2 (v1) `powLimit` placeholder — unchanged; size from the fleet after the GPU kernel port.
### H3 (v1) Header-only MatMul work is unverified and free at floor difficulty — unchanged, and it is the root of N-2.

### N-1. Post-quantum sigop accounting can be zeroed by a miner (script) — VERIFIED
- `script/interpreter.cpp:2508-2545` `WitnessSigOps` statically scans `witness.stack[size-2]` as the P2MR leaf. The
  executor (`:2343-2350`) first pops an annex when `stack.size() >= 3 && stack.back()[0] == 0x50`, so with an annex present
  the counter scans the control block instead and counts 0. Separately, `OP_CHECKSIGFROMSTACK` (`:1287-1333`) is absent from
  the counter's switch and calls `CPQPubKey::Verify` directly, bypassing the signature cache.
- The only remaining bound is the per-input validation weight (`serialized witness + 50`, `:2371`), which an oversized annex
  inflates (annex is popped before the element-size check at `:2200-2208`).
- Impact: a miner can pack ≈ 480,000 ML-DSA-44 or ≈ 48,000 SLH-DSA verifications into a 24 M-weight block versus the
  9,600 / 960 the `MAX_BLOCK_SIGOPS_COST = 480,000` budget was sized for — a ~50× validation-cost bypass (tens of seconds
  per block single-threaded). Policy rejects annexes and CSFS is standard-whitelisted, so this is miner-only.
- Fix: mirror the annex pop in `WitnessSigOps`; add CSFS cases (50/500) to the counter; route CSFS through
  `checker.VerifyPQSignature` so the salted cache covers it; cap annex size at consensus.

### N-2. Verification budgets collapse into "IBD mode" on a forged best header (P2P / PoW) — VERIFIED
- `net_processing.cpp:5604-5610` and `:6013-6018`: `is_ibd = true` whenever `ActiveHeight() + 10 < m_best_header->nHeight`.
  With `is_ibd`, the per-address Phase-2 budget becomes `max(32, 200,000)`/min (`pow.cpp:3168-3174`) and the global 512/min
  budget is skipped (`net_processing.cpp:2121-2127`).
- `m_best_header` is header-only work (H3): at floor difficulty a forged 11-header fork costs a few thousand hashes.
  After that every peer's expensive-verification budget on the victim is effectively unlimited; each garbage 1 MiB block
  costs the victim ≈ 50 ms of CPU (A'/B' reconstruction + Freivalds) and, per N-3, is verified twice.
- Fix: derive `is_ibd` from the validated chain only (`IsInitialBlockDownload()`), or require `m_best_header`'s ancestry
  to have block data; never skip the global budget — raise its cap in IBD instead.

### N-3. Payload failures are `BLOCK_MUTATED`: header never invalidated, punishment ladder dead, double verification — VERIFIED
- `validation.cpp:10156-10186`: on mainnet the product-committed check fails → the code then runs the full Freivalds path
  on the same block (`:10163-10168`, a second A'/B' reconstruction + Freivalds) → rejects as `BLOCK_MUTATED
  "invalid-product-payload"`. `AcceptBlock` does not mark `BLOCK_MUTATED` headers failed, so the block is re-requested
  indefinitely. `IsMatMulPhase2Failure` (`net_processing.cpp:2184-2188`) matches only the unreachable
  `"high-hash"/"matmul phase2 proof of work failed"` string, so `RegisterMatMulPhase2FailureForPeer` and the
  DISCONNECT→DISCOURAGE→BAN ladder never run; the generic `BLOCK_MUTATED → HandleDoSPunishment(100)` only disconnects
  outbound peers (`net.h:1017-1030`), so inbound attackers keep their connection.
- Fix: short-circuit with `else if`; return `BLOCK_CONSENSUS` (or a dedicated result) for product-committed failures so
  the header is marked failed; classify `invalid-product-payload`/`missing-product-payload` as Phase-2 failures; apply
  `Misbehaving` for inbound peers.

### N-4. Valid blocks between 16 MB and 24 MB cannot be transmitted (P2P) — VERIFIED
- Consensus and the miner allow `nMaxBlockSerializedSize = 24,000,000` including payload (`validation.cpp:9762-9770`,
  `consensus.h:16`, `chainparams.cpp:271`); transport rejects any message over `MAX_PROTOCOL_MESSAGE_LENGTH = 16,000,000`
  (`net.h:65`) and there is no send-side check (`net.cpp:868-880`). Three 6.5 MB shielded transactions fill a block past
  16 MB. Every peer requesting such a block disconnects; a majority miner can partition the network with valid blocks.
- Fix: raise the transport cap above the block cap plus framing, or lower the block cap; add a send-side assertion.

### N-5. Uncommitted MatMul v2 payload vectors accepted, stored and relayed up to 24 MB (consensus) — VERIFIED
- `primitives/block.h:119-152`: `matrix_a_data`/`matrix_b_data` are serialized with every full block but are covered by
  neither header, merkle root nor digest. Malformed vectors are only `LogDebug`'d (`validation.cpp:9740-9747`,
  `:10113-10120`); the sole bound is the 24 MB size cap; `WriteBlock` stores them.
- Attack: append ~24 MB of junk to any fresh 2 KB block (same hash) and be first to deliver it; every node validates,
  writes 24 MB and relays the padded copy. 144 blocks/day → ≈ 3.4 GB/day per node at zero PoW cost.
- Fix: reject blocks with non-empty v2 vectors on mainnet (seeds are deterministic; the vectors serve no purpose);
  strip before storage.

### N-6. Wallet encryption does not protect the post-quantum spending seed (wallet) — VERIFIED
- The 32-byte PQ master seed is written in cleartext as `walletdescriptorpqseed` (`wallet/scriptpubkeyman.cpp:2669-2672`,
  `walletdb.cpp:405-408`) and loaded unconditionally (`walletdb.cpp:1161-1172`). There is no encrypted record type;
  `DescriptorScriptPubKeyMan::Encrypt` (`scriptpubkeyman.cpp:2357-2375`) covers only secp256k1 keys; `EncryptWallet`
  leaves the plaintext record in place. Every PQ key is HKDF-derived from this seed (`pq/pq_keyderivation.cpp:38-81`).
- Impact: anyone holding an "encrypted" wallet.dat can spend all transparent P2MR funds without the passphrase.
  Only shielded material (`pqmasterseedcrypt`) is actually protected.
- Fix: add an encrypted PQ-seed record, write it instead of plaintext when `IsCrypted()`, inject on unlock, migrate and
  erase plaintext in `EncryptWallet`, and make `CheckDecryptionKey` verify against it for PQ-only wallets.

### N-7. Full block templates mine invalid blocks: the miner never reserves weight for the mandatory C' payload — VERIFIED
- `node/miner.cpp:878,901-908` clamps `nBlockMaxWeight` to `MAX_BLOCK_WEIGHT` with only `DEFAULT_BLOCK_RESERVED_WEIGHT
  = 8,000` (`policy/policy.h:46`); `miner.cpp:1112` clears `matrix_c_data` before `TestBlockValidity`; the 1,048,576-byte
  payload is appended after solving (`rpc/mining.cpp:4743`) and `CheckBlock` counts it against the 24,000,000-byte cap
  (`validation.cpp:9764-9769`). A template holding ≥ ~22.95 MB of transactions fails `bad-blk-length` after mining.
- Attack: ~20 max-standard transactions at min relay fee make every default miner burn a full PoW solution.
- Fix: reserve `n²·4 + varint` in the template weight budget and run `TestBlockValidity` with a synthetic payload.

### N-8. Template fallback never evicts the offending transaction — VERIFIED
`miner.cpp:1128-1158`: on `TestBlockValidity` failure the assembler retries with `use_mempool = false` and returns an empty
template; nothing removes the failing transaction (no `removeRecursive`/`RemoveStaged` in miner.cpp). Any tx that passes
ATMP but fails block context makes every template empty until expiry (336 h). Fix: bisect/evict and alert.

### N-9. Mempool memory accounting ignores the shielded bundle — VERIFIED
`core_memusage.h:32-41` sums `vin`/`vout` only; `nUsageSize` therefore undercounts a 2.4 MB / 11 MB standard shielded tx
by nearly its whole size, so `TrimToSize` against the 1 GB default is ineffective. Fix: add
`RecursiveDynamicUsage(const CShieldedBundle&)`. (Closing S-2 by making shielded txs non-standard also removes this.)

### N-10. Release and update trust still points at the upstream projects — VERIFIED
`contrib/verify-binaries/verify.py:8-18,49` (bitcoincore.org, bitcoin.org, bitcoin-core/guix.sigs);
`contrib/verify-commits/trusted-keys` and `trusted-git-root` are Bitcoin Core's; `contrib/autoupdate/install.sh:327-335`
embeds a secp256k1 release key byte-identical to the upstream chain's installer, with default algo `secp256k1`
(`install.sh:41`) while the node defaults to `ml-dsa-44`. Anyone holding that key can push code to an operator who follows
BOOTSTRAP.md. Fix: QTC builder keys and guix.sigs; rotate the installer key to a QTC-owned PQ key or delete the channel.

### N-11. No fuzz target reaches MatMul header/payload verification — VERIFIED (absent)
`src/test/fuzz/` has no reference to `CheckMatMulProofOfWork_*`, `CheckMatMulPreHashGate` or `matrix_c_data` parsing;
shielded and PQ parsing are fuzzed. Add targets for seed derivation, payload size/range, and Phase 2 with mutated C'.

---

## MEDIUM

### C-2. Default reorg hysteresis defers every reorg until the rival leads by two blocks of work — VERIFIED
`validation.cpp:8672-8720` with the `EMERGENCY` defaults: a node on A(h) that sees B(h+1) stays on A until B(h+2).
Lengthens every tip race, raises orphan rate, favours first-seen and selfish mining; a minority two blocks ahead
reorgs the majority three deep. Fix: default `hysteresis_depth ≥ 2`, or apply the margin only beyond `warn_depth`.

### C-3. Two divergent shielded value-balance functions — VERIFIED divergence, PLAUSIBLE exploit
`shielded/bundle.cpp:1048-1104` (pool debit) vs `:1109-1135` (transparent credit) disagree for INGRESS/EGRESS/REBALANCE.
Inert on mainnet because the sunset gate admits only `V2_SEND`/`V2_RECOVERY_EXIT`, where they agree; a future "re-open
the pool" activation would make an EGRESS_BATCH an inflation path. Fix: single function; add per-family equality tests.

### C-4. Shielded consensus keyed to the active chainstate — VERIFIED, unreachable today
`validation.cpp:7021-7022`: turnstile/proof checks are skipped on non-active chainstates while `CheckTxInputs` still
credits `value_balance`. Unreachable without assumeutxo snapshots (none configured). Fix: fail closed.

### P-4. All inbound Tor peers share one MatMul budget key — VERIFIED
Budgets keyed by `CNetAddr` (`net_processing.cpp:952, 2096, 2147`); inbound onion peers carry the proxy address, so one
attacker exhausts the budget for every honest Tor peer, while IPv6 /64s get unlimited keys. Fix: key onion/I2P by NodeId,
bucket IPv6 by /64.

### P-5. Self-asserted `NODE_MATMUL_CONSENSUS` bit gates outbound selection and sync — VERIFIED
`net_processing.cpp:1858-1871, 4383-4387, 6960-6963`: honest peers without the bit are dropped at VERSION; any attacker
sets bit 27. An eclipse multiplier on top of empty seeds. Fix: soft preference only; inbound fallback when no preferred
peers exist.

### S-1. Recovery-exit ML-DSA and membership verification run before the pool turnstile in ConnectBlock — VERIFIED
`validation.cpp:7248-7252` precede `:7329`; a block of 6.5 MB recovery-exit txs makes every validator do N ML-DSA
verifies plus witness parses, and possibly one O(chain) anchor-history replay (`:7150-7159`), before rejection. Bounded
by one valid-PoW block per attempt. Fix: turnstile first, or `nShieldedRecoveryExitActivationHeight = INT32_MAX`.

### S-2. Shielded transactions are standard and fully parsed before the sunset gate — VERIFIED
≈34 k LoC of lattice code is reachable from relay for a pool that can never hold value. Linear cost, no lattice
verification reachable, so nuisance-level, but needless surface. Fix (≈15 lines): a `fShieldedPoolDisabled` consensus
flag checked first in `RejectShieldedHeightGateViolation` (`validation.cpp:489`) and in `IsStandardTx`; optionally
reject `flags & 2` at deserialization.

### W-2. Auto-update auto-enables on mainnet when a release key is configured — VERIFIED
`node/autoupdate.cpp:1125`: `enabled = IsArgSet("-autoupdate") ? GetBoolArg : chain == MAIN`; returns null only because
the default pubkey is empty. Help text and init validation (`init.cpp:590, 1165-1207`) assume off-by-default. A config
that sets pubkey, manifest URL and origin gets seamless installer execution with no explicit opt-in and no validation.
Fix: `GetBoolArg("-autoupdate", false)` unconditionally; always run the validation block.

### W-3. Signed manifest binds neither freshness nor code — VERIFIED
`autoupdate.cpp:813-821, 991`: only `remote > compiled-in`; no expiry, no persisted high-water mark. Installer falls back
to building a moving branch head when `git_commit` is absent (`contrib/autoupdate/install.sh:1279-1296, 1398-1437`).
Replay/downgrade by anyone who can serve the origin path; repo write access suffices if the signer omits `git_commit`.
Fix: sign `issued_at`/`expires_at` and URL; persist last-applied version; require `git_commit` and `script_sha256`.

### M-7. Default template caps at 25 mempool transactions — VERIFIED
`policy.h:42` `DEFAULT_BLOCK_MAX_TEMPLATE_TXS{25}` (enforced `miner.cpp:1497,1647`): ≈ 0.04 tx/s regardless of the 24 MB
capacity; permanent backlog and fee ratchet; also makes N-7 reachable (25 × 1.2 MWU > 24 MWU). Fix: unlimited or weight-based.

### M-8. PQ validation weights underprice verification on the reference builds — PLAUSIBLE
`script.h:70-82`: ML-DSA-44 = 50 WU (same as Schnorr), SLH-DSA-128s = 500; only reference implementations are compiled
(`libbitcoinpqc/CMakeLists.txt`). Benchmark on the slowest supported validator and raise weights (ML-DSA ≥ 150,
SLH-DSA ≥ 2,000) or ship optimised variants everywhere. Supersedes v1 M5.

### M-9. Default CI skips the 437 upstream functional suites — VERIFIED
`feature_block.py`, `mempool_accept.py`, `p2p_invalid_block.py`, `mining_prioritisetransaction.py` etc. never run; these
are the suites that would catch N-7/M-7 and weight-scale regressions. Fix: enable in CI.

### M-10. 9.2 MB prebuilt Windows binary committed in-tree — VERIFIED
`contrib/prebuilt/windows/qtc-29.4.0-…zip` with a self-attesting `.sha256`, referenced from README. Unauditable and the
wrong version. Fix: remove; distribute only signed releases.

### (v1) M2 timewarp bounded, M3 seeds/mesh, M5 SLH-DSA weight — unchanged.

---

## LOW

- **Q-3** Control-byte bit 0 uncommitted → 1-bit wtxid malleability of every P2MR spend (`interpreter.cpp:2361, 2365`).
- **P-6** cmpctblock at payload heights sends an untracked full-block GETDATA after charging a budget unit
  (`net_processing.cpp:5676-5683`); compact blocks are effectively dead on mainnet (payload never carried).
- **P-7** Outbound peers disconnected on a single unconnecting-headers message (`:3233-3235`).
- **P-8** Mining guard (when enabled) keys off unverified best-known heights (`mining_guard.cpp:281-283`).
- **F-5** ASERT fail-closed paths return `powLimit`, the easiest target (`pow.cpp:1836-1848`); unreachable on mainnet.
- **F-6** Empty-block penalty keys on `pindexPrev->nTx == 1`, which is 0 for headers-only ancestors; live on regtest/testnet.
- **S-3** Allocate-before-read on shielded length prefixes (`shielded/v2_types.h:180-187`), bounded by the 16 MB message cap.
- **W-4** Installer script hash optional under a hidden flag; `-autoupdatedevorigin` honoured from config on mainnet.
- **W-5** RPC password and the release-signing seed passed on argv in scripts and `qtc-util` (`contrib/mining/*`,
  `bitcoin-util.cpp:50,230`).
- **W-6** PQ keygen uses `GetRandBytes` rather than `GetStrongRandBytes` (`pqkey.cpp:120-124`); RNG-failure fallback
  zero-fills instead of aborting (`libbitcoinpqc/.../utils.c:55-69`).
- **B-11** PQ library provenance: cryptoquick libbitcoinpqc 0.1.0, pq-crystals Dilithium FIPS-204 final, SPHINCS+ ref
  ("not production-hardened"); Darwin arm64 uses the `shake-a64` variant — a per-platform consensus path that needs KAT and
  differential coverage on both variants; pin the sphincsplus upstream commit.
- **B-12** Developer paths (containing the upstream name) embedded in `contrib/qtc-launch/results-600s/*.json:32`;
  miner fallback branches and full-block-plus-payload assembly untested.
- **Info** `-miningchainguard` help text says default 1; code default is off. Freivalds returns pass on 0 rounds
  (callers pre-check). SMILE protocol gate is dead code (same version number). Genesis coinbase is in the UTXO set and
  its script equals the shieldedv2dev genesis script.

---

## Verified sound (headline items; details in the seven subsystem reports)
Subsidy/fee/coinbase arithmetic with `CheckedAdd`; weight/size/sigop constants consistent across nets; timestamp rules;
height gates (`>= X` with INT32_MAX sentinels); Freivalds challenge derivation binds C' before r; M31 arithmetic and
NEON/scalar parity with fail-closed self-test; seed v3 covers every mutable header field; ASERT int128 math; no
assert reachable from peer data in the block or script paths; PQ sighash coverage and domain separation; ML-DSA and
SLH-DSA canonical decoding; FIPS-205 mode selection consistent between wallet and verifier; RPC auth/cookie/bind as
Core; secret zeroisation; no secrets logged; shielded turnstile and empty-tree recovery-exit root; sunset gate ordering
in the mempool path.

---

## Summary and remediation order

| # | Finding | Sev | Area | Effort |
|---|---|---|---|---|
| 1 | N-6 PQ seed stored in plaintext in encrypted wallets | High | wallet | 1–2 days, wallet-format change |
| 2 | N-7 + M-7 template ignores payload; 25-tx cap | High | miner | hours, no consensus change |
| 3 | N-1 annex/CSFS zero the PQ sigop count; M-8 weights | High | script (consensus) | 1 day + benchmark; consensus change |
| 4 | N-2 + N-3 budgets collapse on forged headers; payload failures never invalidate | High | P2P/PoW | 1–2 days; no consensus change |
| 5 | N-5 uncommitted 24 MB payload vectors | High | consensus | hours; consensus change (reject on mainnet) |
| 6 | N-4 16 MB transport vs 24 MB blocks | High | P2P | hours; pick one cap |
| 7 | N-10 upstream release/update trust; W-2/W-3 auto-update defaults and freshness | High | ops/build | key ceremony + code |
| 8 | H1/H2/H3 genesis time, powLimit, header forgery | High | launch | after GPU kernel port; H3 needs checkpoints/minimum work |
| 9 | C-2 reorg hysteresis default; P-4/P-5 budget keys and service bit | Medium | consensus policy/P2P | hours |
| 10 | S-1/S-2/N-9/C-3/C-4 shielded surface | Medium | consensus | the 15-line `fShieldedPoolDisabled` gate closes all five |
| 11 | N-11 + M-9 fuzz and functional coverage | High/Medium | tests | ongoing |
| 12 | Lows | Low | various | batch |

All consensus-affecting fixes (items 3, 5, 10 and any weight change in M-8) belong in the same fork as the genesis
regeneration and the GPU kernel port, before any public release. Items 1, 2, 4, 6, 7 are not consensus changes and can
land immediately.

## What was NOT covered
No fuzzing, no functional-test runs, no dynamic analysis; no review of the miner/pool/stratum tooling outside this tree;
no re-derivation of the lattice and PQ primitives beyond wiring and canonical-encoding checks; GPU kernels (gated off).
Subsystem reports with full detail: iCloud `QTC/network/QTC_Security_Review_v2_subsystem_reports_2026-09-09.md`.
