// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P2.5: SubmitBatchSync 入口拓扑排序
//
// 目的：批量 RPC `submitrawtransactions` 的 tx 列表可能含父子链；
//       拓扑排序保证父先于子；环 / 重复 txid → 整批拒绝。
//
// Kahn 算法：
//   1. 建图：每个 tx 节点；input.prevout.txid 在 batch 内 → 有边 parent → child
//   2. 入度 0 的节点进 queue；逐个出 queue，把出边目标节点入度 -1，新入度 0 的进 queue
//   3. 跑完后所有节点都在结果列表 → 无环；否则 → 环

#ifndef BITCOIN_VALIDATION_TOPO_SORT_H
#define BITCOIN_VALIDATION_TOPO_SORT_H

#include "primitives/transaction.h"

#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tbc {
namespace validation {

class BatchTopoSortError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// 拓扑排序 — 抛 BatchTopoSortError 当：
//   - 重复 txid → "batch-duplicate-txid"
//   - 环 → "batch-cycle"
inline std::vector<CTransactionRef>
TopoSort(std::vector<CTransactionRef> txs) {
    // 重复 txid 检测
    std::unordered_set<TxId> seen;
    seen.reserve(txs.size());
    for (const auto& tx : txs) {
        if (!seen.insert(tx->GetId()).second) {
            throw BatchTopoSortError("batch-duplicate-txid");
        }
    }

    // 建索引：txid → 在 txs 中的下标
    std::unordered_map<TxId, size_t> idx_of;
    idx_of.reserve(txs.size());
    for (size_t i = 0; i < txs.size(); i++) {
        idx_of[txs[i]->GetId()] = i;
    }

    // 邻接表 + 入度
    std::vector<std::vector<size_t>> adj(txs.size());
    std::vector<int> indeg(txs.size(), 0);
    for (size_t i = 0; i < txs.size(); i++) {
        for (const CTxIn& in : txs[i]->vin) {
            auto it = idx_of.find(in.prevout.GetTxId());
            if (it != idx_of.end() && it->second != i) {
                // 父在 batch 内：有边 父 → 子
                adj[it->second].push_back(i);
                indeg[i]++;
            }
        }
    }

    // Kahn 算法
    std::vector<size_t> queue;
    queue.reserve(txs.size());
    for (size_t i = 0; i < txs.size(); i++) {
        if (indeg[i] == 0) queue.push_back(i);
    }

    std::vector<CTransactionRef> sorted;
    sorted.reserve(txs.size());
    size_t head = 0;
    while (head < queue.size()) {
        size_t i = queue[head++];
        sorted.push_back(txs[i]);
        for (size_t child : adj[i]) {
            if (--indeg[child] == 0) {
                queue.push_back(child);
            }
        }
    }

    // 残留 in-degree > 0 → 环
    if (sorted.size() != txs.size()) {
        throw BatchTopoSortError("batch-cycle");
    }

    return sorted;
}

} // namespace validation
} // namespace tbc

#endif // BITCOIN_VALIDATION_TOPO_SORT_H
