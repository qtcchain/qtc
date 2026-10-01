> **QTC note.** On QTC mainnet, testnet and regtest `nMatMulNonceSeedHeight` is **0**: the nonce-bound seed-v2 rule
> described below applies from genesis and there is no legacy seed contract on the live chain. The height `125000`
> in this document is the activation height on the predecessor network where the rule was first deployed.

# QTC MatMul Nonce-Bound Seed V2

Date: 2026-06-07 (predecessor network); adopted by QTC from genesis
Activation on QTC: height `0` (mainnet, testnet, regtest)

## Summary

This rule closes the e1 MatMul-PoW amortization issue. On QTC it applies to every block; on the
predecessor network, where it was first deployed, it activated at height `125000`.

Before this rule, MatMul matrix seeds were derived as:

```text
H(prev_block_hash || height || which)
```

That made the expensive A/B matrix instance fixed across all nonce attempts for one parent
and height. The issue was economic rather than a shielded inflation bug, but it meant a miner
could amortize the generated instance across many mutable header attempts. That is different
from ordinary acceleration: making each required MatMul cheaper with better CPU/GPU code is
valid, while reusing one consensus work instance across many nonce attempts underprices the
intended work.

At and after `nMatMulNonceSeedHeight`, seeds are derived as:

```text
H("QTC_MATMUL_SEED_V2" || prev || height || version || merkle ||
  time || bits || nonce64 || matmul_dim || which)
```

This binds the instance to the mutable header and especially to `nNonce64`. Changing the nonce,
timestamp, merkle root, target bits, version, or dimension changes A/B.

## Consensus And Mining Changes

- `Consensus::Params::nMatMulNonceSeedHeight` is `0` on QTC mainnet, testnet and regtest
  (it was `125000` on the predecessor network).
- Regtest has `-regtestmatmulnonceseedheight=<n>` for activation-boundary testing.
- `SetDeterministicMatMulSeeds(...)` selects the legacy derivation before activation and seed-v2
  at/after activation.
- Contextual block validation recomputes the expected seeds from the submitted header and
  rejects mismatches as `bad-matmul-seeds`.
- The post-activation solver uses a nonce-by-nonce path. It derives fresh seeds, A/B, sigma,
  digest input, and optional Freivalds payload for every attempted nonce.
- Metal and CUDA remain digest/matrix acceleration backends, not consensus seed-derivation engines.
  After activation, accelerated backends must carry candidate-specific `seed_a` and `seed_b` through
  the batch path so backend base-matrix caches cannot reuse a stale fixed instance across nonce
  attempts.
- The post-activation safety rule forbids shared-A/B nonce windows. It does not forbid GPU
  optimization that batches multiple nonce attempts while carrying distinct A/B for every attempt.
  CUDA and Metal now have nonce-seed-specific GPU scan and variable-base digest batch paths for this
  case; CPU and unsupported/fallback backends remain on the conservative nonce-by-nonce path.
- On Apple builds the Metal backend still builds by default, and macOS source builds again precompile
  MatMul/oracle kernels into `.metallib` files first with embedded source compilation only as fallback.
- RPC mining/work-profile reporting now marks fixed-instance reuse as unavailable when the
  consensus nonce-seed upgrade is active.

## Boundary Safety

The rule is height-gated in the code so that a network can adopt it mid-chain. On QTC the gate is
height `0`, so there is no legacy seed contract on the live chain and no boundary to manage; the
regtest option exists only for testing the gate itself.

## Tests

Focused coverage added in this branch:

- `pow_tests/MatMulNonceSeedV2_binds_mutable_header_fields`
- `pow_tests/MatMulNonceSeed_activation_boundary_selects_legacy_then_v2`
- `pow_tests/MatMulNonceSeed_solver_mines_and_verifies_at_activation_boundary`
- `pow_tests/MatMulNonceSeed_solver_disables_shared_base_matrix_batching`
- `pow_tests/ChainParams_REGTEST_matmul_activation_override_options`
- `pow_tests/ChainParams_REGTEST_matmul_activation_override_args`
- `matmul_params_tests/matmul_params_defaults_mainnet`

The local `test_qtc` target builds, and the focused MatMul nonce-seed tests pass.
