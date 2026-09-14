# Seeds

Utility to generate the fixed-seed table that is compiled into the client
(see [src/chainparamsseeds.h](/src/chainparamsseeds.h) and the other utilities
in [contrib/seeds](/contrib/seeds)).

## Launch-network seed design

QTC bootstraps through two independent layers, both compiled into the binary
(`src/kernel/chainparams.cpp`):

1. **DNS seeds** (primary). Two names per network, each served by a different
   DNS provider (Cloudflare and DigitalOcean) under domains held at two
   different registrars, so that no single provider or registrar outage can
   take bootstrap down:

   | Network | DNS seeds                                                  |
   |---------|------------------------------------------------------------|
   | mainnet | `seed.qtc.gold`, `seed.qtc.exchange`                       |
   | testnet | `testnet-seed.qtc.gold`, `testnet-seed.qtc.exchange`       |

   Both names must resolve to the same set of inbound-capable public nodes.

2. **Fixed seeds** (fallback when DNS is unavailable). The inbound-capable
   launch nodes, listed here and serialized in BIP155 form into
   `src/chainparamsseeds.h`:

   | Network | File             | Entries                                            |
   |---------|------------------|----------------------------------------------------|
   | mainnet | `nodes_main.txt` | `157.230.194.146:19755`, `167.99.181.131:19755`     |
   | testnet | `nodes_test.txt` | `157.230.194.146:29755`, `167.99.181.131:29755`     |

   Only nodes that accept inbound connections belong in these lists; an
   outbound-only node must never be added. testnet4 and signet have no public
   QTC network, so `nodes_testnet4.txt` and `nodes_signet.txt` stay empty and
   the corresponding `vFixedSeeds` are cleared in `chainparams.cpp`.

The same two hosts (without ports) form the default mining peer mesh in
`src/node/mining_guard.cpp`; keep the three lists in step when a host is
rotated.

## Regenerating the fixed-seed header

For a small, hand-maintained launch network, edit `nodes_main.txt` /
`nodes_test.txt` directly (one `<ip>:<port>` per line, `#` comments allowed)
and regenerate the header from the repository root:

```
python3 contrib/seeds/generate-seeds.py contrib/seeds > src/chainparamsseeds.h
```

`generate-seeds.py` takes the *directory* containing `nodes_main.txt`,
`nodes_signet.txt`, `nodes_test.txt` and `nodes_testnet4.txt`; all four files
must exist (empty is fine). Then run the `qtc_seed_tests` unit tests, which
decode the header and check it against the expected `ip:port` entries.

## Crawler-derived lists (larger networks)

Once the network has enough public peers, `makeseeds.py` can build the lists
from DNS-crawler exports instead. Update `PATTERN_AGENT` and `MIN_BLOCKS` in
`makeseeds.py` as the QTC version/height evolves, collect crawler output into
`seeds_main.txt` / `seeds_test.txt` / `seeds_testnet4.txt` / `seeds_signet.txt`
(address, uptime, service flags, blocks, user agent, ...), then from
`contrib/seeds`:

```
curl https://raw.githubusercontent.com/asmap/asmap-data/main/latest_asmap.dat > asmap-filled.dat
python3 makeseeds.py -a asmap-filled.dat -s seeds_main.txt > nodes_main.txt
python3 makeseeds.py -a asmap-filled.dat -s seeds_test.txt > nodes_test.txt
python3 makeseeds.py -a asmap-filled.dat -s seeds_testnet4.txt -m 72600 > nodes_testnet4.txt
# Optional: only if operating a custom QTC signet
python3 makeseeds.py -a asmap-filled.dat -s seeds_signet.txt > nodes_signet.txt
python3 generate-seeds.py . > ../../src/chainparamsseeds.h
```
