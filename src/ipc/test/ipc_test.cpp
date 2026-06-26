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

#include <ipc/process.h>
#include <fs.h>

#include <cassert>
#include <future>
#include <memory>
#include <stdexcept>
#include <system_error>
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

void ParseAddressTest()
{
    // Use a non-existent datadir so connect() always fails with ENOENT after
    // canonicalizing the address (never before).
    std::unique_ptr<ipc::Process> process{ipc::MakeProcess()};
    fs::path datadir{"/var/empty/notexist"};

    // Helper: returns true when the system_error is ENOENT (no-such-file).
    auto is_notexist = [](const std::system_error& e) {
        return e.code() == std::errc::no_such_file_or_directory;
    };

    // check_valid: address should be canonicalized to expect_address, then
    // connect() should throw std::system_error(ENOENT) because the socket path
    // doesn't exist.
    auto check_valid = [&](std::string address, const std::string& expect_address) {
        bool threw_notexist = false;
        try {
            process->connect(datadir, "test_bitcoin", address);
        } catch (const std::system_error& e) {
            threw_notexist = is_notexist(e);
        }
        assert(threw_notexist && "expected std::system_error ENOENT");
        assert(address == expect_address);
    };

    // check_invalid: address parsing itself should throw std::invalid_argument
    // with a message containing expect_substr.  Address must remain unchanged.
    auto check_invalid = [&](std::string address, const std::string& expect_addr_after,
                              const std::string& expect_substr) {
        bool threw_invalid = false;
        std::string what;
        try {
            process->connect(datadir, "test_bitcoin", address);
        } catch (const std::invalid_argument& e) {
            threw_invalid = true;
            what = e.what();
        }
        assert(threw_invalid && "expected std::invalid_argument");
        assert(what.find(expect_substr) != std::string::npos);
        assert(address == expect_addr_after);
    };

    // Bare "unix" resolves to <datadir>/test_bitcoin.sock.
    check_valid("unix",  "unix:/var/empty/notexist/test_bitcoin.sock");

    // "unix:" (empty path after colon) also resolves to <datadir>/test_bitcoin.sock.
    check_valid("unix:", "unix:/var/empty/notexist/test_bitcoin.sock");

    // "unix:path.sock" (relative) resolves to <datadir>/path.sock.
    check_valid("unix:path.sock", "unix:/var/empty/notexist/path.sock");

    // Oversized path: address is set before the length check fails, then
    // ParseAddress returns false and connect() throws std::invalid_argument.
    // The long-path case canonicalizes the address then fails the length check.
    check_invalid(
        "unix:0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000.sock",
        "unix:/var/empty/notexist/0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000.sock",
        "exceeded maximum socket path length");

    // Unrecognized scheme throws invalid_argument; address unchanged.
    check_invalid("invalid", "invalid", "Unrecognized address 'invalid'");
}
