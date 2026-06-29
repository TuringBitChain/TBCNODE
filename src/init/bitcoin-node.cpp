// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#include <init/node_ipc.h>

#include <config.h>
#include <interfaces/echo.h>
#include <interfaces/init.h>
#include <interfaces/ipc.h>
#include <interfaces/mining.h>
#include <node/context.h>

#include <memory>

namespace {
class BitcoinNodeInit : public interfaces::Init
{
public:
    BitcoinNodeInit(int argc, char** argv)
        : m_ipc{interfaces::MakeIpc("bitcoin-node", argc > 0 ? argv[0] : "", *this)} {}

    std::unique_ptr<interfaces::Mining> makeMining() override
    {
        m_node.config = &GlobalConfig::GetConfig();
        return interfaces::MakeMining(m_node);
    }
    std::unique_ptr<interfaces::Echo> makeEcho() override { return interfaces::MakeEcho(); }
    interfaces::Ipc* ipc() override { return m_ipc.get(); }
    bool canListenIpc() override { return true; }
    const char* exeName() override { return "bitcoin-node"; }

    node::NodeContext m_node;
    std::unique_ptr<interfaces::Ipc> m_ipc;
};
} // namespace

interfaces::Ipc* SetupNodeIpc(int argc, char** argv)
{
    static BitcoinNodeInit init{argc, argv};
    // If launched as an IPC server child (-ipcfd N), serve and signal exit; else fall through to listen.
    int exit_status;
    if (init.m_ipc->startSpawnedProcess(argc, argv, exit_status)) {
        std::exit(exit_status);
    }
    return init.m_ipc.get();
}
