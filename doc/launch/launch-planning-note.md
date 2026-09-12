# QTC launch planning note — 2026-09-11/12

Baseline: v0.0.7 (`8191d937`) · Status: RECORD of decisions and documents produced; nothing provisioned

## 1. What was done

1. **Difficulty floor sized and applied (v0.0.7).** `powLimit` 0x1e013333 → 0x1e033333 from the measured A6000
   (8,800 digests/s); mainnet genesis regenerated at the unchanged timestamp (`9ba00506…b8e2`); full suite 3,292 cases;
   published and tagged. Detail: `QTC_powLimit_Sizing_2026-09-10`, `QTC_Release_v0.0.7_Notes_2026-09-11`.
2. **RTX 4000 Ada measurement attempted, not achieved.** Thunder has no Ada; RunPod community pod never became
   SSH-ready (deleted, cents); DigitalOcean CLI token expired (`doctl auth init` required). The floor holds without it;
   re-check rule recorded in the sizing note.
3. **Launch build specification written** (`QTC_Launch_Build_Spec_2026-09-11`): phases, gates, configuration reference,
   go/no-go checklist, timeline, cost, risks. Revised the same day from a ≈ 6-week to a **4-week baseline** (T0 = day 28).
4. **Review draft produced** (`QTC_Launch_Plan_Review_Draft_2026-09-12`): a 20-minute read with the seven decisions the
   programme needs, sign-off page, PDF delivered; copies at the top level of the QTC folder and in `software/`.
5. **Seed DNS design settled on two domains.** Explained the options (one domain with a delegated subzone vs two
   domains); chosen: **Option 2** — two domains at two registrars, domain 1 authoritative at Cloudflare, domain 2 at
   DigitalOcean DNS, `seed.<domain1>` and `seed.<domain2>` both compiled into `vSeeds`, registrar lock + auto-renew +
   expiry alerts, because a lapsed domain hands a compiled-in seed name to a stranger. Both launch documents updated.
6. **Explained for the record**: how difficulty is controlled after launch (ASERT from genesis, hard floor, timestamp
   guards; only hashrate and height-activated upgrades move it), how miners are added and removed (permissionless;
   installer, supervisor, clean stop, wallet backup), what DNS seeds are and Bitcoin's examples.

## 2. Decisions taken

| Decision | Choice |
|---|---|
| Programme length | 4 weeks to T0; 14-day burn-in not shortened; functional suites (M-9) and fuzz targets (N-11) deferred to the first point release |
| Seed DNS | Two separate domains, two registrars, Cloudflare + DigitalOcean DNS (Option 2) |
| Floor | 0x1e033333 applied; re-check only if A6000 + Ada ≥ ≈ 20,000 digests/s |

## 3. Decisions still open (listed in the review draft)

Auto-update off or on at launch (recommended off); provider layout (recommended two providers); accept burn-in
restart on consensus findings; launch window and announcement channel; sign-off by the reviewer.

## 4. Blockers on the operator's side

- `doctl auth init` with a new DigitalOcean token (needed for the Ada measurement, Node B, Node C, domain 2).
- Cloudflare "Edit zone DNS" API token in the shell (domain 1 records).
- Two seed domain registrations.

## 5. Documents produced (iCloud QTC)

| Location | Document | Formats |
|---|---|---|
| `software/` | QTC_powLimit_Sizing_2026-09-10 (+ `powlimit_sizing.py`) | md, docx, pdf |
| `software/` | QTC_Release_v0.0.7_Notes_2026-09-11 | md, docx, pdf |
| `software/` and top level | QTC_Launch_Build_Spec_2026-09-11 (4-week baseline, two-domain seeds) | md, docx, pdf |
| `software/` and top level | QTC_Launch_Plan_Review_Draft_2026-09-12 | md, docx, pdf |
| `software/` | QTC_Repo_Publish_Log_2026-09-09 (v0.0.7 rows) | md, docx, pdf |
| `software/` | this note | md, docx, pdf |
