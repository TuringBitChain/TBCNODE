# P5 RPC 入口替换 + 废 PTV + 集成（实施指南）

**当前状态**：SCAFFOLD（指南 + 关键代码框架）。**真改 production RPC / PTV 路径留 P5 真集成**——本卡 dev session 不动 src/rpc/rawtransaction.cpp / src/txn_validator.cpp 的主路径。

---

## P5.1 sendrawtransaction / sendrawtransactions（保留逐笔语义）

**改造目标**（详 detailed-design §8.1 + §8.2）：

```cpp
// src/rpc/rawtransaction.cpp（实施时改）
UniValue sendrawtransaction(const Config& config, const JSONRPCRequest& request) {
    TxRef tx = DecodeHexTx(request.params[0].get_str());

    // P5.1 改：走 dispatcher.SubmitSync（含 IsBatchWriteInProgress 检测 → REJECT_OVERLOADED）
    CValidationState state = tbc::validation::g_dispatcher.SubmitSync(tx);

    if (state.IsValid()) return tx->GetId().ToString();
    if (state.GetRejectCode() == REJECT_OVERLOADED) {
        throw JSONRPCError(RPC_VERIFY_REJECTED, "server-busy: " + state.GetRejectReason());
    }
    throw JSONRPCError(RPC_TRANSACTION_REJECTED, FormatStateMessage(state));
}

UniValue sendrawtransactions(const Config& config, const JSONRPCRequest& request) {
    auto txs = DecodeHexTxArray(request.params[0]);
    // F7：保留逐笔语义（部分成功），不做 atomic
    std::vector<CValidationState> results;
    for (auto& tx : txs) results.push_back(g_dispatcher.SubmitSync(tx));
    return BuildBatchResult(results);
}
```

**前置条件**：dispatcher.SubmitSync 真实现（依赖 worker pool / cacheCoins / Chainstate 全接入）。

## P5.2 P2P SubmitAsync

```cpp
// src/net/net_processing.cpp（实施时改）
void ProcessTxMessage(CNode* pfrom, ...) {
    TxRef tx = ReadTxFromMessage(...);
    g_dispatcher.SubmitAsync(tx);   // 不阻塞 net thread
}
```

## P5.3 废 PTV

`src/txn_validator.cpp` 删除：
- `processValidation` 单笔/批量
- `threadNewTxnHandler` / `mStdTxns` / `mNonStdTxns` / `mProcessingQueue`
- `mMainMtx` / `mMainCV` / `mTxnsProcessedCV`

`src/net/validation_scheduler.h/cpp` 整个删除（TOPO_SORT 旧框架）。

## P5.4 orphan_txns / txn_recent_rejects 跟 dispatcher 集成

在 `g_dispatcher.MarkCommitted(txid)` 内部 trigger orphan replay：

```cpp
void ChainDispatcher::MarkCommitted(const TxId& txid) {
    // 现有 P2.1 状态机改 COMMITTED
    // P5.4 新增：trigger orphan replay
    auto children = g_orphan_txns->FindChildren(txid);
    for (auto& child_tx : children) SubmitAsync(child_tx);
}
```

## P5.5 submitrawtransactions 新 RPC（atomic 拓扑排序，C-C best-effort）

```cpp
// src/rpc/rawtransaction.cpp（新增）
UniValue submitrawtransactions(const Config& config, const JSONRPCRequest& request) {
    auto txs = DecodeHexTxArray(request.params[0]);
    try {
        // 入口拓扑排序（环 / 重复 → 整批拒）
        auto sorted = tbc::validation::TopoSort(std::move(txs));
        // batch-budget 30s 逐笔 commit best-effort
        return g_dispatcher.SubmitBatchSync(sorted);
    } catch (const tbc::validation::BatchTopoSortError& e) {
        throw JSONRPCError(RPC_INVALID_REQUEST, e.what());
    }
}

// waitformempoolentry — MED-3 给交易所同步语义
UniValue waitformempoolentry(const Config& config, const JSONRPCRequest& request) {
    TxId txid = ParseHashV(request.params[0], "txid");
    int timeout_ms = request.params[1].get_int();

    // 注册 g_signal_dispatcher subscriber，等 TransactionAddedToMempool 信号
    auto handle = g_signal_dispatcher.Subscribe(txid);   // 待加 Subscribe(TxId) 重载
    if (handle.WaitFor(std::chrono::milliseconds(timeout_ms))) {
        return UniValue(true);
    }
    throw JSONRPCError(RPC_MISC_ERROR, "timeout");
}
```

## P5.6 集成测试 + P3 末 full round-trip

实施 `tools/full-roundtrip.sh`（基于 P0.0b smoke 扩展）：
- vN 跑 1000 regtest 块（含完整 dispatcher / cacheCoins / seqlock / worker / signal_dispatcher）
- 关停 → v1 binary 启动同 datadir 验证 0 reindex
- 反向同样

---

## P5 KPI（GATE-M2）

| KPI | 类型 | 状态 |
|-----|------|------|
| sendrawtransaction 走 dispatcher 行为对比 v1 0 偏差 | 硬 | ⏳ 真集成 |
| sendrawtransactions 部分成功语义保留 | 硬 | ⏳ |
| P2P SubmitAsync net thread 不阻塞 | 软 | ⏳ |
| 老 PTV 文件删除 | 硬 | ⏳ |
| 3 层 orphan chain 父 commit replay | 硬 | ⏳ |
| submitrawtransactions atomic + best-effort 文档化 | 硬 | ✅ 本指南 |
| waitformempoolentry RPC 实现 | 硬 | ⏳ |
| full round-trip 双向 | 硬 | ⏳ |

---

## 已具备前置（dev session 完成）

- ✅ ChainDispatcher 状态机（P2.1）
- ✅ 路由策略 FindWorkerForChain / PickLeastLoadedWorker（P2.2）
- ✅ PerChainWorker 框架（P2.3）
- ✅ TxStash + token bucket（P2.4）
- ✅ TopoSort + 环检测（P2.5）
- ✅ doubleCheck 4 项协议（P3.1）
- ✅ Resubmit 双类策略（P3.2）
- ✅ ScopeGuard RAII rollback（P3.3）
- ✅ AsyncTrim（P3.4）
- ✅ REJECT_OVERLOADED 0x45（P3.5）
- ✅ GbtSnapshotProvider（P3.6）
- ✅ SignalDispatcher（P3.7）
- ✅ ConnectBlock 锁次序文档（P4.1）
- ✅ subscriber 反向锁审计（P4.5b）

---

## 真集成时业务方需做

1. dispatcher 接入 init.cpp（实例化 worker pool + Start）
2. RPC handler 改走 dispatcher
3. P2P ProcessTxMessage 改 SubmitAsync
4. 删 PTV 老代码
5. orphan_txns 集成 trigger replay
6. 跑 GATE-M2 KPI 矩阵
