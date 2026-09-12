# QTC mainnet powLimit sizing from the measured A6000 rate (security review H2)

Date: 2026-09-10 · Tree: `qtc-main` `e9fe78e5` (= v0.0.6) · Status: APPLIED 2026-09-11, published as `v0.0.7` (remote `73e40680` + `8191d937`, local `83ea9002` + `813c37ea`) · Script: `powlimit_sizing.py` (this folder)

## 1. What is being sized

Under Option B (`QTC-LAUNCH-SAFETY.md` D3) ASERT governs from genesis and `powLimit` is the hard floor: the easiest
target the chain can ever have. If the launch fleet cannot find blocks at the floor in about 600 s, the chain runs slower
than target until hashrate grows, because ASERT cannot ease below the floor. If the floor is far too easy, the fleet
mints blocks faster than 600 s until ASERT tightens (a bounded launch overshoot) and CPU-only header production at floor
difficulty gets cheaper (review H3, addressed post-launch by minimum chain work and a checkpoint).

Rule (D3): per-digest success probability `P = target / 2^256`; interval at the floor = `1 / (P × R)` where R is the
fleet's full-digest rate. Two hard requirements from §7: `powLimit` must be compact-exact and genesis `nBits` must equal
`compact(powLimit)`, so changing the floor regenerates genesis.

## 2. Inputs (measured 2026-09-10, n = 512, b = 16, r = 8, oracle v2, digest v4)

| Hardware | Full digests / s | Source |
|---|---|---|
| RTX A6000, Thunder container | 8,800 (9,600 digest-only; GPU-bound ≈ 13,000) | `qtc-matmul-solve-bench --digest-batch-bench 256` |
| Apple M5, Metal | 840 | `qtc-matmul-metal-bench` |
| One CPU core (M5 / x86 server) | 3.6 / 1.0 | same benches, CPU backend |
| RTX 4000 Ada (Node B) | not measured; 0.4–0.6× A6000 assumed (4,400 used) | RunPod had no stock; DigitalOcean CLI unauthenticated |

## 3. Candidates

All exponent `0x1e` (target = mantissa × 2^216), all compact-exact (round-trip checked).

| Candidate | compact | P per digest | log2 P |
|---|---|---|---|
| placeholder (current code) | `0x1e013333` | 7.15e-8 | −23.74 |
| A: fleet-sized, A6000 + Ada at 600 s | `0x1e021e4a` | 1.26e-7 | −22.92 |
| **B: one A6000 at 600 s (recommended)** | **`0x1e033333`** | **1.91e-7** | **−22.32** |
| C: one A6000 at 300 s (2× headroom) | `0x1e066666` | 3.81e-7 | −21.32 |

### Expected seconds per block at the floor

| Fleet | placeholder | A | B | C |
|---|---|---|---|---|
| A6000, container (measured) | 1,589 | 900 | **596** | 298 |
| A6000, bare metal (est.) | 1,075 | 609 | 403 | 202 |
| A6000 + RTX 4000 Ada (est.) | 1,059 | 600 | 397 | 199 |
| Apple M5 Metal | 16,644 | 9,429 | 6,242 | 3,121 |
| one M5 CPU core | 44.9 d | 25.5 d | 16.9 d | 8.4 d |
| one x86 server core | 161.8 d | 91.7 d | 60.7 d | 30.3 d |

### Launch overshoot (deterministic ASERT walk from genesis, τ = 172,800 s)

| Candidate | fleet A6000 + Ada: blocks until ≈ 600 s cadence / hours / blocks ahead of schedule | A6000 alone |
|---|---|---|
| placeholder | starts slower than target (1,059 s); no overshoot | 1,589 s blocks, no overshoot |
| A | on target from block 1 | 900 s blocks, no overshoot |
| **B** | 1,337 blocks / 196 h / **163 blocks ahead** | on target from block 1 |
| C | 1,908 blocks / 243 h / 451 ahead | 1,622 blocks / 223 h / 283 ahead |

### CPU-only production at the floor (H3 surface)

100 M5 cores: placeholder 2.2 blocks/day, A 3.9, B 5.9, C 11.9. All are far below the fleet's rate; the floor is not
a security boundary, ASERT and (post-launch) minimum chain work are.

## 4. Recommendation: `0x1e033333`

- Sized so the one card that has actually been measured, at its measured containerized rate, holds the 600 s
  schedule by itself. Losing Node B, or Node B under-delivering, does not push the chain below target cadence.
- With both launch cards present the chain starts at ≈ 400 s blocks and ASERT settles to 600 s over about eight
  days, minting ≈ 163 blocks ahead of schedule (≈ 1.1 days of issuance). Acceptable; candidate C doubles it and A
  leaves no margin if the A6000 is alone.
- The placeholder is 2.67× too hard for the measured fleet: one A6000 would run 26-minute blocks indefinitely.
- Post-stall behaviour is unchanged by the value: the catch-up excess after a stall of S seconds is S/600 blocks
  whatever the floor; the floor only sets how fast that excess is burned.

Values to apply (mainnet only; test chains keep their own floors):

```
consensus.powLimit = uint256{"0000033333000000000000000000000000000000000000000000000000000000"};
genesis nBits     = 0x1e033333   // must equal compact(powLimit); genesis hash changes
```

## 5. Applied (2026-09-11)

- `src/kernel/chainparams.cpp` mainnet: `powLimit = 0x033333·2^216`, genesis `nBits 0x1e033333`, sizing comment rewritten.
- Mainnet genesis regenerated at the unchanged `nTime 1789063200`:
  `9ba00506445039aa7315dc1ce61eded19ec75d31edbfed3643cb1e4f3c3db8e2` (was `44c4f064…1504d4`); merkle unchanged
  `68668615…`. Test, testnet4, signet, regtest and shieldedv2dev genesis unchanged.
- Pinned values updated in `pow_tests`, `matmul_dgw_tests`, `pq_genesis_tests`; trackers `QTC-LAUNCH-SAFETY.md` (§13,
  D3 checked), `QTC-SECURITY-REVIEW.md` (H2 closed), `QTC-FORK.md`.
- Verified: pow/matmul_dgw/pq_genesis/matmul_params/matmul_asert/chainparams/validation_chainstate suites no errors;
  main boots on the new genesis, test chain unchanged, five strict-validation regtest blocks mined; full unit suite
  3,292 cases, no errors, before commit.
- Consensus-bearing: 0.0.6 nodes cannot follow; publish as 0.0.7.

## 5a. What applying involved (procedure kept for the launch-day regeneration)

1. `src/kernel/chainparams.cpp` mainnet `powLimit` and the genesis `nBits` argument; update the sizing comment.
2. Regenerate mainnet genesis (`genesis_regen_hooks.py` / `genesis_bake4.py`), bake the new hash and the
   `assumevalid`/chain-tx data; unit tests that pin the mainnet genesis hash.
3. Re-run `contrib/qtc-launch/stall_sim.py` for the mechanics (it uses an easy regtest floor by design; the mainnet
   value itself cannot be exercised on a CPU regtest, 5.2 M digests per block).
4. Consensus-bearing: publish as a version bump (0.0.7) with a distinct user agent; nodes on 0.0.6 cannot follow.
5. Genesis is regenerated again at launch (H1) with the launch-day timestamp; the floor chosen here carries over
   unless the RTX 4000 Ada measurement or a fleet change moves R materially (re-run `powlimit_sizing.py`).

## 6. Re-check triggers

- RTX 4000 Ada measured: if A6000 + Ada ≥ 20,000/s, consider candidate A' sized to the two-card fleet with the
  single-A6000 fallback ≤ 900 s.
- A6000 on bare metal (≈ 13,000/s): candidate B gives ≈ 400 s alone; still fine.
- Any third launch card: re-run the script with the new R.
