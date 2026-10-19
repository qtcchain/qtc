# QTC Agent Wallet Specification — wallets, spending and QTC buy/sell for AI agents

Status: draft specification, revision 2, 2026-10-19 (first draft 2026-09-24). Written against
`main` at `0ce4f64` (release v0.2.4; mainnet v2 live since 2026-10-05, protocol floor at mainnet
height 4100). Revision 2 corrects the Tier 2 descriptor, the block-spacing constant, witness-size
and fee estimates, the fee and price sources, and the RPC allowlist; see §14.
Companions: [`../doc/qtc-key-management-guide.md`](../doc/qtc-key-management-guide.md) §7
(rules for AI systems), [`../doc/qtc-block-explorer-spec.md`](../doc/qtc-block-explorer-spec.md)
§7 (public JSON API and broadcast gateway), [`wqtc-bridge-architecture.md`](wqtc-bridge-architecture.md)
(HTLC atomic swaps), [`wqtc-contracts.md`](wqtc-contracts.md), [`wQTC.md`](wQTC.md).

---

## 1. Purpose and scope

This specification defines how an autonomous AI agent (an LLM-driven program with tool access)
can **create a QTC wallet, receive and hold QTC, spend QTC, and buy or sell QTC** against
another asset, without ever holding seed material and without exceeding limits its owner set.

It specifies:

- the **Agent Wallet Service (AWS)**: a small daemon that sits between the agent and `qtcd`,
  owns the keys, enforces policy, and exposes an MCP tool surface and a REST API;
- the **custody tiers** an owner can choose, from software-enforced limits to limits enforced
  in P2MR script;
- the **trade venues** through which an agent buys or sells QTC: trustless HTLC atomic swaps,
  request-for-quote (RFQ) desks that settle by atomic swap, and the federation bridge to EVM;
- the **policy engine**, approval flow, audit log, and kill switch;
- the **test plan** on regtest and the rollout order.

Out of scope: a custodial exchange, an on-chain order book (QTC has none), the shielded pool
(sunset height 0 on mainnet v2; testnet keeps it until height 125 000, which regtest and testnet
tests must not rely on), and legal or tax treatment of agent-executed trades.

### 1.1 Design principles

1. **The agent never sees a secret.** Seeds, private descriptors, passphrases and signer
   material stay in the AWS and the node wallet. The agent holds a scoped bearer token. This
   is the key-management guide's §7 applied as an architectural boundary, not a guideline.
2. **Policy before signature.** Every spend is evaluated against the owner's policy before
   the wallet signs. Post-signing mutation of a PSBT is treated as a security violation.
3. **Limits in script where the stakes justify it.** Tier 2 puts the owner's co-signature and
   recovery path into the P2MR tree, so a compromised AWS cannot exceed them.
4. **Trades settle atomically or not at all.** The default buy/sell path is an HTLC swap. No
   venue in this spec takes custody of the agent's QTC except the federation bridge, which is
   labelled as such.
5. **Everything is auditable from public data.** Addresses, txids, PSBTs, quotes and policy
   decisions are logged; secrets are never logged.

---

## 2. Actors and trust boundaries

| Actor | Holds | Trusts | Must never receive |
|---|---|---|---|
| **Owner** (human or organisation) | owner ML-DSA key (Tier 2), AWS admin credential, backup archive passphrase | nothing; sets policy | — |
| **Agent** (LLM program) | AWS bearer token with scopes | AWS to enforce policy honestly | seeds, xprvs, passphrases, private descriptors, signed PSBTs it did not request |
| **Agent Wallet Service** | encrypted descriptor wallet(s), policy file, audit log, node RPC cookie | local `qtcd` | owner key (Tier 2) |
| **`qtcd`** (local, loopback RPC) | chainstate, wallet files | network consensus | public exposure of RPC |
| **Explorer / gateway** (public) | nothing of the agent's | — | RPC credentials |
| **Counterparty / RFQ desk** | its own keys | the HTLC, not the agent | anything but the swap parameters |
| **Bridge federation** (optional venue) | custody of locked QTC | M-of-N operators | — |

Network layout: the agent talks only to the AWS (loopback or mTLS). The AWS talks only to
`qtcd` on loopback and to public HTTPS endpoints (explorer, RFQ desks, EVM RPC). `qtcd` RPC
is never exposed; `rpcwhitelist` restricts the AWS RPC user to the methods in §9.

---

## 3. Custody tiers

Owners pick one tier per agent wallet. Both use descriptor wallets with P2MR addresses
(`qtc1z…`, ML-DSA-44 primary leaf, optional SLH-DSA backup leaf), as the node requires.

### Tier 1 — Software-guarded hot wallet

- One encrypted descriptor wallet per agent, created by the AWS with `createwallet` and
  `encryptwallet`. The passphrase lives in the AWS keyring (OS keychain or HSM-backed), never
  in config files or environment variables.
- Policy (§6) is enforced by the AWS before it calls `walletprocesspsbt`.
- Suitable for balances the owner can afford to lose to a full AWS compromise. The policy
  file's `max_balance_atoms` caps how much may sit here; excess is swept to the owner.

### Tier 2 — Script-guarded wallet (recommended above the Tier 1 cap)

Receive and change descriptor:

```
mr(multi_pq(2,<agent_mldsa>,<policy_mldsa>),        # PRIMARY LEAF: normal spend, agent + policy co-signer
   {csv_multi_pq(<cooldown>,1,<owner_mldsa>),        # backup leaf: owner recovery after cooldown
    ctv_multi_pq(<sweep_template>,1,<agent_mldsa>)}) # optional backup leaf: pre-committed sweep-to-owner
```

**Why the 2-of-2 is the first argument.** QTC's `mr(primary, {backup tree})` has no Taproot-style
internal key and no key-path spend: a P2MR output can only be spent by revealing a leaf script and
its control block (`src/script/interpreter.cpp`, witness v2 branch), and the first argument of
`mr()` is itself a spendable *primary leaf*; a bare key there becomes a single-signature
`CHECKSIG` leaf (`src/script/descriptor.cpp`, `mr()` parser). The first draft of this spec wrote
`mr(<agent_mldsa>,{...})`, which would have given the agent key a unilateral spend path and made
the co-signer decorative. Rule: **no live key is ever the bare first argument of an agent or swap
descriptor.** The AWS validates every descriptor it imports or accepts from a counterparty by
parsing it with `getdescriptorinfo` and rejecting any primary leaf other than the intended
multisig, HTLC or unspendable key.

- `agent_mldsa` is held by the agent wallet in the AWS. `policy_mldsa` is held by a
  **separate policy co-signer process** (same host in a different user, or a separate host)
  that signs only PSBTs carrying a valid policy decision (§6.4). A compromised AWS alone cannot
  spend.
- `owner_mldsa` is offline. The CSV leaf lets the owner reclaim everything after the cooldown
  without any AWS cooperation. It also serves as the kill switch of last resort.
- The CTV leaf, if present, lets the agent alone execute exactly one pre-committed action:
  sweep the whole balance to the owner. It cannot be repurposed because the template hash
  fixes the outputs.
- Uses only descriptor forms the node supports today (`multi_pq`, `csv_multi_pq`,
  `ctv_multi_pq`; see `doc/qtc-pq-multisig-spec.md`). No consensus change.

### Tier comparison

| Property | Tier 1 | Tier 2 |
|---|---|---|
| AWS compromise can drain | yes, up to cap | no, needs policy co-signer too |
| Owner recovery without AWS | via backup archive | via CSV leaf on chain |
| Per-spend witness size | ~3.8 KB (one 2,420-byte ML-DSA signature, one 1,312-byte key, leaf, control) | ~7.5 KB (two signatures and both keys in the leaf script, plus control); the multisig spec quotes ~8.8 KB for 2-of-3 |
| Setup | one command | key ceremony for three keys |

Fees are charged on full serialized size (QTC has no witness discount), so a Tier 2 spend costs
roughly twice a Tier 1 spend at the same fee rate. The policy engine's fee ceiling accounts for this.

---

## 4. Wallet lifecycle

All operations below are AWS operations invoked by the agent through MCP tools (§8) or REST
(§9). Node RPCs named are what the AWS calls internally.

| Step | Agent tool | AWS actions (node RPCs) | Owner involvement |
|---|---|---|---|
| **Create** | `wallet.create` | `createwallet` (descriptors, blank=false), `encryptwallet`, `getnewaddress` for the first receive address; Tier 2 additionally `importdescriptors` for the `mr(...)` tree after receiving `policy_mldsa` and `owner_mldsa` | Tier 2: supplies owner pubkey; approves tier |
| **Fund** | `wallet.receive_address` | `getnewaddress` (`address_type=p2mr`) | sends QTC |
| **Backup** | not exposed to agent | `z_verifywalletintegrity`, then `backupwalletbundlearchive` with `-stdinbundlepassphrase`; archive written to owner-controlled storage | holds archive passphrase |
| **Balance / history** | `wallet.balance`, `wallet.history` | `getbalances`, `listunspent`, `listtransactions` | — |
| **Spend** | `wallet.send` | §5 | approval if policy says so |
| **Rotate agent key** | `wallet.rotate` | new descriptor via `createwalletdescriptor`; sweep old to new via §5; old descriptor marked inactive | Tier 2: new tree import needs owner pubkey again (unchanged) |
| **Freeze** | `wallet.freeze` (agent may self-freeze) | `walletlock`; AWS refuses signing until owner unfreezes | owner unfreezes |
| **Retire** | not exposed to agent | sweep to owner address, `unloadwallet`, archive retained | approves |

Naming: wallet files are `agent-<agent_id>-<tier>`. The AWS refuses to load any wallet it
did not create unless the owner imports it explicitly.

---

## 5. Spend pipeline

Every outgoing transaction, including the QTC leg of a swap, goes through one pipeline:

```
agent request ─► policy evaluation ─► PSBT build ─► (approval) ─► sign ─► verify ─► broadcast ─► track
```

1. **Request.** `{to, amount_atoms | send_max, purpose, memo?, max_fee_rate?}`. Amounts are
   integers in atoms (1 QTC = 10^8 atoms); the AWS never accepts floats.
2. **Policy evaluation** (§6) returns `allow`, `needs_approval`, or `deny` with a reason code.
3. **PSBT build.** `walletcreatefundedpsbt` with `changeAddress` from the same descriptor,
   `lockUnspent=true`, an anti-fee-sniping locktime, and a `fee_rate` chosen in this order:
   the local node's `estimatesmartfee`; else the explorer `/fees` endpoint; else the policy's
   `min_fee_rate_atoms_per_byte` floor, which must not be below the node's `minrelaytxfee`
   (`getmempoolinfo`). Both estimators return no value while fee history is empty, as on mainnet
   today, so the floor is the normal case. The result is clamped by
   `max_fee_rate_atoms_per_byte`. The AWS records the PSBT hash.
4. **Approval** if required: the owner receives the decoded PSBT (destination, amount, fee)
   through the configured channel and approves by PSBT hash. Approval expires after
   `approval_ttl_seconds`; an expired approval requires a fresh build.
5. **Sign.** Tier 1: `walletprocesspsbt`. Tier 2: `walletprocesspsbt` in the agent wallet, then
   the PSBT plus the signed policy decision go to the policy co-signer, which verifies the
   decision signature and the PSBT hash before adding its ML-DSA partial signature. Then
   `finalizepsbt`.
6. **Verify.** The AWS re-decodes the final transaction and asserts outputs equal the approved
   outputs byte for byte. Mismatch aborts and raises a security alert.
7. **Broadcast.** `testmempoolaccept` then `sendrawtransaction` on the local node. The public
   gateway is a fallback only when the local node has no peers.
8. **Track.** The txid is returned immediately; the AWS polls `gettransaction` and emits
   `confirmed` at the policy's confirmation depth (default 6) and `dropped` if evicted.

---

## 6. Policy engine

### 6.1 Policy document

One JSON document per agent wallet, signed by the owner's admin credential, versioned, and
loaded by the AWS at start. Example:

```json
{
  "version": 3,
  "agent_id": "trader-01",
  "tier": 2,
  "limits": {
    "per_tx_atoms": 5000000000,
    "per_day_atoms": 20000000000,
    "per_day_tx_count": 40,
    "max_balance_atoms": 100000000000,
    "max_fee_rate_atoms_per_byte": 20,
    "min_fee_rate_atoms_per_byte": 1,
    "min_confirmations_to_spend": 1
  },
  "destinations": {
    "mode": "allowlist",
    "allow": ["qtc1z…owner", "qtc1z…desk-a-refund"],
    "allow_swap_htlc_addresses": true
  },
  "approval": {
    "require_above_atoms": 1000000000,
    "channel": "webhook",
    "ttl_seconds": 900
  },
  "trading": {
    "venues": ["htlc_p2p", "rfq"],
    "counterparties": ["desk-a", "desk-b"],
    "max_slippage_bps": 50,
    "max_open_swaps": 3,
    "min_refund_lock_blocks": 40,
    "max_refund_lock_blocks": 400,
    "reference_price_sources": ["desk-a", "desk-b"],
    "max_price_deviation_bps": 200
  },
  "time_windows": [{"days": "mon-fri", "utc": "12:00-22:00"}],
  "kill_switch": {"freeze_on_policy_denials_per_hour": 5}
}
```

### 6.2 Evaluation rules

Evaluated in order; the first `deny` wins, then `needs_approval`, else `allow`.

1. Wallet not frozen; policy signature valid; policy version is the newest the owner published.
2. Time window (if any) contains now.
3. Destination is allowlisted, or is an HTLC address the AWS itself derived for an open swap
   and `allow_swap_htlc_addresses` is true, or `mode` is `denylist` and the destination is
   absent from it.
4. `amount ≤ per_tx_atoms`; rolling 24 h sum plus amount `≤ per_day_atoms`; rolling count
   `< per_day_tx_count`.
5. Fee rate `≤ max_fee_rate_atoms_per_byte`; inputs have `≥ min_confirmations_to_spend`.
6. For trades: venue and counterparty allowed; quote within `max_price_deviation_bps` of the
   median of `reference_price_sources`; slippage bound honoured; open swaps `< max_open_swaps`;
   refund locktime inside the configured range.
7. `amount > require_above_atoms` → `needs_approval`.

### 6.3 Rate limiting and anomaly response

- Denials are counted per hour; crossing `freeze_on_policy_denials_per_hour` freezes the
  wallet and notifies the owner. A frozen wallet only unfreezes by owner action.
- The AWS refuses any request from a token whose scope does not include the tool.

### 6.4 Signed policy decision (Tier 2)

For every `allow`, the AWS emits `{psbt_sha256, policy_version, decision, amount_atoms,
destinations[], expires_at}` signed with the AWS's own ML-DSA decision key. The policy
co-signer verifies this envelope, verifies the PSBT hash matches, re-checks amounts and
destinations against its own copy of the policy, and only then signs. Two independent
evaluations must agree for a Tier 2 spend.

---

## 7. Buying and selling QTC

QTC has no on-chain exchange. This spec defines three venues; the policy names which an agent
may use. Prices are quoted as `counter_asset_atoms per QTC`; the counter asset on the EVM leg
is any ERC-20 (typically a USD stablecoin or wQTC). The default EVM leg is **QTC-EVM**, the
QTC-anchored layer 2 (testnet chain id 20821; mainnet release planned 31 October 2026), with
other EVM chains allowed per policy by chain id.

### 7.1 Venue A — Trustless HTLC atomic swap (default)

Uses the P2MR HTLC tree the node already supports and the `WQTCAtomicSwapHTLC.sol` reference
contract; the Python SDK `contrib/wqtc/qtc_wqtc.py` provides `HtlcLeg`, `build_claim`,
`build_refund`, and `extract_preimage`.

QTC leg descriptor (from the SDK):

```
mr(<internal>,{htlc(<hash160>,<claimer_mldsa>),refund(<locktime>,<sender_mldsa>)})
```

Hashlock is `RIPEMD160(SHA256(preimage))` on both chains; both are EVM precompiles.

`<internal>` is **not** a key-path key (see §3): it is the primary leaf of the tree, and a live key
there could spend the HTLC output unilaterally before the timeout. The AWS always sets it to the
published **unspendable ML-DSA-44 key** `QTC_UNSPENDABLE_MLDSA44` (an ML-DSA public key whose
`t1` component is derived by hashing a fixed public string, so that no secret key of the required
norm exists under the MLWE assumption; the exact derivation and the hex value belong in the
key-management guide), and it refuses any counterparty descriptor whose primary leaf is anything
else. The reference SDK's `HtlcLeg.internal_pubkey` docstring calls this "the key-path internal
key"; that wording is wrong and the SDK example must pass the unspendable key.

**Sell QTC for a token** (agent is the QTC sender):

1. Agent obtains a counterparty offer (§7.2) containing the counterparty's ML-DSA claim key,
   EVM address, token, price, and expiry.
2. AWS generates the 32-byte preimage (agent never sees it until reveal), derives the QTC HTLC
   address with `getdescriptorinfo` and `deriveaddresses`, imports it watch-only, and funds it
   through the spend pipeline (§5) with `refund_locktime = tip + L_qtc`.
3. Counterparty locks the token in the EVM HTLC with the same hashlock and
   `L_evm < L_qtc` in wall-clock terms. The AWS converts QTC block locks to time using the
   chain's target spacing read from the node (`getdifficultyhealth.target_spacing_s`), which is
   **600 s on mainnet, testnet and testnet4** and 90 s only on regtest; it never hard-codes the
   constant. Because ASERT lets real intervals run well under target when hashpower is rising,
   the AWS applies a safety factor (default 0.5× target per block) when computing the earliest
   possible QTC expiry, and rejects offers where the counterparty's timeout would not expire first.
4. AWS verifies the EVM lock on chain (amount, token, hashlock, recipient, timeout), then
   claims it, revealing the preimage.
5. Counterparty claims the QTC leg with the preimage. If they never lock, the AWS refunds
   after `L_qtc` via `bridge_buildrefund`-style spend of the refund leaf.

**Buy QTC with a token** (agent is the token sender): mirror image. The counterparty funds the
QTC HTLC; the AWS verifies it with the explorer `/address/{addr}/utxo` endpoint and its own
node, locks the token with the shorter timeout, and claims the QTC with the preimage the
counterparty reveals (`extract_preimage` reads it from their EVM claim transaction).

Safety rules enforced by the AWS, not left to the agent:

- Timeout ordering: the side that reveals the preimage must always have the longer lock.
- Confirmation depth before acting on the counterparty's lock: configurable, default 6 on QTC.
- Never fund a second leg for an offer whose first leg is not confirmed.
- One preimage per swap, never reused; swap state machine persisted so a restart resumes
  correctly (`offered → funded → counterparty_locked → claimed | refunded`).

### 7.2 Venue B — RFQ desks

A desk is an HTTPS service that quotes and settles by Venue A. This spec fixes the wire
format so any desk can serve any agent.

```
POST /v1/quote      {side: "buy"|"sell", base: "QTC", quote: "<token>@<chain_id>",
                     amount_atoms, taker_mldsa, taker_evm}
→ 200               {quote_id, price, counter_amount, expires_at, maker_mldsa, maker_evm,
                     htlc_terms: {hash_algo: "hash160", qtc_lock_blocks, evm_lock_seconds},
                     maker_signature}
POST /v1/accept     {quote_id, hashlock?, qtc_htlc_address?, evm_htlc_id?}
→ 200               {swap_id, next_action, deadline}
GET  /v1/swap/{id}  → state machine view
```

Signatures are ML-DSA over the canonical JSON so the AWS can prove what was quoted. Desks are
allowlisted per policy; the AWS refuses unknown desks and unsigned quotes.

### 7.3 Venue C — Federation bridge to an EVM DEX

Lock QTC to the bridge's P2MR address (`bridge_address` with the agent's EVM recipient as
`destination_id`), receive wQTC after M-of-N attestation, trade on any EVM DEX, burn wQTC to
redeem. This is **custodial** during the bridged period; the policy must name it explicitly
and the AWS labels every such position `custodial: true`. See `wqtc-contracts.md` for the mint
and redeem lifecycle and the unaudited status of the reference contracts.

### 7.4 Price references and slippage

The AWS computes a reference price as the median of the configured desk quotes. No
trade-based VWAP exists today: the explorer publishes no price, and the figure on qtc.markets is
a published **asking-price rule** (a US$10 launch price adjusted daily for network work), not an
observed market price, so it **must not** be used as a reference source. With fewer than two
independent desk quotes the reference is undefined and every trade is `needs_approval`. A quote
deviating more than `max_price_deviation_bps` from the reference is denied. Slippage is bounded per swap by
`max_slippage_bps` relative to the accepted quote; because HTLC swaps are fixed-price, this
only matters for Venue C DEX trades, where the AWS sets the DEX router's minimum-out.

---

## 8. MCP tool surface

The AWS exposes one MCP server. Tool names, required scopes, and schemas:

| Tool | Scope | Input | Output |
|---|---|---|---|
| `wallet.create` | `wallet:admin` | `{agent_id, tier, owner_mldsa?, policy_mldsa?}` | `{wallet_id, receive_address, tier}` |
| `wallet.receive_address` | `wallet:read` | `{wallet_id, label?}` | `{address}` |
| `wallet.balance` | `wallet:read` | `{wallet_id}` | `{confirmed_atoms, pending_atoms, immature_atoms, locked_in_swaps_atoms}` |
| `wallet.history` | `wallet:read` | `{wallet_id, before?, limit?}` | `{entries:[{txid, direction, amount_atoms, fee_atoms, height, purpose}]}` |
| `wallet.send` | `wallet:spend` | `{wallet_id, to, amount_atoms | send_max, purpose, max_fee_rate?}` | `{status: "broadcast"|"needs_approval"|"denied", txid?, psbt_sha256?, reason?}` |
| `wallet.freeze` | `wallet:read` | `{wallet_id, reason}` | `{frozen: true}` |
| `market.reference_price` | `market:read` | `{quote_asset}` | `{price, sources:[...], as_of}` |
| `market.quote` | `trade:quote` | `{wallet_id, side, quote_asset, amount_atoms, desks?}` | `{quotes:[...]}` |
| `trade.swap` | `trade:execute` | `{wallet_id, quote_id}` | `{swap_id, state, next_action}` |
| `trade.status` | `trade:read` | `{swap_id}` | `{state, qtc_txids, evm_txs, deadlines}` |
| `trade.cancel` | `trade:execute` | `{swap_id}` | `{state}` (refund path if past locktime) |
| `policy.describe` | `wallet:read` | `{wallet_id}` | the policy document minus secrets, plus today's remaining limits |
| `fees.estimate` | `market:read` | `{target_blocks}` | `{atoms_per_byte}` |

Rules:

- Every tool description states that amounts are integer atoms and that the agent cannot
  obtain keys through any tool. There is deliberately no `wallet.export`, `wallet.backup`,
  or `wallet.import` tool for agents.
- Tool results never include raw PSBTs unless the token holds `wallet:psbt` (intended for
  external-signer workflows, off by default).
- `wallet.send` with `status: "needs_approval"` returns a `psbt_sha256` the agent may poll on;
  it must not retry the send with a different amount to dodge the threshold, and the policy's
  daily counters make splitting ineffective.

---

## 9. REST API and node RPC allowlist

REST mirrors the MCP tools one-to-one under `/v1/` with bearer auth; it exists for agent
frameworks without MCP support. Errors use stable codes:

| HTTP | code | meaning |
|---|---|---|
| 400 | `bad_request` | schema or unit error (floats, missing purpose) |
| 401 | `unauthorized` | bad token |
| 403 | `scope_denied` | token lacks scope |
| 409 | `policy_denied` | see `reason` (`destination_not_allowed`, `per_tx_limit`, `per_day_limit`, `fee_rate`, `time_window`, `price_deviation`, `frozen`, …) |
| 202 | `needs_approval` | with `psbt_sha256` and `expires_at` |
| 423 | `frozen` | wallet frozen |
| 503 | `node_unavailable` | local node not synced or unreachable |

Node `rpcwhitelist` for the AWS user (everything else denied):

```
createwallet, loadwallet, unloadwallet, encryptwallet, walletpassphrase, walletlock,
getnewaddress, getbalances, listunspent, listtransactions, gettransaction,
walletcreatefundedpsbt, walletprocesspsbt, finalizepsbt, decodepsbt, analyzepsbt,
testmempoolaccept, sendrawtransaction, estimatesmartfee, getmempoolinfo, getblockchaininfo,
getblockcount, getnetworkinfo, getdifficultyhealth, getrawtransaction, gettxout,
getdescriptorinfo, deriveaddresses, importdescriptors, createwalletdescriptor, lockunspent,
z_verifywalletintegrity, backupwalletbundlearchive
```

No `dumpprivkey`-class, `exportwalletbundle`, `exportpqkey` with private data, or
`gethdkeys private=true` for this user.

---

## 10. Security requirements

1. **Secret handling** follows the key-management guide §7.2 exactly: seeds, xprvs,
   passphrases, archive passphrases, raw backups and private descriptors are never returned
   by any tool, never logged, never placed in environment variables or command lines
   (`-stdinwalletpassphrase` / `-stdinbundlepassphrase` only).
2. **Token scopes** are least-privilege; a trading agent normally gets `wallet:read`,
   `wallet:spend`, `market:read`, `trade:*` and not `wallet:admin`.
3. **Prompt-injection resistance.** The AWS treats every input from the agent as untrusted.
   Destinations, amounts and quotes found in web pages, emails or counterparty messages carry
   no authority; only the policy allowlist and the owner's approval do. Tool descriptions say
   so explicitly so the model does not infer otherwise.
4. **Post-signing immutability.** The final transaction must match the approved outputs; any
   difference is a hard failure with an alert.
5. **Kill switch** at three levels: token revocation (agent loses access), `wallet.freeze`
   (AWS stops signing), owner CSV recovery leaf (Tier 2, no AWS needed).
6. **Audit log** is append-only, hashed in a chain, and records request, policy decision,
   PSBT hash, txid, and outcome. It is exportable for the owner and contains no secrets.
7. **Node hygiene:** loopback RPC, cookie auth, `rpcwhitelist`, `disablewallet=0` only on the
   AWS node, `txindex=1` so `getrawtransaction` can verify counterparty funding. Before trusting
   a local broadcast the AWS checks `getnetworkinfo.connections` is above zero and the node is
   not in initial block download.
8. **Backups** are taken after wallet creation and after any descriptor change, verified with
   `z_verifywalletintegrity` first, and restore-tested on regtest before mainnet use.
9. **Counterparty verification** for swaps happens against the local node, with the explorer
   as a cross-check, never against the counterparty's claims alone.
10. **Financial-action boundaries** the owner should adopt: agents may execute trades only
    within the policy; anything above `require_above_atoms` is a human decision; the AWS never
    accepts card, bank or fiat credentials, which are out of scope for this design entirely.

---

## 11. Test plan

Regtest, driven by the functional test framework:

| Test | Asserts |
|---|---|
| `agent_wallet_lifecycle` | create, fund, balance, send within limits, backup archive restores on a second node with equal balance |
| `agent_policy_limits` | per-tx, per-day, count, fee-rate, time-window and destination denials each return the right code; daily counter survives restart |
| `agent_tier2_cosign` | Tier 2 spend fails without policy co-signer; succeeds with it; owner CSV leaf sweeps after cooldown; CTV sweep leaf cannot send anywhere but the template |
| `agent_tier2_primary_leaf` | the agent key alone cannot spend a Tier 2 output (no single-signature leaf exists); a descriptor with a bare live key as first argument is rejected by the AWS validator |
| `agent_htlc_descriptor_guard` | an HTLC descriptor whose primary leaf is a live key is rejected; with the unspendable key, only the hashlock and refund leaves can spend |
| `agent_fee_floor` | with empty fee history the build uses the policy floor, never below `minrelaytxfee`, and never above the ceiling |
| `agent_timeout_mapping` | timeout ordering uses the node's target spacing with the safety factor; a 90 s regtest constant leaking into a 600 s network is caught |
| `agent_swap_sell_qtc` | full HTLC sell against an EVM devnet (Anvil) HTLC; preimage extracted; counterparty no-show refunds after locktime |
| `agent_swap_buy_qtc` | mirror; timeout ordering violation is rejected before funding |
| `agent_swap_restart` | AWS killed mid-swap resumes from persisted state without double-funding |
| `agent_prompt_injection` | destinations embedded in tool inputs outside the allowlist are denied; approval cannot be satisfied by agent-supplied text |
| `agent_post_sign_mutation` | altered finalized tx is refused with a security alert |
| `agent_rpc_whitelist` | AWS RPC user cannot call export or private-key methods |

Testnet: 14 days of scheduled swaps against one desk with caps at testnet values, one key
rotation, one forced freeze and owner unfreeze, one Tier 2 owner recovery.

---

## 12. Rollout

| Phase | Deliverable | Gate |
|---|---|---|
| 1 | AWS with Tier 1, `wallet.*`, `fees.*`, policy engine, audit log; Python reference implementation over `qtc_wqtc.py` | regtest suite green |
| 2 | Venue A swaps against `WQTCAtomicSwapHTLC.sol` on an EVM testnet; `market.*`, `trade.*` tools | swap tests green; testnet swaps for 14 days |
| 3 | Tier 2 co-signer process and owner recovery ceremony docs | Tier 2 tests green; recovery drill |
| 4 | RFQ desk wire format published; first desk integration | two desks quoting |
| 5 | Venue C (bridge) enabled behind policy flag | audited bridge contracts; federation live |

Mainnet enablement for any agent waits for Phase 3, so no agent ever holds more than the
Tier 1 cap without script-enforced limits. Any node-side change this work needs (none is
expected: every descriptor form above exists today) follows the release rule in
`doc/release-process.md`: a fresh chain anchor and a stated activation height in every release.

---

## 13. Open decisions

1. Whether the policy co-signer (Tier 2) is a mode of the AWS binary or a separate binary.
   Separate is safer; the same binary is simpler to ship.
2. Which desk operates first, and whether QTC provides a reference RFQ desk implementation.
3. Whether to expose `wallet:psbt` at all in v1, or defer external-signer agents.
4. Approval channel defaults: webhook, email, or a signed message via the explorer.
5. Whether `market.reference_price` needs a trade-based VWAP source before mainnet enablement,
   or desk medians suffice. (The qtc.markets asking price is not a candidate; see §7.4.)
6. Who publishes `QTC_UNSPENDABLE_MLDSA44` and its derivation (§7.1), and whether the node
   should expose it through `getdescriptorinfo` so wallets can validate it without a copy.

---

## 14. Change log

| Date | Change |
|---|---|
| 2026-09-24 | First draft against `qtc-main` 7971d8e5. |
| 2026-10-19 | Revision 2 after review against `main` 0ce4f64: Tier 2 descriptor corrected (2-of-2 as the primary leaf; `mr()` has no key path); HTLC primary leaf must be the unspendable key; block spacing 600 s read from the node with a safety factor; witness sizes ~3.8 KB / ~7.5 KB and fees roughly double; fee source order with a policy floor; price reference limited to desk quotes, qtc.markets excluded; RPC allowlist completed; QTC-EVM named as the default EVM leg; shielded-pool heights per network; five tests added. |
