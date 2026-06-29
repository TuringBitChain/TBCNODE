// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_IPC_TEST_IPC_TEST_H
#define BITCOIN_IPC_TEST_IPC_TEST_H

#include <primitives/transaction.h>
#include <script/script.h>

//! Simple implementation class used by the ipc_test.capnp IpcTestInterface.
//! passTransaction echoes a CTransaction back to the caller; the round-trip
//! exercises CustomBuildField / CustomReadField from common-types.h.
class IpcTestImplementation
{
public:
    CTransaction passTransaction(CTransaction tx) { return tx; }
};

//! Perform a CTransaction IPC serialization round-trip test via an in-process
//! Cap'n Proto pipe.  Declared in plain C++ (no capnp or proxy headers) so a
//! C++17 Boost.Test runner can call it without pulling in C++20 generated code.
void SerializeRoundTripTest();

//! Test unix-socket address canonicalization via ipc::MakeProcess()->connect().
//! Exercises ParseAddress for: bare "unix", "unix:<path>", oversized path
//! (throws std::invalid_argument), and unrecognized scheme (throws
//! std::invalid_argument).  Declared in plain C++ so the C++17 runner can call it.
void ParseAddressTest();

//! Test ipc::Protocol connect() and serve() methods end-to-end over a
//! socketpair.  Exercises the real CapnpProtocol serve/connect path, the Init
//! bootstrap, and the Echo makeEcho() round-trip.  Declared in plain C++ so
//! the C++17 Boost.Test runner can call it.
void IpcSocketPairTest();

//! Test ipc::Process bind() and ipc::Protocol listen()/connect() methods
//! over a real unix socket.  Exercises multiple addresses and multiple connects.
//! Declared in plain C++ so the C++17 Boost.Test runner can call it.
void IpcSocketTest();

//! Test overflow-safe timeout conversion: std::chrono::milliseconds ↔ Float64.
//! Verifies:
//!  - 5000ms round-trips to 5000ms (normal case).
//!  - node::BlockWaitOptions{} (timeout == milliseconds::max()) round-trips
//!    back to milliseconds::max(), NOT a negative/overflowed int64 value.
//! Declared in plain C++ so the C++17 Boost.Test runner can call it.
void TimeoutConversionTest();

class Config;

//! M3b acceptance test: serve interfaces::Mining over a real Cap'n Proto socket
//! via the Init::makeMining bootstrap, then drive create→solve-PoW→submitSolution
//! across the socket and assert the tip advances by one.
//!
//! Must be called from inside a TestChain100Setup fixture (chainActive +
//! g_miningFactory live).  config is the regtest GlobalConfig.
//! Uses the default OP_TRUE coinbase script (server-side; not pushed over IPC).
//! Declared in plain C++ (no capnp/proxy headers) so the C++17 Boost.Test
//! runner (ipc_tests.cpp) can call it.
void IpcMiningTest(const Config& config);

//! M3c acceptance test: serve interfaces::Mining over a REAL unix socket
//! established by interfaces::Ipc::listenAddress/connectAddress (not a
//! socketpair).  Drives the same create→solve-PoW→submitSolution flow as
//! IpcMiningTest, but over the real named-socket path.
//!
//! Must be called from inside a TestChain100Setup fixture (chainActive +
//! g_miningFactory live).  config is the regtest GlobalConfig.
//! GetDataDir() is used by listenAddress/connectAddress to resolve the socket
//! path, so the socket lands in the test datadir.
//! Declared in plain C++ (no capnp/proxy headers) so the C++17 Boost.Test
//! runner (ipc_tests.cpp) can call it.
void IpcListenMiningTest(const Config& config);

#endif // BITCOIN_IPC_TEST_IPC_TEST_H
