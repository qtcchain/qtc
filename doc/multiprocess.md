# Multiprocess Bitcoin

_This document describes usage of the multiprocess feature. For design information, see the [design/multiprocess.md](design/multiprocess.md) file._

## Current Status

Multiprocess is currently disabled in this repository. Top-level CMake forces
`WITH_MULTIPROCESS=OFF`, so passing `-DWITH_MULTIPROCESS=ON` does not enable
the feature and does not build the supplemental multiprocess executables.

The build and usage details below are retained as historical reference for the
feature design, not as a supported current build path.

## Build Option

`-DWITH_MULTIPROCESS=ON` is not currently supported in this tree because the
top-level CMake configuration forces multiprocess support off.

## Debugging

The `-debug=ipc` command line option can be used to see requests and responses between processes.

## Installation

When multiprocess support is re-enabled in the future, it will require
[Cap'n Proto](https://capnproto.org/) and
[libmultiprocess](https://github.com/bitcoin-core/libmultiprocess) as
dependencies. A historical way to get started without installing these
dependencies manually was to use the [depends system](../depends) with the
`MULTIPROCESS=1` [dependency option](../depends#dependency-options) passed to
make:

```
cd <BITCOIN_SOURCE_DIRECTORY>
make -C depends NO_QT=1 MULTIPROCESS=1
# Set host platform to output of gcc -dumpmachine or clang -dumpmachine or check the depends/ directory for the generated subdirectory name
HOST_PLATFORM="x86_64-pc-linux-gnu"
cmake -B build --toolchain=depends/$HOST_PLATFORM/toolchain.cmake
cmake --build build
build/bin/bitcoin-node -regtest -printtoconsole -debug=ipc
BITCOIND=$(pwd)/build/bin/bitcoin-node build/test/functional/test_runner.py
```

Even with that dependency setup, the current top-level CMake configuration
still forces `WITH_MULTIPROCESS=OFF`.

Alternately, when the feature is re-enabled, you would install
[Cap'n Proto](https://capnproto.org/) and
[libmultiprocess](https://github.com/bitcoin-core/libmultiprocess) packages on
your system and run CMake with multiprocess enabled. That path is not currently
active in this repository.

## Usage

If multiprocess support is re-enabled, `bitcoin-node` would be a drop-in
replacement for `bitcoind`, and `bitcoin-gui` would be a drop-in replacement
for `qtc-qt`, with no intended differences in use or external behavior. In
that design, `bitcoin-gui` would spawn a `bitcoin-node` process to run P2P and
RPC code, communicating with it across a socket pair, and `bitcoin-node` would
spawn `bitcoin-wallet` to run wallet code, also communicating over a socket
pair. This would let node, wallet, and GUI code run in separate address spaces
for better isolation, and allow future improvements like being able to start
and stop components independently on different machines and environments.
[#19460](https://github.com/bitcoin/bitcoin/pull/19460) also adds a new `bitcoin-node` `-ipcbind` option and a `bitcoind-wallet` `-ipcconnect` option to allow new wallet processes to connect to an existing node process.
And [#19461](https://github.com/bitcoin/bitcoin/pull/19461) adds a new `bitcoin-gui` `-ipcconnect` option to allow new GUI processes to connect to an existing node process.
