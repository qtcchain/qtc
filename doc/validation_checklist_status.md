# Deep Audit Checklist Tracking

This interim tracker captures the execution status of the consolidated QTC
validation checklist. Use it whenever the checklist is run locally, in CI, or
as part of any release-readiness gate so that the latest health snapshot stays
visible to the entire team.

## How to Run the Checklist

```
./scripts/generate_validation_checklist.sh \
  --build-dir ./build-qtc \
  --artifact ./.qtc-validation/latest-checklist.json \
  --log-dir ./.qtc-validation/logs
```

The script prints a human-readable summary to stdout and writes a JSON artifact
plus per-check logs under `.qtc-validation/`. Each check corresponds to a
previously existing verification suite:

| Check ID   | Scope                                                            |
|------------|------------------------------------------------------------------|
| consensus  | `scripts/test_qtc_consensus.sh`                                  |
| parallel   | `scripts/test_qtc_parallel.sh`                                   |
| benchmark  | `scripts/m9_qtc_benchmark_suite.sh`                              |
| production | `scripts/verify_qtc_production_readiness.sh`                     |
| scaling    | `scripts/m8_pow_scaling_suite.sh`                                |
| mining     | `scripts/m7_mining_readiness.sh`                                 |

Environment variables such as `QTC_CHECKLIST_OVERRIDE_CONSENSUS` can be pointed
at stub binaries when smoke-testing automation; otherwise the default commands
above run directly.

## Execution Log

Update the table below each time the checklist runs. The `Last status` and
`Timestamp` columns are taken from the JSON artifact (`generated_at` and the
individual `checks` entries). Feel free to add notes about regressions or
follow-ups that were filed.

| Run | Timestamp (UTC) | Overall Status | Artifact Path                              | Notes |
|-----|-----------------|----------------|--------------------------------------------|-------|
| -   | _pending update_| _pending_      | `.qtc-validation/latest-checklist.json`    | -     |

| Check ID   | Last Status | Last Duration (s) | Log Path                                | Notes |
|------------|-------------|-------------------|-----------------------------------------|-------|
| consensus  | _pending_   | -                 | `.qtc-validation/logs/consensus.log`    |       |
| parallel   | _pending_   | -                 | `.qtc-validation/logs/parallel.log`     |       |
| benchmark  | _pending_   | -                 | `.qtc-validation/logs/benchmark.log`    |       |
| production | _pending_   | -                 | `.qtc-validation/logs/production.log`   |       |
| scaling    | _pending_   | -                 | `.qtc-validation/logs/scaling.log`      |       |
| mining     | _pending_   | -                 | `.qtc-validation/logs/mining.log`       |       |

## Consuming the JSON Artifact

The JSON report is shaped as:

```json
{
  "generated_at": "2024-02-07T16:26:16Z",
  "build_dir": "/path/to/build-qtc",
  "overall_status": "pass",
  "checks": [
    {
      "id": "consensus",
      "description": "Consensus determinism + KAWPOW compatibility",
      "status": "pass",
      "seconds": 42,
      "log": "/workspace/.qtc-validation/logs/consensus.log"
    }
  ]
}
```

Grab fresh values via:

```
jq -r '.checks[] | "\(.id): \(.status) (\(.seconds)s)"' ./.qtc-validation/latest-checklist.json
```

Attach the artifact and this tracker to release readiness reviews so that the
full execution history stays auditable between deep audit runs.
