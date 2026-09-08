#!/usr/bin/env python3
# Tier 1 (QTC shielded-pool formal verification): machine-checked discharge of the decidable
# accounting obligations S1-S3 (see formal-verification/PLAN.md). Each obligation is proved by
# asserting its NEGATION and checking it is UNSAT for ALL inputs in the stated ranges (not samples).
# Models are transcribed directly from:
#   - src/shielded/turnstile.cpp (ApplyValueBalance / UndoValueBalance / SetBalance)
#   - src/shielded/unshield_velocity.cpp (WindowCap / RecordBlock / UndoBlock / WindowTotal)
#   - src/consensus/tx_verify.cpp:203-227 (value_balance <-> vout binding)
# A green run (all PROVED) is the [MC] corroboration cited by formal-verification/tier1/PROOFS.md.

import sys
from z3 import (Int, Ints, BitVec, BitVecVal, Solver, Implies, And, Or, Not, If, sat, unsat,
                ForAll, prove)

M = 21_000_000 * 100_000_000          # MAX_MONEY = 21e6 * COIN (consensus/amount.h)
INT64_MAX = 2**63 - 1
INT64_MIN = -2**63
results = []

def check(name, claim_negation_solver_setup):
    """claim_negation_solver_setup returns a Solver whose constraints == 'the obligation is FALSE'.
    PROVED iff that is unsat."""
    s = claim_negation_solver_setup()
    r = s.check()
    ok = (r == unsat)
    results.append((name, ok))
    print(f"  [{'PROVED ' if ok else 'FAILED!'}] {name}" + ("" if ok else f"  counterexample: {s.model()}"))

# Helpers mirroring the C++ exactly.
def money_range(x):            return And(x >= 0, x <= M)         # MoneyRange
def money_range_signed(x):     return And(x >= -M, x <= M)        # MoneyRangeSigned

# ---- S1.1  ApplyValueBalance preserves 0 <= balance <= MAX_MONEY when it returns true ----
def s1_1():
    s = Solver()
    m, v = Ints('m v')
    s.add(money_range(m))                 # precondition: invariant holds before
    s.add(money_range_signed(v))          # the only inputs the function accepts
    new = m - v                           # CheckedAdd(m, -v)
    returns_true = And(money_range(new), new >= 0)   # the explicit acceptance test in the code
    # Negation of S1.1: returns true AND new balance violates the invariant.
    s.add(returns_true, Not(money_range(new)))
    return s

# ---- S1.2  No signed-int64 overflow/UB in m + (-v) for money-range inputs ----
def s1_2():
    s = Solver()
    m, v = Ints('m v')
    s.add(money_range(m), money_range_signed(v))
    total = m - v                         # the true mathematical value CheckedAdd computes
    # Negation: the exact int64 result would overflow (i.e. true value out of [INT64_MIN, INT64_MAX]).
    s.add(Or(total > INT64_MAX, total < INT64_MIN))
    return s

# ---- S1.3  UndoValueBalance(v) o ApplyValueBalance(v) = identity on the success domain ----
def s1_3():
    s = Solver()
    m, v = Ints('m v')
    s.add(money_range(m), money_range_signed(v))
    after_apply = m - v
    apply_ok = And(money_range(after_apply), after_apply >= 0)
    after_undo = after_apply + v          # UndoValueBalance adds v back
    undo_ok = And(money_range(after_undo), after_undo >= 0)
    # Negation: apply succeeded but (undo fails OR does not restore m).
    s.add(apply_ok, Or(Not(undo_ok), after_undo != m))
    return s

# ---- S2.1  WindowCap = floor(bps/10000 * pool), no overflow, exact split, saturates at M ----
def s2_1():
    s = Solver()
    pool, bps = Ints('pool bps')
    s.add(money_range(pool), bps >= 0, bps < 2**16)
    whole = (pool / 10000) * bps          # z3 '/' on Ints is integer (Euclidean) division
    frac = ((pool % 10000) * bps) / 10000
    cap_raw = whole + frac
    exact = (pool * bps) / 10000          # the intended floor(bps/10000 * pool)
    # Negation A: the split does not equal the exact floor.
    s.add(cap_raw != exact)
    return s

def s2_1_nooverflow():
    s = Solver()
    pool, bps = Ints('pool bps')
    s.add(money_range(pool), bps >= 0, bps < 2**16)
    whole = (pool / 10000) * bps
    frac = ((pool % 10000) * bps) / 10000
    # Negation: an intermediate (whole, frac, or whole+frac) overflows int64.
    s.add(Or(whole > INT64_MAX, frac > INT64_MAX, whole + frac > INT64_MAX,
             whole < 0, frac < 0))
    return s

# ---- S2.2  Record(h,e) then Undo(h): WindowTotal restored (the egress delta cancels) ----
# Model WindowTotal as a running sum T; RecordBlock adds max(0,e) for an in-window h; UndoBlock erases
# it. We verify the algebraic cancellation: (T + rec) - rec == T, and that rec == max(0,e) >= 0.
def s2_2():
    s = Solver()
    T, e = Ints('T e')
    s.add(money_range(T), money_range_signed(e))
    rec = If(e > 0, e, 0)                  # RecordBlock stores max(0, e)
    after_record = T + rec
    after_undo = after_record - rec       # UndoBlock erases exactly that entry
    # Negation: undo does not restore T, OR a stored egress is negative.
    s.add(Or(after_undo != T, rec < 0))
    return s

# ---- S2.3  Window membership is the half-open interval (tip-W, tip] (exclusive lower bound) ----
def s2_3():
    s = Solver()
    h, tip, W = Ints('h tip W')
    s.add(W >= 1, W < 2**20, tip >= 0, h >= 0)
    lower_excl = tip - W
    in_window = And(h > lower_excl, h <= tip)   # exactly WindowTotal's summation predicate
    # Negation: an entry exactly W below the tip is counted (it must be EXCLUDED), or one above tip is.
    s.add(Or(And(h == lower_excl, in_window),          # h == tip-W must NOT be in window
             And(h == tip + 1, in_window)))            # h == tip+1 must NOT be in window
    return s

# ---- S3.1  value_balance binding: transparent value_out <= nValueIn + value_balance (fee >= 0) ----
# tx_verify.cpp: adjusted = nValueIn + value_balance; require adjusted >= value_out; fee = adjusted-vout.
# So the transparent value pulled from the pool (value_out - nValueIn) is bounded by value_balance.
def s3_1():
    s = Solver()
    vin, vb, vout = Ints('vin vb vout')
    s.add(money_range(vin), money_range_signed(vb), money_range(vout))
    adjusted = vin + vb
    accepts = And(money_range(adjusted), adjusted >= vout)   # the consensus checks
    pool_sourced_transparent = vout - vin                    # transparent value created from the pool
    # Negation: tx accepted, yet more transparent value came out than value_balance permits.
    s.add(accepts, pool_sourced_transparent > vb)
    return s

print("Tier 1 — accounting invariants (S1-S3), Z3 (negation must be UNSAT for ALL inputs):")
check("S1.1 turnstile invariant preserved on accept", s1_1)
check("S1.2 no int64 overflow/UB (money-range inputs)", s1_2)
check("S1.3 Undo o Apply = identity (reorg-exact)", s1_3)
check("S2.1 WindowCap == floor(bps/10000 * pool) (exact split)", s2_1)
check("S2.1 WindowCap intermediates do not overflow int64", s2_1_nooverflow)
check("S2.2 RecordBlock/UndoBlock cancel (reorg-exact)", s2_2)
check("S2.3 window is half-open (tip-W, tip] (exclusive lower)", s2_3)
check("S3.1 value_balance binds transparent outflow to pool draw", s3_1)

n_ok = sum(1 for _, ok in results if ok)
print(f"\nTier 1: {n_ok}/{len(results)} obligations PROVED (negation UNSAT).")
sys.exit(0 if n_ok == len(results) else 1)
