# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

TBCNODE is the reference full-node implementation for **TuringBitChain (TBC)**, a Bitcoin-derived blockchain **forked from BSV 1.0.5 Release Candidate** (last BSV commit is `b8cdd4e89`; first TBC commit is `72cf010ff` "TBCNODE version 1.9.9" on 2024-07-29). It produces the usual suite of binaries: `bitcoind` (full node), `bitcoin-cli` (RPC client), `bitcoin-tx` (tx editor), `bitcoin-miner` (CPU miner), `bitcoin-seeder` (DNS seeder). Client version is defined in `src/clientversion.h` (currently 3.3.0).

TBC took the BSV chain at height **824190** (`TBCFirstBlockHeight` in `chainparams.cpp:107`). In `-pruneblocks` mode nodes skip block data before this height, so the ~19.6 亿 BSV-era coins are effectively locked; only the ~1.41 亿 TBC subsidy issued from 824190 onwards is in circulation.

TBC retains the Bitcoin-SV "post-Genesis" consensus model (large blocks, raised script limits, Turing-style script) and layers TBC-specific additions on top (e.g. `filled_miner_bill_v2`, `x_only_pubkey`, `tbc_check*` script features — see the test files starting with `tbc_` / `filled_miner_bill_v2` in `src/test/`). When making consensus-adjacent changes, always check the `consensus::Params` struct in `src/consensus/params.h` and its use in `src/chainparams.cpp` — activation heights (e.g. `schnorrMultisigHeight`) differ per network and are the canonical switches for new features.

## Build

The canonical build is **CMake** (the old autotools flow referenced in `bitbucket-pipelines.yml` and some docs is legacy). C++17 is required.

```bash
cmake -B build -S . -DENABLE_PROD_BUILD=ON
cmake --build build -j$(nproc)
```

Key CMake options (see top of `CMakeLists.txt` and `src/CMakeLists.txt`):

- `-DENABLE_PROD_BUILD=ON` — production flags (use for any real build).
- `-DBUILD_BITCOIN_WALLET=OFF` — disables wallet (requires BerkeleyDB otherwise).
- `-DBUILD_BITCOIN_ZMQ=OFF`, `-DBUILD_BITCOIN_SEEDER=OFF`, `-DBUILD_BITCOIN_BENCH=OFF`, `-DBUILD_BITCOIN_MINER=OFF` — disable optional components.
- `-Denable_debug=ON` — adds `-DDEBUG -DDEBUG_LOCKORDER` (expensive; helpful for mutex ordering bugs).
- `-Denable_asan=ON` / `-Denable_ubsan=ON` / `-Denable_tsan=ON` — sanitizers (mutually exclusive pairs as enforced in CMake).
- `-DEXTRA_WARNINGS=ON` — adds `-Wshadow -Wsuggest-override`.

Binaries land in `build/src/` (e.g. `build/src/bitcoind`). `ccache` is used automatically if present. See `INSTALL.md` for OS-specific dependency installs (Ubuntu 20.04/22.04/24.04, macOS). A Docker flow exists via `Dockerfile-node`.

## Tests

Two test suites, both driven through CMake `check` / `check-all` targets:

### Unit tests (Boost.Test, C++)

Sources: `src/test/*_tests.cpp` plus `src/wallet/test/`. The `test_bitcoin` binary is registered in `src/test/CMakeLists.txt`.

```bash
# Build & run all unit tests
cmake --build build --target check -j$(nproc)

# Run the test binary directly with Boost filters
./build/src/test/test_bitcoin --run_test=<suite>           # one suite
./build/src/test/test_bitcoin --run_test=<suite>/<case>     # one case
./build/src/test/test_bitcoin --log_level=test_suite        # verbose
```

TBC-specific unit tests live alongside upstream ones: `tbc_script_validation.cpp`, `tbc_check{datasig,sig,multisig}_tests.cpp`, `filled_miner_bill_v2_tests.cpp`, `x_only_pubkey_tests.cpp`. Add new test files to the `add_test_to_suite(bitcoin test_bitcoin ...)` list in `src/test/CMakeLists.txt` — the build will not pick them up automatically.

### Functional tests (Python, RPC/P2P)

Located in `test/functional/`. Require Python 3 + `python3-zmq`. Must be run after a successful build because the framework spins up real `bitcoind` processes.

```bash
# All functional tests
cmake --build build --target check-functional

# Or directly (from build/ or source tree):
test/functional/test_runner.py                     # default suite
test/functional/test_runner.py example_test        # single test (name or path)
test/functional/test_runner.py --extended          # full suite
test/functional/test_runner.py --jobs=8            # parallelism (default 4)
test/functional/<name>.py                          # run a single test script directly
```

Functional tests cache a pre-mined 200-block chain in `test/cache/`; if it goes bad, `rm -rf test/cache` and `killall bitcoind` (warning: kills *all* bitcoind on the system). See `test/README.md` for debugger attach, log combining (`combine_logs.py`), and `--tracerpc`.

`cmake --build build --target check-all` runs both unit and functional suites.

## Architecture

Large C++ codebase (~190 files in `src/`). The entry points and the main subsystems:

- **`src/bitcoind.cpp` → `src/init.cpp`** — daemon startup; wires every subsystem. This is the map for "what runs at boot".
- **`src/validation.{h,cpp}`** — block/tx validation, UTXO connect/disconnect, reorg logic, activation-height helpers (`IsSchnorrMultisigEnabled`, etc.). Very large; the canonical source of consensus behavior.
- **`src/consensus/`** — pure consensus primitives (`params.h`, `merkle`, `validation.h`). `Params` lives here; `chainparams.cpp` sets per-network values (mainnet / testnet / regtest / STN).
- **`src/script/`** — script interpreter (`interpreter.cpp`), opcode table (`opcodes.cpp`), `CScript` (`script.cpp`), `StackMemoryUsage` accounting (`limitedstack.cpp`), signature caches. Post-Genesis script limits live in `src/consensus/consensus.h`.
- **`src/primitives/`** — `CBlock`, `CTransaction`, `CBlockHeader` (pure data).
- **`src/txmempool.cpp` + `src/txn_validator.cpp`** — mempool + parallel transaction validator (PTV). `src/mining/journal*.cpp` is the journal that feeds the journaling block assembler (`src/mining/journaling_block_assembler.cpp`, now default; `src/mining/legacy.cpp` retained).
- **`src/net/`** — P2P layer. `net.cpp` is connection mgmt; `net_processing.cpp` is the protocol state machine; `net_message.cpp`/`stream.cpp` handle framing.
- **`src/rpc/`** — JSON-RPC surface (`server.cpp` registers tables from `blockchain.cpp`, `mining.cpp`, `rawtransaction.cpp`, etc.). `src/httpserver.cpp` + `src/httprpc.cpp` is the transport.
- **`src/wallet/`** — optional, gated on `BUILD_BITCOIN_WALLET`. BerkeleyDB-backed (`walletdb.cpp`, `db.cpp`).
- **`src/config/`** — generated `bitcoin-config.h` (CMake configure step writes build-time flags).
- **`src/secp256k1/`** — vendored libsecp256k1 (treat as third-party; do not edit in this repo).
- **`src/leveldb/`, `src/univalue/`, `src/crypto/ctaes/`** — other vendored deps.
- **`depends/`** — cross-compilation toolchain for reproducible builds (gitian), not needed for normal local builds.

Key cross-cutting patterns:

- **Activation heights**, not flags, gate new consensus behavior. When adding a TBC feature, add a height to `consensus::Params`, set it in every network in `chainparams.cpp`, and branch in `validation.cpp` / `script/interpreter.cpp` via an `IsFooEnabled(config, height)` helper.
- **`Config`** (`src/config.h`) is the canonical runtime config object, passed by const ref into virtually every validation function. Do not read globals; take `Config` in.
- **`CValidationState` / task cancellation** — validation returns rich state; long-running validations accept a `task::CCancellationToken` (`src/taskcancellation.h`).
- **Parallel validation**: the PTV (`txn_validator.cpp`) and checkqueue (`src/checkqueue.h`, `checkqueuepool.h`) drive concurrent script checks. Assume validation callbacks run on worker threads.

## Style & Conventions

- See `src/.clang-format` — 4-space indent, 80 cols, Cpp11 style, braces attached. Run the clang-format-diff script from `contrib/devtools/` on patches (see `doc/developer-notes.md`).
- CamelCase functions (`ValidateTransaction`), lowerCamelCase locals (`canDoThing`), `m_` prefix on class members, `UPPER_SNAKE_CASE` globals, `lower_snake_case` namespaces.
- Function names begin with a verb; variables are nouns / tensed verbs.
- Do not auto-refactor style of touched files — forward-porting from the Bitcoin-SV/Core upstreams relies on minimal drift.
- Release notes live per version in `doc/release-notes-v*.md`; add an entry when shipping user-visible changes.

## Running a Node Locally

After building, the typical dev loop:

```bash
./build/src/bitcoind -conf=<path>.conf -datadir=<path> -standalone    # -standalone for dev/test
./build/src/bitcoin-cli -conf=<path>.conf getinfo
```

A full reference config is in `INSTALL.md` (txindex, large mempool, journaling assembler).
