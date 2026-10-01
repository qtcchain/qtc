QTC version 0.1.2
=================

Released 2026-10-01, tag `v0.1.2`. First update after the mainnet launch. It
contains the two mining fixes found on launch night and nothing else; v0.1.1
(the first fix alone) was tagged but never built, so this release supersedes it.

**Not a consensus change.** Nodes on v0.1.0 and v0.1.2 interoperate; relays and
wallets need not hurry. **Miners should upgrade**: both fixes affect whether a
miner produces templates and finds blocks when the network is idle.

Verify downloads against the signed `SHA256SUMS` as described in the README.

Fixes
-----

- **Mining guards see peers at the genesis tip** (`4bd83237`). At launch, the
  template-readiness guard refused `getblocktemplate` on every miner at height
  0, and the advisory chain guard reported `insufficient_peer_consensus`. Until
  a peer announces a header or block its sync height is unknown; at genesis
  nothing has been announced because nobody may mine, so nothing is ever
  announced. The guards now fall back to the peer's version-handshake starting
  height when the sync height is unknown (a known sync height always wins, so a
  lagging peer cannot be talked back in). The mainnet default for
  `-miningminoutboundpeers` is 2 (was 3), matching a three-host launch mesh;
  the minimum stays outbound-only because inbound peers are cheap to sybil.
  The mining supervisor no longer disconnects a manual peer that has not yet
  announced a header. Operators who worked around the launch refusal with
  `miningminsyncedoutboundpeers=0` or `miningchainguard=0` should remove those
  lines.
- **Random nonce start when the template time is frozen at the floor**
  (`c3d15354`). Mining opened 46 minutes before the genesis time, so every
  template's `nTime` was pinned at the minimum and byte-identical; both
  launch miners re-scanned the same nonce range from zero until the clock
  passed T0. v0.1.0 already randomised the start when the template time was
  frozen at the drift *ceiling* (after a long stall); the same now applies at
  the *floor*. A caller-chosen nonce is never overridden.

Both fixes carry unit tests (readiness classification, chain-guard peer
height, frozen-floor template start) and a supervisor shell test.

Operational note
----------------

The mainnet launch fleet (two GPU miners, two relays, explorer) rolled to v0.1.2
on 2026-10-01 at 15:24 UTC with no lost blocks, using a clean `qtc-cli stop`
on each node and restarting the daemons before the miners.
