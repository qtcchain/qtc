QTC version 0.2.3
=================

Tag `v0.2.3` (draft; not yet tagged). Wallet performance and RPC update. No
consensus rule changes: blocks are valid or invalid exactly as before, and
v0.2.0 through v0.2.3 nodes interoperate. **Activation height: none.**

**Who should upgrade:** everyone who spends from a wallet, in particular
miners sweeping coinbase outputs and anyone whose wallet descriptor carries an
SLH-DSA backup leaf (the default `mr(pqhd(...),pk_slh(pqhd(...)))` layout).
Relays can upgrade when convenient.

Verify downloads against the signed `SHA256SUMS` as described in the README.

Wallet performance
------------------

- **Spending no longer signs the SLH-DSA backup leaf.** The P2MR signer
  visited leaves in script-byte order, which put the 32-byte SLH-DSA key push
  ahead of the ML-DSA key push, so every spend produced a full SLH-DSA-128s
  signature (about 1.6 s on a 2 GHz x86_64 core with the reference SPHINCS+
  code) and then discarded it in favour of the ML-DSA leaf. Leaves are now
  visited preferred-algorithm first and the signer stops as soon as the
  preferred leaf signs. Multisig leaves sign fallback-algorithm keys only when
  the preferred keys do not reach the threshold. Which leaf is chosen is
  unchanged; only the wasted work is gone.
- **Fee quotes no longer run real signing.** The wallet sized P2MR inputs by
  signing them for real, several times per transaction (change prototype,
  each candidate input set, and all of it twice for the avoid-partial-spends
  retry). Sizes now come from placeholder signatures chosen with the keys the
  wallet actually holds, so they are still exact, including the node's sigop
  pricing for SLH-DSA inputs. Two latent defects on the placeholder path were
  fixed on the way: the dummy signature checker rejected post-quantum
  placeholders (every provider-based P2MR quote fell to the 20 000 vB
  maximum), and probe inputs with a null prevout looked like a coinbase, for
  which the node counts no witness sigops.
- Measured on the same regtest node and wallet copy, 2-input spend, against
  the v0.2.2 binary: `fundrawtransaction` 13.3–14.3 s → 0.2–1.2 s,
  `signrawtransactionwithwallet` 3.3–3.9 s → 0.01 s, `send` 15.5–16.4 s →
  0.2–0.7 s. `getbalance` and other wallet RPCs no longer queue behind them.

RPC changes
-----------

- `preferred_pq_algo` (values `ml_dsa_44`, `slh_dsa_128s`) now reaches the
  signer on every RPC that signs P2MR inputs, so the leaf the fee was quoted
  for is the leaf that gets signed:
  - `send` and `sendall` previously quoted for the requested leaf but signed
    the default ML-DSA leaf through the PSBT path.
  - `walletprocesspsbt` accepts `preferred_pq_algo` in its options object.
  - `signrawtransactionwithwallet` gains a fourth optional argument
    `preferred_pq_algo`; pass the value given to `fundrawtransaction`.
  - `bumpfee` and `psbtbumpfee` accept `preferred_pq_algo` in their options;
    the replacement is sized for that leaf and `bumpfee` signs with it.
    `psbtbumpfee` returns an unsigned PSBT, so pass the same value to
    `walletprocesspsbt`.
  Omitting the option keeps the previous behaviour (ML-DSA leaf). The fee
  estimator now honours the option as well, so a spend that asks for
  `slh_dsa_128s` is quoted at the SLH-DSA size (sigop-bound, 20 000 vB per
  input) instead of the ML-DSA size.

Build
-----

- **Optional AVX2 SLH-DSA backend on x86_64.** `cmake -DQTC_SLHDSA_AVX2=ON`
  builds `libbitcoinpqc` from the SPHINCS+ `shake-avx2` variant: SLH-DSA-128s
  signing 1.59 s → 0.40 s and verification 1.56 ms → 0.64 ms on the same
  core. The variant produces byte-identical signatures to the reference
  backend in both FIPS-205 modes and each backend verifies the other's
  signatures. It is off by default and not used for the Guix release binaries,
  which must run on CPUs without AVX2. See `doc/build-unix.md`.
- `bench_qtc` gains `bench_mldsa_sign` and `bench_slhdsa_sign`.

Also in this release
--------------------

- **`libbitcoinpqc`: SLH-DSA signing now uses its own entropy source.** The
  SPHINCS+ sources and the Dilithium shim both defined `randombytes()`, and
  link order gave SLH-DSA signing the ML-DSA shim, so the SLH-DSA wrapper's
  seeded hedging was never used and the documented deterministic C API
  (`random_data = NULL`) was not deterministic. The SPHINCS+ sources now call
  `slh_dsa_randombytes()`. Wallet signatures are unchanged and still hedged
  with fresh entropy; only the raw C API without caller entropy becomes
  deterministic, and a unit test now checks that on every backend.

Chain anchor
------------

*To be filled at tag time per `doc/release-process.md`: refreshed chain
anchor (height and block hash) and the activation-height statement above.*
