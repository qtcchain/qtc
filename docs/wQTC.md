# wQTC — Canonical Wrapped QTC (specification & integrator note)

**Status: forward-looking specification, announced with the 0.31.0 milestone.** This document defines how
the *canonical* wrapped representation of QTC on EVM chains ("wQTC") will work, so bridges, exchanges, and
DeFi integrators can build against it correctly ahead of its introduction. **It requires no change to QTC
consensus** — QTC stays an 8-decimal, `int64`-satoshi chain. wQTC is an EVM-side token; all decimal handling
lives in the bridge/wrapper contracts.

> Why canonical: QTC will publish a single authoritative wQTC standard (and reference contracts) so the
> ecosystem converges on **one** wrapped representation per chain, rather than fragmenting into competing,
> non-fungible wrappers. Third-party bridges should wrap *this* standard.

## 1. Token parameters
| Field | Value |
|---|---|
| Name / Symbol | Wrapped QTC / **wQTC** |
| Standard | ERC-20 (+ EIP-2612 `permit` recommended) |
| **Decimals** | **18** (EVM-native) |
| Value parity | **1 wQTC := 1 QTC** |
| Supply | Floating; equals QTC locked in bridge custody (1:1 by value) |

## 2. Unit model & the 10^10 scaling factor
- QTC smallest unit: **1 satoshi = 10⁻⁸ QTC** (`COIN = 100,000,000`; `CAmount` is `int64`).
- wQTC smallest unit: **10⁻¹⁸ wQTC** (1 "wei").
- Bridge scale factor: **S = 10¹⁰** ( = 10¹⁸ / 10⁸ ).
  - `wQTC_wei = sats × 10¹⁰`
  - `sats = wQTC_wei ÷ 10¹⁰`
- Consequence: the **lowest 10 decimal digits of a wQTC amount are "sub-satoshi"** — finer than QTC can
  represent. wQTC minted by the bridge is always a multiple of 10¹⁰ (it originated from integer sats); sub-sat
  precision only appears from EVM-side operations (AMM swaps, contract splits, rebasing/yield).

## 3. Mint (QTC → wQTC): lock-and-mint
1. User sends an integer-sat QTC amount to bridge custody on QTC.
2. After confirmations, the bridge mints `sats × 10¹⁰` wQTC to the user's EVM address.
3. Always exact (no rounding) — minted wQTC is a multiple of 10¹⁰.

## 4. Redeem (wQTC → QTC): burn-and-release, **round down**
1. User burns `W` wQTC-wei on the EVM side, targeting a QTC address.
2. Bridge releases `floor(W ÷ 10¹⁰)` satoshis of QTC.
3. The **sub-satoshi remainder `W mod 10¹⁰` is NOT released** — it remains as spendable wQTC (see §5).

**Requirement:** the wrapper/bridge MUST enforce round-down (never attempt to release a fractional sat). The
cleanest implementation is to **only accept redemptions in multiples of 10¹⁰** and let users keep the remainder
as wQTC; an equivalent design accepts any amount and refunds/keeps the `mod 10¹⁰` dust as wQTC.

### Worked example
- Redeem `0.5000000005` wQTC = `500000000500000000` wei.
- `floor(500000000500000000 ÷ 10¹⁰) = 50000000` sats = **0.50000000 QTC released.**
- Dust kept as wQTC: `500000000` wei = `0.0000000005` wQTC (= 5×10⁻¹⁰ wQTC, **< 1 sat**).

## 5. Dust handling (the "can't bridge the last sliver" case)
- The maximum amount ever non-redeemable at once is **< 1 satoshi = < 10⁻⁸ QTC** — economically negligible.
- It is **not lost**: it stays as ordinary wQTC, fully usable/tradeable on EVM. A holder redeems the whole-sat
  portion now and either (a) leaves the dust as wQTC, or (b) **aggregates** dust across holdings/time until it
  reaches ≥ 1 sat, then redeems it.
- Holders do NOT need to "acquire more QTC" to redeem their balance — only the final sub-sat fraction needs
  rounding up if they want to move it across.
- Optional bridge feature: a periodic **dust-sweep** that aggregates rounding remainders.

## 6. Backing & accounting invariants (bridge MUST hold)
- **1:1 backing at the satoshi level:** total redeemable wQTC (in sats, floored) ≤ QTC locked in custody.
- Mint increases locked QTC and wQTC supply by the same sat-value; redeem decreases both by the released sats.
- Track the aggregate sub-sat dust separately; it is backed by custody but only redeemable once aggregated to
  whole sats. Never allow total releasable sats to exceed locked sats.
- wQTC is **only** minted against confirmed locked QTC — no algorithmic/uncollateralized issuance.

## 7. Integrator checklist
- Treat wQTC as **18 decimals**; do not assume sub-sat redeemability — **expect round-down on redeem.**
- For QTC-side amounts, work in integer satoshis (`int64`); convert at the boundary with `×/÷ 10¹⁰`.
- Display/accept QTC with 8 decimals; wQTC with 18.
- Wrap the **canonical** wQTC contract per chain (addresses to be published); do not deploy competing wrappers.
- Honor `MAX_MONEY` (21,000,000 QTC) — wQTC supply can never exceed it in value.

## 8. Trust & security model
- The bridge is a **custodial lock-and-mint** system; its custody/operator/verifier design is the trust point
  (out of scope here). QTC consensus is unchanged and unaware of wQTC.
- Canonical contracts should be audited, upgrade-guarded, and pause-capable; publish addresses through an
  authoritative QTC channel so integrators can verify they are wrapping the real thing.

## 9. Status / roadmap
- **0.31.0:** this specification is published (ecosystem-prep). No on-chain change.
- **Future:** publication of the canonical wQTC reference contracts + the production bridge. Integrators should
  build to this spec now so they are ready at launch.
