QTC version 0.2.0
=================

Tag `v0.2.0`. **Consensus release: QTC mainnet v2.** This release restarts the
mainnet from a new genesis block with a **2 000 000 QTC treasury allocation**
minted in block 1 to the project treasury address. The first mainnet chain
(launched 30 September 2026) is abandoned; nothing mined on it carries over.
Total supply becomes **23 000 000 QTC**: 2 000 000 treasury plus 21 000 000 on
the unchanged mining schedule (50 QTC per block, halving every 210 000 blocks).

Read this first
---------------

- **The treasury allocation is a founder allocation**, decided by the project
  owner on 3 October 2026 and stated here, in the README, in
  `doc/launch/v2-treasury-reset.md` and on <https://keys.qtc.gold> before any
  public release. Address:
  `qtc1ztrwsedmxswv4q0kuzw5avlw0yxntucaz89vtmvkluds5u4332ntqwq3gct` (the key
  ceremony's treasury address; its key was proven on the first chain by
  transaction `c9dae50ea25de4facdd173d164967930e37d2f3038890d08c8e9d4b2e1ec6688`).
- **The first chain is not continued.** Blocks mined on it between 30 September
  and the v2 launch, including by outside miners, have no value on v2. Anyone
  holding v1 binaries can keep that chain alive; the project does not follow it.
- **v1 and v2 nodes cannot connect to each other**: the network message start
  changes (`51 54 43 21`), so a v1 node never learns about v2 and vice versa.

Consensus
---------

- `nTreasuryPremineHeight = 1`, `nTreasuryPremineAmount = 2 000 000 QTC`,
  `treasuryPremineScript` = the treasury address's P2MR script. Block 1's
  subsidy is the ordinary subsidy plus the allocation, and its coinbase must pay
  the allocation to the treasury script in exactly one output
  (`bad-cb-treasury-premine` otherwise). No other height is affected. The block
  assembler emits the output automatically.
- New genesis block `d5f04a8a320b4e7bb454c7c564ca9cf191f70d7521d2486b62274df776f4eb24`,
  time 2026-10-05 06:00:00 UTC (1791180000). The first chain's anchor from
  v0.1.3 (checkpoint at height 300, minimum chain work, tx statistics) is reset
  to the fresh-chain state and will be re-pinned once v2 has history.
- Everything else is unchanged: MatMul proof of work (n = 512), ASERT from
  block 0 with half-life 172 800 s and floor `0x1e011da5`, 600 s spacing,
  P2MR-only outputs, shielded pool closed from genesis, ports 19755 / 19754,
  DNS seeds, `qtc1z…` addresses. Test networks and regtest are unaffected
  (regtest gains `-regtesttreasurypremine{height,amount,script}` for tests).

Network
-------

- Protocol version **800002**, minimum peer protocol **800002**. The v2 network
  has no older generation; future floors follow `doc/upgrade-policy.md`.

Also in this release
--------------------

- `doc/upgrade-policy.md`: release kinds, protocol floor convention, flag-day
  rule for consensus changes.
- CI: `.dockerignore` no longer excludes `ci/`.
- README, CONTRIBUTING, SECURITY.md and the documentation index rewritten;
  predecessor documents archived under `doc/history/` or removed; release notes
  for 0.1.0, 0.1.2, 0.1.3 (built, never rolled) and 0.1.4 (never built).

Upgrading
---------

Every node: stop the v1 node cleanly, move the v1 data directory aside (it is
useless on v2 but harmless to keep), install v0.2.0 and start with an empty
data directory. Miners: mining opens at the published v2 genesis time, not
before. Wallets created on v1 can be reused on v2 (`-walletcrosschain=1` on
first load; v1 coins do not exist on v2).
