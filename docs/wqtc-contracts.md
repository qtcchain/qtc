# wQTC contracts — how they work, the decisions, and EVM integration

The authoritative guide to the wQTC EVM contracts: their mechanics, the design decisions baked into
them, and how to deploy and integrate them on an EVM chain. Companion to the token spec
[`wQTC.md`](wQTC.md), the [bridge architecture](wqtc-bridge-architecture.md), and the security dossier
[`contrib/wqtc/SECURITY.md`](../contrib/wqtc/SECURITY.md). Contracts live in `contrib/wqtc/evm/`.

> The contracts are **UNAUDITED references**. An external audit + invariant-fuzz campaign is mandatory
> before any real value flows.

---

## 1. Overview

wQTC is a wrapped, EVM-native representation of QTC. Two bridging models ship together:

- **A — federation lock-and-mint** (the canonical "wQTC" path). Lock QTC to a federation-controlled
  P2MR address; an M-of-N attestor set authorizes a mint of wQTC on the EVM chain; burning wQTC
  requests a QTC release. Contracts: `WQTC.sol`, `WQTCBridge.sol`, `ECDSAMultisigVerifier`.
- **B — trustless HTLC atomic swap**. No custodian — both legs lock under the same hashlock
  (`RIPEMD160(SHA256(preimage))`, byte-compatible with QTC's `OP_HASH160`); revealing the preimage to
  claim one leg exposes it for the other. Contract: `WQTCAtomicSwapHTLC.sol`.

```
WQTC.sol                 ERC-20 wQTC (18 dec) — ERC20 + Permit(2612) + 3009 + roles + pause + rescue + optional hook
WQTCBridge.sol           Model A mint/burn — EIP-712 attestation, replay guard, circuit breaker, guardian veto, redeem lifecycle
  └ IAttestationVerifier pluggable auth (ECDSA v1 → zk later)
  └ ECDSAMultisigVerifier M-of-N ECDSA verifier (classical EVM leg)
WQTCAtomicSwapHTLC.sol   Model B trustless swap
```

---

## 2. How it works

### 2.1 The token — `WQTC`
A standard ERC-20 (OpenZeppelin v5.6) with **18 decimals**, plus:
- **EIP-2612 `permit`** — gasless approvals by signature.
- **EIP-3009** — `transferWithAuthorization` (anyone relays), `receiveWithAuthorization` (only the payee
  relays — front-run safe), `cancelAuthorization`. Random 32-byte nonces; each is single-use
  (`authorizationState[authorizer][nonce]`). Enables gasless transfers and meta-transactions.
- **Roles** (`AccessControlDefaultAdminRules`, i.e. 2-step admin transfer + enforced delay):
  `MINTER_ROLE` & `BURNER_ROLE` (held by the **bridge**, never an EOA), `PAUSER_ROLE` /`UNPAUSER_ROLE`
  (separate — fast to pause, slow to unpause), `RESCUER_ROLE`.
- **Issuance pause** — `pauseIssuance()` blocks mint/burn (the backing-safety lever) but **does not
  freeze holder transfers** (no censorship lever by default).
- **Optional compliance hook** — `setComplianceHook()` (Timelock only) can install an `IComplianceHook`
  consulted on every transfer/mint/burn; **unset by default** (neutral). This is the
  Circle/cbBTC blacklist *capability*, made opt-in and explicit instead of unconditional.
- **`rescueERC20`** — recover *foreign* tokens mis-sent to the contract; can never touch wQTC.

**Unit model.** `1 QTC satoshi == 1e10 units of wQTC` (the `SAT_SCALE`). Mints are always whole-sat
multiples — no sub-satoshi backing is created by the bridge; sub-sat balances arise only from EVM-side
division and are backed in aggregate. See the decimals decision in §3.

### 2.2 The bridge — `WQTCBridge` (Model A)

**Mint.** `mint(qtcTxid, vout, to, amountSat, proof)`:
1. Rejects if `mintPaused`, zero recipient/amount, or the QTC outpoint was already minted/queued.
2. Computes the **EIP-712 typed digest** `mintDigest(qtcTxid, vout, to, amountSat)` — domain
   `{name:"WQTCBridge", version:"1", chainId: <live block.chainid>, verifyingContract: this}`, struct
   `MintAttestation(bytes32 qtcTxid,uint32 vout,address to,uint64 amountSat)`.
3. Calls `verifier.verifyMint(digest, proof)` (pluggable; see §2.3).
4. Enforces the **circuit breaker** (rolling-window sat cap + hard wQTC supply ceiling).
5. **Optimistic minting / guardian veto:** if `amountSat > optimisticThresholdSat` (and a
   `guardianDelay` is set) the mint is *time-queued*; a `GUARDIAN_ROLE` may `cancelQueuedMint` within
   the delay, and after the delay anyone calls `executeQueuedMint`. Otherwise it mints immediately.
6. Mints `amountSat × 1e10` wQTC; sets the outpoint replay guard before minting (checks-effects-interactions).

**Replay & forks.** One mint per QTC outpoint (`minted[depositKey(txid,vout)]`, `depositKey` uses
`abi.encode`). The EIP-712 domain binds the *live* chain id and this contract, so an attestation can't
be replayed across chains, bridges, deposits, recipients, or amounts — including after a chain fork.

**Redeem.** `redeem(amountWqtc, qtcDestination)`: rounds **down** to whole sat, burns the full
`amountWqtc` (dust → backing surplus), records a `Redeem`, emits `RedeemRequested`. The federation then
releases QTC and calls `fulfillRedeem(redeemId, qtcTxid)` to record it on-chain. If a redeem can't be
honored, `refundRedeem(redeemId)` (governance, after `redeemRefundTimeout`) re-mints to the burner — no
silently-lost funds.

**Controls.** `GOVERNANCE_ROLE` (a Timelock) sets the verifier, limits, and refunds; `GUARDIAN_ROLE`
cancels suspicious queued mints; `PAUSER_ROLE` toggles `mintPaused`/`redeemPaused` independently;
`FEDERATION_ROLE` marks redeems fulfilled. All external state-changers are `nonReentrant`.

### 2.3 The verifier — `ECDSAMultisigVerifier` (v1) and the zk path
`IAttestationVerifier.verifyMint(digest, proof)` is the **only** thing the bridge trusts for mint
authorization, and it's swappable. v1 ships `ECDSAMultisigVerifier`:
- `proof = abi.encode(bytes[] signatures)`, signatures ordered by **ascending recovered signer
  address** (cheap distinctness); verified with OpenZeppelin `ECDSA.recover` (rejects high-s
  malleability, bad `v`, and the zero-address case); returns true iff `≥ threshold` signatures come
  from the registered signer set. `rotateSigners()` (Timelock) replaces the set and clears the old one.

This is **classical** security on the EVM leg. The *authoritative* security is QTC's M-of-N
**post-quantum** attestation; the QTC lock + refund leaf bound exposure. Because ML-DSA/SLH-DSA can't be
verified in EVM gas, the endgame is to swap `verifier` for a **zk-attestation verifier** — a
constant-gas SNARK proving "≥M registered attestor PQ-signatures over the digest" — *without changing
the bridge, the lock format, or the statement*. See [architecture §8](wqtc-bridge-architecture.md).

### 2.4 The atomic swap — `WQTCAtomicSwapHTLC` (Model B)
`open(recipient, token, amount, hashlock, timeout, salt)` locks an ERC-20 under a 20-byte hashlock with
**balance-delta accounting** (fee-on-transfer/USDT-safe) and an **in-contract, sender-bound swap id**
(`computeId`, so it can't be squatted/front-run). `claim(id, preimage)` checks
`RIPEMD160(SHA256(preimage)) == hashlock` and pays the recipient, **revealing the preimage on-chain**
for the QTC leg. `refund(id)` returns to the sender after `timeout`. The hashlock is byte-identical to
QTC's `OP_HASH160` (verified: `0x42×32 → 8739f40ec4dbf569dcb38134c6e7310908566981`).

---

## 3. Design decisions (and why)

- **18 decimals (EVM-native).** Norm for wrapped BTC is 8 (WBTC/cbBTC/FBTC/tBTC); we follow the BTCB
  18-dec model deliberately for uniform EVM/DeFi math and sub-satoshi granularity. Safe because mints
  are whole-sat, sub-sat is EVM-internal and aggregate-backed, and redeem dust is *solvency-positive*
  (accrues to surplus). Isolated to `WQTC.decimals()` + `WQTCBridge.SAT_SCALE`.
- **No unconditional blocklist; optional governance-gated hook.** Neutral by default (WBTC stance); the
  censorship *capability* exists only as an opt-in, Timelock-installed module.
- **Issuance pause, not transfer freeze.** We can halt mint/burn (and mint/redeem independently at the
  bridge) without freezing holders.
- **Non-upgradeable core; swappable verifier + tunable limits.** Avoids the entire Nomad/Parity
  uninitialized-proxy bug class; the one thing likely to change (the attestation verifier) is pluggable,
  and limits/roles are adjustable — so the system is patchable where it matters without proxy risk.
- **Classical EVM-leg trust in v1, PQ via zk later.** Explicit and documented; never mistake the
  ECDSA verifier for on-chain PQ verification.
- **Circuit breaker + guardian veto.** A compromised verifier/federation is bounded (caps) and
  vetoable (queue + guardian), not an instant unlimited drain.

---

## 4. Deployment

Order and wiring (use a **Timelock owned by a multisig** as `admin`/governance everywhere):
```
1. WQTC           = new WQTC(timelock, adminDelay)                         // 18-dec token
2. Verifier       = new ECDSAMultisigVerifier(timelock, adminDelay, signers[], M)   // M-of-N
3. WQTCBridge     = new WQTCBridge(WQTC, Verifier, bridgeId, timelock, adminDelay)
4. WQTC.grantRole(MINTER_ROLE, bridge); WQTC.grantRole(BURNER_ROLE, bridge)
5. WQTC.grantRole(PAUSER_ROLE, opsMultisig); WQTC.grantRole(UNPAUSER_ROLE, timelock); WQTC.grantRole(RESCUER_ROLE, opsMultisig)
6. Bridge.grantRole(GOVERNANCE_ROLE, timelock); Bridge.grantRole(GUARDIAN_ROLE, guardianSet);
   Bridge.grantRole(PAUSER_ROLE, opsMultisig); Bridge.grantRole(FEDERATION_ROLE, federationRelayer)
7. Bridge.setLimits(maxSupplyWqtc, windowMintCapSat, windowDuration, optimisticThresholdSat, guardianDelay, redeemRefundTimeout)
8. (optional) WQTCAtomicSwapHTLC = new WQTCAtomicSwapHTLC()               // shared, permissionless
```
Build/test: `cd contrib/wqtc && forge install OpenZeppelin/openzeppelin-contracts@v5.6.1 foundry-rs/forge-std && forge test` (21 passing).

Per-chain notes: the EIP-712 domain captures the **live** chain id, so the same code is safe on any
EVM chain and across forks. Deploy fresh per chain (distinct `bridgeId` if you ever run two bridges on
one chain). No proxy ⇒ no initializer/storage-gap concerns.

---

## 5. Integrating wQTC on an EVM chain

### 5.1 dApp / wallet / DeFi integrators (consume wQTC)
- **Read `decimals()` — it is 18.** Never hardcode 8 for wQTC (it is the BTCB model, not WBTC).
  Display QTC with 8, wQTC with 18; 1 wQTC = 1 QTC.
- Use **`permit`** (EIP-2612) for gasless approvals, and **`transferWithAuthorization`/
  `receiveWithAuthorization`** (EIP-3009) for gasless/meta transfers. `DOMAIN_SEPARATOR()` and
  `nonces(addr)` are exposed; EIP-3009 nonces are random 32-byte values via `authorizationState`.
- **Index events:** `Transfer`, `Minted`/`MintQueued`/`MintCancelled`, `RedeemRequested`/
  `RedeemFulfilled`/`RedeemRefunded`, `MintPausedSet`/`RedeemPausedSet`, `VerifierUpdated`,
  `SignersRotated`.
- Be aware redeem of **sub-1-sat** amounts reverts (`BelowOneSat`) and fractional-sat redeems round
  down (the dust is burned).

### 5.2 Federation / bridge operators (authorize mints, fulfill redeems)
- **Lock detection:** watch QTC for deposits to the federation P2MR lock address (carrying the EVM
  recipient in `destination_id`); wait for sufficient confirmations.
- **Attest a mint:** each signer signs the EIP-712 `MintAttestation` (use `mintDigest` / standard
  `signTypedData` with the bridge's domain). Bundle `≥M` signatures **sorted by signer address**,
  `abi.encode(bytes[])`, and submit `mint(...)` (anyone can relay; the statement binds the recipient).
- **Limits & safety:** set `windowMintCapSat`, `maxSupplyWqtc`, `optimisticThresholdSat`, `guardianDelay`
  conservatively; run an off-chain **backing-invariant watcher** (locked sat ≥ circulating wQTC / 1e10)
  that auto-pauses (`setMintPaused(true)`) on divergence — this is the single most important operational
  control (Ronin went 6 days undetected).
- **Redeems:** observe `RedeemRequested`, release QTC to `qtcDestination`, then `fulfillRedeem`. If a
  destination is un-releasable, governance `refundRedeem` after the timeout.
- **Signer rotation:** `rotateSigners(newSet, M)` via the Timelock; the old set is cleared.

### 5.3 Atomic-swap integrators (Model B, no federation)
- **Build the QTC leg** as `mr(<internal>, {htlc(<H160>, <claimerPk>), refund(<locktime>, <senderPk>)})`
  with `H160 = RIPEMD160(SHA256(preimage))`; lock the EVM leg via `WQTCAtomicSwapHTLC.open(...)` under
  the **same** `hashlock`. Assemble + import the lock with stock descriptor RPCs:
  `getdescriptorinfo` (add checksum) → `deriveaddresses` (the P2MR lock address) → `importdescriptors`
  (watch for the deposit), then `sendtoaddress` to fund it.
- **Spend it with the node wallet RPCs** (these encapsulate the control-block + CSFS message + preimage
  witness assembly and signing, returning a fully-signed raw tx):
  ```
  # Recipient claims with the preimage (this REVEALS the preimage on-chain for the EVM leg):
  buildhtlcclaim  "<descriptor#cksum>" {"txid":"<txid>","vout":<n>} "<preimage_hex>" "<dest_address>" <fee_sat>
      -> {"hex":"<signed raw tx>", "complete":true}

  # Funder refunds after the locktime (only spendable once tip height >= <locktime>):
  buildhtlcrefund "<descriptor#cksum>" {"txid":"<txid>","vout":<n>} "<dest_address>" <locktime> <fee_sat>
      -> {"hex":"<signed raw tx>", "complete":true}
  ```
  Broadcast the returned `hex` with `sendrawtransaction`. `contrib/wqtc/qtc_wqtc.py`'s
  `build_claim` / `build_refund` are thin wrappers over exactly these calls.
- **Timeout asymmetry (critical):** the slower/first-moving leg (QTC) MUST have a strictly **longer**
  timeout than the faster leg (EVM) — i.e. the QTC `refund(<locktime>)` height must be safely later
  than the EVM `open(..., timeout)` — e.g. QTC 24–48h, EVM 6–12h — or the secret-revealer can be raced.
  The contract enforces only sanity bounds (`MIN/MAX_TIMEOUT`); the integrator/SDK enforces the gap.
- After the EVM `claim` (or the QTC `buildhtlcclaim` broadcast), extract the revealed preimage from the
  witness/event to claim the opposite leg. `contrib/wqtc/qtc_wqtc.py`'s `extract_preimage` recovers it
  from a confirmed QTC claim's witness stack.

### 5.4 Relayers / watchtowers
- Run the **backing-invariant watcher** and event monitors above; alert + auto-pause on anomaly.
- For atomic swaps, watch for `Claimed(id, preimage)` to relay the preimage cross-chain.

---

## 6. Security & audit
See [`contrib/wqtc/SECURITY.md`](../contrib/wqtc/SECURITY.md) for the full incident-corpus → defense
mapping, the residual-risk list (federation key custody, no on-chain QTC SPV, HTLC timeout asymmetry),
and the invariant/fuzz + external-audit plan required before mainnet value.
