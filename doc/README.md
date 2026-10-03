# QTC documentation

Start with the top-level [README](/README.md). This index lists the documents that describe QTC as launched; inherited
Bitcoin Core documents that still apply are listed at the end.

## Running QTC

- [Public node bootstrap](qtc-public-node-bootstrap.md) — archival and fast-start nodes from this repository
- [Download and go](qtc-download-and-go.md) — from a signed release archive to a working node and wallet
- [Fast start (assumeutxo)](../contrib/faststart/README.md), [assumeutxo.md](assumeutxo.md)
- [Configuration reference](qtc-conf.md), [init.md](init.md), [files.md](files.md), [reduce-memory.md](reduce-memory.md), [reduce-traffic.md](reduce-traffic.md)
- [Tor](tor.md), [I2P](i2p.md), [CJDNS](cjdns.md), [P2P bad ports](p2p-bad-ports.md), [DNS seed policy](dnsseed-policy.md)
- [JSON-RPC interface](JSON-RPC-interface.md), [REST interface](REST-interface.md), [ZMQ](zmq.md)

## Mining

- [Mining tools](../contrib/mining/README.md) — supervised mining loop, health checks, backups
- [Mining operations](qtc-mining-ops.md)
- [CUDA troubleshooting](qtc-cuda-mining-troubleshooting.md), [CUDA multi-device](qtc-cuda-multi-device.md), [Metal tuning](qtc-metal-mining-tuning.md)
- [Linux release builds and supported GPUs](linux-release-builds.md)

## Consensus and cryptography

- [MatMul proof-of-work specification](qtc-matmul-pow-spec.md)
- [Launch safety: difficulty curve and floor](/QTC-LAUNCH-SAFETY.md), [launch documents](launch/)
- [Post-quantum cryptography specification](qtc-pqc-spec.md)
- [PQ multisig specification](qtc-pq-multisig-spec.md), [PQ multisig tutorial](qtc-pq-multisig-tutorial.md), [implementation tracker](pq-multisig-full-implementation-tracker.md)
- [BIPs implemented](bips.md)

## Wallets and keys

- [Key management guide](qtc-key-management-guide.md)
- [Managing wallets](managing-wallets.md), [offline signing](offline-signing-tutorial.md), [multisig tutorial](multisig-tutorial.md), [PSBT](psbt.md), [external signer](external-signer.md)

## Releases and security

- [Release process](release-process.md), [release notes](release-notes/), [release-signing keys](release-signing-keys.md)
- [Upgrade policy](upgrade-policy.md) — release kinds, the peer protocol floor, flag-day activation for consensus changes
- [GitHub release automation](qtc-github-release-automation.md)
- [Security documentation](security/README.md), [security review](/QTC-SECURITY-REVIEW.md), [reporting a vulnerability](/SECURITY.md)
- [Historical documents](history/)

## Building

- [Unix](build-unix.md), [macOS](build-osx.md), [Windows MSVC](build-windows-msvc.md), [Windows 11 handbook](qtc-windows-11-compile-handbook.md), [FreeBSD](build-freebsd.md), [NetBSD](build-netbsd.md), [OpenBSD](build-openbsd.md)
- [Dependencies](dependencies.md), [Developer notes](developer-notes.md), [Productivity notes](productivity.md), [Benchmarking](benchmarking.md), [Fuzzing](fuzzing.md)
- [Shared libraries](shared-libraries.md), [Design notes](design/), [Policy](policy/README.md)
- [Translation process](translation_process.md), [translation strings policy](translation_strings_policy.md), [Assets attribution](assets-attribution.md)

## License

MIT, see [COPYING](/COPYING).
