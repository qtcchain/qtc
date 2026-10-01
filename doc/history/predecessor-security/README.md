> **Historical record from the predecessor code base.** These documents describe the shielded-pool hardening
> programme and audits of the inherited code (activation heights 61000/125000 on the predecessor network). On QTC
> mainnet the shielded pool is closed from genesis. Kept so the audit trail of inherited code stays traceable.

# QTC Security Documentation

This directory is the stable entrypoint for QTC security hardening, audit
closeout, and forward-looking security work.

Report QTC security issues to the QTC Development Team at `team@qtc.dev`.

Use these files in the following order:

1. [current-status.md](current-status.md)
   - Current security status for the active codebase and hardening branch.
2. [hardfork-61000.md](hardfork-61000.md)
   - What the `61000` shielded hardening fork changes, and what stays
     backwards-compatible.
3. [roadmap.md](roadmap.md)
   - Remaining security-margin work and future upgrade lanes.
4. [findings/audit-20260328-closeout.md](findings/audit-20260328-closeout.md)
   - High-level closeout summary for the 2026-03-28 source audit.
5. [findings/pr134-20260401-lifecycle-followon-closeout.md](findings/pr134-20260401-lifecycle-followon-closeout.md)
   - Closeout for the later PR #134 lifecycle/operator-note follow-on review.
6. [findings/l12-pq128-parameter-upgrade.md](findings/l12-pq128-parameter-upgrade.md)
   - The remaining open PQ-128 parameter-set redesign item.
7. [../qtc-security-audit-report-2026-06-06-reassessment.md](../qtc-security-audit-report-2026-06-06-reassessment.md)
   - Reassessment of the June 2026 security report after the 125,000 shielded-sunset
     and MatMul nonce-seed hardening work.
8. [../qtc-matmul-nonce-seed-v2-125000.md](../qtc-matmul-nonce-seed-v2-125000.md)
   - Height-125,000 MatMul nonce-bound seed-v2 activation note.
9. [../qtc-c002-lattice-review-2026-06-07.md](../qtc-c002-lattice-review-2026-06-07.md)
   - Internal C-002 lattice-focused review of the post-123,000 SMILE2 verifier path.
10. [../../formal-verification/PLAN.md](../../formal-verification/PLAN.md)
   - Tiered formal verification of the shielded value-soundness stack:
     Tier 1 accounting firewall (turnstile / supply floor / velocity cap),
     Tier 2 verifier-relation binding (serial<->key, value/inflation), Tier 3
     reduction to Module-SIS. 21 machine-checked obligations plus a
     paper-rigorous `PROOFS.md` per tier; run `python3
     formal-verification/run_all.py`.

Detailed tracker:

- [../qtc-security-fixes-20260328-tracker.md](../qtc-security-fixes-20260328-tracker.md)

Directory policy:

- keep one stable summary file per active security program or unresolved item
- prefer durable filenames over `tmp-*` or `*-temp-*` trackers
- when a finding is closed, keep the closeout summary here and archive the
  implementation details into the code/tests rather than a temporary scratch
  file
