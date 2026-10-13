# QTC v0.2.5 (release candidate 3: testnet roll, 13 October 2026)

Testnet consensus parameter change and a wallet fix. **Mainnet consensus is unchanged**: mainnet blocks are valid or invalid exactly as before, and v0.2.0 through v0.2.5 interoperate on mainnet.

**Activation heights: testnet3 2290 (OP_RETURN cap, consensus); mainnet 3000 (protocol floor, network rule only).**

## Protocol floor (forced upgrade at mainnet height 3000)

The protocol version is **800004**. Until mainnet height 3000 the network accepts peers on 800002 and 800003 as before. From height **3000**, about 6 days after the tag at the October 2026 block spacing (which makes the v0.2.4 floor at 4100 redundant: 800002 and 800003 are retired together at 3000), a v0.2.5 node disconnects peers that speak less than 800004, at the handshake and on open connections: nodes still on v0.2.4 or earlier keep validating blocks but lose their connections to upgraded nodes. Block validity is unchanged. The one-off exception to the upgrade policy is recorded in `doc/upgrade-policy.md` next to the v0.2.4 one; test-only overrides `-minv025version` / `-v025floorheight`.

## Testnet3: OP_RETURN cap raised to mainnet's 1,004 bytes at height 2290

Testnet3 kept the legacy 83-byte OP_RETURN script cap while mainnet was raised to 1,004 bytes on 18 September 2026 (regtest already mirrors mainnet). The QTC-EVM anchor transaction carries a 101-byte payload, so its first testnet anchor was rejected by testnet consensus on 13 October 2026.

From testnet height **2290** the cap is 1,004 bytes, as on mainnet; below it the 83-byte cap still applies. This is a consensus loosening on testnet only: every testnet node must run v0.2.5 before height 2290 or it will reject blocks that carry larger OP_RETURN outputs. The mempool enforces the cap in force at the next block height, so such a transaction cannot relay before activation; the block assembler and `generatetoaddress`/`generateblock` use the template height. `-opreturnraiseheight=<height>` places the raise on regtest for tests (`feature_opreturn_raise.py`).

Parameters: `Consensus::Params::nMaxOpReturnBytesLegacy` (83), `nOpReturnRaiseHeight` (testnet3 2290, mainnet and regtest 0), `MaxOpReturnBytesAt(height)`.

## Wallet: tracking a chain whose timestamps trail the clock (#31)

After a stall, templates are clamped to MTP+12 h, so a live chain can trail real time by days. A wallet created on such a chain got a wall-clock birth time hours ahead of every block time, and `blockConnected()` skipped each new block until a manual `rescanblockchain` (seen on the testnet on 13 October: a funding transaction reported 0 confirmations two blocks after it was mined).

- New descriptors get a creation time clamped to the chain tip's maximum block time (`CWallet::ChainClampedTime`, new `Chain::getTipMaxTime`).
- `blockConnected()` scans a block regardless of the birth time when its chain time trails the wall clock by more than `TIMESTAMP_WINDOW`; imported keys with an explicit timestamp and wallets created elsewhere are covered too. Blocks replayed during initial block download are scanned as well.
- Functional test `wallet_birthtime_lagging_chain.py`.

Workaround on older versions: `rescanblockchain <height before the first expected payment>` once; the wallet's birth time is lowered and it tracks from then on.

## Chain anchor

The mainnet chain anchor is unchanged in this release candidate (height 1895, `bbc9f64e…`); the final v0.2.5 tag refreshes it per `doc/release-process.md` before the mainnet roll.

## Known issues

- `fundrawtransaction` occasionally fails with "Fee needed > fee paid" on the placeholder-quote path (#21); retry, or fund with explicit inputs.
