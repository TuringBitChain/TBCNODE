// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.

#include "util/scope_guard.h"

#include <stdexcept>
#include <boost/test/unit_test.hpp>

using tbc::MakeScopeGuard;

BOOST_AUTO_TEST_SUITE(scope_guard_tests)

// 正常路径：guard 析构调 lambda
BOOST_AUTO_TEST_CASE(normal_dispatch) {
    bool fired = false;
    {
        auto g = MakeScopeGuard([&] { fired = true; });
    }
    BOOST_CHECK(fired);
}

// Disarm 后 guard 析构不调
BOOST_AUTO_TEST_CASE(disarm_skips) {
    bool fired = false;
    {
        auto g = MakeScopeGuard([&] { fired = true; });
        g.Disarm();
    }
    BOOST_CHECK(!fired);
}

// H8: 析构里抛异常被吞，不 terminate
BOOST_AUTO_TEST_CASE(destructor_swallows_exception) {
    // 如果 guard 析构 throw 没被吞，进程会 std::terminate
    // 这里只要 BOOST_CHECK_NO_THROW 路径执行到说明没 terminate
    BOOST_CHECK_NO_THROW({
        auto g = MakeScopeGuard([] { throw std::runtime_error("test"); });
    });
}

// 异常 unwinding 中析构也调
BOOST_AUTO_TEST_CASE(unwinding_still_fires) {
    bool fired = false;
    try {
        auto g = MakeScopeGuard([&] { fired = true; });
        throw std::runtime_error("simulated");
    } catch (...) {}
    BOOST_CHECK(fired);
}

BOOST_AUTO_TEST_SUITE_END()
