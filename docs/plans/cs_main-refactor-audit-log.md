## cs_main 重构方案——审核记录

**版本**：v2.6.1（修订版）
**审核范围**：v1.0 → v3.0 → v2.0 → v2.1 → v2.2 → v2.3 → v2.4 → v2.5 → v2.6 → v2.6.1 全套设计演进
**审核维度**：事实错误 / 死锁 / Race / 性能 / 共识 / 兼容性

> 概要设计见 `cs_main-refactor-plan.md`
> 详细设计见 `cs_main-refactor-detailed-design.md`

---

## 0. 总览

本次重构方案历经 **11 个版本**，每版都被独立审核打回，最终落定 v2.5。

| 版本 | 核心思路 | 撤回 / 修订原因 |
|-----|---------|---------|
| v1.0–v1.2 | 全套引入 ChainstateManager（参考 Bitcoin Core 22+） | 工作量超出可承受范围（30+ 月），scope 不可控 |
| v1.3 | worker 自管 cs_main 短锁 | 跨线程 LOCK 在 recursive_mutex 上死锁 |
| v1.4 | snapshot 拍 ChainContext + 主线程串行 commit + PTV 反转 | PTV 反转破坏 TOPO_SORT |
| v1.5 | snapshot 拍 ChainContext + worker 内 commit | 同 batch 内父子链 snapshot 看不到父 |
| v3.0 | 缩小 cs_main + 复用现有 PTV | mMainMtx + cacheCoins 没改，跨 RPC 仍串行 |
| v2.0 | Per-Chain Worker + cacheCoins 并发哈希表 + 细粒度锁 + 4 项 doubleCheck | 一轮三方审核暴露 9 项 CRITICAL |
| v2.1 | v2.0 + 读写锁 BatchWrite + 16-shard inflight + 异步 trim | 二轮三方审核暴露 8 项 CRITICAL（含 v2.0 修复 bug） |
| v2.2 | v2.1 + SnapshotForDoubleCheck + dsDetector 扩展 + BIP68 1024 数组 + busy-503 | 三轮三方审核暴露 12 项 CRITICAL，发现 BIP68 已禁可大幅简化 |
| v2.3 | v2.2 - BIP68 数组 - dsDetector 扩展 + seqlock chainstate + reorg 独立队列 | 四轮三方审核暴露 4 项 seqlock 内存模型 + API 实现细节 CRITICAL |
| v2.4 | v2.3 + 显式 atomic_thread_fence + libcuckoo insert 返回值 + 删除 ChainContext + REJECT_TOOBUSY + GBT condvar 长轮询 + waitformempoolentry | 五轮独立审核暴露 2 项事实错误 + 3 项工时漏算 + 5 项设计未闭环 |
| v2.5 | v2.4 + REJECT_OVERLOADED 独立码 + clang TSA 单写者 + pcoinsTip shared_ptr + 采样窗口 baseline + round-trip 拆两次 + reorg 动态上限+stash + atomic 批 RPC 拆新接口 + AsyncTrim trade-off + GBT 单 worker + subscriber 反向锁审计 + 第六轮自审 N1-N8 | 第七轮独立审核暴露 3 项 CRITICAL（C-A/B/C）+ 5 项 HIGH（H-D/E/F/G/H）+ 4 项 MED/LOW（M-I/J/K/L）|
| v2.6 | v2.5 + Snapshot tip_index/genesisActivationHeight + ConnectBlock 锁次序 + atomic best-effort + try_lock RAII + sync.h 非破坏性增量 + subscriber 审计 +2w | 第八轮独立审核暴露 2 项 CRITICAL 共识 race（H-D MEMPOOL_HEIGHT 漏 + C-B Flush 失败回滚）+ 1 HIGH（BlockIndex 不变量过强）+ 2 MED（doc stale）|
| **v2.6.1** | **v2.6 + perInputScriptFlags 复刻 GetInputScriptBlockHeight + Flush 失败显式 abort + BlockIndex 不变量收紧 steady-state-only + RPC matrix/sunk-cost 文档对齐** | **当前定稿** |

---

## 1. v2.0 之前的审核发现

### 1.1 v1.3 致命问题（worker 自管短锁）

**问题**：worker 内自己 `LOCK(cs_main)` 拍数据快照
**审核结论**（cpp-reviewer）：
- `cs_main` 是 `boost::recursive_mutex`，跨线程 LOCK 不能复用主线程已持锁
- 主线程持有 cs_main → 启 worker → worker 试图 LOCK → 死等
- 单 RPC 路径根本没 worker 池可用

**结果**：v1.3 撤回。

---

### 1.2 v1.4 致命问题（PTV 反转破坏 TOPO_SORT）

**问题**：v1.4 把 PTV 改成"snapshot 阶段不阻塞，commit 阶段串行"
**审核结论**（architect + cpp-reviewer）：

```
TBC commit 1caf096c3（2026-02-25）引入 ValidationScheduler，
默认策略 TOPO_SORT，依赖"父先 commit、子后 commit"的语义。

v1.4 反转后：
  worker A 拍 snapshot 父
  worker B 拍 snapshot 子（父尚未 commit）→ snapshot 看不到父
  → 子被拒为 Missing inputs
```

**结果**：v1.4 撤回。

---

### 1.3 v1.5 致命问题（snapshot 父子盲点）

**问题**：v1.5 让 worker 在 snapshot 上做完验证后自己 commit
**审核结论**（cpp-reviewer）：

```
worker A 拿 tx_father（input UTXO 在 chain）
  → 拍 snapshot S1
  → snapshot S1 = chain UTXO + mempool（不含 tx_father 的 child）
  → 验证 + commit_father

worker B 拿 tx_child（input = tx_father.vout[0]）
  → 几乎同时拍 snapshot S2
  → S2 也是 "chain UTXO + mempool"，但 tx_father 还没 commit 进 mempool
  → S2 看不到 tx_father → tx_child 被拒
```

**根因**：snapshot 模型在"父子相邻"场景下天然有盲点，不管怎么 retry 都会丢一拍。

**结果**：v1.5 撤回。

---

### 1.4 v3.0 致命问题（mMainMtx 没改）

**问题**：v3.0 思路是"缩小 cs_main 作用域 + 复用现有 PTV"，听起来最省事
**审核结论**（architect）：

```
src/txn_validator.h:278   mutable std::shared_mutex mMainMtx{};
src/txn_validator.cpp:147  unique_lock<shared_mutex>(mMainMtx)  // sync 路径
src/txn_validator.cpp:225  unique_lock<shared_mutex>(mMainMtx)  // async 路径

processValidation 整批拿 unique_lock(mMainMtx) → sync RPC 调用之间互斥。

即使 cs_main 缩小了，mMainMtx 没动，跨 RPC 客户端依然串行。
```

**根因**：v3.0 只动 cs_main，没动 mMainMtx，本质上没解决"跨 RPC 串行"问题。

**结果**：v3.0 撤回，进入 v2.0 全面重构。

---

## 2. v2.0 设计过程中的事实错误更正

v2.0 早期设计假设了 5 个**错误的代码事实**，被 architect/security-reviewer/cpp-reviewer 审核打回，逐一更正：

### 2.1 错误 1：`pcoinsTip` 没有锁

**早期假设**：pcoinsTip 完全无锁，需要新加 shared_mutex
**事实**（src/coins.h:329）：

```cpp
mutable std::mutex mCoinsViewCacheMtx{};
```

CCoinsViewCache 已经自带 std::mutex 保护 cacheCoins。**改造方向不是"加锁"而是"换并发数据结构"**。

---

### 2.2 错误 2：`GetCoin` 是只读

**早期假设**：worker 内 `GetCoin` 只读 UTXO，可以无锁并发
**事实**（src/coins.cpp:79–101）：

```cpp
CCoinsMap::iterator FetchCoinNL(...) {
    ...
    auto it = cacheCoins.emplace(...);  // 物理写入
    ...
}
```

**`GetCoin` 内部走 `FetchCoinNL` lazy fill，物理写 cacheCoins**。

→ "无锁并发读"在原设计里直接 UB（多 worker 同时 emplace 同一 key）。
→ 这是 v2.0 必须把 cacheCoins 换成并发哈希表（libcuckoo）的原因。

---

### 2.3 错误 3：`ConnectBlock` 直接写 pcoinsTip

**早期假设**：ConnectBlock 写 pcoinsTip 时有 cs_main 罩着，不需要协调
**事实**（src/validation.cpp 4032+）：

```cpp
ConnectBlock 内：
  CCoinsViewCache view(pcoinsTip.get());  // 临时 view
  ...
  view.Flush();  // BatchWrite 到 pcoinsTip
```

**ConnectBlock 写的不是 pcoinsTip 本身，是临时 view + BatchWrite**。

→ ChainDispatcher 协调 worker 时，需要在 BatchWrite 前后专门处理 cacheCoins 的并发安全。
→ v2.0 详细设计里 §5"ConnectBlock 协调"专门覆盖此 race。

---

### 2.4 错误 4：`TxnValidation` 只读 chainActive 1–2 处

**早期假设**：TxnValidation 只在头部读一次 chainActive.Tip()
**事实**（src/validation.cpp 1515+，TxnValidation 函数体）：

```
chainActive.Height()       直接读，>=8 处
chainActive.Tip()->...     直接读，>=5 处
GetMedianTimePast(...)     间接读 chainActive
IsXxxEnabled(... height)   间接读 chainActive
```

**总数 13+ 处直接 chainActive 读，加上间接读 200+ 处**。

→ ChainContext 必须一次性把所有需要的字段全拍下来，worker 只读 ctx，不再回头读 chainActive。
→ v2.0 P1 阶段 200+ 处替换的工作量来源。

---

### 2.5 错误 5：`AddUncheckedNL` 50µs

**早期假设**：mempool commit 50µs，可以在 worker 内做
**事实**（基于 mempool 多索引 + 多 hash + journal 写入实测）：

```
AddUncheckedNL：
  - 计算 6+ 个 ordered/hashed index 的 key
  - 写 mapTx, mapLinks, journal
  - 触发 mining notification

实测中位数 1–5ms（FT 长链场景），P99 可达 10ms。
```

→ worker 内持 unique_lock(mempool.smtx) 1–5ms 是可接受的（远小于验证 30ms），但要避免在 lock 内做不必要的工作。
→ doubleCheck 拆成"无锁阶段"和"持锁阶段"。

---

## 3. v2.0 当前设计的 Race 审核

### 3.1 由 cpp-reviewer / security-reviewer 列出的 race 一览

| Race | 触发场景 | v2.0 处理 |
|------|---------|---------|
| **R1**：worker 拍 ctx 和 ConnectBlock 切 tip 之间 | ctx 拍完 → tip 切了 → tx 在旧 tip 上验证 | **doubleCheck #1**：commit 前 `ctx.tip_hash == chainActive.Tip()->GetBlockHash()`，不等则 Resubmit |
| **R2**：worker A 验证 tx_father, worker B 验证 tx_child（同链） | 父尚未 commit，子查不到父 | **dispatcher 路由**：父在 worker A 的 in-flight 队列时，子被路由到 A，A 串行处理 |
| **R3**：两个 worker 同 spend 同一 UTXO（独立链） | 跨链双花 | **doubleCheck #3**：commit 前 unique_lock(mempool.smtx) 后再 `mempool.HaveCoin(input)`，已花则 reject |
| **R4**：activation height 在 worker 验证期间被切 | 极端情况：tx 用旧 script_flags 验证，提交时已激活新 flags | **doubleCheck #2**：commit 前 `ctx.script_flags == 当前 GetScriptFlags(tip)`，不等则 Resubmit |
| **R5**：mempool.AddUnchecked 之间 race | 两 worker 同时 emplace 同一 txid | mempool.smtx unique_lock 串行化（已有保护） |
| **R6**：mempool 的 parent 在 worker 验证期间被驱逐 | tx 验证用 parent，parent 因为 fee 被 evict | **doubleCheck #4**：commit 前 `mempool.exists(parent_txid)`，不存在则 reject |
| **R7**：cacheCoins 并发 emplace 同 key | 多 worker FetchCoin 同一 UTXO | **libcuckoo**：upsert 原子，并发安全 |
| **R8**：ConnectBlock BatchWrite 期间有 worker 读 cacheCoins | reorg 中 UTXO 半新半旧 | **§5 协调**：BatchWrite 期间 dispatcher 暂停 worker 派发 |
| **R9**：dispatcher 路由判断和 worker 完成 commit 之间 | 父刚 DONE，dispatcher 已派发到错误 worker | **in-flight 表 shared_mutex 保护**：路由查询和 worker callback 序列化 |
| **R10**：reorg 期间 worker 处理 tx 用了已被 disconnect 的 UTXO | reorg 切链，旧链 tx 失效 | **doubleCheck #1 tip_hash**：reorg 后 tip 变，全部 Resubmit |

---

### 3.2 doubleCheck 的 4 项归纳

| # | 检查项 | 失败处理 |
|---|------|---------|
| 1 | `ctx.tip_hash == 当前 chainActive.Tip()` | Resubmit（重新 dispatch） |
| 2 | `ctx.script_flags == 当前 GetScriptFlags(tip)` | Resubmit |
| 3 | `mempool.HaveCoin(每个 input)` 不存在双花 | reject TX_MEMPOOL_POLICY |
| 4 | `mempool.exists(每个 mempool parent)` | reject Missing inputs |

**所有 4 项都在 worker 拿 unique_lock(mempool.smtx) 之后立刻执行**，确保检查到 commit 之间无空隙。

---

## 4. 共识等价性审核

### 4.1 共识规则改动评估

**审核结论**（security-reviewer + architect 联合）：

| 模块 | 改动 |
|-----|-----|
| KYC 验证 | ❌ 不动 |
| schnorrMultisig 激活 | ❌ 不动 |
| CHECKDATASIG / TuringTXID | ❌ 不动 |
| FilledMinerBill v2 | ❌ 不动 |
| script interpreter | ❌ 不动 |
| ConnectBlock 验证逻辑 | ✅ 只改锁协调（§5），不改验证语义 |
| Coinbase 校验 | ❌ 不动 |
| Difficulty / DAA | ❌ 不动 |

**v2.0 共识规则零修改**——仅改"调度和锁"。

---

### 4.2 共识等价性验证机制（强制）

```
启动前：
  对 origin/main 跑全套 RPC 历史回放，记录 chain tip + UTXO hash

P3 阶段后：
  在同一历史数据上跑 v2.0 实现，对比 chain tip + UTXO hash
  → 必须 100% 一致才能进 P4

P6 灰度：
  shadow node 模式：v2.0 节点连主网，不出块，每 10 块对比 UTXO hash
  → 4 周内任何一次不一致 → 立刻回滚
```

---

## 5. 节点互通性审核

### 5.1 跟其他节点的兼容性

**审核结论**：v2.0 是**纯节点内部加速**，跟其他节点 100% 互通。

| 项 | v2.0 后 |
|----|--------|
| P2P 协议 | ✅ 不变 |
| 消息格式 / netMagic | ✅ 不变 |
| chainstate 磁盘格式 | ✅ 不变 |
| mempool.dat 格式 | ✅ 不变 |
| block file 格式 | ✅ 不变 |
| RPC 接口语义 | ✅ 不变 |
| ZMQ topic | ✅ 不变 |
| 共识 hash | ✅ 跟 reference 节点 100% 一致 |

**v2.0 节点重启后立刻能连上开发网/真主网，无需任何外部协调**。

---

### 5.2 cherry-pick BSV 上游的兼容性

**早期误述**：v2.0 会"彻底放弃 cherry-pick"
**事实**（基于 git log 审核）：

```
TBC 自 commit 72cf010ff 起独立维护，
最近 1 年（2025–2026）没有从 BSV 上游 cherry-pick 任何 commit。
```

→ TBC 实际**不依赖 cherry-pick**，所谓"破坏 cherry-pick 兼容"是无意义的代价。
→ v2.0 plan 已删除该误述。

---

## 6. 性能审核

### 6.1 性能瓶颈定位

**审核工具**：架构 review + 代码 trace

| 路径 | 当前耗时 | v2.0 后 |
|-----|---------|--------|
| 单 tx 验证（30ms 纯 CPU） | 30ms | 30ms（不变）|
| 单批 RPC（10 tx）整批持锁 | 300ms 整批串行 | 30ms（10 worker 并发）|
| 跨 RPC 客户端 | 完全互斥 | 真并行 |
| FT 长链 100 tx 单子链 | 3000ms 整批串行 | 3000ms（同链必串行，符合预期）|
| FT 50 子链 × 20 tx | 30000ms | 600ms（50 worker 并发）|

**TPS 30 → 1000+ 的算术**：32 核 × 单核 30 tps × 0.7（dispatcher + 锁开销）≈ 670 tps（保守）/ 1500 tps（FT 极优）

---

### 6.2 dispatcher 是否成新瓶颈

**审核结论**：dispatcher 单线程做 in-flight 查询和路由，理论吞吐 50000 ops/s（hash 表 lookup 20µs）。

→ **决策门 M1**：dispatcher 实测 QPS < 200 → 拆分 dispatcher 或改为 sharded design。
→ 详细设计 §2 留有 sharding 扩展接口。

---

## 7. 失败回滚预案

### 7.1 决策门触发的回滚（v2.6）

| 门 | 失败标志 | 回滚动作 |
|---|---------|---------|
| **M-1a**（P0.0a 完成，5 周，v2.6 H-G +1 周）| libcuckoo 5000 万 entry 性能退化 / TSan 出 race / lock-hierarchy 实现失败 / sync.h 非破坏性增量编译期破坏 401 callsite / boost::recursive_mutex try_lock spike 失败 / H-F 异常注入泄漏 | 整套 v2.6 放弃，沉没 5 周 |
| **M-1b**（P0.0b 完成，11 周）| seqlock memory-model 证明失败 / BatchWrite p99 > 200ms / baseline 采样窗口失败 / smoke round-trip 不兼容 | 整套放弃，沉没仅 **11 周**（5 + 6） |
| **M0**（P0 完成，10-11 月）| TSan/helgrind 72h 出 race / 32 worker 并发吞吐 < 单线程 16x / cachedCoinsUsage 1h 漂移 | 整套放弃，回到现状 |
| **M1**（P1+P2 完成，14-15 月）| 260 处 chainActive AST grep 漏改 / Snapshot tip_index 引用失效（C-A）/ dispatcher 16-shard QPS < 1000 / 单元测试不通过 | 重新设计或放弃 |
| **M2**（P3+P4+P5 完成，20-21 月，v2.6 H-H +2 周给 P4.5b subscriber 反向锁审计）| functional test 不通过 / 开发网部署不稳 / TPS < 600 / 共识等价性有 diff / reorg 6 块 KPI < 200 / P3 末 full round-trip 不兼容 / **C-B phantom-tip 边缘 case 共识漂移** | 回到上一个 stable，重做 P3-P4 |
| **M3**（P6 完成，22 月）| 开发网 4 周稳定通过 + 真主网 1 周观察 0 panic / 0 fork / chain hash 一致 / mempool diff 0 持久告警；任一红 → `mainnet-rollback.sh`（< 5 分钟全网切回 v1）。**开发网 ≡ 主网，无 shadow / canary / 渐进**（v2.6.1 简化）| 全网回滚 v2.6.1 |

---

### 7.2 灰度过程中的紧急回滚

```
所有节点保留 v1 binary。
任何阶段发现共识 hash 跟 reference 不一致：
  1. 立刻停止 v2.5 节点
  2. 启动 v1 binary（chainstate format 不变，无需 reindex）
  3. 节点继续工作
  4. 发布 post-mortem
```

v1 ↔ vN binary 互换演练 v2.5 拆两次（详细设计 §10.6 / 概要 §2.6）：

- **P0.0b smoke**（11 周决策门，v2.6 H-G +1w）：仅验证 sync.h 非破坏性增量 + lock-hierarchy 改造、Chainstate seqlock 早期实现不破坏 disk format，方案 90% 还没动
- **P3 末 full**（M2 决策门）：完整方案接入后再演练，dispatcher / libcuckoo cacheCoins / seqlock chainstate / worker commit 全在线

两次都过才进 P6 灰度——**前置硬决策门**，11 周内 smoke 不通过即放弃整套。

---

## 8. v2.0 → v2.1 的 9 项 CRITICAL 修订

三方审核（architect / cpp-reviewer / security-reviewer）对 v2.0 暴露 **9 个 CRITICAL + 15 个 HIGH**。可解性逐项评估后全部可解，方向正确，整改为 v2.1。

### 8.1 CRITICAL 处理

| # | 问题 | v2.1 解法 | 文档位置 | 状态 |
|---|------|----------|---------|------|
| C1 | doubleCheck race（tip_hash/script_flags 读未持 chainstate.cs） | `Chainstate::GetTipHash()/GetScriptFlags()` 内部强制 `shared_lock(cs)` 返回副本 | 详细设计 §1.2 / §3.2 / §6.1 | ✅ 已写入 |
| C2 | CheckSequenceLocks 未被 ChainContext 覆盖（共识 fork 风险） | `CheckSequenceLocks(ChainContext&)` 改造 + 260 处 AST grep 穷举 | 详细设计 §1.1 / §8 P1.4-P1.5 | ✅ 已写入 |
| C3 | BatchWrite 原子性破裂（libcuckoo per-bucket 锁 vs 块级原子） | **解法 B**：`pcoinsTip.batchWriteMtx`（shared_mutex），worker shared / ConnectBlock unique。代价：BatchWrite 期间 worker 卡 5-15 秒，平均 TPS 损失 1-2% | 详细设计 §1.3 / §4.2 / §4.3 / §5 | ✅ 已写入 |
| C4 | 路由 TOCTOU + 父子顺序丢失 | in-flight 改为 `{QUEUED/RUNNING/COMMITTED}` 状态机 + 5ms 延迟 GC + `SubmitBatchSync` 入口 TopoSort | 详细设计 §1.4 / §2.2 / §2.3 | ✅ 已写入 |
| C5 | UTXO 复活窗口（GetCoinConcurrent cache-miss 非原子） | libcuckoo `uprase_fn` 原子 cache-miss 回填 | 详细设计 §4.3 | ✅ 已写入 |
| C6 | 双重计数 OOM（cachedCoinsUsage 重复 fetch_add） | `uprase_fn` 仅 `inserted == true` 才 `fetch_add` | 详细设计 §4.2 / §4.3 | ✅ 已写入 |
| C7 | 三锁死锁/活锁（缺全局 lock-hierarchy） | `lock-hierarchy.md` 全局锁层级 + clang TSA 注解 + DEBUG_LOCKORDER build | 详细设计 §0 + 单独文档 | ✅ 已写入（lock-hierarchy.md 待 P0.0 启动前完成） |
| C8 | mempool eviction 重入死锁（TrimToSize 重入 smtx） | 异步 trim-thread 单线程持 unique_lock 处理；commit 临界区不 trim | 详细设计 §3.3 | ✅ 已写入 |
| C9 | M0 决策门过松 | M0 加四项量化 KPI（32 worker 吞吐 ≥ 16x / BatchWrite p99 ≤ 200ms / 1h 无漂移 / baseline 已建） | 概要设计 §5 / 详细设计 §8 P0 | ✅ 已写入 |

### 8.2 主要 HIGH 处理

| # | 问题 | v2.1 解法 | 文档位置 |
|---|------|----------|---------|
| H1 | per-input flags 未在 scriptcache key 中 | 任何 input 含非零 perInputScriptFlags 时跳过 scriptcache | 详细设计 §3.2 |
| H2 | promise 永不 set / future 无超时 | `future.wait_for(30s)` + watchdog 60s 强制重启 worker；Resubmit 不丢失原 promise | 详细设计 §1.5 / §2.2 / §2.3 / §3.4 |
| H3 | LevelDB 慢路径饿死 ConnectBlock | 二级 LRU（64MB）+ worker 数 cap = `min(N_CORE, 16)` | 详细设计 §1.3 / §4.3 |
| H4 | inflight 单锁瓶颈 / tip_index 裸指针 | inflight 16-shard sharded shared_mutex；ChainContext 改 POD 字段，无裸指针 | 详细设计 §1.1 / §1.4 |
| H5 | wallet/ZMQ 信号顺序丢失 | `g_signal_dispatcher` 单线程 per-tx FIFO 序号 | 详细设计 §3.2 / §7.6 |
| H6 | 工期低估（13-19 月） | v2.1 上调 17-24 月 / 25-38 人月 → v2.5 再上调 18-23 月 / 27-40 人月（F3 + F4 + F10 净 +6 周） | 概要设计 §0 / §3 / §4 |

### 8.3 子系统影响矩阵补全

补 wallet/ZMQ/REST/GBT/reorg/orphan_txns/pruning/indexer 的回归测试用例 → 详细设计 §11。

---

## 9. 启动前 checklist（v2.6）

| # | 项 | 状态 |
|---|---|------|
| 1 | 业务方对 18-22 月工期 + 26-40 人月（主开发 100% + reviewer/QA 30-50%）+ TPS 下限 600 + `submitrawtransactions` best-effort 语义 + **开发网 ≡ 主网简化策略** sign-off | ⏳ 待确认 |
| 2 | 人员到位（1 主开发 + 1 reviewer + 1 QA） | ⏳ 待确认 |
| 3 | P0.0a + P0.0b spike（共 11 周）：libcuckoo soak / TSan / lock-hierarchy / sync.h 非破坏性增量 / boost::recursive_mutex try_lock 4 版本 spike + H-F 异常注入 / seqlock memory-model / BatchWrite p99 / 采样窗口 baseline / smoke round-trip | ⏳ 待启动 |
| 4 | `lock-hierarchy.md` 全局锁层级文档 | ⏳ 待编写 |
| 5 | 共识等价性 baseline（采样窗口：关键激活高度 ±1000 块 + 每 5000 块全量；4-8 实例并发） | ⏳ 待启动 |
| 6 | v1 ↔ vN binary 互换回滚演练 P0.0b smoke + P3 末 full 拆两次 | ⏳ 待演练 |
| 7 | TSan/ASan/UBSan/helgrind 72 小时压测环境 | ⏳ 待准备 |
| 8 | 子系统影响矩阵 (wallet/ZMQ/REST/GBT/reorg/orphan/pruning/indexer) 回归用例 + subscriber 反向锁审计列表（F10） | ⏳ 待编写 |
| 9 | RPC 兼容性矩阵：老 `sendrawtransactions` 逐笔语义不破坏 + 新 `submitrawtransactions` **ordered + topo-sorted, best-effort**（C-C，非 ACID）RPC 文档 | ⏳ 待编写 |

---

## 9a. v2.4 → v2.5 第五轮独立审核（10 项问题）

第五轮独立审核（事实层面 grep 验证 + 架构 / cpp / 安全维度复审）对 v2.4 暴露 **10 项问题**：2 项事实错误（必修）、3 项工时漏算、5 项设计未闭环。全部已落地 v2.5。

### 9a.1 事实错误（必修）

| # | 问题 | v2.4 假设 | 实际事实（grep 验证） | v2.5 解法 | 文档位置 |
|---|------|----------|----------|----------|---------|
| **F1** | RPC busy 复用 `REJECT_TOOBUSY = 0x44` | 声称"已存在但未使用" | `src/net/net_processing.cpp:1194/1204/1608` 已用作 GETDATA 拒绝 | 新增独立 `REJECT_OVERLOADED = 0x45`，RPC submit 路径专用 | 详细设计 §8.1 |
| **F2** | `Chainstate::UpdateTip` 用 `AssertLockHeld(cs_main)` 静态强制单写者 | 声称"编译期+运行时双保证" | `src/sync.h:64-83` AssertLockHeld 仅 DEBUG_LOCKORDER 编译时生效，release build 是 noop | 改 clang TSA `EXCLUSIVE_LOCKS_REQUIRED(cs_main)` 编译期 + release build 也启用的 try_lock abort 守卫 | 详细设计 §1.1 |

### 9a.2 工时漏算

| # | 问题 | v2.4 估算 | v2.5 修正 | 文档位置 |
|---|------|----------|----------|---------|
| **F3** | `pcoinsTip` 由 `extern CCoinsViewCache*` 改 `shared_ptr` 的 lifetime sweep 没列入工时 | P0.4-P0.5 共 2 周 | 新增 P0.4a 子任务（2 周）：全代码库 raw 调用点 lifetime sweep + Shutdown 顺序重排 + TSan 覆盖 | 详细设计 §9.P0 |
| **F4** | P0.0b 共识等价性 baseline "1-2 周" 主网采样并发 | 4 周 | 6 周（baseline 改采样窗口：关键激活高度 ±1000 块 + 每 5000 块全量；4-8 实例并发 3-4 周） | 详细设计 §9.P0.0b |
| **F10** | P4.5 子系统验证未覆盖 subscriber 反向锁路径 | 3-4 周 | 5-6 周（拆 P4.5a 行为验证 + P4.5b subscriber 反向锁逐一审计） | 详细设计 §9.P4 / §11 |

### 9a.3 设计未闭环

| # | 问题 | v2.4 设计 | v2.5 修正 | 文档位置 |
|---|------|----------|----------|---------|
| **F5** | v1↔v2.4 round-trip 在 P0.0b 4 周时演练 | "P0.0b 末验证完整兼容性" | 拆两次：P0.0b smoke（仅 disk format 不破坏）+ P3 末 full（dispatcher 接入后真兼容） | 详细设计 §10.6 |
| **F6** | reorg 队列上限固定 10000，溢出落 orphan_txns | 6 块 reorg × 5 万 tx = 29 万落 orphan，永久丢失 | 上限改动态（按断链深度 × 平均块 tx × 1.5），溢出落**专用 ReorgStash**（200k 容量、10 分钟 TTL，不污染 orphan_txns） | 详细设计 §2.5 |
| **F7** | `SubmitBatchSync` atomic 语义直接绑 `sendrawtransactions` | 老 RPC 行为 break（部分成功 → 全 batch 拒绝） | atomic 语义只给新 RPC `submitrawtransactions`；老 `sendrawtransactions` 保留逐笔语义 | 详细设计 §8.2 / §8.2a |
| **F8** | AsyncTrim 声称"不阻塞 commit" | 仅不在同帧栈，仍互斥 unique(smtx) | 写明真实开销："trim 持锁 10-50ms × 命中 commit 路径累计百毫秒级"；evict 拆批控制持锁粒度 | 详细设计 §7.2 + 概要 §3.1 |
| **F9** | `GbtSnapshotProvider::RefreshAsync` 用 `std::thread().detach()` | 连续触发 → 线程爆炸 + smtx 抢锁 | 单 refresh worker + 合并队列（pending atomic 标志），连续触发只产生 1 次实际拷贝 | 详细设计 §7.3 |

### 9a.4 工期影响

总工期从 17-22 月 → **18-23 月**（净 +6 周：F3 +2 / F4 +2 / F10 +2）。投入 25-38 → 27-40 人月。

---

## 9b. v2.5 自审第六轮（8 项 N 类问题）

v2.5 文档刚落定即调一次独立 reviewer 审核，发现 v2.5 改动自身仍存 8 项二次问题。**全部已立刻修复**，未影响 v2.5 定稿。

| # | 问题 | 严重度 | v2.5 内修复方案 |
|---|------|------|-----------|
| **N1** | F1 同步漏改：detail.md L554 `SubmitBatchSync` 超时 + L1438 P3.5 子任务表仍写 `REJECT_TOOBUSY` | CRITICAL | grep 全文，两处替换为 `REJECT_OVERLOADED` |
| **N2** | F6 同步漏改：detail.md §11 子系统表 + §12 race 表仍写 "10000 上限" | CRITICAL | 两处改为"动态 10000-100000 + 专用 ReorgStash"描述 |
| **N3** | F2 try_lock 守卫依赖 `boost::recursive_mutex` 跨版本语义未验证 | HIGH | P0.0a.5 新增 spike：4 个 boost 版本（1.65/1.71/1.74/1.83）单元测试；备选方案 `std::atomic<thread::id>` writer-owner；KPI 写入 M-1a 决策门 |
| **N4** | GBT `RefreshLoop` 持 mempool.smtx 期间读 `chainActive.Tip()`，反向取 cs_main 违反锁层级 | HIGH | tip 改走 `GetChainstate().Capture()` seqlock，不再反向取 cs_main |
| **N5** | trade-off "10 周沉没" vs audit "8 周沉没" 矛盾 | MEDIUM | 统一为 10 周（P0.0a 4w + P0.0b 6w） |
| **N6** | audit-log §7/§9 大量 v2.4 残留（v2.4 binary、17-24 月、6-8 周） | MEDIUM | 全段升 v2.5 视角，加 round-trip 拆两次说明、加 RPC 兼容性 checklist 第 9 条 |
| **N7** | race retry 超限和 token bucket 限速仍 fallback 到 `orphan_txns`，与 F6 "不污染 orphan_txns" 设计意图不符 | MEDIUM | 新增 `RaceStash`（100k 容量 / 5 分钟 TTL），与 ReorgStash 共用 `TxStash` 模板，两池独立 metrics |
| **N8** | plan §1.3 版本演进表只列到 v2.4 | LOW | 增加 v2.5 行 + v2.4 行写明被打回的 10 项问题 |

**第六轮闭环**：N1+N2 grep 验证零残留；N3 spike 写入 M-1a 决策门，失败有备选；N4 锁顺序违反消除；N5/N6/N8 文档对齐；N7 设计意图闭环。

---

## 9e. v2.6.1 P6 简化（业务方澄清 2026-04-28）

**业务方关键澄清**：TBC 开发网跟主网共识规则 100% 一样（仅 chainparams 字段 netMagic / fork heights / seeds 不同，详见 `src/chainparams.cpp:107` `TBCFirstBlockHeight` 跟各 netMagic 定义）。

**结果**：开发网 4 周稳定 = 真主网兼容性已验证。原 P6 设计中：

| 原阶段 | 工时 | v2.6.1 简化后 |
|-------|-----|--------------|
| 开发网部署 4 周 | 4w | **保留**（核心兜底）|
| 真主网 shadow node 4 周（不出块，每块对比 hash）| 4w | **删除**（开发网已等价验证）|
| 真主网 canary 1 节点 4 周 | 4w | **删除** |
| 真主网渐进 10% / 30% / 100% | 4w | **删除**，改"开发网通过即直接全网部署 + 1 周观察" |

P6 总工时：14-20 周 → **11-13 周**（净 -3~-7 周）

总工期 19-24 月 → **18-22 月**（少 3-7 周）。投入 28-44 → 26-40 人月。沉没决策窗口仍 11 周（M-1a 5w + M-1b 6w 不变）。

**任务卡变更**：

- P6.7 加强为"核心兜底"角色（4 周开发网，至少 3 个 v2.6.1 + 至少 2 个 v1 节点混跑，至少 3 次 reorg）
- P6.8 重写为"真主网部署 + 1 周观察 + Day 1 真回滚演练"
- P6.9 删除（原 canary + 渐进）
- GATE-M3 KPI 矩阵简化对齐
- 主文档（plan / detail）灰度策略段简化

**风险变化**：
- R3 R4 R5 兜底由 12 周 P6 灰度 → 5 周 P6 灰度（开发网 4w + 真主网 1w）。**仍可接受**——业务方明确开发网行为 ≡ 主网，等价验证有效
- 新增"假设依赖"风险：如果未来 TBC 开发网跟主网共识字段开始分化（不再 100% 一样），P6 简化策略失效，需要补回 shadow node。建议在 chainparams.cpp 加 commit hook 监控这种分化

---

## 9c. v2.5 → v2.6 第七轮独立审核（12 项问题）

第七轮独立审核以全新视角扫 v2.5（含 N1-N8 修复后定稿），暴露 **12 项新问题**：3 CRITICAL（共识 race 类）/ 5 HIGH / 4 MED-LOW。全部已落地 v2.6。

### 9c.1 CRITICAL（共识 race 类）

| # | 问题 | v2.5 假设 | 实际事实 | v2.6 解法 | 文档位置 |
|---|------|----------|----------|----------|---------|
| **C-A** | `Snapshot` 仅 5 字段，`CheckSequenceLocks` 等需要 `CBlockIndex*` 节点访问祖先；260 处 chainActive AST grep 落不下来 | "worker 不持 cs_main，全靠 Snapshot 5 字段" | `validation.cpp:413-492` 多处需 `tip->GetAncestor()` / `pprev` 链；BlockIndex 节点本身不删除（src/blockstorage/block_index_store.cpp 无 erase）但需明文不变量 | Snapshot 加 `const CBlockIndex* tip_index` + `genesisActivationHeight`；§1.1 写明"BlockIndex 节点永不删除"不变量；P0.0a 加单元测试断言 | 详细设计 §1.1 |
| **C-B** | ConnectBlock 锁次序 `view.Flush() → UpdateTip(seqlock)` 存在 phantom-tip 窗口 | "doubleCheck #1 救回" | worker 阶段 1 拍 snap 落在 Flush 完成、UpdateTip 未完成的窗口 → 旧 tip + 新 UTXO + 旧 script_flags 验证 30ms；激活高度 ±1 块边缘 case 共识不同 | 锁次序改 UpdateTip 在 view.Flush() 之前；epoch 单调；阶段 1+2+3 跨 epoch 由 doubleCheck 兜底；阶段 3 验证用的 script_flags 跟此 epoch tip 一致 | 详细设计 §5 |
| **C-C** | `submitrawtransactions` 承诺 atomic 但无回滚机制 + per-tx 30s × 100 笔 = 50 分钟阻塞 HTTP | "atomic 语义" | 已 commit 不可回滚；mempool 无 undo log | 改 "ordered + topo-sorted, best-effort" 语义；batch-budget 30s 而非 per-tx；客户端按数组结果做幂等处理；plan §3.1 加为第 7 项 trade-off | 详细设计 §8.2a / 概要 §3.1 |

### 9c.2 HIGH

| # | 问题 | v2.6 解法 | 文档位置 |
|---|------|----------|---------|
| **H-D** | perInputScriptFlags 没进 Snapshot，worker 跳 scriptcache 决策不可达 | 加 `genesisActivationHeight`；阶段 3 给出明确 `if (input_coins[i].nHeight < snap.genesisActivationHeight) per_input_flags[i]=0` 算法；P0.0a 加等价性单元测试 | 详细设计 §3.2 |
| **H-E** | `RaceStash` / `ReorgStash` Drain/GC 锁顺序未定义 + 100k/200k 容量无监控 | TxStash 改单 mtx 串行所有方法；Drain 消费式取出即删；加 `GetMetrics()` 暴露 push/drop_full/drop_ttl/drain/size；持续溢出告警 | 详细设计 §1.4 |
| **H-F** | F2 try_lock + 立即 unlock 没用 RAII，异常时死锁（count 失衡） | 改 `std::unique_lock<…>(cs_main, std::try_to_lock)`，析构自动 unlock；P0.0a.5 加异常注入测试（ASan + helgrind） | 详细设计 §1.1 + §9.P0.0a |
| **H-G** | sync.h 改造影响 401 处 LOCK 调用宏 | 非破坏性增量：默认 `LEVEL_DEFAULT = INT_MAX/2`；现有 callsite 零修改；新 mutex 显式标 level；P0.0a.4 工时 +1 周 | 详细设计 §0 + §9.P0.0a |
| **H-H** | P4.5b subscriber 反向锁审计 2 周低估 + 没列已知订阅点 | 工时 2w → 4w；列出 7+ 已知订阅点（wallet × 3 / zmq / index/txindex / journaling_block_assembler / CMainSignals 全量 / waitfornewblock）；每条产出"锁顺序矩阵" | 详细设计 §9.P4 |

### 9c.3 MED / LOW

| # | 问题 | v2.6 解法 | 文档位置 |
|---|------|----------|---------|
| **M-I** | 沉没窗口 v2.5 后 = 4w + 6w = 10w，audit / plan 标注不一致 | v2.6 = 5w + 6w = 11w（H-G +1w）；plan §3.1 / §5 / audit §7.1 全段对齐 | 三份文档同步 |
| **M-J** | "27-40 人月" + "1 主开发 + 1 reviewer + 1 QA" 算术不闭合 | v2.6 改 "28-44 人月，主开发 100% × 23 月 + reviewer/QA 各 30-50% × 23 月"；明示 allocation | 概要 §0 / §3.1 / §8 |
| **M-K** | shadow node mempool diff "10 笔报警" 误报严重 | 拆两类：瞬态 60s 累计 200 笔 → WARNING；持久单 txid 缺失 > 10 分钟 → ALERT | 详细设计 §10.7 |
| **M-L** | plan §2.5 改造文件清单跟 detail §9 子任务不对齐（init.cpp/sync.h 影响范围模糊） | plan §2.5 加注 sync.h 是非破坏性增量；init.cpp 标注 shared_ptr + Shutdown 序重排 | 概要 §2.5 |

### 9c.4 工期影响

总工期从 18-23 月 → **19-24 月**（净 +3 周：H-G +1 / H-H +2）。投入 27-40 → 28-44 人月（M-J 算术明示）。沉没窗口 10 → 11 周。

---

## 9d. v2.6 → v2.6.1 第八轮独立审核（5 项问题）

第八轮独立审核 v2.6 ——**反例**性质：v2.6 自己引入的 H-D 算法跟 validation.cpp:3508-3514 现行 `GetInputScriptBlockHeight` 不等价（漏 MEMPOOL_HEIGHT 哨兵分支），C-B 锁次序改了但 Flush 失败处理路径没改。这两项是真共识 race，必须修。

### 9d.1 CRITICAL（修共识 race）

| # | 问题 | v2.6 假设 | 实际事实 | v2.6.1 解法 | 文档位置 |
|---|------|----------|----------|----------|---------|
| **P-1** | H-D worker `per_input_flags` 算法用 `<` 跟 chain UTXO 比，但 MEMPOOL_HEIGHT 是哨兵常量（~0x7FFFFFFF），不能直接做高度比较；现行 `GetInputScriptBlockHeight` 先把 MEMPOOL_HEIGHT 替换成 chainActive.Height()+1 再比 | "H-D 推 perInputScriptFlags 已等价现行" | validation.cpp:3508-3514 函数体 + 3597 调用点 | 复刻 `GetInputScriptBlockHeight` 等价：MEMPOOL_HEIGHT → snap.height+1，然后 `>= genesisActivationHeight` 判 SCRIPT_UTXO_AFTER_GENESIS；P0.0a 加 5 项单元测试矩阵 | 详细设计 §3.2 |
| **P-2** | C-B 调换锁次序后 Flush 失败 → seqlock 已写新 tip 但 cacheCoins 半新半旧；worker 看新 epoch 但读不到对应 UTXO | "if (!view.Flush()) return state.Error" | 现行 validation.cpp:4944 系列用 `assert(flushed)` 直接 abort | v2.6.1 显式 `LogPrintf + std::abort()`；不变量：abort 后下次启动从 LevelDB 重建（chainstate format 不变，等价于现行 assert）；P0.0a 加 fault-injection 单元测试 | 详细设计 §5 |

### 9d.2 HIGH

| # | 问题 | v2.6.1 解法 | 文档位置 |
|---|------|----------|---------|
| **P-3** | "BlockIndex 节点永不删除" 不变量过强；validation.cpp:7359 `delete entry.second` + init.cpp:2623 reindex 路径会 erase | 收紧为 **steady-state-only** 不变量；列出三个不适用窗口（启动早期 reindex / Shutdown 后期 / 异常 reset）；明确 Shutdown 顺序约束（worker pool stop → dispatcher stop → signal_dispatcher stop → pcoinsTip.reset → 然后才 UnloadBlockIndex）；P0.0a ASan 集成测试 | 详细设计 §1.1 |

### 9d.3 MED（文档对齐）

| # | 问题 | v2.6.1 解法 | 文档位置 |
|---|------|----------|---------|
| **P-4a** | 详细设计 §8.2 RPC 兼容性矩阵 L1486 仍写 `submitrawtransactions` "atomic" | 改 "ordered + topo-sorted, best-effort"；标 C-C 非 ACID | 详细设计 §8.2 |
| **P-4b** | audit-log §7.2 + checklist + detail §10.6 仍写 "10 周决策门" / "10 周内 smoke" | 全部 10w → 11w（5w P0.0a + 6w P0.0b） | 三份文档 |

### 9d.4 工期影响

零额外工期影响——P-1 / P-2 是 P3 阶段已规划工时内的实现细节修正；P-3 是 P0.0a 单元测试 + Shutdown 序约束（在 P0.4a lifetime sweep 工时内）；P-4 是文档同步无工时。

**v2.6.1 真闭环**：所有第八轮发现的 5 项已修，无新工期增加。

---

## 10. 审核签字

| 角色 | 审核范围 | v2.0 结论 | v2.1 结论 | v2.5 第五轮 |
|-----|---------|----------|----------|----------|
| security-reviewer | RISK-1 ~ RISK-10 + CheckSequenceLocks + UTXO 复活窗口 + 三锁死锁 | 4 CRITICAL（C1/C2/C3/C4） | 全部解法已写入 v2.1 详细设计 | F1 reject 码冲突 / F2 单写者强制 / F10 subscriber 反向锁 → 已落地 v2.5 |
| cpp-reviewer | mMainMtx / snapshot 父子盲点 / doubleCheck race / 双重计数 / 三锁死锁 / eviction 重入 | 4 CRITICAL（C1/C3/C6/C8） | 全部解法已写入 v2.1 详细设计 | F3 pcoinsTip lifetime / F8 AsyncTrim 互斥 / F9 GBT 线程爆炸 → 已落地 v2.5 |
| architect | CCoinsViewCache 已有锁 / FetchCoinNL 写 / ConnectBlock 临时 view / TxnValidation 200+ 处 / BatchWrite 原子性 / 路由 TOCTOU / lock-hierarchy 缺失 / M0 KPI 过松 | 4 CRITICAL（C3/C4/C7/C9） | 全部解法已写入 v2.1 详细设计 | F4 baseline 工时 / F5 round-trip 拆两次 / F6 reorg 上限 / F7 RPC 兼容 → 已落地 v2.5 |

**v2.5 设计 ready 进入 P0.0 spike 阶段**。spike 通过 → 业务方 sign-off 后启动 P0。

---

## 相关文档

- **概要设计**：[`cs_main-refactor-plan.md`](./cs_main-refactor-plan.md)
- **详细设计**：[`cs_main-refactor-detailed-design.md`](./cs_main-refactor-detailed-design.md)
- **审核记录**（本文）
- **开发网纪律**：[`../learn/24-dev-mainnet-parity.md`](../learn/24-dev-mainnet-parity.md)
