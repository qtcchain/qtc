# QTC Corporate Spend — product design for Ramp-style controlled spending on QTC

Status: draft product design, revision 1, 2026-10-20. Written against `main` at `5adbc7d`
(mainnet v2 live since 2026-10-05; v0.2.5 published 2026-10-20). Reference product studied:
Ramp corporate cards (<https://ramp.com/corporate-cards>, read 2026-10-20).
Companions: [`../docs/qtc-agent-wallet-spec.md`](../docs/qtc-agent-wallet-spec.md) (the policy
engine, custody tiers and spend pipeline this design reuses), [`qtc-pq-multisig-spec.md`](qtc-pq-multisig-spec.md)
§6 (descriptor grammar), [`qtc-key-management-guide.md`](qtc-key-management-guide.md) §4.2–§4.3 and
§7, [`qtc-block-explorer-spec.md`](qtc-block-explorer-spec.md) §7 (JSON API and broadcast gateway),
[`browser-wallet-backend.md`](browser-wallet-backend.md), [`../docs/qtc-evm-l2-spec.md`](../docs/qtc-evm-l2-spec.md)
§3, §8.2 and §11, [`../docs/wqtc-bridge-architecture.md`](../docs/wqtc-bridge-architecture.md).

---

## 1. Purpose and scope

This document describes how a company could run its operating spend on QTC with the controls
Ramp sells to finance teams: limits set before money moves, merchant and category restrictions,
approval workflows by amount and role, real-time visibility, and automated receipt and ledger
handling. It maps each Ramp capability to a QTC mechanism, states which parts exist today, which
need building, and which do not transfer at all, and proposes a phased rollout.

It specifies:

- the **decomposition of Ramp's offer** into its software and non-software parts (§2);
- the **mapping** of each part onto QTC primitives, with status (§3);
- the **architecture**: treasury vault, spend cards, policy engine, approvals, audit, receipts,
  merchant registry, card rails, admin console (§4);
- the **security model** and how it differs from a SaaS card program (§5);
- the **phases**, their dependencies and gates (§6);
- what **does not transfer** from Ramp and why (§7), the **economics** (§8) and the
  **open questions** (§9).

Out of scope: a credit or lending product (see §7), a custodial exchange, the legal, tax and
accounting treatment of a company holding QTC, and the internal design of any third-party
card-issuing platform.

Throughout, a **spend card** means a policy-guarded sub-wallet, whether its holder is a person,
a team, a vendor relationship or an AI agent. "Card" is used because that is the mental model
finance teams already have; nothing in Phases 0–2 involves a payment card.

### 1.1 Design principles

1. **Enforce in script where the balance justifies it.** Ramp's controls live in Ramp's
   database. A Tier 2 spend card's limit is a second signature that a separate process refuses
   to give. The company never has to trust the service for balances above the Tier 1 cap.
2. **One policy engine for every rail.** The same evaluation order answers a QTC-native spend,
   a QTC-EVM contract call and, later, a card network authorization request.
3. **The owner can always get the money back without the service.** Every Tier 2 descriptor
   carries an owner recovery leaf.
4. **Reuse what exists.** No consensus change, no new descriptor form, no new node RPC is
   required for Phases 0 and 1.
5. **Say plainly what is not replicable.** Credit, interchange-funded rewards and global card
   acceptance come from a bank and a card network, not from software.

---

## 2. What Ramp sells, decomposed

Ramp's corporate card page (read 2026-10-20) makes four pillars and a set of rails claims.

| Pillar | Ramp claim | What it actually is |
|---|---|---|
| Control before spend | Virtual cards per vendor, team or purpose with their own limit; category and merchant blocks; questionable vendors auto-flagged for approval | An authorization-time policy engine. The card network asks Ramp to approve each transaction and Ramp declines out-of-policy ones in that message |
| Visibility and approvals | Personalized approval workflows by amount or role; spend visible as it happens | Routing rules plus a transaction feed |
| Expense automation | Receipts by SMS, app or integrations; auto-generated receipts and memos; reminders; 200+ accounting integrations | Matching, OCR and exporters on top of the feed |
| Terms and rewards | Cashback on every swipe; partner offers; no annual fee or interest; no personal credit check or guarantee; credit limit from business revenue; 30-day charge payback | A charge card underwritten on the business and funded by interchange |
| Rails | Visa network, 200+ countries, local issuing in 30+ countries, Apple and Google Pay, 3DS, SSO | A bank identification number (BIN) sponsor, an issuer-processor and the Visa network |

Only the first three pillars are software. The fourth is a lending relationship and the fifth is a
card-network relationship. The software parts are the ones QTC can reproduce, and for the first
pillar QTC can reproduce them with stronger guarantees (§5).

---

## 3. Mapping to QTC primitives

| Ramp feature | QTC mechanism | Where | Status (2026-10-20) |
|---|---|---|---|
| Virtual card per vendor, team or purpose with a limit | One spend card = one policy document (`per_tx_atoms`, `per_day_atoms`, `per_day_tx_count`, `max_balance_atoms`) over its own descriptor wallet, funded from the treasury vault | Agent wallet spec §3, §6.1 | Built in `contrib/agent-wallet` (PR qtcchain/qtc#37, open); regtest only; unaudited |
| Merchant blocks | `destinations.mode` allowlist or denylist | Agent wallet spec §6.2 rule 3 | Built |
| Category blocks | Category attached to a destination by a merchant registry; policy allows or denies categories | §4.7 of this document | Not built; no registry exists |
| Questionable vendor auto-flagged | Unregistered or non-allowlisted destination returns `needs_approval` instead of `deny` (a per-card choice) | §4.3 | Policy engine returns `deny` today; one new mode |
| Approval by amount | `approval.require_above_atoms`, webhook channel, `ttl_seconds`, approval by PSBT hash | Agent wallet spec §5 step 4, §6.1 | Built, single threshold |
| Approval by role, multi-step | Approval routing table per card; approver signs the PSBT hash with an ML-DSA key | §4.4 | Not built |
| Real-time visibility | Every spend is an on-chain transaction; hash-chained audit log in PostgreSQL; explorer JSON API | Agent wallet README; explorer spec §7 | Built |
| Receipts, memos, reminders | `purpose` and `memo` on every request; receipt hash committed in the data carrier; reminder job on unmatched spends | Agent wallet spec §5 step 1; §4.6 | Memo built; receipt commitment and reminders not built |
| Accounting integrations | Export job: spends with fiat value at the reference price at spend time | §4.6 | Not built |
| Physical and virtual cards, Visa, Apple Pay | Card program through an issuer-processor; authorization webhook answered by the policy engine; QTC converted through RFQ desks | §4.8 | Not built; external partners required |
| No personal guarantee | No custodian: the company's own keys hold the funds | §5 | Inherent |
| 30-day charge payback, credit limit | Not replicable natively; prepaid from treasury | §7 | Out of scope until a lending partner exists |
| Cashback, partner offers | Only fundable from interchange, so only with card rails | §7, §8 | Depends on Phase 3 |
| 3DS, SSO | ML-DSA-44 signatures on every spend; Tier 2 co-signer; kill switch; freeze on repeated denials; owner recovery leaf | Agent wallet spec §3, §6.3 | Built |
| Machines paying machines | The agent wallet service is agent-first: scoped tokens per tool, no key export tools | Agent wallet spec §8 | Built; this is the segment Ramp does not serve |

---

## 4. Architecture

```
  officers (k-of-n, decaying)                         offline owner key
          │                                                  │
          ▼                                                  │
  ┌─────────────────────────┐   scheduled top-ups            │ CSV recovery leaf
  │   TREASURY VAULT (L1)   │ ───────────────────┐           │ on every Tier 2 card
  │   P2MR multi_pq k-of-n  │                    │           │
  └─────────────────────────┘                    ▼           ▼
                                   ┌──────────────────────────────────┐
                                   │          SPEND CARDS             │
                                   │  Tier 1: software policy, capped │
                                   │  Tier 2: 2-of-2 holder key +     │
                                   │          policy co-signer        │
                                   └──────────────┬───────────────────┘
                                                  │ every spend
                                                  ▼
            ┌──────────────── POLICY ENGINE (agent wallet spec §6) ────────────────┐
            │ frozen? → window? → destination/category? → limits? → fee? →        │
            │ approval threshold? ── approvals (role routing, signed by hash) ──   │
            │ hash-chained audit log ── receipt commitments ── exports            │
            └──────┬──────────────────────┬──────────────────────────┬────────────┘
                   ▼                      ▼                          ▼
         QTC-native payee          QTC-EVM payee              CARD RAILS (Phase 3)
         P2MR address              PQ account, token,         issuer-processor auth
         (contractor, miner,       escrow, subscription       webhook → policy engine
          another company)         contract                   → RFQ desk sells QTC
                                                              → fiat float settles
```

### 4.1 Treasury vault

The company master wallet is a P2MR multisig over the descriptor forms the node supports today
(`multi_pq`, `csv_multi_pq`, `ctv_multi_pq`; pq-multisig spec §6). The recommended shape is the
team treasury pattern in the key management guide §4.2 with the timelocked recovery of §4.3:
a k-of-n of officers' ML-DSA keys as the primary leaf, and a backup leaf that lowers the
threshold after a long cooldown so a lost key cannot strand the treasury.

The vault funds spend cards by **push, on a schedule**: a replenish-to-limit job proposes one
PSBT per period that tops each card up to its `max_balance_atoms`. Officers sign it like any
treasury spend. Cards never pull from the vault, so a compromised card cannot reach treasury
funds. This is the QTC equivalent of Ramp's company-level credit limit: the sum of card caps is
bounded by what the vault chooses to push.

Mainnet-capable today. Nothing in this section needs the agent wallet service.

### 4.2 Spend cards

A spend card is one descriptor wallet plus one signed policy document (agent wallet spec §6.1)
plus one or more scoped access tokens. Card kinds, all expressed as policy documents rather than
as separate code paths:

| Kind | Holder | Typical policy |
|---|---|---|
| Employee card | one person's key | per-transaction and daily limits; category allowlist; approval above a threshold; business-hours window |
| Vendor card | nobody; the company pays one counterparty | `destinations.allow` of exactly one address; `per_day_tx_count` of 1; amount cap at the contract value |
| Team budget | a team lead's key, several requesters | monthly cap through `max_balance_atoms` and the top-up schedule; approvals routed to the lead |
| Agent card | an AI agent's scoped token | the agent wallet spec as written: `per_tx_atoms`, allowlist, `freeze_on_policy_denials_per_hour` |

Custody follows the agent wallet spec's tiers. **Tier 1** (software-enforced, one encrypted
wallet, loss bounded by `max_balance_atoms`) suits petty-cash-sized cards. **Tier 2** puts a
policy co-signer into the script:

```
mr(multi_pq(2,<holder_mldsa>,<policy_mldsa>),
   {csv_multi_pq(<cooldown>,1,<owner_mldsa>),
    ctv_multi_pq(<sweep_template>,1,<holder_mldsa>)})
```

The holder cannot spend without the co-signer, the co-signer cannot spend at all on its own, and
the owner reclaims the balance after the cooldown with neither. The first argument of `mr()` is
a spendable primary leaf, so the 2-of-2 must sit there and a bare key must never (agent wallet
spec §3, PR qtcchain/qtc#36).

Costs that follow from Tier 2: a spend carries two 2,420-byte ML-DSA signatures and both keys in
the leaf, about 7.5 KB of witness, and QTC charges fees on full serialized size, so a Tier 2 spend
costs roughly twice a Tier 1 spend at the same fee rate. Fee history is empty on mainnet today, so
the policy floor `min_fee_rate_atoms_per_byte` is the normal case.

### 4.3 Policy engine

Reused unchanged from the agent wallet spec §6.2: first `deny` wins, then `needs_approval`, else
`allow`, evaluated as frozen → policy signature and version → time window → destination →
amounts and counters → fee rate and confirmations → approval threshold. The HMAC-signed,
versioned policy document and the per-day counters that survive restarts are what make Ramp's
"preset controls" hold here.

Extensions this product needs, all additive:

1. **Category rules.** A `categories` block with `allow` and `deny` lists, resolved through the
   merchant registry (§4.7). An unregistered destination is `needs_approval` when the card sets
   `unknown_destination: "approve"`, else `deny`. This reproduces Ramp's "questionable vendors
   are flagged, not blocked".
2. **Approval routing** (§4.4) in place of the single `require_above_atoms`.
3. **Required fields.** `require_purpose: true` and `require_receipt_within_hours` so a spend
   is incomplete, and the card is throttled, until a receipt commitment lands (§4.6).
4. **Recurring templates.** A vendor card may carry a `recurring` block (amount, interval,
   tolerance) so a monthly bill is `allow` without an approval each time, and a deviation is
   `needs_approval`.
5. **Fiat-denominated limits** as an option: `per_tx_fiat_cents` with the reference price
   source from agent wallet spec §7.4. Open question 4 in §9.

### 4.4 Approvals

The agent wallet spec approves by PSBT hash through a webhook with a TTL. This product adds a
routing table per card:

```json
"approval": {
  "routes": [
    {"above_atoms": 0,           "approvers": ["lead-key"],              "quorum": 1},
    {"above_atoms": 5000000000,  "approvers": ["lead-key", "cfo-key"],   "quorum": 2},
    {"above_atoms": 50000000000, "approvers": ["cfo-key", "ceo-key"],    "quorum": 2}
  ],
  "ttl_seconds": 86400
}
```

An approver signs `{psbt_sha256, policy_version, amount_atoms, destinations[], expires_at}` with
their own ML-DSA key. The service records the signature in the audit log and, for Tier 2, forwards
the envelope to the policy co-signer, which re-checks it against its own policy copy (agent wallet
spec §6.4). A Ramp approval is a button press logged by Ramp. A QTC approval is a signature the
approver cannot later deny and the service cannot forge.

### 4.5 Visibility and audit

Every spend is a QTC transaction, so the ledger of record is the chain and the admin console
reads it through the explorer JSON API (explorer spec §7) rather than through the service. The
service's hash-chained audit log adds what the chain does not carry: who requested, which policy
version decided, who approved, what the reference price was. The two are reconciled by txid; a
spend in one and not the other is an alert.

Cardholder and approver interfaces follow the browser wallet backend contract: keys derived and
signatures made locally, broadcast through the rate-limited gateway, node RPC never exposed.

### 4.6 Receipts, memos and the close

Ramp's expense automation is receipt capture, matching and export. The QTC version:

- **Memo and purpose** are already mandatory fields on a spend request (agent wallet spec §5).
- **Receipt commitment.** The cardholder uploads the receipt to the company's receipt store; the
  service writes `SHA-256(receipt) ‖ txid` into a data-carrier output of a later housekeeping
  transaction, batched daily, within the 1,000-byte `OP_RETURN` payload (1,004-byte cap) the
  EVM layer's anchors already rely on (EVM spec §3 and §8.2). The receipt itself stays off
  chain. An auditor can later prove a receipt
  existed by the close date without the company exposing its contents.
- **Reminders.** A job lists spends past `require_receipt_within_hours` with no commitment,
  notifies the holder, and throttles the card after a grace period. This is Ramp's "let Ramp
  send reminders for missing items".
- **Exports.** A nightly job emits each spend with `amount_atoms`, the fiat value at the
  reference price at spend time, purpose, category, approver and receipt hash, in CSV and in
  the import formats of the common ledgers. The reference price source is the one the agent
  wallet spec §7.4 settles on; the qtc.markets asking price is not a candidate there and is not
  one here.

### 4.7 Merchant and category registry

Merchant category codes exist only inside the card networks. For QTC-native spend a registry is
needed before category rules mean anything. Proposed as a contract on QTC-EVM:

- a merchant registers one or more payee addresses (L1 P2MR or L2 PQ account) under a display
  name and a category from a fixed list modelled on the coarse MCC groups (travel, software,
  advertising, fuel, entertainment, gambling, and so on);
- registration is signed with the merchant's ML-DSA key through the `MLDSA44_VERIFY`
  precompile (EVM spec §11.1), so a registry entry is attributable;
- categories can be disputed and re-assigned by a curator role; the policy engine caches the
  registry and treats an entry younger than a configurable age as unregistered.

Until the EVM layer reaches mainnet, the registry can ship as a signed JSON list published the
way release manifests are. Open question 3 in §9.

### 4.8 Card rails (Phase 3)

This is the only way to get Ramp's acceptance footprint, and it is a partnership, not code.
The pieces:

1. **Issuer-processor and BIN sponsor.** A platform that issues Visa or Mastercard cards under a
   sponsor bank's BIN and sends an authorization webhook for every transaction with the
   merchant, MCC and amount.
2. **Authorization decisioning.** The webhook is answered by the same policy engine as §4.3,
   with the MCC mapped onto the category list. Ramp's entire first pillar is this one step.
3. **Settlement.** The card program settles in fiat. Two models: (a) a pre-funded fiat float the
   company replenishes by selling QTC through the RFQ desks of agent wallet spec §7.2 on a
   schedule; (b) sell-at-authorization, where each approved transaction triggers a desk quote
   and swap. Model (a) is simpler and keeps the desk out of the authorization path. Either way
   the party holding the float carries QTC price risk between sale and settlement.
4. **Compliance.** Know-your-business onboarding, sanctions screening and money-transmission
   licensing or an agent-of-the-licensee arrangement with the sponsor. Typical lead time is six
   to twelve months with a partner that already runs crypto-funded card programs.

Interchange on card spend is what would fund rewards (§8).

### 4.9 Admin console and roles

The console is the planned QTC web wallet extended with company roles. Its custody model is
still open (§9, question 1); this design assumes the browser derives and uses role keys locally
and never holds treasury keys.

| Role | Can |
|---|---|
| Owner | create the vault, publish policy documents, hold the recovery key, unfreeze |
| Admin | create and retire cards, set routes, run top-ups for officer signature |
| Approver | sign approval envelopes for routes naming their key |
| Bookkeeper | read everything, run exports, attach receipts on a holder's behalf |
| Cardholder | spend within policy, attach receipts, see own card |

Agents are cardholders whose "interface" is the MCP and REST surface of the agent wallet service.

---

## 5. Security model

| Threat | Ramp | This design |
|---|---|---|
| Service compromised | All limits are database rows; the attacker who controls the database controls spend, bounded by Ramp's own fraud controls | Tier 1 cards lose at most `max_balance_atoms` each; Tier 2 cards lose nothing without the co-signer; the vault is unreachable |
| Co-signer compromised alone | not applicable | Nothing; it holds one of two required keys and no funds |
| Cardholder key stolen | Freeze the card in the console | Same, plus per-transaction and daily caps are script-enforced on Tier 2 |
| Policy tampered | Trust in Ramp's access controls | Policy is HMAC-signed by the owner's admin secret and versioned; the engine accepts only the newest published version; Tier 2 co-signer holds its own copy |
| Split a spend to dodge the threshold | Ramp's rules engine | Rolling 24 h sum and count make splitting ineffective (agent wallet spec §8) |
| Insider approver | Audit log | Approval is an ML-DSA signature over the PSBT hash; large routes require a quorum |
| Service disappears | Funds are with Ramp's bank | Owner sweeps every Tier 2 card through the CSV leaf; Tier 1 cards restore from verified encrypted backups |
| Quantum adversary | RSA and ECC throughout the card stack | ML-DSA-44 on every signature; SLH-DSA backup leaves where configured |

The rules for AI systems in the key management guide §7 apply to agent cards without change: no
seed material, no passphrases, no descriptor secrets through any tool.

What this design does **not** protect against: a merchant registry curator mis-categorizing a
payee (mitigated by `needs_approval` on unknowns), an owner publishing a bad policy, and QTC
price moves between a fiat-denominated limit and the spend.

---

## 6. Phases

| Phase | Deliverable | Depends on | Gate |
|---|---|---|---|
| 0 — Vault | Treasury multisig with timelocked recovery; manual top-ups to plain wallets; a written key ceremony | Nothing new; descriptors exist on mainnet | Recovery drill on regtest, then one mainnet drill with a small balance |
| 1 — QTC-native cards | Spend cards (Tier 1 and 2), approval routing, audit, console roles; payees are QTC addresses (contractors, miners, other companies, agents) | Agent wallet service Phases 1 and 3 (spec §12); external review of `contrib/agent-wallet`; web wallet custody decision | Audit complete; backup restore test; regtest suite green; the spec's rule that no agent or card holds more than the Tier 1 cap without Tier 2 |
| 2 — Expense automation | Receipt commitments, reminders, exports; merchant registry as signed list, then as an EVM contract | Phase 1; EVM layer on mainnet for the contract form | Export reconciles to chain for a full month with zero unmatched spends |
| 3 — Card rails pilot | Issuer-processor integration; authorization webhook answered by the policy engine; fiat float funded through RFQ desks | Phase 1; two desks quoting (agent wallet spec §12 Phase 4); sponsor bank and compliance partner | Pilot with a handful of cards and a capped float for one quarter |
| 4 — Credit and rewards | 30-day terms and cashback | A lending partner willing to underwrite against QTC treasuries; interchange from Phase 3 | Not planned; listed so nobody assumes it comes free with Phase 3 |

Phase 0 is possible today. Phase 1 is where the product exists. Phases 2 and 3 are independent of
each other once Phase 1 is live.

---

## 7. What does not transfer from Ramp

- **Credit.** Ramp underwrites the business and fronts thirty days of spend. Without a lender the
  QTC product is prepaid from the company's own treasury. That is a controls product, not a
  "replace your card" product, and the positioning must say so.
- **A free product.** Ramp charges nothing because interchange pays. A QTC-native product has no
  interchange until Phase 3.
- **Acceptance.** Visa reaches 200+ countries. QTC-native spend reaches QTC-accepting payees.
  Phase 1 is honest only if the first customers already pay such payees: contractors, miners,
  other QTC businesses, AI agents buying QTC-priced services.
- **Category codes.** MCCs come from the network. Off rails they must be rebuilt (§4.7).
- **Cashback and partner offers.** Funded by interchange and by Ramp's partner book.
- **Reimbursement in 40 currencies.** Out of scope; a Phase 3 program inherits the issuer's
  currency support.

---

## 8. Economics

Ramp is free to the customer and earns interchange. Options here, not mutually exclusive:

| Source | Available from | Note |
|---|---|---|
| Software fee per card per month | Phase 1 | The only revenue that does not depend on partners |
| Spread on QTC-to-fiat conversion | Phase 3 | Shared with the RFQ desk; must be disclosed as a cost of the float |
| Interchange | Phase 3 | Shared with the issuer-processor and sponsor; funds any rewards |
| Ecosystem value | Phase 1 | Companies holding QTC for operating spend is demand for the asset; the design must still stand on its own as a product |

No part of this design assumes treasury QTC is used to subsidize rewards.

---

## 9. Open questions

1. **Web wallet custody model.** The console is the planned web wallet, whose custody model and
   post-quantum key constraints are still to be settled. This design assumes local key use with
   no server-held treasury keys; if the wallet goes another way, §4.9 changes.
2. **Co-signer packaging.** Separate binary or a mode of the service (agent wallet spec §13.1).
   Separate is safer and this product should insist on it for Tier 2 cards.
3. **Merchant registry home and curation.** Signed list now, EVM contract later, or EVM only?
   Who curates categories and arbitrates disputes, and is that a role the project wants?
4. **Unit of account for limits.** QTC-denominated limits are exact but drift in purchasing
   power; fiat-denominated limits need the reference price source that agent wallet spec §13.5
   leaves open. Likely both, with fiat limits marked as approximate.
5. **Card rails partner and jurisdiction.** Which issuer-processor and sponsor, in which
   jurisdiction, and who holds the fiat float and the QTC price risk between sale and
   settlement.
6. **Approval key custody.** Approvers sign with ML-DSA keys. Where those keys live (browser,
   hardware, the service's keyring) decides how strong §4.4's non-repudiation claim really is.
7. **Receipt storage.** Hash on chain, blob where: the company's own store, the explorer host,
   or a third party. Privacy and retention rules follow from the answer.
8. **Human cardholders on an agent-shaped API.** The agent wallet service exposes MCP and REST
   for agents. Whether human cards use the same REST surface or a thinner cardholder API, and
   whether the service's name survives a product that is mostly for people.
9. **Tier 2 fee cost.** About 7.5 KB of witness per spend. Acceptable for corporate spend at
   current fee levels; revisit if fee history fills in and rates rise.
10. **Accounting and regulatory posture.** A company holding QTC for operating spend has
    accounting and possibly licensing consequences this document does not address. Needs
    counsel before Phase 1 goes to a customer.

---

## 10. References

- Ramp, "Corporate cards", <https://ramp.com/corporate-cards> (read 2026-10-20).
- [`../docs/qtc-agent-wallet-spec.md`](../docs/qtc-agent-wallet-spec.md), revision 2, 2026-10-19.
- `contrib/agent-wallet`, reference implementation, PR qtcchain/qtc#37 (open, unaudited).
- [`qtc-pq-multisig-spec.md`](qtc-pq-multisig-spec.md), [`qtc-pq-multisig-tutorial.md`](qtc-pq-multisig-tutorial.md).
- [`qtc-key-management-guide.md`](qtc-key-management-guide.md).
- [`qtc-block-explorer-spec.md`](qtc-block-explorer-spec.md) §7.
- [`browser-wallet-backend.md`](browser-wallet-backend.md).
- [`../docs/qtc-evm-l2-spec.md`](../docs/qtc-evm-l2-spec.md) §3, §8.2, §11.
- [`../docs/wqtc-bridge-architecture.md`](../docs/wqtc-bridge-architecture.md), [`../docs/wqtc-contracts.md`](../docs/wqtc-contracts.md).
