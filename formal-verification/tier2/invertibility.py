#!/usr/bin/env python3
# Tier 2, obligation S4.2 [MC]: every difference of two DISTINCT monomial Fiat-Shamir challenges
# c = ±X^k (256 of them) is a UNIT in R_q = F_q[X]/(X^128+1). This is the load-bearing fact for the
# special-soundness extractor (two accepting transcripts on c != c' yield a genuine short witness /
# short M-SIS collision; see formal-verification/tier2/PROOFS.md S4.1 and F3_MSIS_SOUNDNESS_REDUCTION.md).
#
# Faithful to the deployed ring: q and the 32 quartic slot roots are taken verbatim from
# src/shielded/smile2/params.h (Q, SLOT_ROOTS). X^128+1 = prod_{i} (X^4 - r_i) with each factor
# irreducible (params.h:22), so by CRT an element is a unit in R_q iff it is a unit in every slot
# ring F_q[X]/(X^4 - r_i); since each slot ring is a field, that is iff its slot image is NONZERO.
# We compute each monomial difference's slot image directly (X^k = r^(k//4) * X^(k%4) mod (X^4-r)).

import sys

Q = 4294966337  # = 2^32 - 959  (params.h:24)
SLOT_ROOTS = [   # the 32 roots r_i = zeta^(2i+1), params.h SLOT_ROOTS[0..31]
 3463736836,1624289040,1783970969,3137426997,3896370500,3624752522,3464556344,1669300415,
 1639633990,2533428898,2579408587,3875052898,2028732914,1655305793,655737010,3259370049,
 831229501,2670677297,2510995368,1157539340,398595837,670213815,830409993,2625665922,
 2655332347,1761537439,1715557750,419913439,2266233423,2639660544,3639229327,1035596288]

assert len(SLOT_ROOTS) == 32

def is_probable_prime(n):
    # Miller-Rabin, deterministic for n < 3.3e24 with these bases -> q (~4.3e9) is certified.
    if n < 2: return False
    for p in (2,3,5,7,11,13,17,19,23,29,31,37):
        if n % p == 0: return n == p
    d, r = n-1, 0
    while d % 2 == 0: d //= 2; r += 1
    for a in (2,3,5,7,11,13,17,19,23,29,31,37):
        x = pow(a, d, n)
        if x in (1, n-1): continue
        for _ in range(r-1):
            x = x*x % n
            if x == n-1: break
        else:
            return False
    return True

# Precompute, per slot, the powers r^(k//4) mod q for k in [0,128) (i.e. r^0..r^31).
def slot_image_of_monomial(sign, k, r):
    """±X^k mod (X^4 - r) as a length-4 coeff vector over F_q. X^4 = r => X^k = r^(k//4) X^(k%4)."""
    coeff = (sign * pow(r, k // 4, Q)) % Q
    v = [0, 0, 0, 0]
    v[k % 4] = coeff
    return v

def is_unit_in_slot(v):
    """v (deg<4) is a unit in F_q[X]/(X^4 - r) iff v != 0 (the slot ring is a field)."""
    return any(c % Q != 0 for c in v)

def main():
    if not is_probable_prime(Q):
        print("FAIL: q is not prime -> slot rings are not fields; check assumptions"); return 1

    # Build the 256 monomial challenges as (sign, k), sign in {+1, -1 == q-1}.
    challenges = [(1, k) for k in range(128)] + [(Q - 1, k) for k in range(128)]
    assert len(challenges) == 256

    n_pairs = 0
    non_invertible = []
    for i in range(len(challenges)):
        s1, k1 = challenges[i]
        for j in range(i + 1, len(challenges)):
            s2, k2 = challenges[j]
            n_pairs += 1
            # difference c_i - c_j must be a unit in every slot.
            ok = True
            for r in SLOT_ROOTS:
                a = slot_image_of_monomial(s1, k1, r)
                b = slot_image_of_monomial(s2, k2, r)
                diff = [(a[t] - b[t]) % Q for t in range(4)]
                if not is_unit_in_slot(diff):
                    ok = False
                    break
            if not ok:
                non_invertible.append((challenges[i], challenges[j]))

    print(f"q prime: True;  challenges: {len(challenges)};  distinct pairs checked: {n_pairs}")
    if non_invertible:
        print(f"  [FAILED!] {len(non_invertible)} non-invertible differences, e.g. {non_invertible[:3]}")
        return 1
    print(f"  [PROVED ] all {n_pairs} monomial-difference pairs are units in R_q (nonzero in all 32 slots)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
