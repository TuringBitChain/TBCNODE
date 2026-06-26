// Copyright (c) 2025-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_IPC_TEST_IPC_ECHO_ROUND_TRIP_H
#define BITCOIN_IPC_TEST_IPC_ECHO_ROUND_TRIP_H

//! Perform a real Echo IPC round-trip (serve + connect via in-process pipe,
//! cap'n proto RPC) and assert the echoed value survives the round-trip.
//! Declared in plain C++ (no capnp/proxy headers) so C++17 test runners can
//! call it without depending on the C++20 proxy layer.
void IpcEchoRoundTripTest();

#endif // BITCOIN_IPC_TEST_IPC_ECHO_ROUND_TRIP_H
