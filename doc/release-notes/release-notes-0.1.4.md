QTC version 0.1.4
=================

Tag `v0.1.4`. **Protocol release** (see `doc/upgrade-policy.md`): the P2P protocol
version advances to 800002. Nodes on v0.1.0–v0.1.3 (protocol 800001) still
connect to v0.1.4 nodes and vice versa. Not a consensus change.

This release carries everything that was prepared for v0.1.3, which was built
and signed but not rolled or published; v0.1.4 supersedes it.

Activation / compatibility
--------------------------

- `PROTOCOL_VERSION` is now **800002**. `getnetworkinfo` reports it, and
  `getpeerinfo` shows each peer's version, so the network's upgrade progress
  is visible for the first time.
- `MIN_PEER_PROTO_VERSION` stays **800001**: every existing QTC node keeps
  connecting. Nothing is refused by this release.
- **Notice:** in **v0.1.5, released no earlier than 14 days after this release
  is published**, the floor rises to 800002. From then on nodes still running
  v0.1.3 or earlier are refused at the version handshake by upgraded peers.
  The v0.1.5 notes will state the exact date and how many public peers were
  still on 800001 at release time. Operators and miners should move to v0.1.4
  within those two weeks.

**Who should upgrade:** everyone, within two weeks. Miners also gain the chain
anchor below.

Verify downloads against the signed `SHA256SUMS` as described in the README.

Chain anchor (H3, from v0.1.3)
------------------------------

Before this release a node syncing from nothing had no way to prefer the real
chain over a forged low-work header chain, which at floor difficulty costs
almost nothing to produce. Three values read from the live chain, identical on
every launch node, are now built in:

- **Checkpoint at height 300**
  (`4773201dbff9b0411c0826e72e5c70a4782b7ce79a09177d38de27969f6551d1`).
  Forks that diverge below this height are rejected outright.
- **Minimum chain work** `…00000001305c4fa5` (the chain work at height 300).
  A syncing node ignores peers whose chain carries less work.
- **Chain transaction statistics** from `getchaintxstats` at height 300, which
  anchor the sync-progress estimate.

`assumevalid` stays unset (coinbase-only history so far). The mainnet startup
warnings about the bootstrap floor, genesis-only checkpoints and missing tx
data are gone. Not a consensus rule change: blocks are valid or invalid exactly
as before.

Also in this release
--------------------

- `doc/upgrade-policy.md`: release kinds, the peer protocol floor convention,
  flag-day activation for consensus changes, emergency releases.
- CI: the Readiness workflow's container build no longer fails because
  `.dockerignore` excluded `ci/`.
- README, CONTRIBUTING, SECURITY.md and the documentation index rewritten for
  the launched network; predecessor documents archived under `doc/history/`
  or removed; release notes for 0.1.0, 0.1.2 and 0.1.3.
- Repository process: `main` changes only through pull requests, with signed
  commits; release tags are immutable.
