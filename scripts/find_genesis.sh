#!/usr/bin/env bash
export LC_ALL=C
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${ROOT_DIR}/build-qtc}"
if [[ $# -gt 0 ]]; then
  shift
fi

BINARY="${BUILD_DIR}/bin/qtc-genesis"
if [[ ! -x "${BINARY}" ]]; then
  echo "error: ${BINARY} is missing. Run scripts/build_qtc.sh first." >&2
  exit 1
fi

DEFAULT_TIMESTAMP="${QTC_GENESIS_TIMESTAMP:-QTC Chain launch candidate}"
DEFAULT_TIME="${QTC_GENESIS_TIME:-1700000000}"
DEFAULT_BITS="${QTC_GENESIS_BITS:-0x207fffff}"
DEFAULT_HEIGHT="${QTC_GENESIS_HEIGHT:-0}"
DEFAULT_MAX_TRIES="${QTC_GENESIS_MAX_TRIES:-1000000}"

"${BINARY}" \
  --timestamp "${DEFAULT_TIMESTAMP}" \
  --time "${DEFAULT_TIME}" \
  --bits "${DEFAULT_BITS}" \
  --height "${DEFAULT_HEIGHT}" \
  --max-tries "${DEFAULT_MAX_TRIES}" \
  "$@"
