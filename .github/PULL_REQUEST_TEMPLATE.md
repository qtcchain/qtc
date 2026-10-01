<!--
Every change to `main` arrives through a pull request and is merged only after the maintainer approves it
(CODEOWNERS). Keep the PR to one logical change. Consensus-affecting changes need a design note and a test
that fails without the change.
-->

## What and why

<!-- One paragraph: the problem, the change, the user-visible effect. Link the issue or design note. -->

## Consensus / policy impact

- [ ] No consensus or policy change
- [ ] Consensus or policy change: activation and compatibility described above

## Validation

- [ ] Unit tests listed (`src/test/`) and passing
- [ ] Functional tests listed (`test/functional/`) and passing, or not applicable
- [ ] If this PR touches `src/libbitcoinpqc/**`: `cd src/libbitcoinpqc/fuzz && make fuzz-smoke` run and the result summarized
- [ ] If this PR touches mining or MatMul code: `verify_release_qtcd.py` / the MatMul regression suites run
