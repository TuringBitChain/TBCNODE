// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_IPC_TEST_IPC_TEST_H
#define BITCOIN_IPC_TEST_IPC_TEST_H

#include <primitives/transaction.h>

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

#endif // BITCOIN_IPC_TEST_IPC_TEST_H
