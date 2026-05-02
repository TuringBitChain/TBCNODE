# cs_main 重构方案——概要设计

**版本**：v2.6.1（修订版，吸收八轮独立审核）
**目标读者**：业务方 + 项目决策方 + 核心开发
**核心目标**：让独立子孙链跨 RPC 客户端**真并行验证**，TPS 30 → 600+

> 详细设计见 `cs_main-refactor-detailed-design.md`
> 审核记录见 `cs_main-refactor-audit-log.md`
> 锁层级见 `lock-hierarchy.md`（P0.0a 启动同步交付）

> v2.6 相对 v2.5 的主要修订（来自第七轮独立审核，12 项新问题）：
> - **C-A**：`Chainstate::Snapshot` 增加 `const CBlockIndex* tip_index`（`CheckSequenceLocks` 等需要 BlockIndex 节点访问祖先），明文写入"BlockIndex 节点永不删除"不变量
> - **C-B**：`ConnectBlock` 锁次序改 `UpdateTip(seqlock 写)` 在 `view.Flush()` 之前，消除 phantom-tip 窗口（worker 阶段 1 拍旧 tip + 阶段 2 读新 UTXO 的共识 race）
> - **C-C**：`submitrawtransactions` 改"原子拓扑排序 + 逐笔 commit best-effort"语义，不承诺 ACID 回滚；改 batch-budget 30s 而非 per-tx 30s
> - **H-D**：Snapshot 增加 `genesisActivationHeight`；worker 阶段 3 显式根据 input.coinHeight 推 perInputScriptFlags
> - **H-E**：`TxStash` Drain 改消费式（取出即删）+ Drain/GC 锁顺序明确；100k/200k 容量加 metrics 告警
> - **H-F**：F2 try_lock 守卫改 RAII `std::unique_lock<…>(cs, std::try_to_lock)`，异常安全；P0.0a.5 加异常注入测试
> - **H-G**：`sync.h` 改造声明为非破坏性增量（默认 `LEVEL_DEFAULT`），现有 401 处 LOCK 调用点零修改；P0.0a.4 工时 +1 周
> - **H-H**：`P4.5b` subscriber 反向锁审计 2w → 4w；列出已知 7+ 处订阅点而非"待审计"
> - **M-I/J/K/L**：沉没窗口标注、人月 allocation 比例、shadow node mempool diff 阈值改 60s 累计 200 笔、plan 文件清单跟 detail 子任务对齐
>
> v2.5 相对 v2.4 的主要修订（来自第五轮独立审核）：
> - **F1**：新增 `REJECT_OVERLOADED = 0x45`，不再复用 `REJECT_TOOBUSY`（已被 net_processing.cpp:1194/1204/1608 用作 GETDATA 拒绝）
> - **F2**：单写者强制改用 clang TSA `EXCLUSIVE_LOCKS_REQUIRED(cs_main)` 编译期 + release build 也保留运行时 `try_lock` abort 守卫，不依赖只在 DEBUG_LOCKORDER 生效的 `AssertLockHeld`
> - **F3**：新增 `pcoinsTip` 由 `extern CCoinsViewCache*` 改 `shared_ptr` 的 lifetime 改造子任务（P0.4 +2 周）
> - **F4**：P0.0b 工时 4 周 → 6 周，共识等价性 baseline 改采样窗口（关键激活高度 ±1000 块 + 每 5000 块全量）
> - **F5**：v1↔vN binary round-trip 拆两次：P0.0b smoke（disk format 不破坏）+ P3 末 full（dispatcher 接入后真兼容性）
> - **F6**：reorg 独立队列上限改动态（按断链深度 × 平均块 tx 数 × 1.5），溢出落**专用 reorg-stash**（不污染 orphan_txns）
> - **F7**：`SubmitBatchSync` atomic 语义仅给新 RPC `submitrawtransactions`；老 `sendrawtransactions` 保留逐笔语义不 break
> - **F8**：AsyncTrim trade-off 写明"trim 持 unique_lock(smtx) 10-50ms，commit 路径相应短暂阻塞"
> - **F9**：GbtSnapshotProvider 改单 refresh worker + 合并队列，废 `std::thread().detach()` 爆炸模型
> - **F10**：P4.5 子系统验证 +2 周，新增 wallet/ZMQ/REST subscriber 反向锁顺序逐一审计
> - 总工期 17-22 月 → **18-23 月**（净 +6 周）

---

## 0. 一句话总结

> **节点内部加速改造**——重构验证调度模型（PTV → ChainDispatcher + Per-Chain Worker Pool + 细粒度锁 + 读写锁 BatchWrite 协调 + seqlock chainstate 元数据），让独立子孙链在多 CPU 核心上真并行验证。**18-22 个月，主开发 100% + reviewer/QA 各 30-50%（合计 26-40 人月），TPS 30 → 600（下限承诺），跟其他节点 100% 互通**。

> **简化策略**：TBC 开发网 ≡ 主网（共识规则 100% 一样，仅 chainparams 字段 netMagic / fork heights / seeds 不同），所以开发网 4 周稳定 = 真主网兼容性已验证；不再做 shadow node 4w + canary 4w + 渐进 4w，节省 9-12 周。

---

## 1. 背景

### 1.1 业务痛点

- 当前 TPS 30
- 多 RPC 客户端并发提交 → cs_main 整批持锁串行
- FT 长子孙链场景 → 单 worker 验证慢
- 物理上有 32 核 CPU，实际只用 1 核

### 1.2 根因

`processValidation` 整批持 `cs_main` + `mMainMtx`，跨 RPC 客户端互相串行——多核 CPU 闲着。

### 1.3 之前撤回的方案

| 版本 | 撤回原因 |
|-----|---------|
| v1.0–v1.2 | 全套 ChainstateManager，scope 不可控 |
| v1.3 | worker 自管短锁，跨线程死锁 |
| v1.4 | snapshot 模型，PTV 反转破坏 TOPO_SORT |
| v1.5 | snapshot 模型，batch 内父子查不到 |
| v3.0 | 缩小 cs_main，但 mMainMtx + cacheCoins 没改 |
| v2.0 | 三方审核暴露 9 项 CRITICAL，方向正确 |
| v2.1 | 二轮审核暴露 8 项 CRITICAL（含 v2.0 修复 bug） |
| v2.2 | 三轮审核暴露 12 项 CRITICAL，发现 BIP68 已禁可大幅简化 |
| v2.3 | 四轮审核暴露 4 项 seqlock 内存模型 + API 实现细节 CRITICAL |
| v2.4 | 五轮独立审核暴露 2 项事实错误（F1/F2）+ 3 项工时漏算（F3/F4/F10）+ 5 项设计未闭环（F5-F9）|
| v2.5 | F1-F10 落地 + 第六轮自审 N1-N8（漏改同步、boost spike、GBT 锁顺序、race-stash、版本对齐）；第七轮发现 C-A/B/C 共识 race + H-D/E/F/G/H + M-I/J/K/L |
| **v2.6** | **当前定稿**：v2.5 + Snapshot 加 tip_index/genesisActivationHeight + ConnectBlock 锁次序消除 phantom-tip + atomic 改 best-effort + try_lock RAII + sync.h 非破坏性增量 + subscriber 审计 +2w |

---

## 2. v2.6 核心设计

### 2.1 设计思想

**抛弃 snapshot 模型 + 抛弃 ChainContext**——worker 每次需要 chainstate 直接读 seqlock，验证完即销毁：

```
[ChainDispatcher 单例]
  接收 tx → 看 input 父在哪个 worker
    父在 worker N 的 in-flight 状态机（QUEUED/RUNNING/COMMITTED/ABORTED）→ 路由到 N（同链）
    父在 mempool / chain → 派发到最空闲 worker（独立链）
  批量 RPC 入口先做拓扑排序（防环 + 防重复 txid）

[Per-Chain Worker 1]   [Worker 2]   ...   [Worker N]
  worker_loop:
    1. snap = g_chainstate.Capture()    ← 无锁 seqlock，~30ns
    2. shared_lock(mempool.smtx) + shared_lock(batchWriteMtx)
       实时读 input UTXO（mempool 父 / libcuckoo / 二级 LRU / LevelDB）
    3. VerifyScript（30ms 真无锁纯 CPU）
    4. snap2 = g_chainstate.Capture()
       unique_lock(mempool.smtx)
       doubleCheck #1/#2/#3a/#3b/#4
       commit (AddUnchecked + dsDetector.Insert) + 真 RAII rollback
       MarkCommitted（in-flight 状态机）
    5. 异步信号 (g_signal_dispatcher) + 水位反压
```

### 2.2 锁设计（核心改动）

**全局锁层级**（强制，详见 `lock-hierarchy.md`，runtime AssertLockOrder + DEBUG_LOCKORDER 双保护）：

```
cs_main (boost::recursive_mutex, level 0)
  > mempool.smtx (std::shared_mutex, level 1)
    > pcoinsTip.batchWriteMtx (std::shared_mutex, level 2)
      > pcoinsTip.metaMtx (std::shared_mutex, level 3)
        > inflight_shard_mtx (std::shared_mutex, level 4)
          > worker.queue_mtx (std::mutex, level 5)
```

| 数据结构 | 锁 | worker 持锁方式 |
|---------|-----|---------------|
| **mempool 内部** | `mempool.smtx`（std::shared_mutex 已有）| 读 shared，commit unique |
| **pcoinsTip cacheCoins** | **libcuckoo 并发哈希表 + `batchWriteMtx`（块级原子保证）**| worker shared，ConnectBlock unique |
| **CoinsViewCache 元数据 (hashBlock)** | `metaMtx`（std::shared_mutex）| 短期持有 |
| **chainstate (tip_hash/script_flags/height/mtp)** | **seqlock + cs_main 单写者**（无新增 mutex）| 读者无锁 Capture，writer 持 cs_main |
| **mapBlockIndex / chainActive** | `cs_main` 保护（保留）| worker 不碰 |
| **in-flight 表** | **16-shard sharded shared_mutex**（day-1）| dispatcher + worker callback |

**worker 验证 + commit 路径完全不持 cs_main**——这是关键。

### 2.3 BatchWrite 协调（C3 解法 B：读写锁）

ConnectBlock 写一个块要 5-15 秒、5-10 万 UTXO 变更。libcuckoo 的 per-bucket 锁让外界看到"半新半旧"窗口，破坏块级原子。

**v2.4 选定解法 B（读写锁）**：

- `pcoinsTip` 加 `shared_mutex batchWriteMtx`
- worker `GetCoinConcurrent` 全程 `shared_lock(batchWriteMtx)`
- ConnectBlock BatchWrite 持 `unique_lock(batchWriteMtx)`，期间 worker 阶段 2/4 阻塞，阶段 3 不阻塞

**TPS 损失精确算术**（替换 v2.0 误述）：

| Worker 阶段 | 持锁 | BatchWrite 期间行为 |
|------------|------|-------------------|
| 1 拍 chain snap | 无锁 seqlock | 不阻塞 |
| 2 读 input UTXO | shared(mempool.smtx) + shared(batchWriteMtx) | **阻塞** |
| 3 VerifyScript | 无锁纯 CPU | **不阻塞**（已在跑的继续跑）|
| 4 commit | unique(mempool.smtx) | **阻塞** |

平均 TPS 损失：BatchWrite 5-15s × 出块间隔 600s ≈ **1-2.5%**。
reorg 6 块连续 KPI：TPS 下限 ≥ 200（M2 决策门）。

### 2.4 chainstate 元数据用 seqlock（v2.4 新设计，v2.5 单写者强制改造）

不再使用 `chainstate.cs` shared_mutex（v2.1/v2.3 设计）。元数据全部用原子+seqlock，writer 在 cs_main 持有期间单线程写：

- writer：`fetch_add(release) + atomic_thread_fence(release) + 写字段 + atomic_thread_fence(release) + fetch_add(release)`
- reader：`load(acquire) + atomic_thread_fence(acquire) + memcpy 读字段 + atomic_thread_fence(acquire) + load(acquire)`，seq 不一致重试

**好处**：
- worker 拍 snap 完全无锁（~30ns），没有"chainstate.cs 跟 cs_main 哪个更高"的争论
- 删除 v2.1 的 `chainstate.cs` 锁，整个锁层级简化一层
- 32 worker × 1000 TPS 不再争 cs_main

**F2 单写者强制（v2.5）**：`UpdateTip` 不再依赖 `AssertLockHeld(cs_main)`（仅 DEBUG_LOCKORDER 生效），改为：
1. 编译期：函数签名加 clang TSA `EXCLUSIVE_LOCKS_REQUIRED(cs_main)`
2. 运行时（release 也启用）：`if (!cs_main.try_lock()) std::abort();` + 立即 unlock 测探（已持锁的本线程递归 try_lock 返回 true，跨线程 try_lock 失败）。这种 trick 在 `boost::recursive_mutex` 上可行；详细设计 §1.1 给出完整代码

### 2.5 改动范围

**改造文件**（6 个）：

```
src/coins.h / coins.cpp           ← cacheCoins 改 libcuckoo + batchWriteMtx + metaMtx + GetCoinConcurrent
src/validation.h / validation.cpp ← 13+ 处 chainActive 替换为 g_chainstate.Capture()（含 tip_index/genesisActivationHeight）
                                    260 处 AST grep；ConnectBlock 锁次序改 UpdateTip 在 view.Flush() 之前（C-B）
src/txn_validator.cpp             ← processValidation 替换为 ChainDispatcher 入口
src/net/validation_scheduler.*    ← 废除（不再用）
src/init.cpp                      ← pcoinsTip 改 shared_ptr（F3）+ g_chainstate / g_dispatcher 初始化 + Shutdown 序重排
src/sync.h                        ← AssertLockOrder hook 进 EnterCritical/LeaveCritical（H-G 非破坏性增量：默认 LEVEL_DEFAULT，401 callsite 零修改；新 mutex 显式标 level）
```

**新增文件**（7 个）：

```
src/validation/chainstate.h         ← Chainstate (seqlock 元数据)
src/validation/chain_dispatcher.h   ← ChainDispatcher (in-flight 状态机 + 拓扑排序 + reorg 独立队列)
src/validation/per_chain_worker.h   ← PerChainWorker
src/validation/lock_hierarchy.h     ← LEVEL_* 常量
src/mempool/async_trim.h            ← 专用 trim worker（不阻塞 commit 链）
src/mining/gbt_snapshot.h           ← GbtSnapshotProvider (condvar 长轮询)
src/util/scope_guard.h              ← 真 RAII rollback (无条件 noexcept)
```

**完全不动**（90%+ 代码）：

- 共识规则（KYC、CHECKDATASIG、TuringTXID、FilledMinerBill、激活高度）
- 网络协议（P2P 消息格式、握手、INV/getdata）
- chainparams（fork 高度、netMagic、seeds）
- Wallet 业务逻辑（仅信号路径改异步分发）
- 挖矿 / DAA
- ConnectBlock 验证逻辑（只改锁协调）
- chainstate / mempool.dat / block file 磁盘格式
- BIP68（TBC Genesis 后已禁用 → CheckSequenceLocks 等价 `if (genesis) return true`）

### 2.6 跟其他节点的兼容性

| 项 | v2.5 后 |
|----|--------|
| 跟开发网其他节点互通 | ✅ 100% |
| 跟真主网节点互通 | ✅ 100%（重启即连）|
| chainstate disk format | ✅ 不变 |
| mempool.dat 格式 | ✅ 不变 |
| block file 格式 | ✅ 不变 |
| 共识 hash | ✅ 跟 reference 节点 100% 一致 |
| v1 binary 切回 | ✅ 重启即用，无需 reindex（P0.0b smoke + P3 末 full）|
| `sendrawtransactions` RPC | ✅ 逐笔语义保留，不 break；新增 `submitrawtransactions` 给 **ordered + topo-sorted, best-effort** 语义（非 ACID）|

**v2.5 只改个体节点的内部加速 — 跑得快的同款节点**。

---

## 3. 真实 trade-off（精确版）

### 3.1 真 trade-off（v2.6 修订为 7 项）

1. **18-22 个月开发周期**（v2.6.1 P6 简化后净 -3~-7 周；开发网 ≡ 主网消除 shadow/canary 12w 流程）
2. **26-40 人月投入**——主开发 100% × 21 月 + reviewer/QA 各 30-50% × 21 月（M-J 算术明示）
3. **高失败风险**——M-1（P0.0a/b 决策门）可能放弃整套，**11 周**沉没成本（5w P0.0a + 6w P0.0b，比 v2.5 +1 周）
4. **BatchWrite 期间 worker 阶段 2/4 阻塞 5-15 秒**——出块瞬间局部 TPS 跌，平均 TPS 损失 1-2.5%
5. **GBT 期间 condvar 长轮询最长 15s**——挖矿节点出块间隔可能局部拉长，正常出块周期内自然恢复
6. **AsyncTrim 跟 commit 路径互斥（F8）**——trim 持 unique_lock(smtx) 10-50ms 期间，新 commit 短暂阻塞；mempool 满载时累计可能 100-300ms
7. **`submitrawtransactions` 是 best-effort 不是 ACID（C-C 新增）**——前 N 笔 commit 成功、第 N+1 笔失败时不回滚已成功；客户端需做幂等处理

### 3.2 不存在的"trade-off"（已澄清的常见误解）

| 误以为是 trade-off | 实际情况 |
|------------------|---------|
| 跟其他节点不兼容 | 100% 互通 |
| 需要全网协调升级 | 单节点升级即生效 |
| 重启需要 reindex | chainstate format 不变，重启即用 |
| BSV 上游 cherry-pick 影响 | TBC 已自维护，不主动 cherry-pick |
| 共识有变化 | 共识规则一行不动 |
| BatchWrite 期间 worker 全部冻结 | 阶段 3 (VerifyScript) 不阻塞，仅阶段 2/4 阻塞 |
| 半新半旧 UTXO 视图 | 解法 B（读写锁）保证块级原子 |

---

## 4. 阶段总览（v2.6 调整后）

| 阶段 | 主题 | 工时 | 风险 |
|-----|------|------|------|
| **P0.0a** | libcuckoo soak / TSan / lock-hierarchy v0.1 / sync.h hook **非破坏性增量**（H-G） + boost::recursive_mutex try_lock spike + H-F 异常注入测试 | **5 周**（H-G +1） | **极高** |
| **P0.0b** | seqlock memory-model 文档 + 正确性证明 / BatchWrite p99 / baseline **采样窗口** / v1↔vN **smoke** round-trip / sign-off | 6 周（F4） | **极高** |
| **P0** | CCoinsViewCache 改造（libcuckoo + batchWriteMtx + insert 返回值 + GetCoinConcurrent + 二级 LRU + atomic 计数）+ **`pcoinsTip` shared_ptr lifetime sweep**（F3） | 14-18 周（+2 周） | **极高** |
| **P1** | 删除 ChainContext，worker 直读 seqlock（含 tip_index + genesisActivationHeight，C-A），260 处 chainActive AST grep（BIP68 直接 return true，省 4 周）| 4-6 周 | 中 |
| **P2** | ChainDispatcher（16-shard inflight 状态机 + 拓扑排序 + ReorgStash/RaceStash + token bucket）+ Per-Chain Worker pool | 8-12 周 | 中 |
| **P3** | worker 内 commit + 4 项 doubleCheck + perInputScriptFlags 推导（H-D）+ AsyncTrim 专用线程 + RPC busy(REJECT_OVERLOADED) + GBT 单 refresh worker | 6-8 周 | 中-高 |
| **P4** | ConnectBlock 切细粒度锁（**UpdateTip 在 view.Flush() 之前**，C-B）+ 子系统影响验证 + AssertLockOrder 全量验证 + **subscriber 反向锁审计 7+ 订阅点**（H-H 4w）| **16-20 周**（+2 周） | 高 |
| **P5** | RPC 入口替换（保留老 `sendrawtransactions` 逐笔 + 新增 `submitrawtransactions` **best-effort**，C-C）+ waitformempoolentry + 废除 PTV + wallet/ZMQ/REST/GBT 信号顺序 + P3 末 full round-trip（F5） | 4-6 周 | 中 |
| **P6** | 测试 + **开发网 4 周稳定**（核心兜底）+ 真主网直接部署 1 周观察（**简化**：开发网 ≡ 主网，无 shadow / 无 canary / 无渐进）+ mempool diff 监控 | **11-13 周** | 高 |

**总计 74-96 周 ≈ 18-22 个月**（3 人并行；P6 简化 -3~-7 周）

详细子任务和工时见 `cs_main-refactor-detailed-design.md`。

---

## 5. 决策门

### M-1a（启动 1 月后）：P0.0a 完成

- libcuckoo soak（5000 万 entry × 24h）零退化
- TSan / helgrind 24h 零 race
- `lock-hierarchy.md v0.1` 提交
- `AssertLockOrder` hook 进 sync.h，识别 boost::recursive_mutex 重入
- **boost::recursive_mutex try_lock 语义 spike（4 个 boost 版本）通过**——若失败 F2 单写者守卫需切换备选方案（`std::atomic<thread::id>` writer-owner）

### M-1b（启动 2.5 月后）：P0.0b 完成（v2.5：4 周 → 6 周）

- seqlock memory-model 文档（writer/reader fence 完整证明）
- BatchWrite p99 ≤ 200ms（10 万 UTXO 块）
- 共识等价性 baseline（**采样窗口**：关键激活高度 ±1000 块 + 每 5000 块全量；并发 4-8 实例 3-4 周完成，比 v2.4 1-2 周更现实）
- v1 ↔ vN binary **smoke** round-trip（仅验证 sync.h + lock-hierarchy 改造不破坏 disk format；P3 末再做 full round-trip 验证 dispatcher 接入后真兼容性）
- **业务方 sign-off**：接受 18-22 月工期 + TPS 600 下限 + `submitrawtransactions` best-effort 语义
- **决策门**：任一 KPI 不达标 → 整套放弃，沉没仅 11 周（5w P0.0a + 6w P0.0b）

### M0（启动 8-9 月后）：P0 完成

- CCoinsViewCache 并发改造完成
- TSan 72h 零 race（含 helgrind 交叉验证）
- **量化 KPI**：
  - 32 worker 并发 GetCoin 吞吐 ≥ 单线程 16×（无 BatchWrite 窗口）
  - 含 BatchWrite 窗口（10 min 周期、10 万 UTXO 块）≥ 8×
  - cachedCoinsUsage 1h 压测无漂移
- **决策门**：任一 KPI 不达标 → 整套放弃

### M1（启动 12-13 月后）：P1+P2 完成

- 260 处 chainActive AST grep 穷举确认
- ChainDispatcher（16-shard + 拓扑排序 + reorg 队列）+ Worker pool 集成
- 单元测试通过
- **决策门**：dispatcher 16-shard QPS < 1000 → 重新设计或放弃

### M2（启动 19-20 月后）：P3+P4+P5 完成

- 全套替换 PTV
- functional test 100% 通过
- 开发网部署
- **P3 末 full round-trip**：vN binary 跑 1000 regtest 块（含 dispatcher、worker、libcuckoo cacheCoins、seqlock chainstate）→ 关停 → v1 binary 启动同步无 reindex，反向同样
- **决策门**：开发网 4 周稳定 + TPS ≥ 600 + 重要 RPC p99 < 100ms + reorg 6 块 KPI ≥ 200 TPS + 共识等价性 4 周零 diff + full round-trip 通过 → 进真主网灰度

### M3（启动 23 月后）：P6 完成

- 真主网全网升级
- 实测 TPS 30 → 600+

---

## 6. 灰度策略（v2.6.1 简化版）

**简化逻辑**：开发网 ≡ 主网（共识规则 100% 一样，仅 chainparams 字段不同），开发网 4 周稳定 = 真主网兼容性已验证。

```
1. 单元测试 + functional test 100% 通过
2. TSan / ASan / helgrind 72 小时压测
3. reorg 注入 + 风暴测试
4. 共识等价性 baseline 全采样对比 100% 一致
5. **开发网部署 4 周（核心兜底）**
   - 至少 3 个 v2.6.1 + 至少 2 个 v1 节点混跑
   - chain tip + UTXO hash 跟 v1 100% 一致
   - 至少经历 3 次 reorg
   - mempool diff 监控双阈值 0 持久告警
   - TPS 实测 ≥ 600
6. **真主网直接全网部署 + 1 周观察**
   - Day 1 真回滚演练（备节点上做）
   - 1 周 0 panic / 0 fork
   - 重要客户端 0 兼容性异常
```

任一阶段共识 hash 跟其他节点不一致 → 立即执行 `mainnet-rollback.sh`（< 5 分钟全网回滚 v1）。chainstate format 兼容，无需 reindex。

---

## 7. 启动前必须做

1. **业务方 sign-off**：接受 18-22 月工期 + 26-40 人月（主开发 100% + reviewer/QA 各 30-50%）+ TPS 600 下限 + `submitrawtransactions` best-effort 语义
2. **人员到位**：1 主开发 + 1 reviewer + 1 QA（单人专注 2 年失败概率 > 50%）
3. **P0.0a + P0.0b spike**（共 11 周，v2.6 加 1 周给 H-G sync.h 非破坏性增量）：7 项 KPI（libcuckoo / TSan / BatchWrite p99 / baseline 采样窗口 / smoke round-trip / lock-hierarchy / boost::recursive_mutex try_lock 4 版本 spike + H-F 异常注入）→ 硬决策门
4. **`lock-hierarchy.md` v0.1 草案**（P0.0a 同步交付）：全局锁层级 + AssertLockOrder hook 实现 + DEBUG_LOCKORDER build 规则
5. **共识等价性 baseline**：主网历史块采样导入 + 每 1000 块 + 关键激活高度全量 `gettxoutsetinfo` 比对
6. **失败回滚演练**：v2.4 节点关停 → v1 binary 启动 → 链 tip + UTXO hash 一致 + 无 reindex
7. **子系统影响矩阵**：wallet / ZMQ / REST / GBT / reorg / orphan_txns / pruning / indexer 回归测试用例

---

## 8. 一行总结

> **节点内部加速重构 — 跟其他节点 100% 互通的"跑得快的同款节点"**。18-22 个月、26-40 人月、TPS 30 → 600（下限承诺）、独立子孙链跨 RPC 真并行、共识零修改、磁盘格式零修改、重启即用、老 RPC 不 break、`submitrawtransactions` best-effort（非 ACID）、开发网 4 周稳定即直接真主网部署（无 shadow / 无 canary）。

---

## 相关文档

- **概要设计**（本文）
- **详细设计**：[`cs_main-refactor-detailed-design.md`](./cs_main-refactor-detailed-design.md)
- **审核记录**：[`cs_main-refactor-audit-log.md`](./cs_main-refactor-audit-log.md)
- **锁层级**：`lock-hierarchy.md`（P0.0a 同步交付）
- **开发网纪律**：[`../learn/24-dev-mainnet-parity.md`](../learn/24-dev-mainnet-parity.md)
