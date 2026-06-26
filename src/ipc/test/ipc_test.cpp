// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// This file requires C++20 because it includes generated capnp proxy headers.
// It is compiled in the bitcoin_ipc_test STATIC library (CXX_STANDARD 20).
// A thin C++17 Boost.Test runner in ipc_tests.cpp calls SerializeRoundTripTest()
// without including any capnp headers.

#include <ipc/test/ipc_test.h>

#include <ipc/test/ipc_test.capnp.h>
#include <ipc/test/ipc_test.capnp.proxy.h>

#include <ipc/capnp/common-types.h>

#include <mp/proxy-io.h>
#include <mp/proxy.h>

#include <primitives/transaction.h>

#include <cassert>
#include <future>
#include <memory>
#include <thread>

void SerializeRoundTripTest()
{
    // Build a transaction to round-trip.
    CMutableTransaction mtx;
    mtx.nVersion = 10;
    mtx.nLockTime = 0;
    const CTransaction tx{mtx};

    // Set up an in-process Cap'n Proto pipe between a ProxyServer and ProxyClient.
    std::promise<std::unique_ptr<mp::ProxyClient<gen::IpcTestInterface>>> client_promise;
    std::unique_ptr<mp::ProxyClient<gen::IpcTestInterface>> client;

    std::thread loop_thread([&] {
        mp::EventLoop loop("SerializeRoundTripTest", [](mp::LogMessage) {});

        auto pipe = loop.m_io_context.provider->newTwoWayPipe();

        // Server side: serves IpcTestImplementation via IpcTestInterface.
        auto impl = std::make_shared<IpcTestImplementation>();
        auto server_conn = std::make_unique<mp::Connection>(
            loop, kj::mv(pipe.ends[0]),
            [&](mp::Connection& connection) {
                return kj::heap<mp::ProxyServer<gen::IpcTestInterface>>(impl, connection);
            });
        server_conn->onDisconnect([&] { server_conn.reset(); });

        // Client side: connects to the server.
        auto client_conn = std::make_unique<mp::Connection>(loop, kj::mv(pipe.ends[1]));
        auto ipc_client = std::make_unique<mp::ProxyClient<gen::IpcTestInterface>>(
            client_conn->m_rpc_system->bootstrap(mp::ServerVatId().vat_id)
                .castAs<gen::IpcTestInterface>(),
            client_conn.get(), /* destroy_connection= */ true);
        (void)client_conn.release();

        client_promise.set_value(std::move(ipc_client));
        loop.loop();
    });

    client = client_promise.get_future().get();

    // Exercise the round-trip: CustomBuildField serializes tx → Data,
    // CustomReadField deserializes Data → CTransaction on the server,
    // passTransaction echoes it back, and the reverse trip happens.
    CTransaction tx2 = client->passTransaction(tx);

    assert(tx2.GetHash() == tx.GetHash());

    // Tear down.
    client.reset();
    loop_thread.join();
}
