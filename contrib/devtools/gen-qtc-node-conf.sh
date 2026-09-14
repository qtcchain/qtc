#!/usr/bin/env bash
set -euo pipefail

PROFILE="${1:-fast}"
BOOTSTRAP_MODE="${2:-discover}"
MANAGED_NODE_NAME="${3:-}"

case "${PROFILE}" in
  fast|archival)
    ;;
  *)
    echo "usage: $0 [fast|archival] [discover|strict-connect|managed-direct] [local|fra|nyc|sfo]" >&2
    exit 1
    ;;
esac

case "${BOOTSTRAP_MODE}" in
  discover|strict-connect|managed-direct)
    ;;
  *)
    echo "usage: $0 [fast|archival] [discover|strict-connect|managed-direct] [local|fra|nyc|sfo]" >&2
    exit 1
    ;;
esac

managed_direct_peers() {
  case "${1}" in
    local)
      cat <<'EOF'
addnode=157.230.194.146:19755
addnode=167.99.181.131:19755
EOF
      ;;
    fra)
      cat <<'EOF'
addnode=157.230.194.146:19755
addnode=167.99.181.131:19755
EOF
      ;;
    nyc)
      cat <<'EOF'
addnode=167.99.181.131:19755
addnode=157.230.194.146:19755
EOF
      ;;
    sfo)
      cat <<'EOF'
EOF
      ;;
    *)
      return 1
      ;;
  esac
}

cat <<'EOF'
# QTC mainnet baseline
server=1
listen=1
port=19335

# Local-only RPC
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
rpcport=19334

# Allow young-chain bootstrap
minimumchainwork=0

# Keep shielded commitment lookups on disk so restart and snapshot recovery stay fast.
retainshieldedcommitmentindex=1

# Runtime defaults
dbcache=4096
maxmempool=300
EOF

if [[ "${BOOTSTRAP_MODE}" == "discover" ]]; then
  cat <<'EOF'

# Scalable bootstrap (recommended): seed with public QTC nodes, keep peer discovery on.
dnsseed=1
fixedseeds=1
addnode=157.230.194.146:19755
addnode=167.99.181.131:19755
EOF
elif [[ "${BOOTSTRAP_MODE}" == "strict-connect" ]]; then
  cat <<'EOF'

# Strict deterministic troubleshooting mode:
# pins outbound peers and disables automatic peer discovery.
dnsseed=0
fixedseeds=0
connect=157.230.194.146:19755
connect=167.99.181.131:19755
EOF
else
  if [[ -z "${MANAGED_NODE_NAME}" ]]; then
    echo "managed-direct requires a managed node name: local|fra|nyc|sfo" >&2
    exit 1
  fi
  if ! MANAGED_PEERS="$(managed_direct_peers "${MANAGED_NODE_NAME}")"; then
    echo "unknown managed node name for managed-direct: ${MANAGED_NODE_NAME}" >&2
    exit 1
  fi
  cat <<EOF

# Managed direct-peer mode:
# disables public seed discovery and pins canonical direct archival peers for the controlled fleet.
dnsseed=0
fixedseeds=0
${MANAGED_PEERS}
EOF
fi

if [[ "${PROFILE}" == "fast" ]]; then
  cat <<'EOF'

# Fast node profile (recommended for most operators)
prune=4096
EOF
else
  cat <<'EOF'

# Archival profile (full historical block bodies)
prune=0
EOF
fi
