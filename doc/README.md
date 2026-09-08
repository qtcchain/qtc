QTC Node
========

Current QTC architecture/readiness source of truth
--------------------------------------------------
Use these docs first when you need the live post-`61000` hardening status,
security closeout, or future-upgrade boundary:

- [QTC Security Documentation](security/README.md)
- [QTC Post-Launch Optimization Roadmap](qtc-postlaunch-optimization-roadmap.md)
- [QTC MatMul Product-Digest Mining Fix](qtc-matmul-product-digest-mining-fix-2026-04-03.md)

Use these March 2026 docs as baseline launch references and historical
comparison points, not as the authoritative post-`61000` architecture summary:

- [QTC Shielded Production Status](qtc-shielded-production-status-2026-03-20.md)
- [QTC SMILE v2 Genesis-Reset Readiness Tracker](qtc-smile-v2-genesis-readiness-tracker-2026-03-20.md)
- [QTC SMILE v2 Transaction-Family Transition](qtc-smile-v2-transaction-family-transition-2026-03-23.md)
- [QTC SMILE v2 Future-Proofed Settlement TDD](qtc-smile-v2-future-proofed-settlement-tdd-2026-03-23.md)
- [QTC SMILE v2 Post-Launch Optimization Tracker](qtc-smile-v2-optimization-tracker-2026-03-21.md)

Operator security and custody docs
----------------------------------
Use these docs first when you need the live QTC wallet, multisig, backup, or
key-management operating model:

- [QTC Key Management Guide](qtc-key-management-guide.md)
- [Managing Wallets](managing-wallets.md)
- [QTC PQ Multisig Tutorial](qtc-pq-multisig-tutorial.md)
- [QTC Shielded Pool Guide](qtc-shielded-pool-guide.md)
- [QTC Shielded-State Startup and Recovery](qtc-shielded-state-recovery.md)
- [QTC Bridge Pending Recovery](qtc-bridge-pending-recovery.md)
- [Support for signing transactions outside of QTC](external-signer.md)

Operator bootstrap and release docs
-----------------------------------
Use these docs first when you need the live binary-install, fast-start, mining,
service-profile, or release-publication workflow:

- [QTC Download-and-Go Guide](qtc-download-and-go.md)
- [Linux Release Build Variants](linux-release-builds.md)
- [Assumeutxo Usage](assumeutxo.md)
- [QTC Mining Node Snapshot Runbook](qtc-mining-node-snapshot-runbook.md)
- [QTC GitHub Release Automation](qtc-github-release-automation.md)
- [Release Process](release-process.md)
- [QTC Metal Mining Tuning](qtc-metal-mining-tuning.md)
- [QTC CUDA Multi-Device Mining](qtc-cuda-multi-device.md)
- [Mining Operator Helpers](../contrib/mining/README.md)
- [Fast-Start Validating Node Helpers](../contrib/faststart/README.md)

The release automation docs above now cover both the one-command local release
cut (`scripts/release/cut_release.py`) and the self-hosted GitHub Actions
workflow used to stage or publish major-architecture bundles. They also cover
the native subset release path (`scripts/release/cut_local_release.py`) for
CLI-only macOS/Linux publishing when Guix artifacts are not yet available.

Historical QTC analysis notes
-----------------------------
Many QTC-specific March 2026 design/audit docs are intentionally preserved as
historical records. When a document is historical, the file now says so at the
top and points back to the current source-of-truth docs above.

Setup
---------------------
QTC Node is the reference client for the QTC blockchain (MatMul AI-native Proof-of-Work). It supports the normal validating-node workflow as well as the fast-start validating-node path that boots from a published snapshot, so operators can download a binary and become useful immediately instead of waiting for a full historical sync.

Running
---------------------
The following are some helpful notes on how to run QTC Node on your native platform.

### Unix

Unpack the files into a directory and run:

- `bin/qtc-qt` (GUI) or
- `bin/qtcd` (headless)

### Windows

Unpack the files into a directory, and then run qtc-qt.exe.

### macOS

Drag QTC Node to your applications folder, and then run QTC Node.

### Need Help?

* Start with the operator docs above if you are bootstrapping, mining, or publishing a release bundle.
* Use the fast-start guides for binary install, snapshot load, and `getchainstates` monitoring.
* Use the issue tracker or project-maintained support channels for repo-specific problems.

Building
---------------------
The following are developer notes on how to build QTC Node on your native platform. They are not complete guides, but include notes on the necessary libraries, compile flags, etc.

- [Dependencies](dependencies.md)
- [macOS Build Notes](build-osx.md)
- [Unix Build Notes](build-unix.md)
- [QTC CUDA MatMul Optimization Notes](qtc-cuda-matmul-optimization-notes-2026-04-13.md)
- [QTC CUDA Multi-Device Mining](qtc-cuda-multi-device.md)
- [Windows Build Notes](build-windows-msvc.md)
- [FreeBSD Build Notes](build-freebsd.md)
- [OpenBSD Build Notes](build-openbsd.md)
- [NetBSD Build Notes](build-netbsd.md)

Development
---------------------
This repo's [root README](/README.md) contains relevant information on the development process and automated testing.

- [Developer Notes](developer-notes.md)
- [Productivity Notes](productivity.md)
- [Release Process](release-process.md)
- [Source Code Documentation (External Link)](https://doxygen.bitcoincore.org/)
- [Translation Process](translation_process.md)
- [Translation Strings Policy](translation_strings_policy.md)
- [JSON-RPC Interface](JSON-RPC-interface.md)
- [Unauthenticated REST Interface](REST-interface.md)
- [Shared Libraries](shared-libraries.md)
- [BIPS](bips.md)
- [Dnsseed Policy](dnsseed-policy.md)
- [Benchmarking](benchmarking.md)
- [Internal Design Docs](design/)

### Resources
* Discuss project-specific development in the repo issue tracker and adjacent maintainer channels used for QTC release and operator work.

### Miscellaneous
- [QTC Shielded Production Status](qtc-shielded-production-status-2026-03-20.md)
- [QTC SMILE v2 Genesis-Reset Readiness Tracker](qtc-smile-v2-genesis-readiness-tracker-2026-03-20.md)
- [QTC SMILE v2 Future-Proofed Settlement TDD](qtc-smile-v2-future-proofed-settlement-tdd-2026-03-23.md)
- [QTC SMILE v2 Transaction-Family Transition](qtc-smile-v2-transaction-family-transition-2026-03-23.md)
- [QTC SMILE v2 Optimization Tracker](qtc-smile-v2-optimization-tracker-2026-03-21.md)
- [QTC Security Documentation](security/README.md)
- [QTC Post-Launch Optimization Roadmap](qtc-postlaunch-optimization-roadmap.md)
- [QTC SMILE v2 Account Registry Redesign (Historical)](qtc-smile-v2-shielded-account-registry-redesign-2026-03-22.md)
- [QTC Shielded Block Capacity, Bridge, and L2 Implementation Handoff (Historical)](qtc-shielded-block-capacity-analysis-2026-03-14.md)
- [QTC PQC Script Profile](qtc-pqc-spec.md)
- [QTC Key Management Guide](qtc-key-management-guide.md)
- [QTC Public Archival Bootstrap](qtc-public-node-bootstrap.md)
- [QTC PQ Multisig Specification](qtc-pq-multisig-spec.md)
- [QTC PQ Multisig Tutorial](qtc-pq-multisig-tutorial.md)
- [QTC PQ Multisig Implementation Tracker](pq-multisig-full-implementation-tracker.md)
- [Assets Attribution](assets-attribution.md)
- [qtc.conf Configuration File](qtc-conf.md)
- [CJDNS Support](cjdns.md)
- [Files](files.md)
- [Fuzz-testing](fuzzing.md)
- [I2P Support](i2p.md)
- [Init Scripts (systemd/upstart/openrc)](init.md)
- [Managing Wallets](managing-wallets.md)
- [Multisig Tutorial (Legacy Bitcoin descriptors)](multisig-tutorial.md)
- [Offline Signing Tutorial](offline-signing-tutorial.md)
- [P2P bad ports definition and list](p2p-bad-ports.md)
- [PSBT support](psbt.md)
- [Reduce Memory](reduce-memory.md)
- [Reduce Traffic](reduce-traffic.md)
- [Tor Support](tor.md)
- [Transaction Relay Policy](policy/README.md)
- [ZMQ](zmq.md)

License
---------------------
Distributed under the [MIT software license](/COPYING).
