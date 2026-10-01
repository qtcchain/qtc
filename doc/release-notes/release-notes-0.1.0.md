QTC version 0.1.0
=================

Released 2026-10-01. This is the build that launched QTC mainnet: genesis block
`4040450ec30f1f9a7ef2d12578e1ea66d0838d7d8181b62c066953ca3baf3406`, genesis
time 2026-09-30 23:00:00 UTC (1790809200), tag `v0.1.0`.

Downloads are published on the GitHub release page. Verify the `SHA256SUMS`
list against the release-signing key (fingerprint
`72DD 01B8 E4BD 52FF BE1D 6576 1E28 9F6E CD6C BF30`, published at
<https://keys.qtc.gold> and in `doc/release-signing-keys.md`) before running
any binary. See the README, section "Install a release".

Upgrade notes
-------------

v0.1.0 is the first release; there is nothing to upgrade from. Operators who ran
the public test network (test3) on a release candidate can keep their test
datadir; the test chain is unchanged. Mainnet starts from an empty datadir.

**Known issue, fixed in v0.1.2:** with v0.1.0 a miner that starts before the
network has announced any block (for example at genesis, or when joining an
idle network) may refuse to produce templates (`getblocktemplate` readiness
refusal) or re-scan the same nonce range while its template time is pinned at
the minimum. Miners should run v0.1.2 or later.

What is in this release
-----------------------

- **Consensus.** MatMul proof of work (dimension 512 over F_{2^31-1},
  transcript block 16, noise rank 8, product-committed digest, pre-hash
  lottery with 18 epsilon bits) with two-phase validation. ASERT difficulty
  from block 0, half-life 172 800 s, hard floor `0x1e011da5`, 600 s target
  spacing, future-time drift bound 43 200 s. Subsidy 50 QTC halving every
  210 000 blocks. Witness v2 P2MR and `OP_RETURN` outputs only; reduced-data
  limits from genesis. The inherited shielded pool is closed from genesis
  (every activation and sunset height is 0); viewing and recovery code remain.
- **Signatures.** ML-DSA-44 (primary) and SLH-DSA-SHAKE-128s (backup) in a
  two-leaf P2MR tree; threshold and timelocked multisig descriptors; CTV and
  CSFS inside P2MR leaves. Falcon opcodes reserved for a future soft fork.
- **Network.** Mainnet P2P 19755, RPC 19754; DNS seeds `seed.qtc.gold` and
  `seed.qtc.exchange`; testnet P2P 29755, RPC 29754 with
  `testnet-seed.qtc.gold` / `testnet-seed.qtc.exchange`. Dandelion++ relay.
- **Mining.** Built-in solver with CPU, CUDA and Metal backends; supervised
  mining loop (`contrib/mining`) with chain-health guards; a template-readiness
  guard that refuses to mine while the node is behind its outbound peers; random
  nonce start when the template time is frozen at the drift ceiling (found by
  the burn-in stall test).
- **Wallet.** Descriptor wallets with post-quantum keys, HD purpose 87h,
  offline signing, encrypted backups.
- **Releases.** Reproducible Guix builds for Linux x86_64 (CPU and CUDA 12),
  Linux aarch64, Windows x64 and macOS arm64/x86_64 (signed and notarized,
  stapled `.dmg`); `scripts/release/verify_release_qtcd.py` ship gate;
  `contrib/verify-binaries` verifies a download against the signed list.

Burn-in
-------

The release candidates rc1 to rc9 ran a public test network from 15 September
2026 with four nodes (two GPU miners, two relays) and an independent ASERT
verifier. Tests covered a forced 14-hour stall (which found and fixed the
template-time nonce deadlock), 24-hour outages with unaided recovery, a timed
launch rehearsal on a throwaway network (227 blocks in 24 h, no reorg), offline
release signing, and clean-machine installs on Windows, macOS and aarch64. No
consensus mismatch was observed at any point. The security review of the launch
tree is in `QTC-SECURITY-REVIEW.md`; the launch planning documents are in
`doc/launch/`.

Known limitations
-----------------

- `getaddressinfo` reports `solvable: false` for P2MR addresses. Cosmetic;
  spending works.
- A minimum chain work and a first checkpoint are not yet set (planned for
  the first post-launch release once mainnet has a few hundred blocks).
- macOS ships a signed, notarized `.dmg`; a `.pkg` installer follows.
- The node's auto-update verifier is present but disabled by default.

Credits
-------

QTC is built on the Bitcoin Core code base. Thanks to the Bitcoin Core and
Bitcoin Knots contributors whose work it inherits.
