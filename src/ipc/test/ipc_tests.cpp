// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Thin C++17 Boost.Test runner for the IPC serialization round-trip test.
// Includes only ipc_test.h — no Cap'n Proto or proxy headers — so this file
// can be compiled as C++17 without the C++20 generated proxy code.

#include <ipc/test/ipc_test.h>

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(ipc_tests)

BOOST_AUTO_TEST_CASE(ipc_serialize)
{
    SerializeRoundTripTest();
}

BOOST_AUTO_TEST_SUITE_END()
