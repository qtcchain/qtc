QTC version 0.2.2
=================

Tag `v0.2.2`. Mining-safety and packaging update for mainnet v2, prompted by
the first external miner reports after the source went public. No consensus
rule changes: blocks are valid or invalid exactly as before, and v0.2.0,
v0.2.1 and v0.2.2 nodes interoperate.

**Who should upgrade:** everyone who mines, and every macOS user. Relays can
upgrade when convenient.

Verify downloads against the signed `SHA256SUMS` as described in the README.

Mining safety
-------------

- **Built-in block generation now applies the mainnet readiness policy.**
  `generatetoaddress`, `generatetodescriptor` and `generateblock`, which the
  supervised mining loop uses, previously bypassed the checks that
  `getblocktemplate` enforces. A node with no peers, only inbound peers, a
  stale validated tip or still in initial sync could keep producing blocks on
  a private fork that the network never accepts. Every generated block now
  passes the same policy as a template request: at least
  `-miningminoutboundpeers` (2) outbound peers, `-miningminsyncedoutboundpeers`
  (2) of them near the tip, not in initial block download, and the validated
  tip within `-miningmaxheaderlag` (3) blocks of the best header. The same
  options disable it (`-miningminoutboundpeers=0` etc.). Test chains and
  regtest keep the old behaviour.
- **`-miningchainguard` is on by default on mainnet** (still off on test
  chains and regtest), so `getmininginfo.chain_guard` reports a real status
  for a newcomer's node instead of `disabled`, and the mining loop's pause
  logic works out of the box.

macOS
-----

- **Precompiled Metal libraries ship inside the macOS archives**
  (`bin/metal/matmul_accel_kernels.metallib`,
  `bin/metal/oracle_accel_kernels.metallib`, built from this tag's sources
  with the Apple Metal compiler and covered by the signed checksum list). The
  node loads them directly and no longer depends on the macOS Metal compiler
  service at runtime, which a daemonized process could not reach.
- **`qtcd -daemon` on macOS** now prints a warning when Metal mining is
  requested, and refuses to start when `QTC_MATMUL_REQUIRE_BACKEND=metal` is
  set, instead of silently falling back to CPU mining.

Also in this release
--------------------

- Help text for `-miningchainguard` updated; release notes for 0.2.1 carried
  the v2 chain anchor, which is unchanged here.
