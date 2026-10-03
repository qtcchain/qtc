# QTC upgrade policy

How QTC releases are rolled out, how old software is retired, and what every node operator and miner can rely on.
Applies from v0.1.3 (October 2026). Changes to this policy go through a pull request like any other change.

## 1. Three kinds of release

| Kind | Example | Compatibility | Operator action |
|---|---|---|---|
| **Non-consensus** | v0.1.2 (mining guards), v0.1.3 (chain anchor, docs) | Old and new nodes interoperate indefinitely | Upgrade when convenient |
| **Protocol** | a release that changes P2P messages or behaviour | Interoperates with the previous generation; may refuse older ones (section 3) | Upgrade within one release generation |
| **Consensus** | a change to block or transaction validity | Activates at a published **flag-day height**; non-upgraded nodes fork off at that height | Upgrade before the activation height |

Every release says which kind it is in the first paragraph of its release notes, and a consensus release carries an
**Activation** section with the height, the rule, and the date the notice was published.

## 2. Non-consensus releases

Nothing to coordinate. The release is published when built, gated and signed; nodes upgrade on their own schedule.
Mining-side fixes are flagged "miners should upgrade" when they affect whether a miner produces blocks (as in v0.1.2),
but a mixed network keeps working.

## 3. Protocol releases and the peer floor

QTC refuses peers whose protocol version is below `MIN_PEER_PROTO_VERSION` (`src/node/protocol_version.h`, enforced in
the version handshake). At launch both constants are `800001`, so anything older than QTC — upstream Bitcoin, the
predecessor code base — is refused today.

Convention from v0.1.4:

1. A release that changes P2P messages or behaviour **bumps `PROTOCOL_VERSION`** (800002, 800003, …). A release that
   does not touch P2P keeps the current value; v0.1.0 to v0.1.3 all speak 800001.
2. **The floor lags by one generation.** When a release with protocol *N* ships, `MIN_PEER_PROTO_VERSION` may rise to
   *N−1*: nodes one generation behind still connect, nodes two generations behind are refused. The floor never rises
   above the previous generation in a single release.
3. Before the floor moves, the release notes state how many peers on the public network would be affected (from the
   `qtc_peer_version` metric on the monitoring host) and the floor moves only in a release, never by configuration.

The floor retires *protocols*, not people: anyone can run the current software and keep mining. It is not used to
exclude participants, and it cannot distinguish releases that share a protocol version.

## 4. Consensus changes and flag days

A change to what makes a block or transaction valid:

1. Is designed and reviewed in writing first (a design note in `doc/` and a pull request discussion), with the
   activation mechanism named: flag-day height on mainnet, usually height-based on test networks too.
2. Ships in a release **at least 14 days before its activation height** (about 2 000 blocks at the 600 s target), with
   the height in the release notes, on the GitHub release page, and on <https://keys.qtc.gold>.
3. Applies identically to the launch fleet and to every other miner. A node that has not upgraded by the activation
   height follows a chain the upgraded network rejects; that is the operator's choice, made in the open.
4. Is never combined with a non-consensus "must upgrade" trick: block size, header fields or version bits are not used to
   force software upgrades outside a published consensus change.

The first mainnet anchor (v0.1.3: checkpoint at height 300, minimum chain work) is **not** a consensus change — it
governs which chain a syncing node follows, not block validity — and so needed no flag day.

## 5. Emergency releases

A release that fixes an actively exploited bug may shorten the notice in section 4; the release notes say so, state the
reason, and the ordinary notice period resumes for the next change. Security reports go through `SECURITY.md`.

## 6. What a release always carries

Reproducible Guix builds for every supported host, ship gates, a `SHA256SUMS` list signed offline by the release key
(fingerprint `72DD 01B8 E4BD 52FF BE1D 6576 1E28 9F6E CD6C BF30`, second channel <https://keys.qtc.gold>), release notes
in `doc/release-notes/`, and an annotated signed tag that is never moved or deleted. See `doc/release-process.md`.
