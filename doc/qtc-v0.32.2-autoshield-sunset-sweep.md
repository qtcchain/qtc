# QTC v0.32.2 Auto-Shield and Sunset Sweep

Date: 2026-06-08
Branch: `codex/v0322-autoshield-sweep`

## Scope

This sweep checked wallet, RPC, node startup, mining, tests, and public docs for
old v0.30/v0.31 shielded-ingress assumptions that conflict with the v0.32.2
production rule set:

- coinbase auto-shield is default-off;
- mined rewards remain transparent unless an operator explicitly opts in before
  the sunset;
- at and after the shielded-pool credit disable height (`125000` on mainnet),
  no wallet path should build new shielded credits;
- at and after the shielded sunset (`125000` on mainnet), no wallet path should
  build private shielded-output appends, control-plane rollover, re-shielding,
  or bridge ingress transactions;
- existing shielded balances remain accounted, and strict transparent
  exit/recovery behavior remains the supported post-sunset direction.

## Findings

The consensus layer already enforced the hard boundary:

- `RejectDisabledShieldedPoolCredit()` rejects negative shielded state value
  balance at/after the pool-credit disable height with
  `bad-shielded-pool-credit-disabled`.
- `RejectShieldedSunsetViolation()` permits only strict transparent exits of
  existing shielded value and recovery exits after the sunset. It rejects
  private transfers, shielded change/recipient appends, bridge control
  operations, rebalance, re-shielding, and other non-exit families.

The wallet layer still had user-facing or background paths that could try to
build transactions that consensus would reject:

- `walletpassphrase` used `-autoshieldcoinbase` default `true`, so unlocking an
  encrypted wallet could auto-shield even though block-connected auto-shield was
  default-off.
- `MaybeAutoShieldCoinbase()` respected `-autoshieldcoinbase=1`, but did not
  stop itself after shielded pool credits were disabled.
- Pending bridge shield recovery could retry shield settlement after sunset,
  producing repeated recovery/build attempts for a path consensus cannot accept.
- Explicit RPCs such as `z_shieldfunds`, `z_shieldcoinbase`, `z_fundpsbt`,
  `z_sendtoaddress`, `z_mergenotes`, bridge ingress, bridge shield settlement,
  and rebalance could still reach transaction construction and discover the
  sunset only through mempool/validation rejection.
- Public docs still described pre-sunset ingress, bridge, and auto-shield paths
  as current production behavior.

Additional source-audit findings not changed in this branch:

- `-allowunpinnedshieldedsnapshot` remains default-on until pinned snapshots
  ship. This is not an auto-shield path, but should be revisited for a
  fail-closed mainnet default once pinned snapshots are available.
- Startup can auto-reconsider failed shielded blocks during retune handling.
  This is separate from wallet shielding and should be narrowed to known
  historical reject reasons if it causes repeated startup retries.
- Mining RPCs still accept generic address input and later reject non-P2MR
  coinbase scripts under P2MR-only consensus. That is a help/error-message
  cleanup, not shielded ingress.

## Changes Made

Wallet background behavior:

- `walletpassphrase` now uses `-autoshieldcoinbase` default `false`, matching
  block-connected wallet behavior.
- `MaybeAutoShieldCoinbase()` exits permanently once
  `IsShieldedPoolCreditDisabled(build_height)` is true, avoiding construction
  and broadcast of transactions that would fail with
  `bad-shielded-pool-credit-disabled`.
- Pending bridge shield recovery marks post-sunset shield settlement as
  `manual_action_required` without constructing a PSBT or incrementing retry
  counters.

Wallet RPC behavior:

- Transparent-to-shielded RPCs fail before planning/building once pool credits
  are disabled: `z_shieldcoinbase`, `z_shieldfunds`, `z_planshieldfunds`,
  `z_fundpsbt`, `bridge_buildingressbatchtx`, `bridge_buildshieldtx`, and
  `bridge_submitshieldtx`.
- Private shielded-output/control RPCs fail before building after the sunset:
  `z_sendtoaddress`, shielded-recipient `z_sendmany`, `z_mergenotes`,
  `z_recoverstrandednote`, `z_rotateaddress`, `z_revokeaddress`,
  `bridge_buildegressbatchtx`, and `bridge_submitrebalancetx`.
- `z_sendmany` remains available for the permitted direction, but now rejects
  any post-sunset candidate that would create shielded outputs/change.

Tests:

- `wallet_shielded_autoshield_optin.py` now verifies that unlocking an encrypted
  wallet does not trigger default auto-shield.
- `wallet_shielded_sunset_rpc_guards.py` lowers the regtest pool-credit and
  sunset gates to height `1` and verifies the guarded RPCs fail before building.
- Opt-in auto-shield tests explicitly set `-autoshieldcoinbaseminheight=0`.

Docs and help:

- Top-level docs and shielded guides now distinguish pre-sunset launch behavior
  from current v0.32.2 production behavior.
- RPC help for shielded-ingress/control endpoints now says the paths are
  historical/pre-sunset only.
- `-fastshieldedstartup` help now matches actual startup behavior: fast startup
  skips the full cross-chain audit when persisted state is valid, but metadata
  sync may still run from available blocks.
- A stale validation comment about the commitment-index retention default was
  corrected.

## Operator Meaning

This is not a consensus relaxation. It makes wallet and RPC behavior fail early
and clearly on paths that v0.32.2 consensus already rejects.

For operators, the expected behavior after the sunset is:

- leave `-autoshieldcoinbase` unset or `0`;
- do not send funds to old shielded/ingress workflows;
- do not use `z_shieldfunds`, `z_shieldcoinbase`, bridge ingress, rebalance, or
  private z-to-z movement as current production paths;
- preserve old wallet backups and use transparent exit/recovery procedures for
  existing shielded balances.
