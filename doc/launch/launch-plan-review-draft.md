# QTC Mainnet Launch Plan — Review Draft

Document: QTC-LAUNCH-REVIEW-1 · Date: 2026-09-12 · Baseline software: v0.0.7 (github.com/qtcchain/qtc, `8191d937`, private) · Status: FOR REVIEW · Prepared for: Gavin Whyte

Purpose: a single document a reviewer can read in twenty minutes to decide whether QTC is ready to enter a 4-week
launch programme, and to record the decisions that programme needs. The supporting analysis lives in the documents
listed in section 12; this draft summarises them and does not add new claims.

## 1. Executive summary

QTC is a new post-quantum proof-of-work chain forked from the upstream v0.33.1 codebase. Since 2026-09-07 the tree
has been renamed, purged of upstream identity, hardened through two security reviews, and published as seven tagged
development releases. The proof-of-work was strengthened after a matrix-multiplication shortcut was found and
modelled (O5), the GPU mining kernels were ported and measured on real hardware, the fee policy was adapted to
post-quantum signature sizes, and the difficulty floor was sized from a measured card. Every finding of the
security review that changes block validity is fixed. Three items remain that are launch-day work by design
(genesis timestamp, minimum chain work, seed names), and two engineering items are proposed for deferral.

The recommendation is to start the 4-week programme now. Launch on day 28 is conditional on a 14-day testnet
burn-in that is not shortened, a launch rehearsal on testnet, and a go/no-go checklist on the launch morning.
Cost to launch is about $850; the first month of mainnet about $950.

## 2. Decisions requested from the reviewer

| # | Decision | Recommendation | Consequence of the alternative |
|---|---|---|---|
| D1 | Enter the 4-week programme (T0 = day 28) | Yes | A 6-week programme keeps a full week of rc slack and lands the functional suites before launch |
| D2 | Defer the functional-suite port (M-9) and MatMul fuzz targets (N-11) to the first point release | Defer | Keeping them adds ≈ 1 week; regressions would otherwise be caught by unit tests and the burn-in only |
| D3 | Ship the launch release with auto-update **off** and no release key compiled in | Off at launch, on in the first point release after a testnet exercise | Shipping it on gives faster emergency upgrades but puts the most powerful trust path live before it has been exercised |
| D4 | Provider layout: miners on two providers (Thunder A6000 + DigitalOcean RTX 4000 Ada, ≈ $950/month) | Two providers | Both miners on Thunder saves ≈ $130/month but a Thunder instance has vanished before without a snapshot |
| D5 | Keep the difficulty floor at `0x1e033333` unless the RTX 4000 Ada measurement moves the fleet rate above ≈ 20,000 digests/s | Keep, re-check on day 3 | A fleet-sized floor leaves no margin if one miner is absent |
| D6 | Accept that a burn-in finding touching consensus, P2P or mining restarts the 14-day clock and moves T0 | Accept | Launching on an untested rc is the failure mode this programme is designed to avoid |
| D7 | Launch timestamp window and announcement channel | Owner's choice | Needed by day 22 |

## 3. What has been built (v0.0.1 → v0.0.7)

| Release | Content |
|---|---|
| v0.0.1 | Identity, genesis, rename and purge of the upstream name, D8/D9 hardening, Option B launch safety (ASERT from genesis with a hard floor), Bitcoin's monetary policy at 600 s |
| v0.0.2 | O5: oracle v2 (8 lanes per SHA-256) and product digest v4 (full-tile hashing), closing the H4 shortcut that let a miner skip the matrix product |
| v0.0.4 | All non-consensus findings of security review v2: encrypted PQ seeds, template payload reservation and eviction, finite budgets, 25 MB transport, auto-update and release-trust cleanup |
| v0.0.5 | Consensus findings: PQ sigop accounting, SLH-DSA weight, legacy payload vectors rejected, mainnet shielded surface disabled, hysteresis depth 1; genesis regenerated |
| v0.0.6 | Metal and CUDA digest kernels ported to the current consensus and measured (A6000 ≈ 8,800 digests/s, M5 ≈ 840); PQ-aware dust and sigop-aware fee quotes; repository-protection script |
| v0.0.7 | Difficulty floor sized from the measured A6000 (`0x1e033333`); mainnet genesis regenerated |

Verification at each step: full unit suite (3,292 cases at v0.0.7), regtest end-to-end mining with strict validation,
independent Python and C++ agreement on the proof-of-work vectors, GPU parity tests on real hardware.

## 4. Security posture

| Status | Items |
|---|---|
| Fixed | 11 High and 12 Medium findings of review v2 across wallet, miner, P2P, consensus and operations (v0.0.4, v0.0.5); GPU gates and fee under-quoting (v0.0.6); floor placeholder H2 (v0.0.7) |
| Launch-day by design | H1 genesis timestamp (regenerated within hours of T0); M-3 seed names and mining mesh (need the hosts' IPs) |
| First point release | H3 header forgery at floor difficulty (minimum chain work and a checkpoint a few thousand blocks in); M-11 stubbed template-readiness guard |
| Proposed deferral (D2) | M-9 functional suites, N-11 fuzz targets |
| Residual | The upstream shielded pool is compiled in but disabled on mainnet; the TRANSCRIPT PoW scheme is CPU-only; RTX 4000 Ada throughput assumed at half an A6000 until measured |

## 5. Launch architecture

Three hosts on three providers, one seed name on two DNS providers, an offline wallet.

| Host | Provider | Role |
|---|---|---|
| Node A | Thunder Compute, RTX A6000 | GPU miner, full archive, public P2P (19755) |
| Node B | DigitalOcean, RTX 4000 Ada | GPU miner, full archive, public P2P |
| Node C | DigitalOcean Basic, reserved IP | Seed relay, DNS-seed health script, monitoring, VPN hub |
| DNS seeds | Two domains at two registrars: domain 1 on Cloudflare, domain 2 on DigitalOcean DNS | `seed.<domain1>`, `seed.<domain2>` → A, B, C; TTL 300 s; no shared provider, zone or registrar |
| Wallet | Air-gapped machine | Coinbase and treasury; P2MR (ML-DSA-44) addresses; descriptors backed up in two places |

Anyone can join: the published installer and the seed names are all a third-party miner needs. Difficulty is
automatic (ASERT, 2-day half-life, hard floor); nobody controls it after launch except through a versioned,
height-activated upgrade.

## 6. The 4-week programme

| Days | Work | Gate |
|---|---|---|
| 1–2 | Two seed domains (two registrars, locked, auto-renew), DigitalOcean and Cloudflare tokens, Thunder key, offline release-key ceremony (ML-DSA-44), offline wallet | credentials, domains and payout addresses in hand |
| 1–3 | RTX 4000 Ada measured; floor re-check (D5); M-11 stub; help-text fixes | consensus parameters frozen |
| 2–5 | Node C → DNS names → Node A → Node B → monitoring and alerts | three hosts up on testnet, alerts tested |
| 5–7 | Seeds, fixed seeds, mesh compiled from the real IPs; auto-update setting (D3); genesis-regeneration dry run; `v0.1.0-rc1` built for seven platforms, attested, signed, verified on a clean machine | rc1 on all hosts |
| 8–21 | Testnet burn-in on rc1 (section 7) | every test green on the same rc |
| 22–24 | Freeze; fleet-rate check; announce T0, seed names, key fingerprints; full launch rehearsal on testnet | rehearsal under 90 minutes without manual fixes |
| 25–27 | Slack for findings; otherwise start point-release work | |
| 28 | Launch (section 8) | chain live, external node bootstraps through the seed |

## 7. Burn-in exit criteria (all on the same rc)

| Test | Pass criterion |
|---|---|
| Fresh node, empty datadir, no manual peers | syncs through the DNS seed within 5 minutes |
| 7 days unattended | median block interval within 20 % of 600 s; zero backend fallbacks; zero unexpected restarts |
| Third-party miner from the installer alone | mines at least one block without operator help |
| 24-hour stall of both miners | difficulty eases to the floor and recovers as modelled; no split |
| 30-minute partition between miners | single chain within 3 blocks |
| ML-DSA and SLH-DSA payments from the offline wallet; dust refusal | fees match quotes |
| 1,000 transactions in 10 minutes | mempool and templates healthy |
| rc2 through the release workflow, upgrade with verified binaries | checksums verify; nodes upgrade cleanly |
| Restore Node A from snapshot | rejoins and syncs |
| Every alert triggered once | every alert fires |

## 8. Launch day

| Time | Step |
|---|---|
| T−48 h | Code freeze; fleet rate confirmed: two cards ≤ 600 s per block at the floor, one A6000 ≤ 900 s |
| T−24 h | Announce T0, seed names, release-key and checksum-key fingerprints on two channels |
| T−6 h | Regenerate mainnet genesis at T0; full unit suite; tag `v0.1.0`; build, attest, sign; verify on a clean machine |
| T−3 h | Install on C, A, B (clean stop first); mining not started |
| T−1 h | Repository made public; protection rulesets applied at once; release published |
| T−30 min | Start nodes C, A, B; all report height 0 with the same genesis hash |
| T0 | Start mining on A, then B; first blocks at ≈ 400 s |
| T+30 min | ≥ 3 blocks, both miners represented, chain guard healthy, coinbase visible to the watch-only wallet |
| T+2 h | External node bootstraps through the seed; open mining announced |

Abort before T0 on: hosts cannot reach each other, genesis hash mismatch, release verification failure. After T0
there are no resets; fixes are height-activated upgrades.

## 9. First month

Point release within a week with minimum chain work and a checkpoint (closes H3) and the M-11 guard; a seed crawler
replacing the static health script; the first assumeutxo snapshot so new miners install in minutes; auto-update
exercised on testnet then enabled (D3); functional suites and fuzz targets (D2). Expect the difficulty to settle
over about eight days with roughly 163 blocks minted ahead of schedule, as modelled.

## 10. Budget

| Item | Amount |
|---|---|
| Programme to T0 (hosts from day 2, two domains, Ada measurement) | ≈ $870 |
| Mainnet, per month (two providers, snapshots, backups, DNS) | ≈ $950 (≈ $1,010 with explorer) |
| Budget alternative D4 (both miners on Thunder) | ≈ $821/month, testnet only |

## 11. Risks

| Risk | Mitigation |
|---|---|
| A launch card under-delivers | Floor holds 600 s on one A6000 alone; single-card interval above 900 s is the abort line |
| Provider instance loss | Weekly snapshots; miners on two providers; relay never on Thunder |
| Header-only work is free at the floor until H3 closes | Point release within a week; header-flood monitoring |
| Seed provider outage or domain lapse | Two domains on two providers at two registrars with lock and auto-renew; fixed seeds compiled in; `addnode` in the published config |
| Release-key compromise | Offline ceremony, two physical copies, auto-update off at launch, fingerprints published out of band |
| Consensus bug after T0 | Height-activated upgrades, hysteresis depth 1, testnet mirrors mainnet for rehearsal |
| Compression to 4 weeks | Consensus/P2P/mining findings restart the burn-in and move T0; non-consensus fixes ship as rc2 |
| Deferred suites and fuzzing | Manual RPC and wallet checks in the burn-in table until the point release |

## 12. Supporting documents (iCloud QTC)

- `software/QTC_Launch_Build_Spec_2026-09-11` — the full build specification this draft summarises (phases, configuration reference, checklist).
- `network/QTC_Minimum_Viable_Launch_Network_2026-09-09` (+ diagram) and `network/QTC_Launch_Network_Deployment_Spec_and_Cost_2026-09-09`.
- `network/QTC_Security_Review_v2_2026-09-09` (+ subsystem reports), `network/QTC_H4_Fix_Options_Model_2026-09-09`, `network/QTC_MatMul_PoW_Literature_Comparison_2026-09-09`.
- `software/QTC_Release_v0.0.5/6/7_Notes`, `software/QTC_GPU_Kernel_Port_2026-09-10`, `software/QTC_powLimit_Sizing_2026-09-10`, `software/QTC_Consensus_Fix_Model_2026-09-10`, `software/QTC_Repo_Publish_Log_2026-09-09`.
- `fees/QTC_vs_Bitcoin_Fee_Structure_2026-09-10`, `fees/QTC_Dust_and_Sigop_Quote_Fix_2026-09-10`.
- In-tree: `QTC-LAUNCH-SAFETY.md`, `QTC-SECURITY-REVIEW.md`, `QTC-FORK.md`.

## 13. Review and sign-off

| Role | Name | Decision (approve / approve with changes / reject) | Date | Notes |
|---|---|---|---|---|
| Owner | | | | |
| Technical reviewer | | | | |
| Operations | | | | |
| Security | | | | |

Reviewer comments:

_______________________________________________________________________________

_______________________________________________________________________________

_______________________________________________________________________________
