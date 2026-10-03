QTC version 0.1.3
=================

Tag `v0.1.3`. Second update after the mainnet launch. It adds the first chain
anchor (security review item H3) and carries the documentation pass that
accompanied the repository going public. No consensus rule changes: blocks are
valid or invalid exactly as before.

**Who should upgrade:** everyone running a node that may ever sync from scratch
or restart after an outage. Relays and wallets first; miners when convenient.
v0.1.2 and v0.1.3 interoperate.

Verify downloads against the signed `SHA256SUMS` as described in the README.

Chain anchor (H3)
-----------------

Before this release a node syncing from nothing had no way to prefer the real
chain over a forged low-work header chain, which at floor difficulty costs
almost nothing to produce. v0.1.3 bakes in three values read from the live
chain and identical on every launch node:

- **Checkpoint at height 300**
  (`4773201dbff9b0411c0826e72e5c70a4782b7ce79a09177d38de27969f6551d1`).
  Forks that diverge below this height are rejected outright.
- **Minimum chain work** `…00000001305c4fa5` (the chain work at height 300).
  A syncing node ignores peers whose chain carries less work, so a fake chain
  cannot stall or mislead initial block download.
- **Chain transaction statistics** from `getchaintxstats` at height 300, which
  anchor the sync-progress estimate.

`assumevalid` stays unset: the chain so far is coinbase-only, so there is
nothing to skip, and the unset value is the safer default. These anchors are
raised with each release; the mainnet startup warnings about the bootstrap
floor, genesis-only checkpoints and missing tx data are gone.

Also in this release
--------------------

- README, CONTRIBUTING, SECURITY.md and the documentation index rewritten for
  the launched network; the predecessor code base's documents archived under
  `doc/history/` or removed; release notes for 0.1.0 and 0.1.2.
- Repository process: `main` changes only through pull requests approved by the
  maintainer, with signed commits; release tags are immutable.
