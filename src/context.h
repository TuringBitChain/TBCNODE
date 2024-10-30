// Copyright (c) 2019-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_CONTEXT_H
#define BITCOIN_NODE_CONTEXT_H

#include <atomic>
#include <cassert>
#include <cstdlib>
#include <functional>
#include <memory>
#include <vector>

class Mining;

//! NodeContext struct containing references to chain state and connection
//! state.
//!
//! This is used by init, rpc, and test code to pass object references around
//! without needing to declare the same variables and parameters repeatedly, or
//! to use globals. More variables could be added to this struct (particularly
//! references to validation objects) to eliminate use of globals
//! and make code more modular and testable. The struct isn't intended to have
//! any member functions. It should just be a collection of references that can
//! be used without pulling in unwanted dependencies or functionality.
class NodeContext {
public:
    static NodeContext& GetInstance() {
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);
        static NodeContext instance; // Guaranteed to be destroyed, instantiated on first use.
        return instance;
    }

    NodeContext(NodeContext const&) = delete;
    void operator=(NodeContext const&) = delete;

    std::unique_ptr<Sv2TemplateProvider> sv2_template_provider;
    std::unique_ptr<CTxMemPool> mempool;
    std::unique_ptr<Mining> mining;

private:
    NodeContext() = default;
    ~NodeContext() = default;
};

#endif // BITCOIN_NODE_CONTEXT_H
