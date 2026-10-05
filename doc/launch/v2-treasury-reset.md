# QTC mainnet v2 — reset with a treasury allocation (design, 3 October 2026)

**Owner decision (3 Oct 2026):** restart mainnet with a 2 000 000 QTC treasury allocation. Approved parameters:
total supply 23 000 000; reuse of the ceremony treasury address; the allocation minted in block 1 by consensus rule;
a new network identity; the first chain abandoned; the allocation disclosed everywhere before any public release.

## 1. What changes

| | v1 (launched 30 Sep 2026) | v2 |
|---|---|---|
| Genesis | `4040450e…3406`, T0 2026-09-30 23:00 UTC | `d5f04a8a…`, T0 **2026-10-05 06:00 UTC** (1791180000), same merkle root |
| Network magic | `51 54 43 01` | `51 54 43 21` — v1 and v2 nodes cannot connect to each other |
| Supply | 21 000 000, all mined | **23 000 000**: 2 000 000 treasury (block 1) + 21 000 000 on the unchanged 50 QTC / 210 000-block schedule |
| Treasury | none | `qtc1ztrwsedmxswv4q0kuzw5avlw0yxntucaz89vtmvkluds5u4332ntqwq3gct` (ceremony address; key proven by tx `c9dae50e…` on v1) |
| Protocol version / floor | 800001 / 800001 | 800002 / 800002 |
| Everything else | — | unchanged: MatMul n=512, ASERT τ 172 800 s, floor `0x1e011da5`, 600 s spacing, P2MR-only outputs, shielded pool closed, ports, seeds, `qtc` HRP |

## 2. Consensus rule (`Consensus::Params`)

`nTreasuryPremineHeight = 1`, `nTreasuryPremineAmount = 2 000 000 QTC`, `treasuryPremineScript = OP_2 <32-byte program>`.

- `GetBlockSubsidyForBlock(1)` = ordinary subsidy + 2 000 000 QTC; every other height unchanged.
- `ConnectBlock` at height 1 requires **exactly one** coinbase output of exactly the premine amount to the treasury
  script, else `bad-cb-treasury-premine`. Keeping the premine for the miner, paying a different amount or a different
  script, or dropping the output all fail; paying it twice fails `bad-cb-amount`.
- The block assembler emits the treasury output automatically, so any v2 miner produces a valid block 1.
- The genesis coinbase stays unspendable by inheritance; that is why the allocation is in block 1.
- Regtest exposes the rule through `-regtesttreasurypremineheight/-amount/-script` for tests
  (`src/test/treasury_premine_tests.cpp`).

## 3. What is lost, stated plainly

Blocks 1–~560 of the first chain and every reward on it, including at least two outside miners' coinbases
(`qtc1zmlzhjz2ty…`, `qtc1za4mgapr…`), are not carried over. The first chain can be kept alive by anyone with v1
binaries; the project does not follow it. The announcement says this in the first paragraph.

## 4. Launch sequence (from the 30 Sep runbook, adjusted)

1. Code + tests merged to `main` (PR), tag `v0.2.0`, Guix five hosts, gates.
2. Owner signs macOS; release list signed offline (MacBook procedure while travelling).
3. Fleet: stop v1 miners and daemons cleanly, archive v1 datadirs, install v0.2.0, start at the chosen T0 (≥ 2 h 30 after
   the Linux gate), open mining at T0 — not before (launch lesson 2).
4. Block 1 carries the treasury output; the Mac watch wallet is re-created for v2 and must show 2 000 000 QTC
   (immature for 100 blocks).
5. Explorer indexes wiped and restarted on v2; keys.qtc.gold updated (genesis, supply, treasury, release list).
6. Verifier, exporters and alerts re-anchored.

## 5. Disclosure checklist (before any public flip)

README chain-parameter table (supply 23 M, treasury allocation row), release notes 0.2.0 (first paragraph), keys.qtc.gold,
announcement text, `doc/launch/` (this file). A reader must never find the allocation by surprise.
