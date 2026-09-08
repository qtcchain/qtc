# Tier 3 — knowledge-soundness reduction to Module-SIS (paper-rigorous)

Discharges obligation **S6** of `formal-verification/PLAN.md`: the knowledge-soundness error for
forging the consensus-critical CT relations (serial↔key, value conservation) is `≤ q_H·Adv_MSIS(params)`
— **not** the `2^-8` per-challenge monomial knowledge error. This is the quantitative form of Tier 2's
binding theorems and the formalization of `redteam/F3_MSIS_SOUNDNESS_REDUCTION.md`.

The reduction is a game-based argument; its **algebraic hops are machine-checked** (`tier3/reduction_z3.py`
H1–H4, plus Tier 2's Lemma 4 invertibility and the factorizations). Full mechanization of the forking
lemma and extractor in a proof assistant (EasyCrypt/SSProve) is out of scope (PLAN.md §5: no toolchain,
person-year effort); §3.4 lists the exact hand-proved lemmas an external EasyCrypt pass would discharge.

**Citations.** Forking lemma: Pointcheval–Stern 2000; Bellare–Neven 2006 (general forking). FS knowledge
soundness of Σ-protocols with abort: Lyubashevsky 2012; Attema–Fehr–Klooß 2022 (ePrint 2021/1377, tight
FS bounds for special-sound protocols). Commitment binding ⇒ M-SIS: BDLOP 2016/997. Protocol: SMILE
2021/564; MatRiCT 2019/1287.

## 3.0 The distinction Tier 3 must make precise

The SMILE2 opening uses a monomial challenge `c ∈ C`, `|C| = 256` (`HashToMonomialChallenge`). Two
*different* error quantities must not be conflated:

- **Opening knowledge error `ε_open = 1/|C| = 2^-8`.** The probability that an *honest extractor*,
  given one accepting transcript, fails to need a second one — i.e. the special-soundness gap. This is
  intrinsic to a 2-special-sound protocol and is *not* the forgery probability.
- **Forgery advantage.** The probability a *cheating prover* produces an accepting proof for a FALSE
  statement. A naive Fiat–Shamir bound for a single-round, `|C|=256` protocol would be `≈ q_H/|C| =
  q_H·2^-8`, which is **vacuous** for a realistic `q_H` (a grinding adversary rehashes the first
  message `~2^8` times to hit a favorable challenge). If forgery rested on that bound, QTC would be
  broken.

The theorem of this tier is that **forgery does not rest on that bound**: a lucky challenge cannot
manufacture the *second short preimage* that a false statement requires. We prove the forgery advantage
is governed by `Adv_MSIS`, by exhibiting, from any single accepting off-witness transcript, a short
non-zero M-SIS solution.

## 3.1 Setup

Three independent, fixed (nothing-up-my-sleeve) commitment matrices over `R_q` (`F3 §1`, `params.h`):
the ring-key matrix `A = BuildGlobalMatrix(SMILE_GLOBAL_SEED)` (reconstructed at every consensus site,
never prover-supplied — `F3 §7.2`, CLOSED), the serial key `b_sn` (seed `0xAA`), and the BDLOP
auxiliary key `B0`. M-SIS rank 10, `d=128`, `q=2^32−959`; verifier short-norm bounds `‖z0‖<β0≈31·√(…)`,
`‖z‖<β` (`ct_proof.cpp:3382-3419`).

**M-SIS(`A`,β):** given uniform `A∈R_q^{m×n}`, find `δ≠0`, `‖δ‖≤β`, `A·δ=0`. `Adv_MSIS` is the best PPT
success probability; at these parameters core-SVP ≈ 128 bits.

A *statement* fixes the public note/account data; it is **true** if a genuine short witness (key `s` with
`pk=A·s`; conserved amounts) exists, else **false**. The FS proof is accepting if the verifier's checks
(Tier 2 relations) pass. A **forger** `F` makes `q_H` random-oracle queries and outputs an accepting
proof for a false statement with advantage `ε`.

## 3.2 Main theorem

**Theorem 6 (knowledge soundness ⇒ M-SIS).** For every PPT forger `F` against either consensus relation
(serial↔key, value-conservation) making `q_H` RO queries,
`ε ≤ q_H·Adv_MSIS(A,β) + q_H·Adv_MSIS(b_sn,β) + q_H·Adv_MSIS(B0,β) + negl`.
In particular `ε` is negligible whenever M-SIS is hard at the deployed parameters; the `2^-8` monomial
error does **not** enter multiplicatively.

*Proof.* By the two relation-specific reductions §3.3 (serial) and §3.4 (value), each turning an
accepting off-witness transcript into a short M-SIS solution for one matrix, composed with the FS/forking
bookkeeping below. Fix the relation and let `B` be the M-SIS solver built from `F`.

**Embedding.** `B` receives an M-SIS instance and plants it as the relevant fixed matrix (legitimate:
the matrices are public NUMS constants, so the planted matrix is identically distributed to the real one
— `F3 §7.2`). `B` simulates the RO lazily and runs `F`.

**Single-transcript extraction (the sharp step).** When `F` outputs an accepting proof `π` for a false
statement, `B` reads a short non-zero M-SIS solution *directly out of `π`* (§3.3/§3.4) — no rewinding.
This is what defeats grinding: the accepting transcript itself *contains* the short preimage, because the
response `z0` is **double-pinned** — it must simultaneously open the chain-reconstructed key `A·z0 =
t0+c0·pk` *and* satisfy the serial/balance relation (`reduction_z3.py` H1, H3). A challenge chosen by
grinding can satisfy the *bare* relation, but the key-opening pins `z0=y0+c0·s` to the genuine `s`, so the
forged serial/amount forces a non-zero short vector in `ker(A)` (resp. a `B0`-collision). `B` outputs it.

**Probability.** `B` succeeds whenever `F` does and the planted matrix is the one the off-witness
transcript collides under; summing over the (≤3) matrices and charging `q_H` for the RO-programming/guess
of `F`'s forgery query (Bellare–Neven forking accounting) gives `ε ≤ q_H·(Adv_MSIS(A)+Adv_MSIS(b_sn)+
Adv_MSIS(B0)) + negl`. The `negl` collects RO collision terms `O(q_H^2/2^{256})` and the rejection-
sampling statistical distance. ∎

The `q_H` factor (not `q_H/|C|`) is the only adversary-budget dependence: it is the cost of locating
`F`'s forgery among its hash queries, standard for FS, and **multiplies `Adv_MSIS`, not `2^-8`.**

## 3.3 Serial↔key off-witness ⇒ M-SIS(`A`)/M-SIS(`b_sn`)  (F3 §3; reduction_z3.py H1–H3)

Let `π` accept for a false serial `σ* ≠ σ_real = ⟨b_sn,s⟩`. The key relation (`ct_proof.cpp:3715`) gives
`A·z0 = t0 + c0·pk` with `pk = A·s` the chain-reconstructed key (Merkle + registry, not prover-chosen,
`v2_proof.cpp:2258`). The serial check (`:3503`) gives `⟨b_sn,z0⟩ = w_sn + c0·σ*` with `w_sn = ⟨b_sn,y0⟩`.

*Case (i): `z0 = y0 + c0·s` (genuine pinning).* Then (H3, machine-checked) the serial check forces
`c0·(⟨b_sn,s⟩ − σ*) = 0`; `c0` is a unit (Lemma 4), so `σ* = ⟨b_sn,s⟩ = σ_real` — contradicting `σ*≠σ_real`.

*Case (ii): `z0 ≠ y0 + c0·s`.* Then `z0` is a *second* short opening of the committed key. By H1
(machine-checked), with `pk=A·s`, `δ := z0 − (y0+c0·s)` satisfies `A·δ = A·z0 − t0 − c0·A·s = 0` (using
`t0 = A·y0`), and `δ ≠ 0`, `‖δ‖ ≤ 2β0` — a **short M-SIS solution for `A`**. (If instead the discrepancy
surfaces in the serial commitment, the same subtraction yields `δ'` with `⟨b_sn,δ'⟩≠0`, `A·δ'=0`: a
solution for `A` outside `ker(b_sn)`, equally an M-SIS break.)

Either case ⇒ contradiction or M-SIS solution. A grinding adversary controls `c0` but Case (i) shows a
lucky `c0` only re-derives the *true* serial; Case (ii) is the sole forgery path and it is M-SIS-hard.

## 3.4 Value off-witness ⇒ M-SIS(`B0`)  (F3 §4; reduction_z3.py H4 + Tier 2 §2.3)

Let `π` accept while `Σa_in > Σa_out + fee` (inflation). Two backstops, neither grindable:

1. **Γ-validity is challenge-independent** (H4, machine-checked; `ct_proof.cpp:3868`): the predicate
   `coeffs[1..3]=0 ∧ |digit|≤bound ∧ Eval(Γ)=0` references only the committed `Γ`, never `c`. So a lucky
   challenge cannot bypass it. By Tier 2 Theorem 5, an accepting proof with valid `Γ` satisfies the
   balance identity `R+Γ=0` *and* `Eval(Γ)=0`, forcing `Σa_in = Σa_out + fee` over `ℤ` (no inflation) —
   *for the committed amounts*. The fee-absorption forge (`Γ=enc(fee)`) is rejected because
   `Eval(enc(fee))=fee≠0` (Tier 2 S5.1e).
2. To escape (1), the forger must make the *committed* amounts differ from the real ones — i.e. open the
   `B0` amount-slot commitments (`f[InputAmountSlot]/f[OutputAmountSlot]`, `:3854`) to a second short
   message. By BDLOP binding (2016/997 Thm 4.2) that is a short `B0`-collision, i.e. **M-SIS(`B0`)**.
   (The base-4 range aggregation uses high-entropy `ρ`-challenges `≈2^4096`, `:3769`, so the range part
   adds no `2^-8`-grindable surface.)

Hence inflation ⇒ defeat a challenge-free structural check (impossible) or M-SIS(`B0`). ∎

## 3.5 Honest scope (the one assessed-not-mechanized link)

- **[MC]** Lemma 4 (invertibility, 32 640 pairs), the relation factorizations (S4.1/S5.2), the carry
  bridge (S5.1a–e), and the extractor/collision identities (H1–H4) are machine-checked.
- **[PR]** The forking/advantage bookkeeping (§3.2) and the case analysis (§3.3/§3.4) are paper-rigorous,
  following standard FS-special-soundness templates (Attema–Fehr–Klooß 2021/1377).
- **The single residual** (`F3 §7.1`): that *no* single off-witness monomial collapses a non-trivial
  framework term `t−t'` which is **not itself** an M-SIS solution. §3.3/§3.4 argue this via the double-
  pinning; it is the one step we mark *assessed*, not machine-proved end-to-end, and is exactly what an
  external EasyCrypt mechanization of the extractor would ratify. The remaining hand-proved obligations
  for such a pass: (a) the forking lemma instance for the QTC transcript hashing (`fs_seed`/`seed_c0`/
  `seed_c`, `:3285-3363`); (b) the rejection-sampling simulation soundness at `σ_key=31/σ_mask=55`; (c)
  the norm bookkeeping `‖z−z'‖≤2β < ` the M-SIS threshold (`F3 §7.4`).
- **Standing assumption:** M-SIS hardness at `(rank 10, d=128, q=2^32−959, β)` — reduced *to*, not proven.

## 3.6 Conclusion

The consensus-critical soundness of the QTC shielded pool — no decoupled-serial double-spend, no
shielded inflation — reduces to Module-SIS at ≈128-bit core-SVP, with the per-challenge `2^-8` monomial
error contained by the `z0` double-pinning and the challenge-independent Γ-validity check. Combined with
Tier 1 (the turnstile firewall bounds loss to the live pool and net new supply to 0, *regardless* of the
inner proof) and Tier 2 (the relations bind the committed witness), QTC has a layered, largely machine-
checked soundness argument: an accounting firewall that holds unconditionally, verifier relations proven
binding, and a forgery hardness equal to breaking M-SIS.
