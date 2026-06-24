// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_CONTEXT_H
#define BITCOIN_NODE_CONTEXT_H

class Config;

namespace node {

//! Minimal node context. TBCNODE accesses chain/mempool/factory via globals; this
//! holds only what the Mining interface needs now. Grows as later sub-projects require.
struct NodeContext {
    const Config* config{nullptr};
};

} // namespace node

#endif // BITCOIN_NODE_CONTEXT_H
