QTC version 0.2.1
=================

Tag `v0.2.1`. First update after the mainnet v2 launch. It adds the v2 chain
anchor (security review item H3) and the refreshed Node.js binding for the
post-quantum signature library. No consensus rule changes: blocks are valid or
invalid exactly as before, and v0.2.0 and v0.2.1 nodes interoperate.

**Who should upgrade:** everyone running a node that may ever sync from scratch
or restart after an outage. Relays and wallets first; miners when convenient.

Verify downloads against the signed `SHA256SUMS` as described in the README.

Chain anchor (H3)
-----------------

A node syncing from nothing has no way to prefer the real chain over a forged
low-work header chain, which at floor difficulty costs almost nothing to
produce. v0.2.1 bakes in three values read from the live v2 chain and identical
on every launch node:

- **Checkpoint at height 300** (`__CHECKPOINT_HASH__`). Forks that diverge
  below this height are rejected outright.
- **Minimum chain work** `…__MINWORK_TAIL__` (the chain work at height 300).
  A syncing node ignores peers whose chain carries less work, so a fake chain
  cannot stall or mislead initial block download.
- **Chain transaction statistics** from `getchaintxstats` at height 300, which
  anchor the sync-progress estimate.

`assumevalid` stays unset: the chain so far is coinbase-only, so there is
nothing to skip, and the unset value is the safer default. These anchors are
raised with each release; the mainnet startup warnings about genesis-only
checkpoints and missing tx data are gone.

Also in this release
--------------------

- `src/libbitcoinpqc/nodejs` 0.3.0: binding updated to the current C API
  (FIPS 205 verification flag, signing with caller-supplied randomness, two
  static libraries with a randombytes shim, Jest 30 tests); development
  dependencies refreshed, clearing the Dependabot alerts on the repository.
- Repository going public: the source tree, release process and signing keys
  are published; `main` changes only through reviewed pull requests with
  signed commits; release tags are immutable.
