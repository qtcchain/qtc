# Security policy

QTC is a live network. Please report vulnerabilities privately so a fix can ship before the problem is public.

## Supported versions

| Version | Status |
|---|---|
| v0.1.2 and later | Supported; fixes land here |
| v0.1.0 | Supported for relays and wallets until the next release; miners should run v0.1.2 (see `doc/release-notes/release-notes-0.1.2.md`) |
| Release candidates (`v0.1.0-rc*`), test networks | Not supported |

## Reporting a vulnerability

Use one of these two channels. **Do not open a public issue or pull request for a security problem.**

1. **GitHub private vulnerability reporting** (preferred): on the repository page, *Security → Report a vulnerability*.
   The report is encrypted in transit, visible only to the maintainer, and becomes the advisory that tracks the fix.
2. **Email: `security@qtc.dev`.** Plain text is fine for the first contact; if the report contains exploit code or
   details that must not travel unencrypted, say so and we will move the exchange to GitHub private reporting.

Include what you can: the version (`qtcd -version`), the network (mainnet, testnet, regtest), steps to reproduce,
your assessment of the impact, and whether you believe the issue is being exploited.

**Never send seeds, passphrases, private keys or wallet files.** Nobody from QTC will ever ask for them.

## What to expect

- Acknowledgement within 2 business days, an initial assessment within 7 days.
- A fix timeline agreed with you. We ask for 90 days of coordinated disclosure, less when the issue is being exploited.
- Credit in the release notes if you want it. There is no bug-bounty programme at present; we say so rather than
  leave it open.

## Scope

In scope: consensus and proof of work (`src/consensus`, `src/pow*`, `src/matmul*`, `src/kernel`), the P2P layer,
wallet and key handling, RPC, the mining supervisor, the release build and signing pipeline, and the project's web
properties (qtc.events, keys.qtc.gold, qtc.dev).

Out of scope: third-party services, social engineering of maintainers or users, and denial of service against the
public test network.

Good-faith research within this scope will not be met with legal action.

## Verifying what you run

Every release ships a `SHA256SUMS` list signed by the release key, fingerprint
`72DD 01B8 E4BD 52FF BE1D 6576 1E28 9F6E CD6C BF30`, published at <https://keys.qtc.gold> and in
`doc/release-signing-keys.md`. Verification steps are in the README under "Install a release". Release signatures
are produced on an air-gapped machine; the signing key never touches a networked computer.
