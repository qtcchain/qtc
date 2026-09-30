# QTC Public Node Bootstrap (Archival)

This runbook is the canonical mainnet bootstrap path for operators who only
have this repository and public Internet access.

For the current precompiled-binary and fast-start service workflow, use
[qtc-download-and-go.md](qtc-download-and-go.md) and
[../contrib/faststart/README.md](../contrib/faststart/README.md). This
archival guide remains useful for full public-node bring-up, but it is no
longer the shortest path for binary users or service-gating operators.

## 1. Key Ops Prerequisite (Before Node Bring-Up)

If the node will mine, decide the payout target first:

- create/select the multisig descriptor and public keys that will receive mined
  rewards,
- generate the destination address (`qtc1z...`) from that descriptor,
- back up descriptor/public key material offline.

This is a **hard prerequisite** for node provisioning. Do not start host bring-up
until you have both:

- a finalized payout address (`qtc1z...`), and
- the corresponding public descriptor text used to derive it.

Do **not** put private keys on public seed nodes unless absolutely required.
Public archival seeds are normally run walletless.

## 2. Mainnet Config

Create `~/.qtc/qtc.conf` (QTC runtime canonical config path):

```ini
server=1
listen=1
port=19755

rpcbind=127.0.0.1
rpcallowip=127.0.0.1
rpcport=19754

# Keep archival history for deterministic bootstrap service
prune=0

# Bootstrap guardrails
minimumchainwork=0
dnsseed=1
fixedseeds=1
addnode=157.230.194.146:19755
addnode=167.99.181.131:19755
addnode=209.38.113.122:19755
```

Notes:

- `19755` is the QTC mainnet P2P port.
- `19754` is the QTC mainnet default RPC port.
- `addnode=` seeds initial peers while preserving broader peer discovery.
- the DNS seeds are `seed.qtc.gold` and `seed.qtc.exchange` (two providers, two registrars);
  the three `addnode=` entries are the fixed public seed nodes (Singapore, Toronto,
  Frankfurt) and match `contrib/seeds/nodes_main.txt`
- `getblocktemplate` enforces an outbound peer floor on mainnet by default
  (`-miningminoutboundpeers=2`) to reduce isolated-mining orphan risk. Only
  outbound connections count: a peer this node chose to open (addrman or
  `addnode`), never an inbound one, because inbound connections are
  unauthenticated and cheap to sybil. Fleet operators who want their own
  nodes to count should `addnode` each other on both sides so every fleet
  link is outbound for both ends. The default is two because the launch mesh
  is three public hosts and a miner that is itself a mesh host can only reach
  the other two. Set `-miningminoutboundpeers=0` only for intentional isolated
  lab mining.
- `getblocktemplate` also enforces that at least two outbound peers are actually
  near tip on mainnet by default
  (`-miningminsyncedoutboundpeers=2`, `-miningmaxpeersyncheightlag=1`).
  This reduces stale/forked mining when outbound peers are connected but lagging.
  A peer that has not announced any header yet (at genesis, or one that joined
  while the network was idle) is judged by its version-handshake starting
  height instead, so a fresh network at height 0 can start mining.
  Set `-miningminsyncedoutboundpeers=0` only for intentional isolated lab mining.
- `getblocktemplate` also enforces a validated-tip/header-lag bound on mainnet
  by default (`-miningmaxheaderlag=3`) so miners do not work from templates that
  are materially behind known headers. Set `-miningmaxheaderlag=0` only for
  intentional isolated lab workflows.
- Longpoll template requests re-check these guards on wakeup, so miners do not
  continue receiving work after a connectivity or validation-lag regression.
- This runbook is archival-only (`prune=0`).
- For newcomer/miner-first mode use `./contrib/devtools/gen-qtc-node-conf.sh fast` (default `prune=4096`, scalable bootstrap).
- For canonical/seed operators use `./contrib/devtools/gen-qtc-node-conf.sh archival` (default `prune=0`, scalable bootstrap).
- Use strict deterministic troubleshooting mode only when needed:
  `./contrib/devtools/gen-qtc-node-conf.sh archival strict-connect`.
- If you control the managed archival fleet (`local` / `fra` / `nyc` / `sfo`),
  use direct managed peers instead of the public bootstrap set:
  `./contrib/devtools/gen-qtc-node-conf.sh archival managed-direct <local|fra|nyc|sfo>`.

## 3. Start and Verify

```bash
./build/bin/qtcd -conf="$HOME/.qtc/qtc.conf" -allowignoredconf=1 -daemon
./build/bin/qtc-cli -conf="$HOME/.qtc/qtc.conf" getnetworkinfo
./build/bin/qtc-cli -conf="$HOME/.qtc/qtc.conf" getpeerinfo
./build/bin/qtc-cli -conf="$HOME/.qtc/qtc.conf" getblockchaininfo
```

Expected:

- peers connected on `:19755`,
- `networkactive: true`,
- `pruned: false` in `getblockchaininfo`.
- continuous progress in `blocks` and `headers` (no long-lived stall).

If you previously used strict deterministic mode (`connect=`), switch back to
`addnode=` + discovery for better mesh resilience and lower stale/orphan risk.

If you are operating the managed archival fleet, do not use the public
`node.qtc.*` hostnames as fixed manual peers. They are acceptable public
bootstrap seeds, but the managed fleet should use `managed-direct` so each node
pins the canonical direct archival peers instead of whatever the public DNS
records currently resolve to.

Troubleshooting:

- If the node stalls near `blocks=16` and `headers=4000` with repeated
  `MatMul per-peer verification budget exhausted` disconnects in `debug.log`,
  you are likely running an older `qtcd` binary.
- Rebuild from current source, then restart:

```bash
cmake --build build -j$(nproc)
./build/bin/qtc-cli -conf="$HOME/.qtc/qtc.conf" stop
./build/bin/qtcd -conf="$HOME/.qtc/qtc.conf" -allowignoredconf=1 -daemon
```

- If Tor logs contain `.onion ... resolve failed ... No more HSDir available to query`,
  treat that as a Tor connectivity problem (HSDir/bootstrap reachability), not a
  chain-consensus failure. For public clearnet bootstrap, set `onion=0` unless
  onion transport is explicitly required.

## 4. Archival Check (Historical Block Body)

```bash
H=$(./build/bin/qtc-cli -conf="$HOME/.qtc/qtc.conf" getblockhash 1)
./build/bin/qtc-cli -conf="$HOME/.qtc/qtc.conf" getblock "$H" 0 > /dev/null
```

If this succeeds (and `pruned: false`), the node has historical block body
access and is operating as archival.

## 5. Mining Payout Guardrail

For built-in test mining, always pass your chosen payout address explicitly:

```bash
./build/bin/qtc-cli -conf="$HOME/.qtc/qtc.conf" generatetoaddress 1 "qtc1z..."
```

Recommended public-only host artifacts (create these during provisioning):

```bash
sudo install -d -m 755 /opt/qtc-runtime/artifacts
echo "qtc1z..." | sudo tee /opt/qtc-runtime/artifacts/mainnet_payout_address.txt >/dev/null
echo "mr(sortedmulti_pq(...))#...." | sudo tee /opt/qtc-runtime/artifacts/mainnet_multisig_descriptor.txt >/dev/null
```

Optional watch-only wallet import (public descriptor only, no private keys):

```bash
./build/bin/qtc-cli -conf="$HOME/.qtc/qtc.conf" -named createwallet \
  wallet_name=main_msig_watch \
  disable_private_keys=true blank=true descriptors=true load_on_startup=true

DESC="$(cat /opt/qtc-runtime/artifacts/mainnet_multisig_descriptor.txt)"
REQ="$(jq -nc --arg d "$DESC" '[{desc:$d, timestamp:"now", active:false}]')"
./build/bin/qtc-cli -conf="$HOME/.qtc/qtc.conf" -rpcwallet=main_msig_watch importdescriptors "$REQ"
```

For external/mainnet mining (`getblocktemplate` + `submitblock`), configure the
miner/pool coinbase destination to the same multisig-derived payout address.
