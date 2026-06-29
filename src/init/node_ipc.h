// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef BITCOIN_INIT_NODE_IPC_H
#define BITCOIN_INIT_NODE_IPC_H

namespace interfaces { class Ipc; }

//! Set by the binary's main from its per-binary SetupNodeIpc(); read by AppInitMain/Shutdown.
//! nullptr for bitcoind (no IPC); non-null for bitcoin-node.
extern interfaces::Ipc* g_node_ipc;

//! Per-binary IPC setup: bitcoind returns nullptr; bitcoin-node constructs the node Init + Ipc
//! (and handles the -ipcfd spawned-server path) and returns its Ipc*.
interfaces::Ipc* SetupNodeIpc(int argc, char** argv);

#endif // BITCOIN_INIT_NODE_IPC_H
