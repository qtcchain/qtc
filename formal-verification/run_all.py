#!/usr/bin/env python3
# Top-level runner for the QTC shielded-pool formal-verification suite (Tiers 1-2 machine-checked
# obligations). Exits 0 iff every [MC] obligation discharges (negation UNSAT / exhaustive pass).
# Tier 3 (soundness reduction) is paper-rigorous (formal-verification/tier3/PROOFS.md) with its
# algebraic hops machine-checked here under Tier 2's relation_z3.py.
import subprocess, sys, os

HERE = os.path.dirname(os.path.abspath(__file__))
SUITE = [
    ("Tier 1  accounting invariants (S1-S3)", "tier1/accounting_z3.py"),
    ("Tier 2  monomial-diff invertibility (S4.2)", "tier2/invertibility.py"),
    ("Tier 2  relation-core identities (S4.1,S5.1,S5.2)", "tier2/relation_z3.py"),
    ("Tier 3  reduction extractor/collision algebra (S6)", "tier3/reduction_z3.py"),
]
rc = 0
for title, rel in SUITE:
    print(f"\n=== {title}  [{rel}] ===")
    r = subprocess.run([sys.executable, "-u", os.path.join(HERE, rel)])
    if r.returncode != 0:
        rc = 1
        print(f"!!! {rel} FAILED (exit {r.returncode})")
print("\n" + ("ALL FORMAL-VERIFICATION OBLIGATIONS DISCHARGED" if rc == 0
              else "FORMAL-VERIFICATION SUITE FAILED"))
sys.exit(rc)
