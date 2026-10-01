Contributing to QTC
===================

QTC is developed in the open at <https://github.com/qtcchain/qtc>. Anyone is welcome to review, test, report
problems and propose patches. The project is small and the chain is live, so the process is deliberately strict.

How changes land
----------------

- `main` accepts changes **only through pull requests**. Direct pushes, force pushes, branch deletion and changes to
  release tags are blocked for everyone, maintainers included, by repository rulesets.
- Every pull request needs the **maintainer's approval** (`.github/CODEOWNERS`) and passing CI before it merges.
  Review comments must be resolved; the history stays linear (rebase, no merge commits from the PR side).
- Releases are cut from `main` by annotated tag, built reproducibly with Guix, and their checksum list is signed
  offline. See `doc/release-process.md` and `doc/release-signing-keys.md`.

What to contribute
------------------

- **Bug reports**: open an issue with the version (`qtcd -version`), the network, the exact log lines and the steps
  to reproduce. For anything security-relevant, do **not** open an issue; follow `SECURITY.md`.
- **Fixes and features**: open an issue or a short design note first for anything that touches consensus
  (`src/consensus/`, `src/pow*`, `src/matmul*`, `src/kernel/chainparams.cpp`), the P2P protocol, the wallet's key
  handling or the mining guards. Small, well-tested fixes can go straight to a pull request.
- **Tests and documentation**: always welcome.

Pull request checklist
----------------------

1. One logical change per pull request, with a title in the form `area: imperative summary`
   (for example `mining: randomise the nonce start when the template time is frozen`).
2. A commit message that explains *why*, not just what. Reference the issue if there is one.
3. Tests: unit tests in `src/test/` or functional tests in `test/functional/`; a consensus change needs a test
   that fails without it. State in the PR which tests cover the change.
4. `cmake --build build && ctest --test-dir build` passes locally. If you touched `src/libbitcoinpqc/**`, run
   `cd src/libbitcoinpqc/fuzz && make fuzz-smoke` and report the result.
5. No new third-party dependencies without discussion; no network calls at build time.
6. Follow the existing style of the file you are editing (`doc/developer-notes.md` for the inherited conventions,
   `clang-format` with the repository configuration for new code).

Review
------

Reviewers test and read the code; "ACK" comments state what was tested. Be specific, be kind, and expect the
maintainer to ask for a smaller change if a pull request does too many things.

Licence
-------

By contributing you agree that your contribution is licensed under the MIT licence in `COPYING`.
