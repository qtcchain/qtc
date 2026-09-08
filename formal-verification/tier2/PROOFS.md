# Tier 2 — verifier-relation soundness (paper-rigorous proofs)

Discharges obligations **S4–S5** of `formal-verification/PLAN.md`: the C-002 nullifier (serial↔key)
binding and the value (inflation) binding of the SMILE2 CT proof. The decidable algebra is
machine-checked — `tier2/invertibility.py` (S4.2, 32 640 pairs over the real ring) and
`tier2/relation_z3.py` (S4.1/S5.1/S5.2 factored-form identities, 8/8) — and the reduction arguments
that turn those identities into unforgeability are proved here, grounded in the lattice-commitment
literature.

**Citations.** BDLOP commitments and their binding under M-SIS: Baum–Damgård–Lyubashevsky–Oechsner–
Peikert, *More Efficient Commitments from Structured Lattice Assumptions*, ePrint 2016/997. Balance/
range arguments via bounded base-b carry correctors: Esgin–Zhao–Steinfeld–Liu–Sakzad, *MatRiCT*, CCS
2019 (ePrint 2019/1287), and Esgin–Steinfeld–Liu, *MatRiCT+*, S&P 2021 (ePrint 2021/545). The
opening/linear-proof protocol and its monomial-challenge special-soundness: Lyubashevsky–Nguyen–Seiler,
*SMILE*, CRYPTO 2021 (ePrint 2021/564). Fiat–Shamir-with-aborts and M-SIS hardness: Lyubashevsky 2012;
Langlois–Stehlé 2015.

## 2.0 Preliminaries and notation

`R_q = Z_q[X]/(X^128+1)`, `q = 2^32−959` (prime, `tier2/invertibility.py` certifies primality by
Miller–Rabin). By `params.h:22`, `X^128+1` factors into **32 irreducible quartic** factors
`X^4 − r_i` over F_q; hence by CRT `R_q ≅ ∏_{i=0}^{31} F_q[X]/(X^4 − r_i)`, a product of 32 copies of
the field `F_{q^4}`. Write `z ↦ (ẑ_0,…,ẑ_31)` for this NTT slot map (`ntt.h`). An element is a **unit
in R_q iff each slot image is nonzero**.

The challenge space is `C = { ±X^k : 0 ≤ k < 128 }`, `|C| = 256` (`HashToMonomialChallenge`,
`ct_proof.cpp:355`); each `c ∈ C` is a unit (a signed power of `X`, and `X` is a unit since `X·X^127 =
X^128 = −1`). The knowledge error *per challenge* is `1/|C| = 2^-8` — the figure that, taken naively,
looks alarming; Tier 3 shows it is not the soundness bound.

A BDLOP/SMILE opening response has the form `f = w − c·m` (sign per `ct_proof.cpp:3841`), where `w` is
the FS-bound commitment to the mask and `m` the committed message; the verifier recomputes `f` from
public data and checks a linear relation. **Binding** (BDLOP Thm 4.2, ePrint 2016/997): under M-SIS
for the public matrix, a PPT adversary cannot open one commitment to two distinct short messages except
with probability `≤ Adv_MSIS`.

## 2.1 Lemma 4 (monomial-difference invertibility) — S4.2 [MC]

**Lemma 4.** For all distinct `c, c' ∈ C`, the difference `c − c'` is a unit in `R_q`.

*Proof.* By CRT it suffices that `c − c'` is nonzero in every slot. For a monomial `±X^k`, its slot-i
image is `±r_i^{⌊k/4⌋}·X^{k mod 4}` (since `X^4 ≡ r_i`), a single nonzero term of degree `k mod 4 < 4`.
For distinct `(±,k) ≠ (±',k')`, the slot image of `c − c'` is the difference of two such terms; it
vanishes only if they have equal degree `k≡k' (mod 4)` and equal coefficient `±r_i^{⌊k/4⌋} =
±'r_i^{⌊k'/4⌋}`. The machine check `tier2/invertibility.py` evaluates this for **all 32 640 distinct
pairs across all 32 slots using the deployed `q` and `SLOT_ROOTS`**, and finds every difference nonzero
in every slot. Since each slot ring is a field, nonzero ⇒ invertible; by CRT `c − c'` is a unit. ∎
*(This is the design hypothesis of SMILE §3 / MatRiCT §2 — “challenge differences are invertible” — here
verified concretely for QTC's parameters rather than assumed.)*

## 2.2 Theorem 4 (serial ↔ key binding) — S4.1

The C-002 “w3” fix. The nullifier (serial) revealed by an input must be the one determined by the
*same* spend key that the proof opens — otherwise one note yields two serials (decoupled double-spend).

**Setup (from `ct_proof.cpp`).** For input `inp` the verifier holds: the framework key relation
(`:3705-3733`) that places `A·z0` inside an opening which must match the committed account — i.e. `z0`
is an accepting BDLOP **opening response for the spend key `s`** under the fixed NUMS matrix `A`
(`SMILE_GLOBAL_SEED`); and the serial check (`:3503-3512`)
`⟨b_sn, z0⟩ == w_sn + c0·serial`, with `w_sn` the FS-bound commitment to `⟨b_sn, y0⟩`.

**Theorem 4.** If a PPT prover produces, for one note (fixed `b_sn`, fixed committed key), two accepting
transcripts revealing serials `σ ≠ σ'`, then it yields either a break of BDLOP binding for `A` or a
short M-SIS solution for `A` — both `≤ Adv_MSIS(params)`.

*Proof.* Run the prover and rewind to obtain, by the forking lemma, two accepting transcripts on the
same first message with distinct challenges `c0 ≠ c0'`. The key relation makes `z0, z0'` openings of the
same commitment, so by special soundness (SMILE §3.2) the extractor computes a witness `s*` with
`z0 − z0' = (c0 − c0')·s*`; by **Lemma 4** `c0 − c0'` is a unit, so `s* = (c0−c0')^{-1}(z0−z0')` is
well-defined and short (the responses are bounded by the rejection-sampling norm `σ_key=31`). Thus `z0
= y0 + c0·s*` with `s*` the genuine extracted key (else two distinct openings of one commitment ⇒ BDLOP
binding break ⇒ short M-SIS collision, ePrint 2016/997 Thm 4.2).

Now expand the serial check using `w_sn = ⟨b_sn, y0⟩` (its FS binding) and `z0 = y0 + c0·s*`:
`⟨b_sn, z0⟩ = ⟨b_sn,y0⟩ + c0·⟨b_sn,s*⟩ = w_sn + c0·⟨b_sn,s*⟩`. The check forces this `= w_sn + c0·σ`,
hence `c0·(⟨b_sn,s*⟩ − σ) = 0`; `c0` is a unit, so **`σ = ⟨b_sn, s*⟩`** — the revealed serial is a
deterministic function of the extracted key. The factorization `⟨b_sn,z0⟩ − expect = c0·(⟨b_sn,s*⟩ − σ)`
is machine-checked (`relation_z3.py` S4.1) and the unit-cancellation `c0≠0 ∧ c0·B=0 ⇒ B=0` is
machine-checked exhaustively over a field (`relation_z3.py` S5.x). The same argument on the second
transcript gives `σ' = ⟨b_sn, s*⟩` (same extracted `s*` — same note, same committed key). Hence `σ = σ'`,
contradicting `σ ≠ σ'`. The only escape is a failure of one of the two BDLOP bindings invoked (`w_sn`,
the key opening), each bounded by `Adv_MSIS`. ∎

**Corollary 4 (no decoupled double-spend).** A note has exactly one verifying serial; the nullifier set
therefore detects every re-spend. This is the formal closure of the Orchard-class `w3` (serial⊥key
forge): in QTC the serial is *cryptographically* tied to the key under M-SIS, not merely well-formed.

## 2.3 Theorem 5 (value binding — no shielded inflation) — S5.1, S5.2

The C-002 “x1c” fix. An accepting CT proof must encode a *conserved* transaction: total input value =
total output value + public fee, over `Z`. The verifier enforces (`ct_proof.cpp:3848-3888`):
- **balance relation** `balance_lhs = Σ f[In] − Σ f[Out] + c·enc(fee) − c·Γ == balance_w`, and
- **Γ-validity (MANDATORY)**: `Γ` is a base-4 carry corrector — in each of the 32 slots only the
  constant coefficient is nonzero, `|digit| ≤ CARRY_BOUND = 4(n_in+n_out+1)`, and the integer
  evaluation `Eval(Γ) = Σ_j d_j·4^j == 0`.

**Theorem 5.** If a CT proof verifies, then `Σ a_in = Σ a_out + fee` over `Z`; no accepting proof
encodes net positive shielded value (no inflation), except with probability `≤ Adv_MSIS` (a forged
commitment opening).

*Proof.* With `f = w − c·m` the openings give `Σf[In] − Σf[Out] = w_bal − c·(Σenc(a_in) − Σenc(a_out))`,
so `balance_lhs = w_bal − c·(R + Γ)` where `R = Σenc(a_in) − Σenc(a_out) − enc(fee)` (this factorization
is machine-checked, `relation_z3.py` S5.2). The check `balance_lhs == w_bal` therefore forces
`c·(R+Γ) = 0`; `c` is a unit (it is in `C`), so by unit-cancellation (machine-checked exhaustively over
a field) **`R + Γ = 0` in `R_q`** — for *any single* monomial challenge, no grinding required.

It remains to transport this ring identity to an integer balance. Three machine-checked facts
(`relation_z3.py` S5.1a/b/c) supply the bridge:
- (a) the base-4 encoding is **positionally faithful**: `Eval∘enc = id` on the amount range and `Eval`
  is injective on in-range digit vectors;
- (b) `Eval(·) = Σ_j (·)_j·4^j` is **Z-linear**, so `Γ` and `R` live in a common evaluation module;
- (c) the carry bound `CARRY_BOUND = 4(n_in+n_out+1) < q/2` keeps every `Γ`-slot constant inside the
  centering window `(−q/2, q/2]`, so a slot equality mod `q` is a genuine **integer** equality (no
  modular wraparound). This is exactly why the `|digit| ≤ CARRY_BOUND` check is mandatory.

By (c) the relation `R + Γ = 0` holds over `Z` (not merely mod `q`); apply `Eval` and use (a),(b):
`Eval(R) + Eval(Γ) = 0`. The Γ-validity check gives `Eval(Γ) = 0`, hence `Eval(R) = 0`, i.e.
`Σ a_in − Σ a_out − fee = 0`, i.e. **`Σ a_in = Σ a_out + fee`** (machine-checked end-to-end,
`relation_z3.py` S5.1d). Conservation holds; net minted shielded value is 0. The only way to violate it
is to open a value commitment to a second short message (BDLOP binding break ⇒ short M-SIS), bounded by
`Adv_MSIS`. ∎

**Corollary 5 (why Γ-validity is load-bearing).** Drop the `Eval(Γ)=0` test and a forger sets
`Γ = enc(fee)`: then `R + Γ = Σenc(a_in) − Σenc(a_out)` can vanish with `Σa_in = Σa_out` while the
public `fee>0` is created from nothing (fee-absorption inflation). The validity check rejects this
precisely because `Eval(enc(fee)) = fee ≠ 0` (machine-checked, `relation_z3.py` S5.1e). This is the
MatRiCT carry-corrector discipline (ePrint 2019/1287 §4): the corrector must evaluate to zero, not to
the fee.

## 2.4 Relation to the Zcash Orchard class

Theorems 4 and 5 are the two halves of the Orchard-class failure that QTC's red-team found and C-002
fixed: a serial decoupled from its key (`w3`, closed by Thm 4) and a value/inflation forge (`x1c`,
closed by Thm 5). Both now reduce, *for a single non-grindable challenge*, to BDLOP binding of the
fixed NUMS matrices — i.e. to short-M-SIS — rather than to the `2^-8` per-challenge knowledge error.
Tier 3 makes the “reduces to M-SIS” quantitative as a knowledge-soundness bound `≤ q_H·Adv_MSIS`.

## 2.5 Scope and the residual

Tier 2 proves the verifier's algebraic relations **bind the committed witness** under the standard
commitment-binding assumption. What remains for Tier 3 is the *quantitative* knowledge-soundness
statement — that an efficient forger of S4/S5 yields an efficient M-SIS solver with the stated
advantage and extraction success — i.e. the game-based reduction, of which the per-hop algebra (Lemma 4,
the factorizations, the carry bridge) is exactly the machine-checked material assembled above. The
single standing assumption is M-SIS hardness at the deployed parameters (`α=β=10`, `q=2^32−959`,
`d=128`), which Tier 3 reduces *to* but does not prove.
