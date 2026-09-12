# QTC Launch Build Specification

Date: 2026-09-11, revised 2026-09-12 to a **4-week baseline** · Software baseline: `v0.0.7` (github.com/qtcchain/qtc `8191d937`, private) · Status: SPECIFICATION, nothing provisioned

This document is the end-to-end build plan from the current tree to a live mainnet: what must be finished in the
software, what infrastructure is built and in what order, how the testnet burn-in is run and judged, the launch-day
procedure minute by minute, and what follows in the first weeks. It consolidates the network design
(`QTC/network/QTC_Minimum_Viable_Launch_Network`), the deployment spec and cost (`QTC/network/QTC_Launch_Network_Deployment_Spec_and_Cost`),
the security review (`QTC-SECURITY-REVIEW.md`) and the launch-safety tracker (`QTC-LAUNCH-SAFETY.md`) into one sequence
with gates.

## 1. Where the software stands

| Area | State at v0.0.7 |
|---|---|
| Consensus | MatMul PoW v3 (n = 512, b = 16, r = 8), oracle v2, product digest v4 (O5); PQ sigop accounting; legacy payload vectors rejected; mainnet shielded surface disabled; ASERT from genesis (τ 172,800 s, 600 s spacing), drift 43,200 s, BIP94 and timewarp reconcile from genesis; `powLimit 0x1e033333` sized from the measured A6000 |
| Genesis | mainnet `9ba00506…b8e2` at nTime 1789063200 (2026-09-10 18:00 UTC), merkle `68668615…`; **regenerated again at launch** |
| Mining backends | CPU, Metal (Apple Silicon), CUDA (sm_86 measured; sm_89 builds untested on hardware); TRANSCRIPT scheme CPU-only |
| Fees | Bitcoin model, WSF 1, sigop pricing 20 B/unit (ML-DSA 50, SLH-DSA 1,000), floors 1 atom/vB, P2MR dust 11,583 atoms, sigop-aware wallet quotes |
| Security review | v2: all non-consensus and consensus findings fixed (v0.0.4, v0.0.5); H2 closed (v0.0.7); H1, H3, M-3 launch-gated; M-9, M-11, N-11 engineering |
| Release tooling | `qtc-release-assets.yml` builds linux-x86_64, linux-x86_64-cuda12, linux-x86_64-cuda13, linux-arm64, windows-x86_64, macos-x86_64, macos-arm64; guix attestation; signed SHA256SUMS; `contrib/verify-binaries`; `contrib/faststart` installer with `--preset miner`; auto-update channel present but with no key, URL or origin compiled in (off) |
| Repository | private; `contrib/devtools/github-protect-main.sh` ready for the go-public flip |

## 2. Target architecture (launch core)

| Host | Provider | Size | Role | Ports |
|---|---|---|---|---|
| Node A | Thunder Compute | RTX A6000 48 GB, 8 vCPU, 64 GB RAM, 250 GB + weekly snapshot | GPU miner (CUDA), full archive, public P2P | 19755/tcp public; RPC and SSH over VPN only |
| Node B | DigitalOcean GPU droplet, NYC2 or TOR1 | RTX 4000 Ada 20 GB, 8 vCPU, 32 GB RAM, 500 GB NVMe | GPU miner (CUDA sm_89), full archive, public P2P | same |
| Node C | DigitalOcean Basic, AMS3 or SFO3 | 4 vCPU, 8 GB RAM, 160 GB SSD, reserved IP, weekly backups | Seed relay (no mining), DNS-seed health script, Prometheus + Grafana, WireGuard hub | 19755/tcp public; 51820/udp VPN |
| DNS seeds | Two domains: domain 1 authoritative at Cloudflare, domain 2 authoritative at DigitalOcean DNS | `seed.<domain1>` and `seed.<domain2>`, each with A records → A, B, C, TTL 300 s | Peer discovery for every new node; no shared provider or zone between the two names | |
| Wallet | Offline machine | Descriptor wallet, P2MR (ML-DSA-44) addresses; coinbase payouts land here | never on a public host |
| Explorer | optional, day 2 | DO Basic 4 vCPU / 8 GB + 100 GB volume; read-only node + web front end | | |

Mainnet P2P port 19755, testnet 29755. Recurring cost ≈ $950/month (≈ $1,010 with explorer); Option B (both miners on
Thunder, ≈ $821/month) is acceptable for the testnet burn-in only. Full sizing rationale in the deployment spec.

## 3. The 4-week schedule

T0 is day 28. The 14-day testnet burn-in is kept whole; the time comes out of the software freeze (14 → 7 days, by
deferring the two engineering items that do not change the binary's behaviour on the network) and by building the
infrastructure in the same week instead of after it. Day numbers are working-calendar days from the start decision.

| Days | Track | Work | Gate at the end |
|---|---|---|---|
| 1–2 | Accounts | Phase 0 complete: domain, DO token, Cloudflare token, Thunder key, release-key ceremony, offline wallet | all credentials and addresses in hand |
| 1–3 | Software | Ada measured on a DO droplet (day 1–2); floor re-check; M-11 stub; help-text fixes | consensus parameters frozen (day 3) |
| 2–5 | Infra | Node C (day 2) → DNS seed names → Node A (day 3) → Node B (day 4) → monitoring and alerts (day 5) | three hosts up on testnet, alerts tested |
| 5–7 | Software | seeds, fixed seeds and mesh compiled from the real IPs; auto-update decision; genesis-regeneration dry run; `v0.1.0-rc1` built for all platforms, attested, signed, verified | rc1 installed on A, B, C (day 7) |
| 8–21 | Burn-in | Phase 3 table on rc1; third-party miner invited day 10; stall test day 14–15; partition test day 17; release-path test with rc2 day 19 | all rows green by day 21 |
| 22–24 | Launch prep | freeze; fleet-rate check; announce T0, seed names and fingerprints; launch runbook rehearsal on testnet (regenerate testnet genesis at a fake T0 end to end) | rehearsal < 90 min, no manual fixes |
| 25–27 | Slack | reserved for burn-in findings; if unused, start Phase 5 items (checkpoint code, seeder) | |
| 28 | Launch | Phase 4 timeline T−6 h → T+2 h | chain live |

What was cut to reach 4 weeks and the risk accepted:

| Deferred | To | Risk accepted |
|---|---|---|
| Functional-suite port (M-9) | first point release | regressions in RPC/wallet behaviour are caught only by unit tests, the burn-in table and manual checks for the first weeks |
| MatMul fuzz targets (N-11) | first point release | parser/kernel edge cases rely on the existing unit vectors; the PoW code has been through two reviews and a literature comparison |
| Explorer | day 2 | none for the network; operators watch via RPC and Grafana |
| Rc iteration slack (was one week) | 3 days (25–27) | a burn-in finding that changes consensus, P2P or mining resets the 14-day clock and pushes T0 by up to two weeks; non-consensus fixes ship as rc2 without a reset |

Rules that keep the compression safe: consensus parameters freeze on day 3 and nothing consensus-bearing changes
after rc1 without restarting the burn-in; the burn-in is not shortened; the launch is rehearsed on testnet before
it is run on mainnet.

## 3a. Build phases, gates and durations (detail)

### Phase 0 — Accounts, keys and names (days 1–2, no hosting cost)

| # | Task | Output | Gate |
|---|---|---|---|
| 0.1 | Register **two** seed domains (e.g. a .org and a .net you will keep) at two different registrars, each with 2FA, auto-renew on, registrar lock on, and a 5-year term where offered. Set calendar reminders 90 days before each expiry. Domain 1 NS → Cloudflare; domain 2 NS → DigitalOcean (`ns1/2/3.digitalocean.com`) | two domains, each delegated to its own DNS provider | `dig NS` on each returns the intended provider |
| 0.2 | DigitalOcean: new API token, `doctl auth init` (the stored token is expired); enable GPU droplets in the account | `doctl account get` succeeds | |
| 0.3 | Cloudflare: add domain 1 as a zone; create an "Edit zone DNS" API token scoped to it. DigitalOcean: add domain 2 with `doctl compute domain create` | `CLOUDFLARE_API_TOKEN` in the operator's shell; domain 2 visible in `doctl compute domain list` | both zones active |
| 0.4 | Thunder Compute: SSH key registered; confirm A6000 availability | `tnr` key + `get_availability` | |
| 0.5 | **Release-signing key ceremony**: generate the ML-DSA-44 release key offline (`qtc-util`), record the fingerprint, store the seed in two physical locations; decide the GPG key for SHA256SUMS | public key hex + GPG fingerprint | key never touches a public host |
| 0.6 | **Offline wallet**: create the coinbase/treasury descriptor wallet on an air-gapped machine, export descriptors and a P2MR receive address for each miner | addresses A and B; encrypted backup in two locations | test restore from descriptors |
| 0.7 | Choose the launch timestamp window and the announcement channel | T0 date | |

### Phase 1 — Software freeze (days 1–7)

Everything that changes the binary or consensus must land before the testnet burn-in starts; the burn-in runs the
launch binary, not a preview. Consensus parameters (floor, genesis procedure) freeze on day 3; network-facing
constants (seeds, mesh) land on days 5–7 once the hosts exist.

| # | Task | Ref | Gate |
|---|---|---|---|
| 1.1 | Measure the RTX 4000 Ada (`qtc-matmul-solve-bench --digest-batch-bench 256` on a DO Ada droplet, sm_89 build) and re-run `powlimit_sizing.py`; keep `0x1e033333` unless A6000 + Ada ≥ ≈ 20,000 digests/s | H2 re-check | number recorded in the sizing note |
| 1.2 | Compile the DNS seeds (`vSeeds`) and the three fixed seeds (`contrib/seeds/nodes_main.txt` → `chainparamsseeds.h`) for mainnet and testnet; needs the reserved IPs from Phase 2, so this lands after 2.1–2.3 | M-3 | fresh node bootstraps through the seed alone |
| 1.3 | Populate the mining-guard default mesh with A, B, C (`mining_guard.cpp` `default_mesh`) | M-3 | `getmininginfo.chain_guard` healthy on a lone node |
| 1.4 | Fix the help-text/default mismatches noted as Info items (`-miningchainguard` default text, auto-update wording) | review Info | `qtcd -help` matches behaviour |
| 1.5 | Replace the stubbed `EnforceMiningTemplateReadiness` guard or delete the stub | M-11 | unit test covering the real check |
| 1.6 | Auto-update decision: ship **off** with no key compiled in (current), or compile the release key + manifest URL + origin and ship on. Recommendation: off for the launch release; enable in the first point release once the channel has been exercised on testnet | W-2, N-10 | documented in release notes |
| 1.7 | Port the upstream functional suites to P2MR addresses and make them CI-blocking; add the MatMul fuzz targets | M-9, N-11 | **Deferred to the first point release in the 4-week baseline** (see §3); work may start in the day 25–27 slack |
| 1.8 | Launch-day genesis regeneration dry run on a branch: `genesis_regen_hooks.py` → build → boot → `genesis_bake4.py`, full suite, discard | H1 | procedure timed (target < 90 min end to end) |
| 1.9 | Cut `v0.1.0-rc1` with `CLIENT_VERSION_IS_RELEASE true`; run `qtc-release-assets.yml` for all seven platforms with guix attestation; sign SHA256SUMS; verify with `contrib/verify-binaries` on a clean machine | | rc archives published to the private repo's releases |

### Phase 2 — Infrastructure build (days 2–5, order matters, in parallel with Phase 1)

| # | Step | Detail |
|---|---|---|
| 2.1 | **Node C first** | DO Basic droplet, reserved IP, Ubuntu 24.04, UFW (19755/tcp, 51820/udp; 22 from the operator IP only), unattended security upgrades, WireGuard hub, Prometheus + Grafana, `qtcd` CPU build as systemd service (`-listen=1 -maxconnections=125 -rpcbind=127.0.0.1`), DNS-seed health script (curl-based Cloudflare and `doctl compute domain records` updates; removes a record when a node misses two checks) |
| 2.2 | **DNS** | `seed.<domain1>` A → C (only C until A and B exist) in the Cloudflare zone; `seed.<domain2>` in the DigitalOcean zone, same records; TTL 300. The health script on Node C updates both zones (Cloudflare REST API and `doctl compute domain records`). Both names are compiled into `vSeeds` in step 1.2. A lapsed domain hands a compiled-in seed name to a stranger, so expiry is an alert, not a calendar note |
| 2.3 | **Node A** | Thunder A6000 x1, 8 vCPU, 250 GB; Ubuntu 22.04/24.04; CUDA toolkit; `qtcd` built `-DQTC_ENABLE_CUDA_EXPERIMENTAL=ON -DQTC_CUDA_ARCHITECTURES=86`; WireGuard client to C; systemd `qtcd` (`-listen=1 -maxconnections=64 -dbcache=2048 -addnode=<B> -addnode=<C> -rpcbind=127.0.0.1`); mining supervisor `contrib/mining/start-live-mining.sh` with `QTC_MATMUL_BACKEND=cuda`, payout to wallet address A; weekly snapshot |
| 2.4 | **Node B** | DO GPU droplet RTX 4000 Ada; same as A with `-DQTC_CUDA_ARCHITECTURES=89`; payout address B |
| 2.5 | Add A and B to both DNS seed names; enable the health script |
| 2.6 | Hand the reserved IPs and seed names to Phase 1.2/1.3; rebuild |
| 2.7 | Monitoring: node exporter on all three; `qtcd` RPC scrape (height, peers, mempool, difficulty, `chain_guard`, `backend_runtime`); alerts: height stalled > 30 min, peers < 3, backend fallback > 0, disk > 80 %, host unreachable |

All three hosts: coinbase to the offline wallet only; no wallet files on public hosts; RPC never exposed beyond the
VPN; `qtc-cli stop` and wait before any restart or resize (abrupt kills corrupt chainstate).

### Phase 3 — Testnet burn-in (days 8–21, 14 days, not shortened; ≈ $460 at Option A prices)

Run the exact launch topology on **testnet** (port 29755) with the `v0.1.0-rc` binary.

| Test | Method | Pass criterion |
|---|---|---|
| Bootstrap | Fresh node on a laptop with an empty datadir and no `addnode` | connects and syncs through the DNS seed within 5 minutes |
| Steady state | 7 days unattended | block interval median within 20 % of 600 s; zero `MATMUL WARNING`/fallback lines; zero unexpected restarts |
| Third-party miner | An external operator joins with the published installer (`qtc-agent-setup.py --preset miner`), Metal or CUDA | mines ≥ 1 block; no operator assistance beyond the docs |
| Stall | Stop A and B for 24 h, restart | ASERT eases to the floor and recovers as modelled in §7 of the launch-safety tracker; no split |
| Reorg / hysteresis | Partition A from B for 30 minutes, reconnect | single chain within 3 blocks; `fork_health` clean |
| Wallet | ML-DSA and SLH-DSA payments from the offline wallet via PSBT; dust refusal | fees match quotes; SLH spend 20,000 vB |
| Fees under load | 1,000 transactions in 10 minutes | mempool and block assembly behave; no template failures |
| Release path | Publish rc2 through the release workflow; upgrade all nodes with `verify-binaries` | checksums verify; if auto-update is on, the channel delivers rc2 |
| Backup/restore | Restore Node A from snapshot into a new instance | rejoins and syncs |
| Monitoring | Trigger each alert artificially | every alert fires once |

Exit gate: every row passes on the same rc; any consensus-relevant fix restarts the 14-day clock.

### Phase 4 — Launch (day 28; T−48 h to T+2 h)

| Time | Step |
|---|---|
| T−48 h | Code freeze. Confirm the fleet rate (A6000 + Ada) against the floor: the two-card interval at the floor must be ≤ 600 s and the single-A6000 interval ≤ 900 s |
| T−24 h | Announce T0 and the seed names; publish the release-key fingerprint and the GPG fingerprint on two channels |
| T−6 h | **Regenerate mainnet genesis** with nTime = T0 (rounded to the hour): `genesis_regen_hooks.py` → build → boot main → `genesis_bake4.py`; update pinned tests; full unit suite (≈ 50 min); tag `v0.1.0`; run `qtc-release-assets.yml` (all platforms, attested, signed); verify on a clean machine |
| T−3 h | Install `v0.1.0` on C, then A, then B (`qtc-cli stop` first; nodes hold no chain yet). Do **not** start mining |
| T−1 h | Flip the GitHub repository to public; immediately run `contrib/devtools/github-protect-main.sh`; verify the two rulesets; publish the release with SHA256SUMS and signature |
| T−30 min | Start `qtcd` on C, A, B; confirm all three see each other and report height 0 with the new genesis hash |
| T0 | Start the mining supervisor on A, then B. First blocks arrive at ≈ 400 s (both cards at the floor) |
| T+30 min | Confirm: ≥ 3 blocks, both miners have found blocks, `getmininginfo.chain_guard` healthy, coinbase outputs visible to the offline wallet's watch-only descriptor |
| T+2 h | Fresh external node bootstraps through the seed; announce open mining with the installer link |

Abort criteria before T0: any node fails to reach the others, genesis hash mismatch between hosts, release verification
failure. After T0 the chain is real; fixes go out as height-activated upgrades, never as a reset.

### Phase 5 — First weeks

| When | Item |
|---|---|
| T+3–7 days | `v0.1.1`: `nMinimumChainWork` and a checkpoint at a height a few thousand blocks back (closes H3); M-11 guard if slipped; functional suites in CI |
| T+7 days | Seeder: replace the static health script with a crawler that serves any healthy public node, and add a second operator's seed hostname in the next release |
| T+14 days | Publish the first assumeutxo snapshot and `snapshot.manifest.json` so new miners install with `--preset miner` in minutes |
| T+14 days | Auto-update: exercise the channel on testnet with the launch key, then enable on mainnet in a release |
| Ongoing | ASERT settles within ≈ 8 days of any fleet change; watch the launch overshoot (≈ 163 blocks ahead of schedule expected); explorer; incident runbook (peer isolation, stale tip, backend fallback, disk) |

## 4. Configuration reference

`qtc.conf` (miner, Node A; Node B identical with its own peers and payout address):
```
server=1
listen=1
maxconnections=64
dbcache=2048
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
addnode=<NodeB-ip>:19755
addnode=<NodeC-ip>:19755
externalip=<own-public-ip>
miningchainguard=1
blockmintxfee=0.00001
```
Environment for the mining supervisor: `QTC_MATMUL_BACKEND=cuda`, `QTC_MATMUL_REQUIRE_BACKEND=cuda`. Verify with
`qtc-matmul-backend-info --backend cuda` (`available=true`, `tiled_product_digest_v4=true`).

Relay (Node C): as above with `maxconnections=125`, no mining supervisor, plus the exporter and the seed health script.

Systemd: `Type=forking`-free simple unit running `qtcd -conf=/etc/qtc/qtc.conf -datadir=/var/lib/qtc`, `Restart=on-failure`,
`TimeoutStopSec=600` (clean shutdown can take minutes), `ExecStop=qtc-cli stop`.

Firewall: 19755/tcp from anywhere; 22/tcp from the operator IP and the VPN; RPC (19334) and exporters only on the VPN
interface.

## 5. Go / no-go checklist (launch morning)

- [ ] Ada measured or explicitly waived; floor confirmed against the two-card and single-card intervals
- [ ] Both seed domains registered, locked, auto-renewing; both seed names and the fixed seeds compiled in; fresh-node bootstrap tested on testnet with the rc
- [ ] Mining-guard mesh populated; help text matches behaviour
- [ ] Burn-in table fully green on the launch rc; no consensus change since
- [ ] Release key fingerprint and GPG fingerprint published; SHA256SUMS signed; `verify-binaries` clean on a machine that never built the code
- [ ] Offline wallet restored once from descriptors; payout addresses match the supervisor configs
- [ ] Genesis regenerated at T0, hash identical on all three hosts and in the release tag
- [ ] Repository public and protected (two rulesets present)
- [ ] Snapshots (Thunder) and backups (DO) enabled; monitoring alerts tested
- [ ] Incident contacts and rollback-free upgrade procedure (flag height) written down

## 6. Timeline and cost

| Phase | Days | Cost |
|---|---|---|
| 0 Accounts, keys, names | 1–2 | ≈ $30–40 (two domains) |
| 1 Software freeze | 1–7 (parallel with 2) | Ada measurement ≈ $5 |
| 2 Infrastructure | 2–5 | hourly from first boot (≈ $32/day for the three hosts) |
| 3 Testnet burn-in | 8–21 | ≈ $460 (hosts run from day 2, so ≈ $830 for days 2–27) |
| Launch prep and slack | 22–27 | included above |
| 4 Launch | 28 | — |
| **Total to T0** | **4 weeks** | **≈ $870** |
| 5 First month of mainnet | T0 + 30 days | ≈ $950 (≈ $1,010 with explorer) |

## 7. Risks and mitigations

| Risk | Mitigation |
|---|---|
| A launch card under-delivers (container overhead, Ada slower than assumed) | Floor sized so one A6000 alone holds 600 s; single-card interval ≤ 900 s is the abort line |
| Thunder instance loss (seen before, no snapshot) | Weekly snapshots; miners on two providers; Node C never on Thunder |
| Header-only work is free at the floor until H3 is closed | `v0.1.1` with minimum chain work and checkpoint within the first week; monitor for header floods |
| Seed provider outage | Two domains on two DNS providers at two registrars, fixed seeds compiled in, `addnode` in the published config |
| Seed domain expiry or registrar account takeover | Two registrars with 2FA and registrar lock, auto-renew, long terms, expiry alerts; a lapsed name is removed from `vSeeds` in the next release and the fixed seeds carry bootstrap meanwhile |
| Release key compromise | Offline ceremony, two physical copies, auto-update off at launch, fingerprint published out of band |
| Consensus bug found after T0 | Height-activated upgrade discipline (no resets); hysteresis depth 1 limits reorg exposure; testnet mirrors mainnet for rehearsal |
| 4-week compression: a burn-in finding that touches consensus, P2P or mining | Burn-in restarts (14 days); T0 slips by up to two weeks rather than launching on an untested rc. Non-consensus fixes ship as rc2 without a reset |
| Deferred functional suites and fuzzing | First point release; manual RPC/wallet checks are part of the burn-in table until then |
| Chain state corruption from hard restarts | `qtc-cli stop` + wait in every runbook and in the systemd `ExecStop`; `TimeoutStopSec=600` |

## 8. Document index

- `QTC/network/`: minimum viable launch network (+ diagram), deployment spec and cost, security reviews, H4 model, literature comparison.
- `QTC/software/`: release notes v0.0.5–v0.0.7, GPU kernel port, powLimit sizing (+ `powlimit_sizing.py`), consensus fork and fix model, repository publish log (go-public procedure), genesis scripts (`genesis_regen_hooks.py`, `genesis_bake4.py`), this build spec.
- `QTC/fees/`: fee structure comparison, dust and sigop quote fix.
- In-tree: `QTC-LAUNCH-SAFETY.md` (§7 stall simulation, §13 floor), `QTC-SECURITY-REVIEW.md`, `QTC-FORK.md`, `contrib/mining/README.md`, `contrib/faststart/README.md`, `contrib/autoupdate/BOOTSTRAP.md`, `contrib/devtools/github-protect-main.sh`.
