# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

@0xd71b0fc8727fdf83;

using Cxx = import "/capnp/c++.capnp";
$Cxx.namespace("gen");

using Proxy = import "/mp/proxy.capnp";
$Proxy.include("ipc/test/ipc_test.h");
$Proxy.includeTypes("ipc/capnp/common-types.h");

interface IpcTestInterface $Proxy.wrap("IpcTestImplementation") {
    passTransaction @0 (tx :Data) -> (result :Data);
}
