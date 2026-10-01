# QTC launch documents

Planning documents for the QTC mainnet launch, kept in-tree so the release that launches the chain carries its own
plan. The iCloud copies (`QTC/`, `QTC/software/`) are the working versions; these are snapshots at the tagged release.

| File | Content |
|---|---|
| `launch-build-spec.md` | End-to-end build plan: 4-week baseline (T0 = day 28), phases and gates, configuration reference, go/no-go checklist, timeline, cost, risks |
| `launch-plan-review-draft.md` | Reviewer version: executive summary, the seven decisions requested, burn-in exit criteria, launch-day timeline, sign-off page |
| `launch-planning-note.md` | Record of the decisions taken on 2026-09-11/12 (4-week baseline, two seed domains, floor) and the blockers |
| `powlimit-sizing.md` | How the mainnet `powLimit` was sized from the measured A6000; model script in `contrib/qtc-launch/powlimit_sizing.py` |

Related in-tree trackers: `QTC-LAUNCH-SAFETY.md`, `QTC-SECURITY-REVIEW.md`, `doc/history/qtc-fork-tracker.md`.
