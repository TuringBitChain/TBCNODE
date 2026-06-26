// Copyright (c) 2025-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// This file requires C++20 because it includes generated capnp proxy headers.
// It is compiled in the bitcoin_ipc_test STATIC library (CXX_STANDARD 20).
// A thin C++17 Boost.Test runner in ipc_echo_tests.cpp calls IpcEchoRoundTripTest()
// without including any capnp headers.

#include <ipc/test/ipc_echo_round_trip.h>

#include <ipc/capnp/echo.capnp.h>
#include <ipc/capnp/echo.capnp.proxy.h>

#include <interfaces/echo.h>

#include <mp/proxy-io.h>
#include <mp/proxy.h>

#include <cassert>
#include <future>
#include <memory>
#include <string>
#include <thread>

void IpcEchoRoundTripTest()
{
    // Follows the mp::test::TestSetup pattern from
    // src/ipc/libmultiprocess/test/mp/test/test.cpp.
    //
    // Serves a ProxyServer<Echo> and connects a ProxyClient<Echo> in-process
    // over a kj two-way pipe (no OS sockets needed), then calls echo() and
    // asserts the result survives the round-trip.
    //
    // echo.capnp uses simple (no-Context) methods, so no ThreadMap exchange is
    // required. Context parameters belong to M3 when the full threading model
    // (with Mining interface) is introduced.

    std::promise<std::unique_ptr<mp::ProxyClient<ipc::capnp::messages::Echo>>> client_promise;
    std::unique_ptr<mp::ProxyClient<ipc::capnp::messages::Echo>> client;

    std::thread loop_thread([&] {
        mp::EventLoop loop("ipc_echo_round_trip", [](mp::LogMessage) {});

        // Create an in-process bidirectional pipe.
        auto pipe = loop.m_io_context.provider->newTwoWayPipe();

        // EchoImpl outlives the Connection (no-op deleter on the shared_ptr).
        auto echo_impl = interfaces::MakeEcho();
        interfaces::Echo* echo_raw = echo_impl.get();

        // -------- Server connection --------
        auto server_connection =
            std::make_unique<mp::Connection>(loop, kj::mv(pipe.ends[0]), [&](mp::Connection& connection) {
                return kj::heap<mp::ProxyServer<ipc::capnp::messages::Echo>>(
                    std::shared_ptr<interfaces::Echo>(echo_raw, [](interfaces::Echo*) {}),
                    connection);
            });
        server_connection->onDisconnect([&] { server_connection.reset(); });

        // -------- Client connection --------
        auto client_connection = std::make_unique<mp::Connection>(loop, kj::mv(pipe.ends[1]));
        auto echo_client = std::make_unique<mp::ProxyClient<ipc::capnp::messages::Echo>>(
            client_connection->m_rpc_system->bootstrap(mp::ServerVatId().vat_id)
                .castAs<ipc::capnp::messages::Echo>(),
            client_connection.get(), /* destroy_connection= */ true);
        (void)client_connection.release();

        client_promise.set_value(std::move(echo_client));
        loop.loop();
    });

    client = client_promise.get_future().get();

    // Call echo() over the IPC connection — the real Cap'n Proto round-trip.
    std::string result = client->echo("echo test");
    assert(result == "echo test");

    // Destroy client (triggers capnp destroy + connection teardown → EventLoop exits).
    client.reset();
    loop_thread.join();
}
