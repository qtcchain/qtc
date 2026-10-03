# QTC

QTC is a proof-of-work blockchain with post-quantum transaction signatures and a proof of work built on dense
matrix multiplication, so the hardware that mines it is the hardware that runs AI workloads. The first mainnet
launched on 30 September 2026; **mainnet v2**, which adds a 2 000 000 QTC treasury allocation in block 1, launches
with release v0.2.0 (genesis below once set). See `doc/release-notes/release-notes-0.2.0.md`.

| | |
|---|---|
| Genesis block (v2) | set at the v2 launch; the v1 genesis was `4040450ec30f1f9a7ef2d12578e1ea66d0838d7d8181b62c066953ca3baf3406` |
| Block explorer | <https://qtc.events> (testnet: <https://testnet.qtc.events>) |
| Latest release | distributed as signed archives; verify before you run it (below) |

This repository holds the full node (`qtcd`), the RPC client (`qtc-cli`), the wallet, the mining tools and the test
suites. The code base descends from Bitcoin Core; the consensus layer, signatures and network are QTC's own.

## Contents

- [Chain parameters](#chain-parameters)
- [Install a release](#install-a-release)
- [Run a node](#run-a-node)
- [Wallet](#wallet)
- [Mining](#mining)
- [MatMul proof of work](#matmul-proof-of-work)
- [Post-quantum signatures](#post-quantum-signatures)
- [Build from source](#build-from-source)
- [Documentation](#documentation)
- [Security](#security)
- [Contributing](#contributing)
- [License](#license)

## Chain parameters

| Parameter | Mainnet |
|---|---|
| Proof of work | MatMul PoW, n = 512 over F<sub>2³¹−1</sub>, transcript block 16, noise rank 8, pre-hash epsilon 18 bits |
| Block interval | 600 s target |
| Difficulty | ASERT from block 0, half-life 172 800 s (2 days); hard floor `0x1e011da5` |
| Block size | 24 MB serialized / 24 MWU, 480 000 sigops cost |
| Subsidy | 50 QTC, halving every 210 000 blocks; 21 000 000 QTC mined |
| Treasury allocation | **2 000 000 QTC minted once in block 1** to `qtc1ztrwsedmxswv4q0kuzw5avlw0yxntucaz89vtmvkluds5u4332ntqwq3gct` (project treasury, decided 3 Oct 2026); total supply **23 000 000 QTC** |
| Outputs | Witness v2 P2MR (`OP_2 <32 bytes>`) and `OP_RETURN` only; reduced-data limits from genesis |
| Signatures | ML-DSA-44 primary, SLH-DSA-SHAKE-128s backup (NIST FIPS 204 / 205) |
| Addresses | Bech32m, HRP `qtc` → `qtc1z…` (testnet `tqtc`) |
| Shielded pool | Closed from genesis (every sunset height is 0); viewing and recovery code retained |
| P2P / RPC ports | 19755 / 19754 (testnet 29755 / 29754) |
| DNS seeds | `seed.qtc.gold`, `seed.qtc.exchange` (testnet: `testnet-seed.qtc.gold`, `testnet-seed.qtc.exchange`) |
| Magic / network | `main` (v2 message start `51 54 43 21`); testnet is `-testnet` (chain `test`), `-regtest` for local development |

Subsidy schedule: 50 QTC for blocks 0–209 999 (block 1 additionally carries the 2 000 000 QTC treasury allocation),
25 QTC to 419 999, 12.5 QTC to 629 999, then halving every 210 000 blocks.

Testnet uses the same MatMul parameters, floor and spacing as mainnet (genesis
`efcba50d2de7cb16fd92423df899eba29e750b2271ef0ea64efe8cfeb4382ddf`). Regtest uses n = 64 for fast local mining.

## Install a release

Releases are built reproducibly with Guix for Linux x86_64 (CPU and CUDA 12), Linux aarch64, Windows x64 and macOS
(Apple silicon and Intel, signed and notarized). Every release ships a `SHA256SUMS` list signed by the QTC release
key.

```bash
# 1. Import the release-signing key and check its fingerprint against https://keys.qtc.gold
gpg --import qtc-release-signing.asc
gpg --fingerprint release@qtc.gold
#   72DD 01B8 E4BD 52FF BE1D  6576 1E28 9F6E CD6C BF30

# 2. Verify the list, then the archive you downloaded
gpg --verify SHA256SUMS.asc SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS

# 3. Unpack
tar -xzf qtc-<version>-x86_64-linux-gnu.tar.gz
./qtc-<version>/bin/qtcd -version
```

`contrib/verify-binaries/verify.py` automates steps 1–2. Do not run a binary whose checksum is not in a list you
verified yourself; the key fingerprint is published on two independent domains for that reason.

## Run a node

```bash
qtcd -daemon                 # mainnet, data in ~/.qtc
qtcd -testnet -daemon        # testnet
qtcd -regtest -daemon        # local test chain
qtc-cli getblockchaininfo
qtc-cli stop                 # always stop cleanly; never kill the process
```

Minimal `~/.qtc/qtc.conf`:

```ini
server=1
listen=1
dbcache=2048
# prune=4096           # fast node; leave unset for an archival node
```

Peer discovery uses the DNS seeds above. If DNS is unavailable, the public relay nodes can be added by hand
(`addnode=157.230.194.146:19755`, `addnode=167.99.181.131:19755`, `addnode=209.38.113.122:19755`);
`contrib/devtools/gen-qtc-node-conf.sh fast|archival` writes a complete configuration.

Data directories: Linux `~/.qtc/`, macOS `~/Library/Application Support/QTC/`, Windows `%APPDATA%\QTC\`.

For a validating node that is usable before the full sync finishes (assumeutxo snapshot), see
[doc/qtc-public-node-bootstrap.md](doc/qtc-public-node-bootstrap.md) and
[contrib/faststart/README.md](contrib/faststart/README.md).

## Wallet

QTC wallets are descriptor wallets whose keys are post-quantum. Addresses are `qtc1z…`.

```bash
qtc-cli createwallet "mywallet"
qtc-cli -rpcwallet=mywallet getnewaddress        # qtc1z...
qtc-cli -rpcwallet=mywallet getbalance
qtc-cli -rpcwallet=mywallet sendtoaddress qtc1z... 1.5
qtc-cli -rpcwallet=mywallet encryptwallet "passphrase"
```

Back up the wallet **with its descriptors**, not just the database file:

```bash
qtc-cli -rpcwallet=mywallet backupwallet /secure/path/mywallet.dat
qtc-cli -rpcwallet=mywallet listdescriptors true > /secure/path/mywallet-descriptors.json   # contains private keys
```

## Mining

Mining is dense matrix multiplication; a GPU is the practical minimum on mainnet. The node contains the solver
(CPU, CUDA, Metal) and a supervised mining loop that mines to an address you control.

```bash
qtc-cli -rpcwallet=miner getnewaddress > ~/miner-address.txt
QTC_MATMUL_BACKEND=cuda contrib/mining/start-live-mining.sh \
  --datadir="$HOME/.qtc" --address-file="$HOME/miner-address.txt"
qtc-cli getmininginfo                 # difficulty, network rate, chain_guard health
qtc-matmul-backend-info --backend=cuda   # confirms the GPU backend is active
```

The loop refuses to mine while the node is behind its peers (`chain_guard`), so a stale node cannot mine an orphan
chain. External miners use `getblocktemplate` / `submitblock`; the template carries the MatMul fields
(`matmul_dim`, `seed_a`, `seed_b`, `target`). Regtest mines instantly with `generatetoaddress`.

## MatMul proof of work

MatMul PoW follows *Proofs of Useful Work from Arbitrary Matrix Multiplication* (Komargodski, Schen, Weinstein,
[arXiv:2504.09971](https://arxiv.org/abs/2504.09971)). For each block:

1. Two seeds derived from the previous block and the height expand into n × n matrices A and B over F<sub>q</sub>,
   q = 2³¹ − 1. Miners cannot choose them.
2. A low-rank perturbation derived from the header and nonce gives A′, B′ — no precomputation across nonces.
3. The miner computes C′ = A′ × B′, commits to the product, and hashes the transcript with SHA-256.
4. The block is valid when the digest meets the ASERT target. A pre-hash lottery (epsilon 18 bits) lets a miner skip
   most nonces without a multiplication, so the work is dominated by real GEMMs.

Validation is two-phase: every node checks headers, seeds and the digest in O(1); full re-multiplication (O(n³)) is
applied to recent blocks (window 1 000) and rate-limited per peer, and a peer that serves one invalid product is
banned. Specification: [doc/qtc-matmul-pow-spec.md](doc/qtc-matmul-pow-spec.md). Launch sizing and the difficulty
floor: [doc/launch/powlimit-sizing.md](doc/launch/powlimit-sizing.md), [QTC-LAUNCH-SAFETY.md](QTC-LAUNCH-SAFETY.md).

## Post-quantum signatures

Every spend is authorized by a NIST post-quantum signature through **P2MR** (pay-to-Merkle-root, witness version 2).
The witness program is the 32-byte root of a two-leaf tree:

| Leaf | Algorithm | Public key | Signature | Role |
|---|---|---|---|---|
| Primary | ML-DSA-44 (FIPS 204, lattice) | 1 312 B | 2 420 B | everyday spends |
| Backup | SLH-DSA-SHAKE-128s (FIPS 205, hash-based) | 32 B | 7 856 B | recovery if lattice assumptions fail |

Script opcodes `OP_CHECKSIG_MLDSA`, `OP_CHECKSIG_SLHDSA`, their `CHECKSIGADD` forms for threshold multisig, plus
`OP_CHECKTEMPLATEVERIFY` and `OP_CHECKSIGFROMSTACK` inside P2MR leaves. Descriptors: `mr(<mldsa>,pk_slh(<slhdsa>))`,
`mr(multi_pq(...))`, timelocked variants; HD purpose `87h`. Falcon opcodes are reserved (`OP_SUCCESS`) for a future
soft fork. Specifications: [doc/qtc-pqc-spec.md](doc/qtc-pqc-spec.md), [doc/qtc-pq-multisig-spec.md](doc/qtc-pq-multisig-spec.md).

## Build from source

Requirements: GCC 11.1+ or Clang 16+, CMake 3.22+, Boost 1.73+, libevent 2.1.8+, Python 3.10+ (tests). Optional:
SQLite (wallet), ZeroMQ (notifications), Qt 5.11+/6.2+ (GUI), CUDA 12 (GPU mining).

```bash
# Linux (Ubuntu/Debian)
sudo apt-get install -y build-essential cmake pkg-config libboost-dev libevent-dev libsqlite3-dev libzmq3-dev
cmake -B build && cmake --build build -j"$(nproc)"

# macOS
brew install cmake boost libevent sqlite pkg-config zeromq
cmake -B build && cmake --build build -j"$(sysctl -n hw.logicalcpu)"

ctest --test-dir build      # unit tests
```

Platform guides: [Unix](doc/build-unix.md), [macOS](doc/build-osx.md), [Windows MSVC](doc/build-windows-msvc.md),
[Windows 11 step by step](doc/qtc-windows-11-compile-handbook.md), [FreeBSD](doc/build-freebsd.md). Reproducible
release builds: [doc/release-process.md](doc/release-process.md) and [contrib/guix](contrib/guix). No binaries are
committed to this repository.

## Documentation

| Topic | Where |
|---|---|
| Consensus and PoW | [doc/qtc-matmul-pow-spec.md](doc/qtc-matmul-pow-spec.md), [QTC-LAUNCH-SAFETY.md](QTC-LAUNCH-SAFETY.md) |
| Signatures and wallets | [doc/qtc-pqc-spec.md](doc/qtc-pqc-spec.md), [doc/qtc-key-management-guide.md](doc/qtc-key-management-guide.md) |
| Running and mining | [doc/qtc-public-node-bootstrap.md](doc/qtc-public-node-bootstrap.md), [doc/qtc-mining-ops.md](doc/qtc-mining-ops.md), [doc/qtc-conf.md](doc/qtc-conf.md) |
| Releases | [doc/release-process.md](doc/release-process.md), [doc/release-signing-keys.md](doc/release-signing-keys.md), [doc/release-notes](doc/release-notes) |
| Launch record | [doc/launch](doc/launch) |
| Security review | [QTC-SECURITY-REVIEW.md](QTC-SECURITY-REVIEW.md), [doc/security](doc/security) |
| Block explorer | [qtcchain/qtc-explorer](https://github.com/qtcchain/qtc-explorer), spec in [doc/qtc-block-explorer-spec.md](doc/qtc-block-explorer-spec.md) |

## Security

Report vulnerabilities privately as described in [SECURITY.md](SECURITY.md). Nobody from QTC will ever ask for a seed,
passphrase or private key. Release signatures are produced offline; only public keys and signatures exist on any
networked machine.

## Contributing

`main` changes only through pull requests reviewed and approved by the maintainer; direct pushes, force pushes and
tag changes are blocked by repository rulesets. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

MIT, see [COPYING](COPYING). Copyright (c) 2009–2025 The Bitcoin Core developers; copyright (c) 2026 The QTC developers.
