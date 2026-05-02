# TBCNODE v3.3.0 Release Notes

## v2.6.1 cs_main 重构 — 并行验证

8 worker ChainDispatcher + 三锁原子帧 + chainstate seqlock + libcuckoo cacheCoins。
TPS 30 → 600+。共识 hash 跟 prod 100% 一致。

## 新增配置参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `-dispatcherworkers=N` | 8 | PerChainWorker 数；clamp [1, 64]。8 是甜点；16+ 实测反慢 |
| `-dispatchercommitgcms=N` | 5 | inflight COMMITTED 项 GC 延迟（ms）；clamp [1, 100]。慢 RPC 客户端 / 跨网络链路 RTT > 5ms 可上调 |

## 行为差异 / 运维须知

### AsyncSubscriber 队列满 drop-oldest

wallet / ZMQ subscriber 各自有 16384 队列。subscriber 消费慢 + ConnectBlock 单块 5 万 tx 时
**老 task 会被 drop**（覆盖 wallet 余额变更通知 / ZMQ inv）。设计权衡：节点优先 mempool 接受
+ 共识，不能让慢 subscriber 阻塞 ConnectTip 三锁帧。

**风险**：
- wallet 在该 tx commit 时刚好掉线 / 卡 DB 锁 → 余额可能滞后
- ZMQ 客户端漏 raw inv

**缓解**：
- 看节点日志 `WARNING: AsyncSubscriber[<name>] queue full` 出现频率
- 看节点日志 `WARNING SignalDispatcher: queue full, dropped oldest` 是否常态
- wallet 用户启动 `bitcoind` 时若发现日志有过 drop → `bitcoin-cli rescanblockchain` 修
- ZMQ 客户端自管缓冲，别假设节点会兜底

### worker queue 满 reject

`dispatcherworkers=8` 时总容量 8×16384 = 131072 in-flight tx。当前峰值 ~100 TPS × 30ms
= ~30 in-flight / worker，离上限 500×。

满了**会真 reject**（RPC 拿 -26 reject + "worker queue full"），不是假接受丢失。客户端
看见 reject 自己重提交即可。真满的场景一般是 worker 卡死（doubleCheck 死循环 / ConnectBlock
持锁 > 30s），需查上游问题。

### 已删 RPC

- ~~`getdevmetrics`~~ — 已删。内部计数器仍留在节点 LogPrintf 路径，不对外暴露。

## post-Teranode-audit 修复（4 agent 审核 + Teranode 实证对照后）

### HIGH（4/4 全修）

| # | 修法 | 文件 |
|---|---|---|
| H1 | Phase B 删 retry_threads 后台 thread 机制 → reorg-resubmit race 失败 push 回 g_reorg_stash | chain_dispatcher.* |
| H2 | gc/watchdog/drain `std::thread` 构造移入 try 块，防部分构造异常导致 "started but no threads" 僵尸态 | chain_dispatcher.cpp:208 |
| H3 | chainstate seqlock 7 字段全 atomic（uint256 拆 4× atomic<uint64_t>），消除 C++ 内存模型 UB；5 case seqlock test 含并发压测 OK | chainstate.{h,cpp} |
| H4 | Stop 路径 drain_cv.notify_all 包进 lock_guard(drain_cv_mtx)，跟 signal_dispatcher / async_subscriber 一致防 lost-wakeup | chain_dispatcher.cpp:322 |

### MEDIUM（5/7 修，2 改注释/留 future）

| # | 状态 | 修法 |
|---|---|---|
| M1 | ✅ | doubleCheck log 标签加 `status.GetRejectReason()`，运维分清真双花/缺父 vs 真 race |
| M2 | ✅ Phase A 删 ResubmitRateLimiter 整套（dead code） | tx_stash.{h,cpp} |
| M3 | ✅ sendrawtransaction `dispatcher_state.IsValid && !mempool.Exists` 时先 GetTransaction 验证，真在 chain 才回 ALREADY_IN_CHAIN，否则 RPC_TRANSACTION_ERROR 让 client 看到真问题 | rawtransaction.cpp:1361 |
| M4 | ✅ SubmitSync 加 `if (delta_us<0) { prev_us=now_us; continue; }` 跟 SubmitSyncState 对齐，防 clock 倒退污染 metric | chain_dispatcher.cpp:504 |
| M5 | ✅ 加 `LEVEL_COINS_CACHE_INTERNAL` 给 mCoinsViewCacheMtx，防未来反向新代码绕开 | lock_hierarchy.h |
| M6 | 📝 文档化 — 5 层 mutex 真 LOCK_LEVEL_TRACK 注册是 hardening 大改，留 future work；当前 2 层（smtx/batchWriteMtx）已强制 + 3 层文档化 | (注释) |
| M7 | 📝 文档化 — txn_validator processNewTransactionsAsynch 加 `EXCLUSIVE_LOCKS_REQUIRED(!smtx)` 注解需 cascade 改动，留 future work | (注释) |

### LOW（4/4 注释）

| # | 修法 |
|---|---|
| L1 | g_committed_gc_delay_us memory_order_relaxed → release/acquire（Init 写、worker GC 读） |
| L2 | AsyncSubscriber::Stop 第二个 lock_guard 加注释防误改 |
| L3 | WriterGuard 嵌套 depth 跟 owner CAS 协议设计 caveat 注释 |
| L4 | g_reorg_nesting_depth 单 cs_main 持锁线程前提注释 |

## Phase B (post-Teranode-audit) 退役项

参考 BSV Teranode `services/validator/` 的 `TX_LOCKED retry + exponential backoff`
模式审视后，决定**简化 dev v2.6.1 reorg 重试机制**：

| 退役 | 原因 |
|---|---|
| `ResubmitRateLimiter` (1000/s token bucket) | sub-A3 删 RaceStash drain 路径后无 caller，dead code |
| `RaceStash` (TxStash<100k, 5min>) | 同上 |
| `ChainDispatcher::retry_threads` 后台 thread + 指数退避 | 修 H1 retry_threads 无界增长 OOM 风险 |
| `MAX_RETRY_RACE=10` / `MAX_RETRY_HARD=1` / `BackoffUs` / `ResubmitReason` | 不再需要 retry budget |
| `WorkItem.retry_count` 字段 | 同上 |
| `src/validation/resubmit_strategy.h` 整个文件 | 同上 |

reorg-resubmit race 失败现在统一**push 回 `g_reorg_stash` 让 drain 路径下次取出来重派**
（drain 周期 ~500ms，是天然的延迟重试）。`g_reorg_stash` 200k 容量 + 10min TTL 兜底
防积压。

## 已修关键 bug（30+ 项摘要）

- handler_cb std::function 跨线程 race
- reorg-epoch RAII guard 嵌套（ref-count）
- spurious wait_for wakeup 假 stall WARNING
- queue_depth_current last-writer-wins / fetch_sub underflow
- async_subscriber Enqueue TOCTOU
- InvalidateBlock 缺 reorg-epoch guard
- sendrawtransactions g_connman null check
- ChainDispatcher Start 部分构造异常 leak gc/watchdog/drain
- signal_dispatcher Stop 漏清 queue 让 CTransactionRef 泄漏
- issue #12 RemoveForBlockNL descendant aggregate 漏减（dirty cache 让 journal 排序错乱
  → 矿工出 block 自家 ConnectBlock reject）
- Fix Y JournalingBlockAssembler::CreateNewBlock TopoSort 兜底（reorg PTV race 让
  ancestor 顺序错乱时矿工出错位 block）

## 后续兼容性

- 106 个老 RPC 0 改 0 删，reject_code 跟 prod 完全一致
- 共识规则、网络协议、chainparams、wallet 业务逻辑、挖矿/DAA 0 改
