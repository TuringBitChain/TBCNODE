// Copyright (c) 2011-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#define BOOST_TEST_MODULE Bitcoin Test Suite

#include "net/net.h"
#include "zmq/mempool_notifier_state.h"

#include <boost/test/unit_test.hpp>

std::unique_ptr<CConnman> g_connman;

const MempoolNotifierState* GetMempoolNotifierState()
{
    // Unit tests pass explicit state to the RPC implementations when enabled.
    return nullptr;
}

[[noreturn]] void Shutdown(void *parg) {
    std::exit(EXIT_SUCCESS);
}

[[noreturn]] void StartShutdown() {
    std::exit(EXIT_SUCCESS);
}

task::CCancellationToken GetShutdownToken()
{
    return task::CCancellationSource::Make()->GetToken();
}
