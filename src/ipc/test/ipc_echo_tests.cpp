// Copyright (c) 2025-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Thin C++17 Boost.Test runner for the Echo IPC round-trip.
// Does NOT include any capnp/proxy headers — the real work is in the C++20
// bitcoin_ipc_test library (ipc_echo_round_trip.cpp).

#include <ipc/test/ipc_echo_round_trip.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(ipc_echo_tests)

BOOST_AUTO_TEST_CASE(echo_round_trip)
{
    IpcEchoRoundTripTest();
}

BOOST_AUTO_TEST_SUITE_END()
