#!/usr/bin/env bash
export LC_ALL=C
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SCRIPT="${ROOT_DIR}/scripts/verify_qtc_production_readiness.sh"
DEFAULT_LOCK_DIR="${ROOT_DIR}/.qtc-parallel-test.lock"

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/qtc-prod-parallel-lock-test.XX""XX""XX")"
CREATED_TEST_LOCK=0
ORIGINAL_OWNER_PID=""
cleanup() {
  rm -rf "${TMP_DIR}"
  if [[ "${CREATED_TEST_LOCK}" -eq 1 ]]; then
    rm -rf "${DEFAULT_LOCK_DIR}"
  fi
}
trap cleanup EXIT

if [[ -d "${DEFAULT_LOCK_DIR}" ]]; then
  if [[ -f "${DEFAULT_LOCK_DIR}/owner.pid" ]]; then
    ORIGINAL_OWNER_PID="$(tr -dc '0-9' < "${DEFAULT_LOCK_DIR}/owner.pid" || true)"
  fi
else
  mkdir -p "${DEFAULT_LOCK_DIR}"
  printf '%s\n' "$$" > "${DEFAULT_LOCK_DIR}/owner.pid"
  printf 'scripts/test_qtc_parallel.sh\n' > "${DEFAULT_LOCK_DIR}/owner.cmd"
  CREATED_TEST_LOCK=1
fi

ARTIFACT="${TMP_DIR}/report.json"
LOG_DIR="${TMP_DIR}/logs"

QTC_PARALLEL_SKIP_DEFAULT_JOBS=1 \
QTC_PARALLEL_TEST_COMMAND="sleep 1" \
QTC_PARALLEL_JOB_TIMEOUT_SECONDS=20 \
"${SCRIPT}" \
  --build-dir "${ROOT_DIR}/build-qtc" \
  --artifact "${ARTIFACT}" \
  --log-dir "${LOG_DIR}" \
  --skip-lint \
  --skip-m7-readiness \
  --skip-m7-pool-e2e \
  --skip-m5-genesis-freeze \
  --skip-m7-external-kawpow \
  --skip-launch-blockers \
  --skip-benchmark-suite \
  --skip-pow-scaling-suite \
  --skip-m7-timeout-check \
  --skip-m7-parallel-check \
  --skip-parallel-timeout-check \
  --skip-swarm-timeout-check \
  --skip-production-loop-guard \
  --skip-live-p2p \
  --skip-live-mining

python3 - <<'PY' "${ARTIFACT}"
import json
import sys

with open(sys.argv[1], encoding="utf-8") as fh:
    data = json.load(fh)

assert data["overall_status"] == "pass", data
statuses = {entry["id"]: entry["status"] for entry in data["checks"]}
assert statuses["binaries"] == "pass", statuses
assert statuses["parallel_gate"] == "pass", statuses
PY

if [[ "${CREATED_TEST_LOCK}" -eq 1 ]]; then
  test -f "${DEFAULT_LOCK_DIR}/owner.pid"
  test "$(cat "${DEFAULT_LOCK_DIR}/owner.pid")" -eq "$$"
elif [[ -n "${ORIGINAL_OWNER_PID}" ]]; then
  test -f "${DEFAULT_LOCK_DIR}/owner.pid"
  test "$(cat "${DEFAULT_LOCK_DIR}/owner.pid")" -eq "${ORIGINAL_OWNER_PID}"
fi

echo "verify_qtc_production_readiness_parallel_lock_isolation_test: PASS"
