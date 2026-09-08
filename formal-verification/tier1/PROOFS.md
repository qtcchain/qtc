# Tier 1 — accounting-layer soundness (paper-rigorous proofs)

Discharges obligations **S1–S3** of `formal-verification/PLAN.md`. The decidable arithmetic lemmas are
machine-checked by Z3 (`accounting_z3.py`, 8/8 PROVED); the cumulative/inductive theorems are proved
here in full. Notation: `M = MAX_MONEY = 21·10⁶·COIN` (`consensus/amount.h`). All quantities are
`CAmount` (signed 64-bit). `MoneyRange(x) ≜ 0 ≤ x ≤ M`; `MoneyRangeSigned(x) ≜ −M ≤ x ≤ M`.

## 1.1 Model

The pool balance is a value `b ∈ ℤ` with two transition functions and one setter, transcribed from
`src/shielded/turnstile.cpp`:

- **Apply(b, v):** defined iff `MoneyRangeSigned(v)`; lets `b' = b − v` (the code computes
  `CheckedAdd(b, −v)`, which equals `b−v` exactly when no 64-bit overflow occurs, Lemma 1.2);
  *succeeds* iff `MoneyRange(b')`, in which case the new balance is `b'`; otherwise the state is
  unchanged and it *fails*.
- **Undo(b, v):** as Apply but with `b' = b + v`.
- **Set(b₀):** succeeds iff `MoneyRange(b₀)`.

`v` is the transaction's `value_balance`: `v > 0` is an unshield (value leaves the pool), `v < 0` a
shield (value enters), `v = 0` a balanced shielded shuffle. A block's pool transition is the
left-to-right composition of `Apply(·, vᵢ)` over its shielded txs (`validation.cpp` ConnectBlock);
genesis starts at `b = 0`.

## 1.2 Lemmas (machine-checked)

**Lemma 1.1 (no overflow) [MC, S1.2].** For `b` with `MoneyRange(b)` and `v` with `MoneyRangeSigned(v)`,
the true integer `b − v` lies in `[−M, 2M] ⊂ (INT64_MIN, INT64_MAX)`; hence `CheckedAdd(b, −v)`
returns `b − v` and never triggers the UB-guard. *Proof.* `M = 2.1·10¹⁵ < 2⁶²`; `b ∈ [0,M]`,
`−v ∈ [−M, M]` so `b − v ∈ [−M, 2M]`, and `2M < 2⁶³−1`. Z3-checked over all `b,v` in range. ∎

**Lemma 1.2 (cap arithmetic) [MC, S2.1].** For `MoneyRange(pool)` and `0 ≤ bps < 2¹⁶`,
`WindowCap(pool,bps) = ⌊bps·pool / 10⁴⌋` (before the `M`-saturation), every intermediate stays in
`[0, INT64_MAX]`, and the whole/fractional split is exact. *Proof.* Write `pool = 10⁴·a + r`,
`0 ≤ r < 10⁴`. Then `bps·pool = 10⁴·a·bps + r·bps`, so `⌊bps·pool/10⁴⌋ = a·bps + ⌊r·bps/10⁴⌋ =
whole + frac`. Bounds: `a ≤ M/10⁴ ≈ 2.1·10¹¹`, `a·bps < 2.1·10¹¹·2¹⁶ ≈ 1.4·10¹⁶ < 2⁶³`;
`r·bps < 10⁴·2¹⁶ ≈ 6.6·10⁸`. Z3-checked. ∎

## 1.3 Theorem 1 — turnstile invariant and the supply floor (net inflation = 0)

**Theorem 1 (turnstile).**
(a) [MC, S1.1] If `Apply(b,v)` (resp. `Undo`, `Set`) succeeds from a state with `MoneyRange(b)`, the
resulting balance satisfies `MoneyRange`.
(b) [MC, S1.3] On the success domain, `Undo(·,v) ∘ Apply(·,v) = id`: `Apply(b,v)=b−v` succeeds ⇒
`Undo(b−v,v)=b` succeeds and restores `b`. (Hence block disconnect exactly reverses connect.)
(c) [PR] **Supply floor.** Let `b₀ = 0` and `b₀ →…→ b_T` be any reachable sequence of successful
Applies with value-balances `v₁,…,v_T`. Then for all `t`, `b_t = −Σ_{i≤t} vᵢ ∈ [0, M]`. Consequently
the total unshielded (transparent value drawn from the pool) `U_t ≜ Σ_{i≤t, vᵢ>0} vᵢ` and total
shielded-in `S_t ≜ Σ_{i≤t, vᵢ<0} (−vᵢ)` satisfy `U_t ≤ S_t`. **The pool never emits more transparent
value than was shielded into it; net new transparent supply attributable to the pool is `≤ 0`.**

*Proof.* (a),(b) are the explicit acceptance tests `MoneyRange(b') ∧ b'≥0` in the code; combined with
Lemma 1.1 (the computed value is the true `b∓v`, no UB), both are immediate and Z3-checked. For (b),
`Apply` success gives `b−v ∈ [0,M]`; then `Undo(b−v,v) = (b−v)+v = b ∈ [0,M]` (true since `b` was
in range by hypothesis), so `Undo` succeeds and returns `b`.

(c) Induction on `t`. Base: `b₀ = 0 = −Σ_{i≤0}vᵢ ∈ [0,M]`. Step: each successful `Apply(b_{t-1},v_t)`
sets `b_t = b_{t-1} − v_t = −Σ_{i≤t}vᵢ`, and (a) gives `b_t ∈ [0,M]`. Now
`Σ_{i≤t} vᵢ = U_t − S_t = −b_t ≤ 0`, i.e. `U_t ≤ S_t`. ∎

**Corollary 1 (firewall independence of the inner proof).** Theorem 1(c) holds for *any* sequence of
value-balances the verifier accepts, including ones produced by a (hypothetically) forged CT proof:
it depends only on the per-tx `MoneyRange` acceptance and the pool's `≥ 0` floor, not on the
soundness of the lattice proof. This is the formal statement of "the turnstile bounds total loss even
if an inner proof breaks" and of why *transparent* holders are unaffected: transparent supply is
public and the pool can never push it above what entered. (Tier 2/3 then show the inner proof is
itself sound, so even the *pool* is not over-drained.)

## 1.4 Theorem 2 — velocity-cap soundness

**Definition (window egress).** For a per-block egress log `E` (a finite partial map height ↦ ℤ≥0,
`E[h] = max(0, b_{h-1}−b_h)` the block's net pool decrease) and window `W`, let
`Total(E, tip, W) = Σ_{ tip−W < h ≤ tip } E[h]` (`unshield_velocity.cpp` `WindowTotal`).

**Theorem 2.**
(a) [MC, S2.3] Membership in `Total` is the half-open interval `(tip−W, tip]`: the entry at height
`tip−W` is excluded and the entry at `tip+1` is excluded; the boundary is exact.
(b) [MC, S2.2] `RecordBlock(h, e)` adds `max(0,e) ≥ 0` to the log and `UndoBlock(h)` removes exactly
that entry; hence `Total` is restored after a connect/disconnect pair (reorg-exact, and consistent
with Theorem 1(b)).
(c) [PR] **Rate bound.** If every connected block `h ≥ h_act` satisfies `Total(E,h,W) ≤
WindowCap(b_{h-1}, bps)` (the ConnectBlock check), then for every height `t` the total net unshield
over the trailing window is at most `⌊bps·b_{t-W}/10⁴⌋`-scale: `Σ_{t−W < h ≤ t} E[h] ≤ ⌊bps·b_{t-1}/10⁴⌋`,
i.e. no more than a `bps/10⁴` fraction of the pool can be unshielded per `W`-block window.

*Proof.* (a),(b) are the half-open summation predicate and the map insert/erase, Z3-checked and
immediate from the code. (c) is the conjunction of the per-block checks at the final block of each
window; by Lemma 1.2 the right side equals the intended `bps/10⁴` fraction of the pool balance at the
window's last block. (The cap is evaluated against `b_{h-1}`, the pool at block start, so it is
self-tightening as the pool drains — strengthening, not weakening, the bound.) ∎

## 1.5 Theorem 3 — value_balance binds transparent outflow to the pool draw

**Theorem 3 [MC, S3.1].** For a shielded tx with transparent inputs summing to `vin` and outputs to
`vout`, with shielded `value_balance = v`, consensus (`tx_verify.cpp:203-227`) accepts only if
`vout ≤ vin + v` (with fee `= vin + v − vout ≥ 0`). Hence the transparent value created in excess of
the tx's own transparent inputs, `vout − vin`, is at most `v` — exactly the value the pool releases.
*Proof.* The code computes `adjusted = vin + v` (no overflow by the `MoneyRange` guards), rejects
unless `adjusted ≥ vout`, and sets `fee = adjusted − vout`. Thus `vout − vin ≤ v`. Z3-checked over
all money-range `vin, vout, v`. ∎

**Corollary 2 (Tier-1 firewall).** Combining Theorems 1(c) and 3: the cumulative transparent value
the pool can release is `U_t = Σ_{vᵢ>0} vᵢ ≤ S_t` (Thm 1), and each release is bound 1:1 to a real
`value_balance` carried by an accepted tx (Thm 3). Therefore — *independently of the lattice proof's
soundness* — (i) net new transparent supply from the pool is `≤ 0`, (ii) the realizable loss is at
most the live pool balance, and (iii) post-activation that loss is additionally rate-limited to
`bps/10⁴` of the pool per `W`-block window (Thm 2). This is the formal, machine-corroborated version
of the firewall described informally in the Zcash post.

## 1.6 Scope

Tier 1 establishes the *accounting* firewall: it does **not** show the lattice proof actually enforces
that an accepted `value_balance` corresponds to legitimately owned, conserved shielded value — that is
Tier 2 (relation soundness) and Tier 3 (reduction to M-SIS). Tier 1's guarantees hold regardless of
those, which is precisely their value as defense-in-depth.
