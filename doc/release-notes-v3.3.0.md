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

满了**会真 reject**，不是假接受丢失。**v3.3.0 起 reject 类型为节点内部错误**（CValidationState
走 `Error()` 路径，reason="worker queue full"），跟"tx 永久 invalid"语义区分：

- RPC 客户端拿到的不是 `REJECT_INVALID` 永久失败，而是节点内部临时拥塞错误
- 单笔 `sendrawtransaction` → 抛 `RPC_TRANSACTION_ERROR`（不是 `RPC_TRANSACTION_REJECTED`）
- 批量 `sendrawtransactions` → invalid 数组中 reject_reason 带 `"node-internal: "` 前缀，
  让客户端能识别节点临时拥塞 vs tx 真无效
- 节点 / 队列拥塞类同语义：worker stopped / node shutting down / handler exception /
  dispatcher not started — 都走 `Error()` 路径，客户端可重试

真满的场景一般是 worker 卡死（doubleCheck 死循环 / ConnectBlock 持锁 > 30s），需查上游问题。

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

## v3.3.0 mempool ancestor cleanup

清理 BSV 1.0.5 RC 遗留的 4 个 cached ancestor aggregate 字段 + LegacyBlockAssembler，修
复 TBC ccf31423c 自加 `ancestorsHeight` 字段在 reorg 路径漏维护的潜在隐患。链 500 入池
时间 ~73ms → ~5ms（500× 减少 boost::multi_index reindex 操作）。

### Phase 0 — reorg 后 chain depth limit 检查变严（用户可感知）

`UpdateTransactionsFromBlock`（DisconnectBlock 回流路径）末尾加 `UpdateAncestorsHeightNL`
刷新 children 的 ancestorsHeight。修复前：reorg 后 children 的 height stale 偏小，
`-limitancestorcount` 检查偏松，能接受比 limit 更深的链。修复后：limit 严格执行。

**用户可见行为**：reorg 后某些 tx 提交可能突然被 reject 报 `too many unconfirmed
ancestors [limit: N]`。触发条件：reorg + 跨 reorg mempool 长链 + 踩 limit。

`-checkmempool=1` 模式下原本会偶发 `assert(ancestorsHeight == GetAncestorsHeight())`
炸的隐患同步修复。

### 重要：祖先策略语义变化（不是 bug，是设计选择）

**`-limitancestorcount` 含义改变**：从 v3.3.0 起，此参数的含义从"祖先**总数**"
改为"祖先**链深**"（最长 input → ancestor 路径长度）。这是 TBC ccf31423c 引入 +
v3.3.0 删除 cached aggregates 后**正式生效**的语义：

| 字段 / 限制 | v3.3.0 前 | v3.3.0 起 |
|---|---|---|
| `-limitancestorcount=N` 含义 | 祖先集合大小 ≤ N | 祖先链深 ≤ N |
| `-limitancestorsize=N` | 实际生效（祖先总字节 KB） | **不再校验**（参数保留兼容外部 caller，函数体不用） |
| `-limitdescendantcount=N` | 实际生效 | **不再校验** |
| `-limitdescendantsize=N` | 实际生效 | **不再校验** |

**实际策略影响**：

宽扇出场景（一笔 tx 多个 input 来自不同的低深度祖先）下，原先的"祖先总数"限制
会拒收，现在的"链深"限制可能放过。例：一笔 tx 引用 50 个独立父 tx（每个父 height=0）
→ 原 count=51 拒收，新 height=1 接受。

**安全考量**：

- 共识层 0 影响（这是 mempool policy）
- 矿工层有 `-blockmintxfee=500 sat/KB`（默认 0.5 sat/byte）独立屏蔽低 fee tx 上块
- 攻击者宽扇出灌池：仍受 `-maxmempool=1GB` 总量限制 + `MEMPOOL_FULL_FEE_INCREMENT`
  fee bumping 防护 + 网络层 `-connect=` / `-whitelist=` / iptables 防 P2P 污染
- 受影响功能测试：`bsv-mempool_ancestorsizelimit.py` 自 ccf31423c 起已 timeout 失败，
  v3.3.0 删除（pre-existing 僵尸测试）

**不是技术修复**，是 v3.3.0 ship 时接受的 breaking change。如果业务依赖"祖先总数"
或 "size/descendant 限制"严格生效，需要在产品 / 矿工配置层做对应防护。

### Phase 3 — LegacyBlockAssembler 删除

| 删除内容 | 影响 |
|---|---|
| `src/mining/legacy.{h,cpp}` 整套（~860 行） | TBC 默认 `JOURNALING`，`LEGACY` 未在功能测试 / 生产中使用 |
| `-blockassembler=LEGACY` CLI 选项 | 节点启动时 `factory.cpp` 抛 `"LEGACY assembler removed since v3.3.0; use -blockassembler=JOURNALING"`，配置错误明确报错而不是静默 fallback |
| `-printpriority` 调试选项 | Legacy-only mining priority 日志，删除（Journaling 不打印 priority） |
| `miner_tests_legacy` 单测 suite + `LegacyTestingSetup` | 跟 Legacy 一起删 |

**搬到 `src/mining/assembler.{h,cpp}` 保留**：`UpdateTime` + `IncrementExtraNonce` 是非
LegacyBlockAssembler-specific 的 block-construction 工具函数，被 RPC mining 端点 +
test_bitcoin 用，挪到 BlockAssembler 自然归属。

### Phase 1+2 — 删 4 cached ancestor aggregates + 5 处 sort 换 ancestorsHeight

**删除字段**（mempool entry 直接成员 + shared `AncestorDescendantCounts` struct）：
- `nCountWithAncestors`
- `nSizeWithAncestors`
- `nModFeesWithAncestors`
- `nSigOpCountWithAncestors`

每入池一个 tx 之前要 BFS 全祖先集，per-ancestor 调 `mapTx.modify(update_ancestor_state)`
触发 boost::multi_index 索引 reindex。链 500 时 ~3500 op，是入池 73ms 慢的根因。

**`AncestorDescendantCounts` struct 改造**（跨 mempool/mining 模块共享 shared_ptr）：
```cpp
struct AncestorDescendantCounts {
    std::atomic<size_t>  ancestorsHeight       {0};   // ← 从 mempool entry 直接成员搬入
    std::atomic_uint64_t nCountWithDescendants {0};   // 保留
};
```

**5 处 sort 用法换 sort key（`nCountWithAncestors` → `ancestorsHeight`）**：

| 位置 | sort key 旧 → 新 | 拓扑序保证 |
|---|---|---|
| `journal_change_set.cpp` REORG/RESET sort | `(nCountWithAncestors, score)` → `(ancestorsHeight, score)` | 父 height < 子 height 严格成立（数学保证父在子前） |
| `txmempool.cpp` `DepthAndScoreComparator` (`getrawmempool` 输出) | 同上 | 同上 |
| `txmempool.cpp` `topoSortedTxFromSet` (BlockMinTxFee 路径) | 同上（含两阶段 snapshot 改造） | 同上 |
| `txmempool.cpp` `CompareDepthAndScoreNL/CompareDepthAndScore` | **删除**（dead code, 0 caller） | n/a |

**snapshot-based sort（journal + topoSortedTxFromSet 两处）**：先 atomic.load() 进 local
immutable vector，再 stable_sort，最后按 orig_idx rebuild。消除 atomic 跨锁 sort 时
比较函数非传递性的 race 窗口（mempool.smtx 写者 + journal mMtx 读者并发场景）。

**行为差异（用户可感知）**：

| 项 | 影响 |
|---|---|
| `getrawmempool true` 输出 tx 顺序 | 同 height 内按 score tie-break，跟旧 `(count, score)` 在 diamond 拓扑下边界顺序略不同 |
| reorg 后 journal sort 的兄弟/无关 tx 间顺序 | 同上；**父子拓扑序数学严格保留**，不影响 block 合法性 / consensus / 矿工 ConnectBlock |
| `prioritisetransaction` 不再调整 descendants 的 ancestor fee 聚合 | JournalingBlockAssembler 用 `entry.GetModifiedFee()` 排序（不依赖 ancestor fee 聚合），prioritise 通过 `mModifiedFee` 直接生效，**实际 mining 排序行为不变** |

### Phase 4 — RPC breaking changes

`entryToJSONNL` + `EntryDescriptionString` 共享 helper 删 ancestor 字段。**4 个 RPC 受影响**：

| RPC | 删除字段 |
|---|---|
| `getrawmempool true` | `ancestorcount` / `ancestorsize` / `ancestorfees` |
| `getmempoolentry` | 同上 |
| `getmempoolancestors true`（每个 ancestor verbose 信息中）| 同上 |
| `getmempooldescendants true`（每个 descendant verbose 信息中）| 同上 |

**保留**：`descendantcount` / `descendantsize` / `descendantfees`（descendant 系字段）
+ `descendant_score` multi_index（仍被 TrimToSize evict 路径使用）。

### Phase 4 — Dead config / dead test 文档化

| 项 | 现状 |
|---|---|
| `-limitancestorsize` CLI 选项 | TBC ccf31423c 起 dead — `CalculateMemPoolAncestorsNL` 函数体不再使用此参数（只看 `ancestorsHeight` vs `limitAncestorCount`），仍保留在 API 签名 + CLI parser 兼容外部 caller 不报错 |
| `-limitdescendantcount` / `-limitdescendantsize` | 同上 dead config |
| `bsv-mempool_ancestorsizelimit.py` 功能测试 | **删除** — 自 ccf31423c 起一直 timeout 失败（验 `-limitancestorsize` 生效，但该 config 已 dead）。dev 跟 prod 树同样 86s/69s 失败确认 pre-existing 僵尸测试，跟 v3.3.0 改动无关 |
| `getmininginfo` 输出 `currentblocksize` / `currentblocktx` | 之前由 LegacyBlockAssembler 维护，删 Legacy 后 vestigial 全局变量留 0（JournalingBlockAssembler 不维护等价计数器）。Future 版本可决定 wire JBA 计数器或删 RPC 字段 |

### Phase 4 — `-checkmempool=1` invariant 覆盖度变化

| invariant | 改前 | 改后 |
|---|---|---|
| `ancestorsHeight == GetAncestorsHeight()` | reorg 后偶发炸（Site 0 bug） | ✓ Phase 0 修复后稳定 |
| `GetSizeWithAncestors() == BFS-recomputed nSizeCheck` | ✓ | ✗ 删除（cached 字段已删） |
| `GetSigOpCountWithAncestors() == BFS-recomputed nSigOpCheck` | ✓ | ✗ 删除 |
| `GetModFeesWithAncestors() == BFS-recomputed nFeesCheck` | ✓ | ✗ 删除 |
| 3 个 descendant cached invariant | ✓ | ✓ 保留 |

仅开发者使用 `-checkmempool=1` 模式调试时感知此变化。

### Out-of-scope（留 future cleanup）

- BSV master Step 4-5：`CPFPGroup` + Primary/Secondary mempool + `EvictionCandidateTracker`
  架构改写
- 删 3 个 descendant cached aggregate 字段 + `descendant_score` multi_index（保留，evict 路径仍在用）
- `mempool` `multi_index` 主索引降级（BSV master 标 `// FIXME: DEPRECATED`）
- `-limitancestorsize` 等 dead CLI 选项 + 函数 API 签名实际删除（外部 caller 多，需级联改）

## 后续兼容性

- 106 个老 RPC 0 改 0 删，reject_code 跟 prod 完全一致
- 共识规则、网络协议、chainparams、wallet 业务逻辑、挖矿/DAA 0 改
