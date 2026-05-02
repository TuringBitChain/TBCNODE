# cs_main 重构方案——详细设计

**版本**：v2.6.1（修订版，吸收八轮独立审核）
**配套**：[`cs_main-refactor-plan.md`](./cs_main-refactor-plan.md)（概要设计）

> 本文档假设读者已读过概要设计。这里给出每个组件的实现细节、数据结构、流程图、race 处理协议、阶段子任务工时分解。
>
> v2.6 相对 v2.5 的主要修订（来自第七轮独立审核，12 项新问题）：
> - **C-A**：`Chainstate::Snapshot` 增加 `const CBlockIndex* tip_index` + `genesisActivationHeight`；明文写 BlockIndex 节点不变量（§1.1）
> - **C-B**：`ConnectBlock` 锁次序改 UpdateTip 在 view.Flush() 之前，消除 phantom-tip 共识 race（§5）
> - **C-C**：`submitrawtransactions` 语义改"原子拓扑排序 + 逐笔 commit best-effort"，batch-budget 30s（§8.2a）
> - **H-D**：worker 阶段 3 显式根据 input.coinHeight 推 perInputScriptFlags（§3.2）
> - **H-E**：`TxStash` Drain 改消费式，Drain/GC 锁顺序定义；100k/200k 容量加 metrics（§2.5 / §1.4）
> - **H-F**：F2 try_lock 守卫改 RAII `std::unique_lock<…>(cs, std::try_to_lock)`，异常安全（§1.1）
> - **H-G**：`sync.h` 改造非破坏性增量（默认 `LEVEL_DEFAULT`），401 callsite 零修改；P0.0a.4 +1 周（§0 / §9.P0.0a）
> - **H-H**：`P4.5b` subscriber 反向锁审计 2w → 4w；列出 7+ 已知订阅点（§9.P4 / §11）
> - **M-I/J/K/L**：文档对齐（§3.1 / §10.7 / §11）
>
> v2.5 相对 v2.4 的主要修订（来自五轮独立审核，10 项问题）：
> - **F1**：`REJECT_TOOBUSY` 已被 `net_processing.cpp:1194/1204/1608` 用作 GETDATA 拒绝；新增 `REJECT_OVERLOADED = 0x45` 给 RPC submit 路径，避免语义冲突（§3.1 / §8.1）
> - **F2**：`Chainstate::UpdateTip` 单写者强制改 clang TSA `EXCLUSIVE_LOCKS_REQUIRED(cs_main)` 编译期 + release build 也启用的 try_lock abort 守卫；不再依赖只在 DEBUG_LOCKORDER 生效的 `AssertLockHeld`（§1.1）
> - **F3**：新增 P0.4a 子任务 — `pcoinsTip` 由 `extern CCoinsViewCache*` 改 `std::shared_ptr<CCoinsViewCache>`，全代码库 raw `pcoinsTip->` 调用点 lifetime sweep + Shutdown 顺序重排（§9.P0）
> - **F4**：P0.0b 4 周 → 6 周；baseline 改采样窗口（关键激活高度 ±1000 块 + 每 5000 块全量）（§9.P0.0b）
> - **F5**：v1↔vN binary round-trip 拆 P0.0b smoke + P3 末 full（§10.6）
> - **F6**：reorg 独立队列上限改动态（按断链深度动态扩到 100k+），溢出落**专用 reorg-stash**（不污染 orphan_txns）（§2.5）
> - **F7**：`SubmitBatchSync` atomic 语义只给新 RPC `submitrawtransactions`；老 `sendrawtransactions` 保留逐笔成功语义（§8.2 / §8.2a）
> - **F8**：AsyncTrim trade-off 显式写明"trim 持 unique(smtx) 10-50ms 期间 commit 路径短暂阻塞"（§7.2）
> - **F9**：GbtSnapshotProvider 单 refresh worker + 合并队列；废 `std::thread().detach()`（§7.3）
> - **F10**：P4.5 子系统验证 +2 周，新增 wallet/ZMQ/REST subscriber 反向锁顺序逐一审计（§9.P4 / §11）
>
> v2.4 相对 v2.3 的主要修订（来自四轮审核）：
> - **K1**：seqlock writer/reader 显式 `atomic_thread_fence`（release/acquire），`memcpy` 拷贝 32 字节字段（§1.1）
> - **K2**：libcuckoo cache miss 用 `insert` 返回 bool（不用错误的 uprase_fn 双参签名），`update_fn` 作为已存在 fallback（§1.3）
> - **K3**：删除 `chainstate.cs`，元数据全部 atomic + seqlock；删除 `ChainContext`，worker 直读 `g_chainstate.Capture()`（§1.1 / §1.2）
> - **K4**：`GetCoinConcurrent` 实现路径明确：L1 libcuckoo / L2 LRU / L3 LevelDB snapshot（§1.3）
> - **H1**：(v2.5 F1 取代) RPC busy 用 `REJECT_OVERLOADED = 0x45`，不再复用 0x44
> - **H2**：reorg 独立队列上限 10000（v2.5 F6 改动态）+ 溢出 reorg-stash（v2.5 F6 改专用池）（§2.5）
> - **H4**：GBT condvar 长轮询 15s，不再 100ms 重试风暴（§5）
> - **H5**：`InflightAbortedGuard` 用显式 `commit_succeeded` 标志，不依赖 `uncaught_exceptions()`（§3.2）
> - **H6**：`AssertLockOrder` hook 进 `sync.h` 现有 `EnterCritical/LeaveCritical`，识别 `boost::recursive_mutex` 重入（§0）
> - **H7**：worker 阶段 4 always 重读所有 input，不依赖 `input_was_from_chain` 标记数组（§3.2）
> - **H8**：`ScopeGuard` 析构无条件 `noexcept`，内部 try/catch 防 terminate（§7.1）
> - **MED-1**：(v2.5 F2 取代) `Chainstate::UpdateTip` 单写者强制改 clang TSA + 运行时 try_lock abort
> - **MED-2**：`#3b` 时序保证（ConnectBlock 锁顺序确保 view.Flush 在 smtx 释放前完成）（§5）
> - **MED-3**：新增 `waitformempoolentry` RPC 给交易所同步语义（§7.7）
> - **MED-4**：P6 加 mempool diff 监控（接受性差异检测）（§10.7）
> - **删除 BIP68 历史数组**（v2.3 Δ4 整段删除）：TBC Genesis 后 `CheckSequenceLocks` 直接 return true（§1.2 注释）

---

## 目录

- [0. 全局锁层级](#0-全局锁层级)
- [1. 数据结构详细定义](#1-数据结构详细定义)
- [2. ChainDispatcher 详细设计](#2-chaindispatcher-详细设计)
- [3. PerChainWorker 详细设计](#3-perchainworker-详细设计)
- [4. CCoinsViewCache 并发改造](#4-ccoinsviewcache-并发改造)
- [5. ConnectBlock 协调机制](#5-connectblock-协调机制)
- [6. doubleCheck 协议](#6-doublecheck-协议)
- [7. 配套基础设施 (ScopeGuard / AsyncTrim / GbtSnapshot / SignalDispatcher)](#7-配套基础设施)
- [8. RPC 入口替换](#8-rpc-入口替换)
- [9. 阶段子任务详细计划](#9-阶段子任务详细计划)
- [10. 测试矩阵](#10-测试矩阵)
- [11. 子系统影响矩阵](#11-子系统影响矩阵)
- [12. Race 处理一览](#12-race-处理一览)

---

## 0. 全局锁层级

**强制层级**（违反即编译期报错或 DEBUG_LOCKORDER abort）：

```
cs_main (boost::recursive_mutex, level 0)
  > mempool.smtx (std::shared_mutex, level 1)
    > pcoinsTip.batchWriteMtx (std::shared_mutex, level 2)
      > pcoinsTip.metaMtx (std::shared_mutex, level 3)
        > inflight_shard_mtx (std::shared_mutex, level 4)
          > worker.queue_mtx (std::mutex, level 5)
```

**注意**：v2.4 删除 `chainstate.cs`，元数据用 seqlock，无需进入锁层级。

**实现**（H6 修复 + H-G 非破坏性增量）：改造 `src/sync.h` 的 `EnterCritical/LeaveCritical`。**关键约束（H-G）**：现有 401 处 `LOCK / LOCK2 / TRY_LOCK / ENTER_CRITICAL_SECTION` 调用宏**零修改**——`level` 参数有默认值 `LEVEL_DEFAULT = INT_MAX / 2`（落在 LEVEL_WORKER_QUEUE 之后），现有 mutex 没标 level 视为"层级未声明"，跟所有标了 level 的 mutex 都满足"低 level 先取"约束。新增 mutex（`batchWriteMtx` / `metaMtx` / `inflight_shard_mtx` 等）显式标 level。这样 sync.h 改造跟 401 callsite 解耦，P0.0a.4 工时只需做 sync.h hook + 新 mutex 标注，不需要全代码库 LOCK 宏修改。

```cpp
// src/sync.h（改造）
struct LockEntry {
    void* mutex_addr;
    bool is_recursive;
    int recursive_depth;
    int hierarchy_level;
    const char* file;
    int line;
};

thread_local std::vector<LockEntry> g_lock_stack;

void EnterCritical(const char* name, const char* file, int line, void* cs,
                   int level, bool is_recursive);
void LeaveCritical(void* cs);

// LEVEL_* 常量在 src/validation/lock_hierarchy.h
constexpr int LEVEL_CS_MAIN          = 0;
constexpr int LEVEL_MEMPOOL_SMTX     = 1;
constexpr int LEVEL_BATCHWRITE_MTX   = 2;
constexpr int LEVEL_META_MTX         = 3;
constexpr int LEVEL_INFLIGHT_SHARD   = 4;
constexpr int LEVEL_WORKER_QUEUE     = 5;
constexpr int LEVEL_DEFAULT          = INT_MAX / 2;  // H-G: 现有未标注 mutex 默认 level
```

**H-G 非破坏性增量原则**：

- 新增 hook 走 `EnterCritical(name, file, line, cs, level = LEVEL_DEFAULT, is_recursive = false)`，默认参数让现有 LOCK 宏调用零变化
- 新增 mutex 在定义点标注实际 level（小于 LEVEL_DEFAULT），跟既有未标注 mutex 一起依然满足层级递增
- DEBUG_LOCKORDER build 启用层级检查；release build 该 hook 退化为 noop（不影响生产性能）
- 401 callsite 兼容性测试：grep `LOCK\(|LOCK2\(|TRY_LOCK\(|ENTER_CRITICAL_SECTION\(` 全部用 default 路径；编译期产物对二进制兼容

`EnterCritical` 行为：
1. **重入识别**：栈中已有同 `cs` 且 `is_recursive == true` → 递增 `recursive_depth`，不入栈新条目
2. **层级检查**：新锁 `level` 必须 > 栈顶 `level`；违反则 `LogPrintf` + `std::abort()`（debug build）

**关键约束**：
- worker 不持 cs_main
- ConnectBlock 仍持 cs_main，可拿下层全部锁
- 不允许任何回调路径反向取锁（信号必须异步分发）
- AssertLockOrder 跟 boost::recursive_mutex（cs_main）+ std::shared_mutex 全部统一在同一深度栈

---

## 1. 数据结构详细定义

### 1.1 Chainstate（seqlock 元数据，K1+K3+MED-1 修复）

```cpp
// src/validation/chainstate.h（新文件）
class Chainstate {
    // seqlock：单写者持 cs_main，多读者无锁
    std::atomic<uint64_t> seq{0};   // 偶数=稳态，奇数=写中

    // 字段（全部由 seq 保护，writer 持 cs_main 排他时单线程更新）
    uint256 m_tip_hash;             // 32 字节非原子，靠 seq 重试容忍撕裂
    const CBlockIndex* m_tip_index; // C-A: BlockIndex 节点指针；不变量见 Capture 注释
    uint32_t m_script_flags;
    int32_t m_height;
    int64_t m_mtp;
    bool m_isGenesisEnabled;
    int32_t m_genesisActivationHeight;

public:
    // C-A + P-3: Snapshot 增加 tip_index 和 genesisActivationHeight
    // - tip_index: worker 在 CheckSequenceLocks 等需要遍历祖先的路径用，read-only
    //   **不变量（仅 steady-state）**：节点处于"启动完成 → Shutdown 开始"之间的 Active 阶段时，
    //     `mapBlockIndex` 内的 `CBlockIndex` 节点对象不被 erase / delete；hash / nHeight / pprev
    //     链不变（nStatus / nFile / nDataPos 等可变字段会随 reorg/prune 改写，但链结构稳定）。
    //   **不适用窗口**：
    //     - 启动早期（init.cpp:2623 reindex 路径调 `UnloadBlockIndex()` → validation.cpp:7359
    //       `delete entry.second; mapBlockIndex.clear();`）
    //     - Shutdown 后期（同上）
    //     - 异常 reset 路径（validation.cpp:8087 `mapBlockIndex.clear();`）
    //   **Shutdown 顺序约束（P-3）**：
    //     1. Worker pool stop（drain in-flight）
    //     2. Dispatcher stop（不再接新 tx）
    //     3. signal_dispatcher stop
    //     4. pcoinsTip.reset()
    //     5. 然后才能调 UnloadBlockIndex / mapBlockIndex.clear
    //     违反该顺序 → worker 局部 snap.tip_index 悬空 → use-after-free
    //   **验证**：P0.0a 单元测试 (a) 断言 Active 阶段 mapBlockIndex 无 erase，
    //     (b) Shutdown 序集成测试用 ASan 覆盖 use-after-free。
    // - genesisActivationHeight: H-D 修复，worker 阶段 3 推 perInputScriptFlags 用
    struct Snapshot {
        uint256 tip_hash;
        const CBlockIndex* tip_index;
        uint32_t script_flags;
        int32_t height;
        int64_t mtp;
        bool isGenesisEnabled;
        int32_t genesisActivationHeight;
    };

    // 读者 API（无锁，标准 seqlock with explicit fences — Preshing pattern）
    Snapshot Capture() const noexcept {
        Snapshot s;
        uint64_t seq_before, seq_after;
        do {
            seq_before = seq.load(std::memory_order_acquire);
            if (seq_before & 1) {
                std::this_thread::yield();
                continue;
            }
            std::atomic_thread_fence(std::memory_order_acquire);  // K1

            // 读取期间值可能撕裂，但视为 opaque bytes，不在循环内 deref/use
            std::memcpy(&s.tip_hash, &m_tip_hash, sizeof(uint256));
            s.tip_index = m_tip_index;        // C-A: 指针读 8 字节单 atomic 等价
            s.script_flags = m_script_flags;
            s.height = m_height;
            s.mtp = m_mtp;
            s.isGenesisEnabled = m_isGenesisEnabled;
            s.genesisActivationHeight = m_genesisActivationHeight;

            std::atomic_thread_fence(std::memory_order_acquire);  // K1
            seq_after = seq.load(std::memory_order_acquire);
        } while (seq_before != seq_after);
        return s;
    }

    // 写者 API（持 cs_main 排他时单线程调用）
    // F2 + H-F 修复：clang TSA 编译期强制 + release build 也启用的 RAII 探针守卫
    void UpdateTip(const CBlockIndex* new_tip, uint32_t script_flags)
        EXCLUSIVE_LOCKS_REQUIRED(cs_main)
    {
        // H-F: 用 RAII unique_lock(try_to_lock) 保证异常安全
        // 析构自动 unlock，即使中间 LogPrintf/abort 路径抛异常也不会留递归 count 失衡
        // boost::recursive_mutex 上 try_lock 在「同线程已持有」时返回 true 并递增 count，
        // 在「跨线程」时返回 false。立即析构释放递增的 count，不影响调用者持锁状态。
        {
            std::unique_lock<CCriticalSection> probe(cs_main, std::try_to_lock);
            if (!probe.owns_lock()) {
                LogPrintf("FATAL: UpdateTip called without cs_main\n");
                std::abort();
            }
            // probe 析构自动 unlock，count 平衡
        }

        seq.fetch_add(1, std::memory_order_release);   // 进写态（奇数）
        std::atomic_thread_fence(std::memory_order_release);  // K1

        uint256 h = new_tip->GetBlockHash();
        std::memcpy(&m_tip_hash, &h, sizeof(uint256));
        m_tip_index = new_tip;            // C-A: 写指针
        m_script_flags = script_flags;
        m_height = new_tip->nHeight;
        m_mtp = new_tip->GetMedianTimePast();
        m_isGenesisEnabled = IsGenesisEnabled(config, m_height + 1);
        m_genesisActivationHeight = config.GetGenesisActivationHeight();   // H-D

        std::atomic_thread_fence(std::memory_order_release);  // K1
        seq.fetch_add(1, std::memory_order_release);   // 退写态（偶数）
    }
};

extern Chainstate g_chainstate;
Chainstate& GetChainstate();  // 懒初始化 Meyers Singleton
```

**K1 完整修复**：writer 两次 fetch_add 之间 + 之前都加 `release fence`；reader 在 seq 检测之间加 `acquire fence`。x86 隐式 OK，ARM/RISC-V 必须显式。`memcpy` 用于 32 字节字段防编译器优化。

**K3**：完全无锁 Capture，CaptureContext 不再持 cs_main。

**F2 单写者强制**（取代 v2.4 MED-1）：`AssertLockHeld(cs_main)` 在 release build 是 noop（src/sync.h:64-83 仅 `DEBUG_LOCKORDER` 编译时有效），不能作为生产环境的强制手段。改三层保证：

1. **编译期**：函数签名标 `EXCLUSIVE_LOCKS_REQUIRED(cs_main)`（clang TSA），调用方未持 cs_main 时编译警告/错误（CI 启用 `-Werror=thread-safety`）
2. **DEBUG_LOCKORDER build**：原有 `EnterCritical/LeaveCritical` 跟踪栈做断言
3. **release build 运行时**：`try_lock + unlock` 探测，跨线程调用直接 abort

`boost::recursive_mutex::try_lock` 在已持锁本线程立即返回 true 且递增 count，跨线程返回 false——这条假设需在 P0.0a.5 spike 中验证（4 个 boost 版本：1.65/1.71/1.74/1.83）。开销：一次原子 CAS（~5ns）。

**备选方案（spike 失败时）**：`std::atomic<std::thread::id> writer_owner`，UpdateTip 入口 `compare_exchange_strong(unset, this_thread::get_id())` 失败即 abort，离开时 store(unset)。语义独立于 boost 版本，性能略低（CAS + memory barrier ≈ 10ns）。文档 §0 锁层级和 P0.0a.4 sync.h 改造同步切换。

### 1.2 删除 ChainContext（K3 简化）

**v2.4 不再有 ChainContext 类型**。worker 直接持 `Chainstate::Snapshot` 局部变量，验证完即销毁。

```cpp
// 旧（v2.3）：ChainContextRef ctx = ...; ctx->height
// 新（v2.4）：auto snap = g_chainstate.Capture(); snap.height
```

**好处**：
- 节省 inflight 队列 240MB 内存峰值（v2.2 H-3 自然消失）
- 去掉 shared_ptr 引用计数
- 数据流单一：每次读 = 一次 seqlock capture
- 简化 reorg：snap 在 worker 局部变量

**BIP68 注释**：TBC Genesis 后 `src/validation.cpp:426` 的 `CheckSequenceLocks` 直接 `return true`。worker 内 `CheckSequenceLocks(snap, tx, flags)` 等价为 `if (snap.isGenesisEnabled) return true;`。**不需要历史块 nTime/nHeight 数组**（v2.2 误设计已删）。

### 1.3 改造后的 CCoinsViewCache（K2+K4 修复）

```cpp
// src/coins.h 改造后
class CCoinsViewCache : public CCoinsViewBacked {
public:
    // 老接口保持兼容（同步调用，内部用 metaMtx）
    bool GetCoin(const COutPoint &outpoint, Coin &coin) const override;
    bool HaveCoin(const COutPoint &outpoint) const override;
    void AddCoin(const COutPoint &outpoint, Coin &&coin, bool overwrite);
    bool SpendCoin(const COutPoint &outpoint, Coin* moveto = nullptr);

    // 并发安全读（worker hot path 用）K4 实现明确
    bool GetCoinConcurrent(const COutPoint &outpoint, Coin &coin) const;
    bool HaveCoinConcurrent(const COutPoint &outpoint) const;

    // BatchWrite 改造（ConnectBlock 用，K2 修复）
    bool BatchWrite(CCoinsMap &mapCoins, const uint256 &hashBlock) override;
    bool Flush();

    // M-v3-2: 用锁状态判 busy（无 atomic 窗口）
    bool IsBatchWriteInProgress() const;

private:
    // libcuckoo 并发哈希表（单 bucket lock 内原子）
    libcuckoo::cuckoohash_map<COutPoint, CCoinsCacheEntry, SaltedOutpointHasher> cacheCoins;

    // C3 解法 B：BatchWrite 协调读写锁
    mutable std::shared_mutex batchWriteMtx;

    // 元数据
    mutable std::shared_mutex metaMtx;
    uint256 hashBlock;                                 // GUARDED_BY(metaMtx)
    std::atomic<size_t> cachedCoinsUsage{0};

    // 二级 LRU（H3 LevelDB 慢路径缓解）
    mutable LRUCache<COutPoint, Coin> levelTwoCache;   // 64MB
};
```

**K4: GetCoinConcurrent 实现明确**

```cpp
bool CCoinsViewCache::GetCoinConcurrent(const COutPoint& outpoint, Coin& coin) const {
    std::shared_lock bw(batchWriteMtx);  // 跟 BatchWrite 互斥（块级原子）

    // L1: libcuckoo 命中
    bool hit = cacheCoins.find_fn(outpoint, [&](const CCoinsCacheEntry& e) noexcept {
        coin = e.coin;
    });
    if (hit) return !coin.IsSpent();

    // L2: LRU
    if (levelTwoCache.Get(outpoint, coin)) return !coin.IsSpent();

    // L3: LevelDB（CCoinsViewDB::GetCoin 内部用 LevelDB ReadOptions snapshot，thread-safe）
    Coin tmp;
    if (!base->GetCoin(outpoint, tmp)) return false;

    // K2: cache miss 回填用 insert 返回 bool 区分 inserted
    bool ins = cacheCoins.insert(outpoint, [&]() {
        CCoinsCacheEntry e;
        e.coin = tmp;
        e.flags = 0;
        return e;
    }());
    if (ins) {
        cachedCoinsUsage.fetch_add(memusage::DynamicUsage(tmp));
    }
    // 已存在则不动（其他 worker 已回填或 ConnectBlock 已写）

    levelTwoCache.Put(outpoint, tmp);
    coin = std::move(tmp);
    return !coin.IsSpent();
}
```

**K2: BatchWrite 修复**

```cpp
bool CCoinsViewCache::BatchWrite(CCoinsMap& mapCoins, const uint256& hashBlockIn) {
    std::unique_lock bw(batchWriteMtx);

    {
        std::unique_lock m(metaMtx);
        hashBlock = hashBlockIn;
    }

    for (auto& [outpoint, entry] : mapCoins) {
        if (!(entry.flags & CCoinsCacheEntry::DIRTY)) continue;

        if (entry.coin.IsSpent()) {
            // erase 路径
            cacheCoins.erase_fn(outpoint, [&](CCoinsCacheEntry& existing) noexcept -> bool {
                cachedCoinsUsage.fetch_sub(memusage::DynamicUsage(existing));
                return true;
            });
        } else {
            // K2: insert 返回 bool，已存在则 update_fn 覆盖（两路径都是单 bucket 原子）
            size_t new_usage = memusage::DynamicUsage(entry);
            CCoinsCacheEntry copy = entry;
            bool ins = cacheCoins.insert(outpoint, std::move(copy));
            if (ins) {
                cachedCoinsUsage.fetch_add(new_usage);
            } else {
                cacheCoins.update_fn(outpoint, [&](CCoinsCacheEntry& existing) noexcept {
                    cachedCoinsUsage.fetch_sub(memusage::DynamicUsage(existing));
                    existing = std::move(entry);
                    cachedCoinsUsage.fetch_add(new_usage);
                });
            }
        }
    }
    mapCoins.clear();
    return true;
}

// M-v3-2: 直接用锁状态判 busy（无窗口）
bool CCoinsViewCache::IsBatchWriteInProgress() const {
    std::shared_lock bw(batchWriteMtx, std::try_to_lock);
    return !bw.owns_lock();
}
```

### 1.4 ChainDispatcher（C4+H-v3-3+H4 修复）

```cpp
// src/validation/chain_dispatcher.h（新文件）

enum class InflightStatus { QUEUED, RUNNING, COMMITTED, ABORTED };

struct InflightEntry {
    WorkerId worker;
    InflightStatus status;
    int64_t commit_time_us = 0;  // COMMITTED 后保留 5ms 再 GC
};

constexpr size_t INFLIGHT_SHARDS = 16;
constexpr int MAX_INPUT_SCAN = 100;
constexpr int MAX_RETRY_RACE = 10;
constexpr int MAX_RETRY_HARD = 1;
// F6: reorg 队列改动态上限（按断链深度 × 平均块 tx 数 × 1.5）
// 默认下限 10000，上限 100000；溢出落专用 reorg-stash（不污染 orphan_txns）
constexpr size_t REORG_QUEUE_MIN = 10000;
constexpr size_t REORG_QUEUE_MAX = 100000;

enum class ResubmitReason { TipChanged, FlagsChanged, Reorg };

class ChainDispatcher {
public:
    CValidationState SubmitSync(TxRef tx);
    void SubmitAsync(TxRef tx);
    std::vector<CValidationState> SubmitBatchSync(std::vector<TxRef> txs);

    void MarkCommitted(TxId txid);
    void MarkAborted(TxId txid);   // H5 修复：worker crash 立即清理
    void Resubmit(WorkItem&& item, ResubmitReason reason);

    // reorg 独立队列触发
    void TriggerReorgResubmit(std::vector<TxRef> txs);

private:
    WorkerId FindWorkerForChain(const CTransaction& tx);
    WorkerId PickLeastLoadedWorker();    // power-of-two-choices

    // 16-shard inflight
    struct Shard {
        mutable std::shared_mutex mtx;
        std::unordered_map<TxId, InflightEntry> map;
    };
    std::array<Shard, INFLIGHT_SHARDS> inflight;

    // worker pool（运行时按 hardware_concurrency 决定大小）
    std::vector<std::unique_ptr<PerChainWorker>> workers;

    // 双 token bucket（H-v3-3）
    ResubmitRateLimiter normal_bucket{1000};   // 正常 retry: 1000/s

    // reorg 独立队列（不消耗 token bucket，但有上限保护内存）
    std::deque<WorkItem> reorg_queue;
    std::mutex reorg_queue_mtx;
    std::condition_variable reorg_cv;
    std::thread reorg_worker;  // 专用 worker 串行消费

    // 异步 GC 线程（M1 修复）
    std::thread gc_thread;
};

ChainDispatcher& GetDispatcher();
```

### 1.5 PerChainWorker

```cpp
// src/validation/per_chain_worker.h（新文件）

struct WorkItem {
    TxRef tx;
    std::optional<std::promise<CValidationState>> promise;  // C4 修复
    int retry_count = 0;
    bool is_reorg_resubmit = false;

    WorkItem() = default;
    WorkItem(WorkItem&&) noexcept = default;
    WorkItem& operator=(WorkItem&&) noexcept = default;
    WorkItem(const WorkItem&) = delete;
};

class PerChainWorker {
public:
    PerChainWorker(WorkerId id, ChainDispatcher* dispatcher);
    ~PerChainWorker();

    void Push(WorkItem&& item);
    size_t QueueSize() const;

    int64_t LastProgressTime() const;  // watchdog 软停语义

private:
    void Run();
    void ProcessItem(WorkItem item);

    WorkerId id;
    ChainDispatcher* dispatcher;
    std::thread worker_thread;
    std::atomic<bool> running{true};
    std::atomic<int64_t> last_progress_us{0};

    std::deque<WorkItem> queue;
    mutable std::mutex queue_mtx;
    std::condition_variable cv;
};
```

---

## 2. ChainDispatcher 详细设计

### 2.1 路由策略

```cpp
WorkerId ChainDispatcher::FindWorkerForChain(const CTransaction& tx) {
    int scan = std::min<int>(tx.vin.size(), MAX_INPUT_SCAN);  // 防 DoS
    std::map<WorkerId, int> votes;

    for (int i = 0; i < scan; i++) {
        TxId parent_txid = tx.vin[i].prevout.GetTxId();
        auto& shard = inflight[std::hash<TxId>{}(parent_txid) % INFLIGHT_SHARDS];
        std::shared_lock lock(shard.mtx);
        auto it = shard.map.find(parent_txid);
        if (it != shard.map.end()) {
            // QUEUED / RUNNING / COMMITTED-未GC 都可作为路由依据
            // ABORTED 不计入投票
            if (it->second.status != InflightStatus::ABORTED) {
                votes[it->second.worker]++;
            }
        }
    }

    if (votes.empty()) return WORKER_NONE;
    return std::max_element(votes.begin(), votes.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; })->first;
}

WorkerId ChainDispatcher::PickLeastLoadedWorker() {
    // L2 修复：thread_local rng 混 thread id + random_device + 时间戳
    thread_local std::mt19937 rng = []{
        std::seed_seq seq{
            std::random_device{}(),
            (uint32_t)std::hash<std::thread::id>{}(std::this_thread::get_id()),
            (uint32_t)GetTimeMicros()
        };
        return std::mt19937(seq);
    }();

    size_t a = rng() % workers.size();
    size_t b = rng() % workers.size();
    return workers[a]->QueueSize() < workers[b]->QueueSize() ? a : b;
}
```

### 2.2 SubmitBatchSync 入口拓扑排序（C4/H1 修复）

```cpp
std::vector<TxRef> TopoSort(std::vector<TxRef> txs) {
    // 检测重复 txid
    std::unordered_set<TxId> seen;
    for (auto& tx : txs) {
        if (!seen.insert(tx->GetId()).second) {
            throw std::invalid_argument("batch-duplicate-txid");
        }
    }

    // 建图 + Kahn 算法
    // 检测环（残留 in-degree > 0）→ throw "batch-cycle"
    // ... 标准 Kahn 实现
}

std::vector<CValidationState> ChainDispatcher::SubmitBatchSync(std::vector<TxRef> txs) {
    std::vector<TxRef> sorted;
    try {
        sorted = TopoSort(std::move(txs));
    } catch (const std::invalid_argument& e) {
        std::vector<CValidationState> all_rej(txs.size());
        for (auto& s : all_rej) s.Invalid(false, REJECT_INVALID, e.what());
        return all_rej;
    }

    std::vector<std::future<CValidationState>> futures;
    for (auto& tx : sorted) {
        WorkItem item;
        item.tx = tx;
        item.promise.emplace();
        futures.push_back(item.promise->get_future());
        // 派发
        WorkerId target = FindWorkerForChain(*tx);
        if (target == WORKER_NONE) target = PickLeastLoadedWorker();
        // 入 inflight + push worker queue
        // ...
    }

    // C-C 修复：batch-budget 30s（不是 per-tx 30s）
    // 100 笔批不再阻塞 100 × 30s = 50min，整批最长 30s
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::vector<CValidationState> results;
    for (auto& f : futures) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            CValidationState s;
            s.Invalid(false, REJECT_OVERLOADED, "submit-timeout-batch");
            results.push_back(s);
            continue;
        }
        if (f.wait_until(deadline) == std::future_status::timeout) {
            CValidationState s;
            s.Invalid(false, REJECT_OVERLOADED, "submit-timeout-batch");
            results.push_back(s);
        } else {
            results.push_back(f.get());
        }
    }
    return results;
}
```

### 2.3 OnDone / Resubmit / MarkAborted（C4/H2 修复）

```cpp
void ChainDispatcher::MarkCommitted(TxId txid) {
    auto& shard = inflight[std::hash<TxId>{}(txid) % INFLIGHT_SHARDS];
    std::unique_lock lock(shard.mtx);
    auto it = shard.map.find(txid);
    if (it != shard.map.end()) {
        it->second.status = InflightStatus::COMMITTED;
        it->second.commit_time_us = GetTimeMicros();
    }
}

void ChainDispatcher::MarkAborted(TxId txid) {
    auto& shard = inflight[std::hash<TxId>{}(txid) % INFLIGHT_SHARDS];
    std::unique_lock lock(shard.mtx);
    auto it = shard.map.find(txid);
    if (it != shard.map.end()) {
        it->second.status = InflightStatus::ABORTED;
        // GC 立即清理（5ms 后）
        it->second.commit_time_us = GetTimeMicros();
    }
}

// Resubmit 同步更新 inflight worker 字段（H-v3-2 修复）
void ChainDispatcher::Resubmit(WorkItem&& item, ResubmitReason reason) {
    if (++item.retry_count > MAX_RETRY_RACE) {
        // N7: race retry 超限走专用 race-stash，不污染 orphan_txns
        // race-stash 与 ReorgStash 同一类设计：FIFO + TTL，但分用以区分语义
        g_race_stash.Push(std::move(item.tx));
        if (item.promise) {
            CValidationState s; s.Invalid(false, REJECT_OVERLOADED, "max-retry-exceeded-race");
            item.promise->set_value(s);
        }
        // metrics: race_stash_push.Inc()
        return;
    }

    // reorg 独立队列不消耗 token bucket
    if (reason == ResubmitReason::Reorg) {
        item.is_reorg_resubmit = true;
    } else {
        if (!normal_bucket.TryConsume()) {
            // N7: 限速时也走 race-stash，不污染 orphan_txns
            g_race_stash.Push(std::move(item.tx));
            // metrics: race_stash_throttle_push.Inc()
            return;
        }
    }

    WorkerId target = FindWorkerForChain(*item.tx);
    if (target == WORKER_NONE) target = PickLeastLoadedWorker();

    auto& shard = inflight[std::hash<TxId>{}(item.tx->GetId()) % INFLIGHT_SHARDS];
    {
        std::unique_lock lock(shard.mtx);
        auto& e = shard.map[item.tx->GetId()];
        e.worker = target;                        // H-v3-2: 同步更新
        e.status = InflightStatus::QUEUED;
    }
    workers[target]->Push(std::move(item));
}
```

### 2.4 GC 线程（M1 修复）

```cpp
void ChainDispatcher::GcLoop() {
    int shard_idx = 0;
    int skip_count = 0;
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));  // 16 shard × 3ms = 48ms 周期
        auto& shard = inflight[shard_idx];

        std::unique_lock lock(shard.mtx, std::try_to_lock);
        if (lock) {
            CleanShard(shard, GetTimeMicros());
            skip_count = 0;
        } else if (++skip_count > 10) {
            // 防饥饿：连续 10 次跳过强制取
            std::unique_lock force(shard.mtx);
            CleanShard(shard, GetTimeMicros());
            skip_count = 0;
        }
        shard_idx = (shard_idx + 1) % INFLIGHT_SHARDS;
    }
}

void CleanShard(Shard& shard, int64_t now) {
    // 每次最多处理 100 条目，超时主动让步（避免长持锁）
    int budget = 100;
    for (auto it = shard.map.begin(); it != shard.map.end() && budget > 0; ) {
        if ((it->second.status == InflightStatus::COMMITTED &&
             now - it->second.commit_time_us > 5000) ||
            it->second.status == InflightStatus::ABORTED) {
            it = shard.map.erase(it);
        } else {
            ++it;
        }
        --budget;
    }
}
```

### 2.5 reorg 独立队列（F6 修订：动态上限 + 专用 reorg-stash）

```cpp
// F6 + N7 + H-E: 两类专用 stash 池（与 orphan_txns 解耦，语义清晰）
// - ReorgStash：reorg 独立队列溢出 → 容量 200k / 10 分钟 TTL
// - RaceStash：race retry 超限 / token bucket 限速 → 容量 100k / 5 分钟 TTL
// 两池独立 GC，metrics 分开统计
//
// H-E 锁顺序定义：所有方法（Push/Drain/GC/Size）持单一 stash mtx，互斥串行。
//   Drain 是消费式（取出即删），不存在跟 GC 同时操作同一 entry 的窗口。
//   外部调用方持其它锁（如 reorg_queue_mtx）的时机：必须先释放外部锁再调 Stash 方法，
//   stash mtx 不嵌入 lock-hierarchy 任何层级——它是叶子锁。
template<size_t MAX_SIZE_, int64_t TTL_US_>
class TxStash {
    std::deque<std::pair<int64_t, TxRef>> stash;  // (timestamp_us, tx)，FIFO
    mutable std::mutex mtx;                        // 叶子锁，不参与 lock-hierarchy

    // H-E metrics：监控持续溢出
    std::atomic<uint64_t> push_total{0};
    std::atomic<uint64_t> drop_full{0};
    std::atomic<uint64_t> drop_ttl{0};
    std::atomic<uint64_t> drain_total{0};

public:
    static constexpr size_t MAX_SIZE = MAX_SIZE_;
    static constexpr int64_t TTL_US = TTL_US_;

    // Push：满则丢弃最老（drop_full 计数），不阻塞，不等 GC
    void Push(TxRef tx) {
        std::lock_guard l(mtx);
        if (stash.size() >= MAX_SIZE) {
            stash.pop_front();
            drop_full.fetch_add(1, std::memory_order_relaxed);
        }
        stash.emplace_back(GetTimeMicros(), std::move(tx));
        push_total.fetch_add(1, std::memory_order_relaxed);
    }

    // Drain：消费式，最多取 N 条，调用方独占持有取出的 tx
    std::vector<TxRef> Drain(size_t max_count = 1000) {
        std::vector<TxRef> out;
        out.reserve(max_count);
        std::lock_guard l(mtx);
        while (!stash.empty() && out.size() < max_count) {
            out.push_back(std::move(stash.front().second));
            stash.pop_front();
        }
        drain_total.fetch_add(out.size(), std::memory_order_relaxed);
        return out;
    }

    // GC：单线程后台 60s 周期；持锁时间 O(过期数)，最多扫 1000 条让步
    void GC() {
        int64_t now = GetTimeMicros();
        size_t budget = 1000;
        std::lock_guard l(mtx);
        while (!stash.empty() && budget > 0) {
            if (now - stash.front().first <= TTL_US) break;
            stash.pop_front();
            drop_ttl.fetch_add(1, std::memory_order_relaxed);
            --budget;
        }
    }

    size_t Size() const {
        std::lock_guard l(mtx);
        return stash.size();
    }

    // H-E metrics 暴露给监控
    struct Metrics {
        uint64_t push_total, drop_full, drop_ttl, drain_total, current_size;
    };
    Metrics GetMetrics() const {
        return { push_total.load(), drop_full.load(), drop_ttl.load(),
                 drain_total.load(), Size() };
    }
};

using ReorgStash = TxStash<200000, 10 * 60 * 1000000LL>;
using RaceStash  = TxStash<100000,  5 * 60 * 1000000LL>;

extern ReorgStash g_reorg_stash;
extern RaceStash  g_race_stash;

void ChainDispatcher::TriggerReorgResubmit(std::vector<TxRef> txs, int reorg_depth) {
    // F6: 按断链深度动态扩上限
    size_t cap = std::clamp<size_t>(
        reorg_depth * 50000 * 3 / 2,        // depth × avg_tx_per_block × 1.5
        REORG_QUEUE_MIN, REORG_QUEUE_MAX);

    std::unique_lock l(reorg_queue_mtx);
    for (auto& tx : txs) {
        if (reorg_queue.size() >= cap) {
            // 溢出 → 专用 reorg-stash（不进 orphan_txns）
            g_reorg_stash.Push(tx);
            continue;
        }
        WorkItem item;
        item.tx = tx;
        item.is_reorg_resubmit = true;
        reorg_queue.push_back(std::move(item));
    }
    reorg_cv.notify_all();
}

// reorg_worker 空闲时从 reorg-stash 回灌
void ChainDispatcher::ReorgStashDrain() {
    auto drained = g_reorg_stash.Drain();
    if (drained.empty()) return;
    // 重新 TriggerReorgResubmit；依然走 reorg 路径不消耗 token bucket
    TriggerReorgResubmit(std::move(drained), /*reorg_depth=*/1);
}

// reorg_worker 串行消费，不抢 normal worker 资源
void ChainDispatcher::ReorgWorkerLoop() {
    while (running) {
        WorkItem item;
        bool got_item = false;
        {
            std::unique_lock l(reorg_queue_mtx);
            // F6: 队列空时尝试从 reorg-stash 回灌（每 1s 唤醒一次）
            if (reorg_cv.wait_for(l, std::chrono::seconds(1),
                    [this] { return !reorg_queue.empty() || !running; })) {
                if (!running) break;
                item = std::move(reorg_queue.front());
                reorg_queue.pop_front();
                got_item = true;
            }
        }
        if (!got_item) {
            ReorgStashDrain();
            continue;
        }
        // 派发到 normal worker pool（不消耗 token bucket）
        WorkerId target = FindWorkerForChain(*item.tx);
        if (target == WORKER_NONE) target = PickLeastLoadedWorker();
        workers[target]->Push(std::move(item));
    }
}
```

---

## 3. PerChainWorker 详细设计

### 3.1 worker thread loop

```cpp
void PerChainWorker::Run() {
    SetThreadName("validator-" + std::to_string(id));
    while (running) {
        WorkItem item;
        {
            std::unique_lock lock(queue_mtx);
            cv.wait(lock, [this] { return !queue.empty() || !running; });
            if (!running) break;
            item = std::move(queue.front());
            queue.pop_front();
        }
        last_progress_us = GetTimeMicros();
        ProcessItem(std::move(item));
    }
}
```

### 3.2 ProcessItem 详细流程（v2.4 修订）

```cpp
void PerChainWorker::ProcessItem(WorkItem item) {
    TxRef tx = item.tx;
    TxId txid = tx->GetId();

    // H5 修复：用显式 commit_succeeded 标志，不依赖 uncaught_exceptions()
    bool commit_succeeded = false;
    auto guard = MakeScopeGuard([&]() noexcept {
        try {
            if (!commit_succeeded) {
                GetDispatcher().MarkAborted(txid);
            }
        } catch (...) { /* never rethrow from guard */ }
    });

    try {
        // === 阶段 1: 拍 chain snap（无锁 seqlock，~30ns）===
        auto snap = GetChainstate().Capture();

        // === 阶段 2: 实时读 input UTXO ===
        std::vector<Coin> input_coins;
        bool inputs_ok = true;
        {
            std::shared_lock m_lock(g_mempool.smtx);
            // GetCoinConcurrent 内部持 shared_lock(batchWriteMtx)，已封装

            for (const auto& in : tx->vin) {
                auto parent_tx = g_mempool.GetNL(in.prevout.GetTxId());
                if (parent_tx && in.prevout.GetN() < parent_tx->vout.size()) {
                    input_coins.push_back(MakeCoinFromMempoolTx(*parent_tx, in.prevout.GetN(), snap.height));
                    continue;
                }
                Coin c;
                if (g_pcoinsTip->GetCoinConcurrent(in.prevout, c)) {
                    input_coins.push_back(c);
                } else {
                    inputs_ok = false;
                    break;
                }
            }
        }
        if (!inputs_ok) {
            FinishReject(item, "missing-inputs");
            commit_succeeded = true;  // 业务层"完成"，guard 不 abort
            return;
        }

        // === 阶段 3: VerifyScript（无锁 30ms）===
        // 用 snap.script_flags（next-block 语义）
        // perInputScriptFlags ≠ 0 时跳过 scriptcache（H1 修复 from v2.1）
        //
        // H-D + P-1 修复：worker 不持 cs_main，必须复刻 src/validation.cpp:3508-3514
        //   `GetInputScriptBlockHeight(coinHeight)` 等价逻辑：
        //     - MEMPOOL_HEIGHT 是哨兵常量（~0x7FFFFFFF），不是真实块高，必须先替换
        //     - 替换后用 chainActive.Height() + 1 ≡ snap.height + 1（worker 等价）
        //     - 然后用 IsGenesisEnabled（即与 genesisActivationHeight 比较 >=）
        // 等价于 src/validation.cpp:3597-3602 现行逻辑：
        //   int h = GetInputScriptBlockHeight(coin.GetHeight());
        //   if (IsGenesisEnabled(config, h)) perInputScriptFlags = SCRIPT_UTXO_AFTER_GENESIS;
        // P0.0a 强制单元测试矩阵：
        //   1. coin.height = chain block N (< genesisActivationHeight) → flags = 0
        //   2. coin.height = chain block N (>= genesisActivationHeight) → flags = SCRIPT_UTXO_AFTER_GENESIS
        //   3. coin.height = MEMPOOL_HEIGHT, snap.height + 1 < genesisActivationHeight → flags = 0
        //   4. coin.height = MEMPOOL_HEIGHT, snap.height + 1 >= genesisActivationHeight → flags = SCRIPT_UTXO_AFTER_GENESIS
        //   5. regtest 上跑 chainActive.Height() < genesisActivationHeight 边缘 case
        std::vector<uint32_t> per_input_flags(input_coins.size());
        for (size_t i = 0; i < input_coins.size(); i++) {
            int input_h = (input_coins[i].nHeight == MEMPOOL_HEIGHT)
                ? snap.height + 1                       // 等价于 chainActive.Height() + 1
                : input_coins[i].nHeight;
            per_input_flags[i] = (input_h >= snap.genesisActivationHeight)
                ? SCRIPT_UTXO_AFTER_GENESIS
                : 0;
        }
        CValidationState state = DoVerifyScripts(*tx, input_coins, snap, per_input_flags);
        if (!state.IsValid()) {
            FinishReject(item, state);
            commit_succeeded = true;
            return;
        }

        // === 阶段 4: commit + 4 项 doubleCheck ===
        auto snap2 = GetChainstate().Capture();  // 在拿 unique_lock 之前拍

        {
            std::unique_lock m_lock(g_mempool.smtx);

            // doubleCheck #1/#2 用本地 snap2，不再调 chainstate（C1 锁层级修复）
            if (snap2.tip_hash != snap.tip_hash) {
                GetDispatcher().Resubmit(std::move(item), ResubmitReason::TipChanged);
                commit_succeeded = true;  // 业务"完成"，guard 不 abort
                return;
            }
            if (snap2.script_flags != snap.script_flags) {
                GetDispatcher().Resubmit(std::move(item), ResubmitReason::FlagsChanged);
                commit_succeeded = true;
                return;
            }

            // doubleCheck #3a：mempool 内 in-flight 双花
            for (const auto& in : tx->vin) {
                if (g_doubleSpendDetector.IsSpentNL(in.prevout)) {
                    state.Invalid(false, REJECT_DUPLICATE, "double-spend");
                    FinishReject(item, state);
                    commit_succeeded = true;
                    return;
                }
            }

            // doubleCheck #3b（H7 修复）：always 重读所有 input
            // 不依赖 input_was_from_chain 标记数组（race 期间 mempool 父可能转 chain）
            for (size_t i = 0; i < tx->vin.size(); i++) {
                Coin c;
                if (!g_pcoinsTip->GetCoinConcurrent(tx->vin[i].prevout, c) || c.IsSpent()) {
                    auto parent = g_mempool.GetNL(tx->vin[i].prevout.GetTxId());
                    if (!parent) {
                        state.Invalid(false, REJECT_INVALID, "input-spent-or-missing");
                        FinishReject(item, state);
                        commit_succeeded = true;
                        return;
                    }
                    // mempool 父存在则 OK，#4 检查
                }
            }

            // doubleCheck #4：mempool 父仍存在
            for (const auto& in : tx->vin) {
                if (auto parent = g_mempool.GetNL(in.prevout.GetTxId()); !parent) {
                    Coin c;
                    if (!g_pcoinsTip->GetCoinConcurrent(in.prevout, c) || c.IsSpent()) {
                        state.Invalid(false, REJECT_INVALID, "parent-evicted");
                        FinishReject(item, state);
                        commit_succeeded = true;
                        return;
                    }
                }
            }

            // commit + 真 RAII rollback
            bool added = false;
            auto rollback = MakeScopeGuard([&]() noexcept {
                try {
                    if (added && !commit_succeeded) {
                        g_mempool.RemoveUncheckedNL(txid);
                    }
                } catch (...) { /* swallow */ }
            });

            CTxMemPoolEntry entry(tx, /* fee */, /* time */, snap2.height, ...);
            g_mempool.AddUncheckedNL(entry, ...);   // 不在此触发 trim（C8 修复 from v2.1）
            added = true;
            g_doubleSpendDetector.InsertNL(*tx);

            commit_succeeded = true;  // 此后 guard/rollback 不动作

            GetDispatcher().MarkCommitted(txid);
        }  // 释放 smtx

        // 阶段 5: 异步信号 + 水位反压
        if (item.promise) item.promise->set_value(state);
        g_signal_dispatcher.Enqueue(SignalType::TransactionAddedToMempool, tx);

        // 水位反压
        if (g_mempool.GetUsage() > g_mempool.GetMaxSize() * 1.2) {
            g_async_trim.NotifyUrgent();  // 专用 trim worker 处理
        } else if (g_mempool.GetUsage() > g_mempool.GetMaxSize() * 1.1) {
            g_async_trim.Notify();
        }
    } catch (const std::exception& e) {
        LogPrintf("Worker %d caught exception on tx %s: %s\n", id, txid.ToString(), e.what());
        if (item.promise) {
            CValidationState s;
            s.Error(std::string("worker-exception: ") + e.what());
            item.promise->set_value(s);
        }
        // 不设 commit_succeeded → guard 触发 MarkAborted
    }
    // guard 析构：commit_succeeded ? 不动作 : MarkAborted
}
```

---

## 4. CCoinsViewCache 并发改造

详见 §1.3。要点：

- libcuckoo 单 bucket 内原子（K2 用 insert + update_fn fallback）
- batchWriteMtx 读写锁保证块级原子（C3 解法 B）
- 二级 LRU 缓解 LevelDB 慢路径（H3）
- IsBatchWriteInProgress 用 try_lock 判断（M-v3-2，无窗口）

---

## 5. ConnectBlock 协调机制（C-B 修订：消除 phantom-tip 窗口）

**C-B 共识 race 描述**（v2.5 隐患）：原次序 `view.Flush() → UpdateTip(seqlock)`。worker 阶段 1 无锁拍 `snap`（~30ns）若刚好落在 Flush 完成、UpdateTip 未完成的窗口：
- `snap.tip_hash` = 旧 tip
- 阶段 2 在 `bw_lock` 释放后读 cacheCoins → 看到**新 UTXO**
- 阶段 3 用 `snap.script_flags`（旧）跑 30ms VerifyScript
- 阶段 4 拍 `snap2`，此时 UpdateTip 已完成，`snap2.tip_hash != snap.tip_hash` → Resubmit

doubleCheck 救回——但前 30ms 已经在"旧 script_flags + 新 UTXO 视图"上跑过验证。在激活高度 ±1 块的边缘 case 上，验证结果跟新 tip 在共识上不同（虽然此次会 Resubmit 重跑，但若 worker 在 Resubmit 前已写 sigcache，会污染后续验证；perInputScriptFlags 跳 sigcache 是部分缓解但非全覆盖）。

**修复（C-B）**：UpdateTip 移到 view.Flush() 之前：

```cpp
bool ConnectBlock(const CBlock& block, CValidationState& state, CBlockIndex* pindex, ...) {
    AssertLockHeld(cs_main);

    CCoinsViewCache view(pcoinsTip.get());   // 注：pcoinsTip 已是 shared_ptr，详见 P0.4a
    DoVerifyBlockScripts(block, view, ...);
    UpdateCoinsForBlock(block, view, pindex);

    // === 提交阶段：按 §0 锁层级原子提交 ===
    {
        std::unique_lock m_lock(g_mempool.smtx);              // level 1
        std::unique_lock bw_lock(pcoinsTip->batchWriteMtx);   // level 2

        // 1. C-B: 先更新 chainstate（seqlock writer，cs_main 已持有）
        //    worker 任何时刻拍 snap 都满足 "tip_hash 跟 script_flags 同 epoch"
        GetChainstate().UpdateTip(pindex, GetBlockScriptFlags(...));

        // 2. Flush UTXO 变更（此时新 UTXO 跟新 tip 在 batchWriteMtx unique 保护下同步可见）
        //    P-2 失败处理：当前生产代码（validation.cpp:4944 系列）对 Flush 失败用 assert(flushed)
        //    硬 abort——cache 已部分写、mid-flush 状态不可恢复。v2.6 沿用此语义但加显式日志：
        if (!view.Flush()) {
            // 进退两难：seqlock 已写新 tip（worker 看到新 epoch）但 cacheCoins 处于半新半旧状态
            // 任何回滚 UpdateTip 都需在 worker 已经读 snap 之后做，引入更深 race
            // 唯一安全选择：abort 节点，下次启动从 LevelDB 恢复（chainstate format 不变）
            LogPrintf("FATAL: view.Flush() failed after UpdateTip(%s)；abort to preserve consistency\n",
                      pindex->GetBlockHash().ToString());
            std::abort();
            // 等价于现行 assert(flushed)，但显式记录是 v2.6 锁次序的代价
        }

        // 3. 移除 mempool 中已上链 tx（含 dsDetector 清理）
        g_mempool.RemoveForBlockNL(block.vtx, ...);
    }  // 释放 smtx + bw（保证顺序：先 bw 后 smtx，反向 LIFO）

    // P-2 不变量：Flush 失败 → abort → 下次启动从 LevelDB 重建 chainstate
    //   - LevelDB 是 ConnectBlock 之前的快照（FlushStateToDisk 在更上层 cs_main 帧内做）
    //   - LevelDB 中 chainstate 跟 cacheCoins 内 BatchWrite 之前的状态一致
    //   - 启动时 pcoinsTip 重建，自然回到上一致 tip，无 reindex
    //   - 该路径在 P0.0a P0.0a.X 加 fault-injection 单元测试覆盖
    //   - std::abort 不调析构（cs_main / smtx / batchWriteMtx 不解锁），但进程退出由 OS
    //     回收整个虚拟地址空间；LevelDB 的 LOCK 文件（fcntl POSIX advisory lock）和
    //     bitcoind.pid 都随 fd close 自动释放，无 datadir 污染。Windows 的 CreateFile 锁同语义。

    // 4. 异步刷新 GBT 模板
    g_gbt_snapshot.RefreshAsync();

    // 5. 异步信号
    g_signal_dispatcher.Enqueue(SignalType::BlockConnected2, pindex);
    return true;
}
```

**为什么这样不引入新 race**：

- worker 阶段 1 拍 `snap` 落在 UpdateTip 之前：`snap.tip_hash` 旧，阶段 2 读 cacheCoins 受 `batchWriteMtx unique` 阻塞——必然等 BatchWrite 完成；BatchWrite 完成时 UpdateTip 已经先做完（同 cs_main 帧内）。阶段 4 拍 snap2 必拿到新 tip → Resubmit。**OK**。
- worker 阶段 1 拍 `snap` 落在 UpdateTip 之后、view.Flush() 之前：`snap.tip_hash` = 新 tip，阶段 2 仍被 `batchWriteMtx unique` 阻塞→等 Flush 完→读 cacheCoins 新 UTXO。`snap.script_flags` 跟新 tip 同 epoch，验证语义正确。
- worker 阶段 1 拍 `snap` 落在 view.Flush() 之后：`snap.tip_hash` = 新 tip，阶段 2 等 `bw_lock` 释放即读到新 UTXO。一致。

**关键不变量**：seqlock 单 epoch 内 `tip_hash + script_flags + tip_index + height + mtp + genesisActivationHeight` 集体原子翻新；BatchWrite 在 epoch 翻新之后才暴露新 UTXO。worker 阶段 1+2+3 即便跨 epoch 拍 snap，doubleCheck #1（`snap2.tip_hash != snap.tip_hash`）会捕获并 Resubmit；阶段 3 验证用的 script_flags 跟此 epoch 的 tip 一致，不会跨配置混读。

**MED-2 修复保留**：smtx unique 期间 view.Flush() 完成后才释放 smtx，#3b GetCoinConcurrent 看到一致状态。

**worker 在 ConnectBlock 期间的行为**：

```
worker 阶段 1（拍 snap）         → 无锁 seqlock，不阻塞；epoch 单调
worker 阶段 2（读 UTXO）         → shared(smtx) + shared(batchWriteMtx) → 阻塞
worker 阶段 3（VerifyScript 无锁）→ 已经在跑的继续跑（snap 不变 → 无 race）
worker 阶段 4（commit）          → unique(smtx) → 阻塞
                                 → ConnectBlock 完成后拿到锁 → doubleCheck 发现 tip 变 → Resubmit
```

平均 TPS 损失 1-2.5%（详见概要 §2.3）。

---

## 6. doubleCheck 协议

### 6.1 4 项必查（C1 修复：全部用本地 snap2，不在 smtx 内调 chainstate）

| # | 检查 | 方法 | 失败处理 |
|---|------|------|---------|
| 1 | tip 没变 | `snap2.tip_hash != snap.tip_hash`（本地比对） | Resubmit(TipChanged) |
| 2 | script_flags 没变 | `snap2.script_flags != snap.script_flags` | Resubmit(FlagsChanged) |
| 3a | mempool 内 in-flight 双花 | `g_doubleSpendDetector.IsSpentNL(input.prevout)` | reject(double-spend) |
| 3b | chain UTXO 已花重读（H7） | `GetCoinConcurrent + IsSpent`（always 所有 input） | reject(input-spent) 或 mempool 父兜底 |
| 4 | mempool 父仍在 / 已上链未花 | `mempool.ExistsNL` ∥ `GetCoinConcurrent + !IsSpent` | reject(parent-evicted) |

### 6.2 Resubmit 上限分两类

```cpp
constexpr int MAX_RETRY_RACE = 10;   // tip_hash / script_flags 变（无害）
constexpr int MAX_RETRY_HARD = 1;    // double-spend / missing-inputs（真失败）

// race retry 用指数退避：1ms, 2ms, 4ms, ...
// 超 MAX_RETRY_RACE → 落 orphan pool 等待主动重试

// reorg Resubmit 走独立队列，不消耗 token bucket
```

---

## 7. 配套基础设施

### 7.1 ScopeGuard（H8 修复，无条件 noexcept）

```cpp
// src/util/scope_guard.h（新文件）
template<typename F>
class ScopeGuard {
    F f;
    bool armed = true;
public:
    explicit ScopeGuard(F&& f_) noexcept : f(std::move(f_)) {}

    // H8: 无条件 noexcept，内部 try/catch 防 terminate
    ~ScopeGuard() noexcept {
        if (armed) {
            try { f(); }
            catch (...) {
                LogPrintf("ScopeGuard handler threw, swallowed\n");
            }
        }
    }

    void Disarm() noexcept { armed = false; }

    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard(ScopeGuard&&) = delete;
};

template<typename F>
ScopeGuard<std::decay_t<F>> MakeScopeGuard(F&& f) {
    return ScopeGuard<std::decay_t<F>>(std::forward<F>(f));
}
```

### 7.2 AsyncTrim（C8+M2 修复，F8 trade-off 校正）

**F8 事实校正**：v2.4 声称 "AsyncTrim 不阻塞 commit"——只是不在同帧栈，仍互斥 `mempool.smtx` unique_lock。trim 单次 evict 1000 个 tx ≈ 10ms，evict 10000 个 ≈ 50ms。mempool 满载时累计可能 100-300ms。**这是真 trade-off**，需在概要 §3.1 明确。

```cpp
// src/mempool/async_trim.h（新文件）
class AsyncTrim {
    std::condition_variable cv;
    std::mutex mtx;
    std::atomic<bool> urgent{false};
    std::atomic<bool> running{true};
    std::thread t;

public:
    void NotifyUrgent() noexcept { urgent.store(true); cv.notify_one(); }
    void Notify() noexcept { cv.notify_one(); }

    void Run() {
        SetThreadName("trim");
        while (running) {
            std::unique_lock l(mtx);
            cv.wait_for(l, std::chrono::milliseconds(urgent.exchange(false) ? 0 : 10));
            if (!running) break;

            // F8: 专用 trim 线程持 smtx unique，跟 commit 路径互斥（不在同帧栈，但仍互斥）
            // 单次 evict ≤ TRIM_BATCH_SIZE（默认 1000），控制持锁时长 ≤ 10ms
            // 大批 evict 拆多次，让 commit 路径有间隙抢锁
            constexpr size_t TRIM_BATCH_SIZE = 1000;
            for (int i = 0; i < 10; i++) {  // 最多 10 批 = 10000 tx
                std::unique_lock m(g_mempool.smtx);
                size_t evicted = g_mempool.LimitMempoolSizeNL(TRIM_BATCH_SIZE);
                m.unlock();   // 主动释放给 commit 路径机会
                if (evicted < TRIM_BATCH_SIZE) break;  // 完成
                std::this_thread::yield();
            }
        }
    }
};
```

**新建议**：`LimitMempoolSizeNL` 增加 `max_evict` 参数返回实际 evict 数量，便于 trim 拆批控制持锁粒度。该改动作为 P3.4 子任务。

### 7.3 GbtSnapshotProvider（F9 修订：单 refresh worker + 合并队列）

**F9 修订理由**：v2.4 用 `std::thread().detach()` 每次 refresh 启新线程。连续 reorg/BatchWrite 期间每秒可能启 N 个线程并发拷贝百万级 mempool，加上跟 ConnectBlock unique_lock 抢锁——线程爆炸 + 锁争抢双重退化。改单 refresh worker + 合并队列：连续触发只产生最新一次的工作量。

```cpp
// src/mining/gbt_snapshot.h（新文件，v2.5 修订）
struct MempoolSnapshot {
    uint256 tip_hash;
    int32_t height;
    std::vector<TxRef> txs;
};

class GbtSnapshotProvider {
    mutable std::shared_mutex snap_mtx;
    std::shared_ptr<const MempoolSnapshot> last_stable;
    std::condition_variable_any refresh_cv;

    // F9: 单 refresh worker + 合并队列
    std::thread refresh_thread;
    std::mutex refresh_mtx;
    std::condition_variable refresh_trigger_cv;
    std::atomic<bool> pending{false};        // 合并标志：连续触发只产生 1 次实际拷贝
    std::atomic<bool> running{true};

public:
    void Start() {
        refresh_thread = std::thread(&GbtSnapshotProvider::RefreshLoop, this);
    }
    void Stop() {
        running = false;
        refresh_trigger_cv.notify_all();
        if (refresh_thread.joinable()) refresh_thread.join();
    }

    // ConnectBlock 释放 smtx 后调（持 cs_main，已 RemoveForBlock 完成）
    // 不阻塞，仅设 pending 标志
    void RefreshAsync() {
        pending.store(true, std::memory_order_release);
        refresh_trigger_cv.notify_one();
    }

    // 单 worker 主循环（N4 修复：tip 读取走 seqlock，不反向取 cs_main）
    void RefreshLoop() {
        SetThreadName("gbt-refresh");
        while (running) {
            {
                std::unique_lock l(refresh_mtx);
                refresh_trigger_cv.wait(l, [this] {
                    return pending.load(std::memory_order_acquire) || !running;
                });
                if (!running) break;
                pending.store(false, std::memory_order_release);  // 合并：清标志
            }

            // N4: 走 chainstate seqlock 读 tip，避免在 mempool.smtx 内反向取 cs_main
            // 锁顺序保持 smtx(level 1) 内不再向上取 cs_main(level 0)
            auto chain_snap = GetChainstate().Capture();
            auto fresh = std::make_shared<MempoolSnapshot>();
            fresh->tip_hash = chain_snap.tip_hash;
            fresh->height = chain_snap.height;
            {
                std::shared_lock m(g_mempool.smtx);
                for (auto it = g_mempool.mapTx.begin(); it != g_mempool.mapTx.end(); ++it) {
                    if (!IsTxConfirmed(it->GetTx().GetId(), fresh->tip_hash)) {
                        fresh->txs.push_back(it->GetSharedTx());
                    }
                }
            }
            // 注：seqlock 拍 snap 与 mempool 读取之间可能跨一次 BatchWrite，
            // RefreshAsync 是 ConnectBlock 释放 smtx 后触发，所以高频场景下下次合并触发会得到一致快照
            // 调用方（GBT WaitFresh）通过 expected_tip 比对处理 stale
            {
                std::unique_lock l(snap_mtx);
                last_stable = fresh;
            }
            refresh_cv.notify_all();
        }
    }

    // condvar 长轮询，最长 timeout 等到 fresh
    std::shared_ptr<const MempoolSnapshot> WaitFresh(uint256 expected_tip,
                                                      std::chrono::milliseconds timeout) {
        std::unique_lock l(snap_mtx);
        if (!refresh_cv.wait_for(l, timeout, [&]() {
            return last_stable && last_stable->tip_hash == expected_tip;
        })) {
            return last_stable;   // 超时返回旧的，调用方比对 tip_hash 处理 stale
        }
        return last_stable;
    }
};
```

**好处**：
- 连续 RefreshAsync N 次只跑 1 次实际拷贝（合并）
- 单线程，不会爆炸
- Stop 路径明确，不依赖 detach

### 7.4 SignalDispatcher（H5 修复，per-tx FIFO）

```cpp
class SignalDispatcher {
    std::deque<SignalEvent> queue;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<uint64_t> global_seq{0};

public:
    void Enqueue(SignalType t, TxRef tx) noexcept {
        std::lock_guard l(mtx);
        queue.push_back({t, tx, global_seq.fetch_add(1)});
        cv.notify_one();
    }

    void Run() {
        SetThreadName("signal-dispatcher");
        // 单线程顺序分发到 wallet / ZMQ / REST / GBT 订阅者
        // per-tx FIFO 由 global_seq 保证
    }
};
```

---

## 8. RPC 入口替换

### 8.1 sendrawtransaction（F1 修复：用新 REJECT_OVERLOADED = 0x45）

**F1 事实更正**：v2.4 声称 `REJECT_TOOBUSY = 0x44` "已存在但未使用"——错。`src/net/net_processing.cpp:1194/1204/1608` 已用作 GETDATA 拒绝码。如果 RPC submit 路径也返回 0x44，客户端无法区分两种 busy 来源。

**改造**：在 `src/consensus/validation.h` 新增独立 reject 码

```cpp
// src/consensus/validation.h（追加）
static const uint8_t REJECT_OVERLOADED = 0x45;  // RPC submit 路径专用
```

```cpp
UniValue sendrawtransaction(const Config& config, const JSONRPCRequest& request) {
    TxRef tx = DecodeHexTx(request.params[0].get_str());
    CValidationState state = GetDispatcher().SubmitSync(tx);
    if (state.IsValid()) return tx->GetId().ToString();

    if (state.GetRejectCode() == REJECT_OVERLOADED) {
        // HTTP 200 + JSON-RPC error，客户端按 503 语义处理
        throw JSONRPCError(RPC_VERIFY_REJECTED, "server-busy: " + state.GetRejectReason());
    }
    throw JSONRPCError(RPC_TRANSACTION_REJECTED, FormatStateMessage(state));
}

// dispatcher 入口
CValidationState ChainDispatcher::SubmitSync(TxRef tx) {
    if (pcoinsTip->IsBatchWriteInProgress()) {
        CValidationState s;
        s.Invalid(false, REJECT_OVERLOADED, "server-busy-batch-write");
        return s;
    }
    // ... 派发
}
```

### 8.2 sendrawtransactions（F7：保留逐笔语义，向后兼容）

```cpp
// 老 API：逐笔派发，部分成功语义不变；不做 TopoSort（如客户端错排序，逐笔失败由 doubleCheck 兜底）
UniValue sendrawtransactions(const Config& config, const JSONRPCRequest& request) {
    auto txs = DecodeHexTxArray(request.params[0]);
    std::vector<CValidationState> results;
    for (auto& tx : txs) {
        results.push_back(GetDispatcher().SubmitSync(tx));   // 单笔
    }
    return BuildBatchResult(results);   // 跟现状格式 100% 一致
}
```

### 8.2a submitrawtransactions（F7 新增 RPC，**ordered + topo-sorted, best-effort**）

**C-C 语义澄清（v2.6）**：v2.5 写"atomic"误导客户端期望 ACID 回滚。实际方案是：

1. **拓扑排序原子**：TopoSort 入口检测环 / 重复 txid → 整批 REJECT（无任何 tx commit）
2. **逐笔 commit best-effort**：拓扑排序通过后逐笔 commit；一笔失败不回滚已 commit 的前面 N 笔
3. **batch-budget timeout**：整批 30s 超时（不是 per-tx），保证 HTTP 响应不阻塞 50 分钟
4. **客户端契约**：返回值数组 `[{txid, status}]`，调用方按状态做幂等处理（已 commit 的 txid 在 mempool / chain 中，重复提交会被 RecentRejects 兜底）

```cpp
// 新 API：ordered + topo-sorted, best-effort（不是 ACID）
UniValue submitrawtransactions(const Config& config, const JSONRPCRequest& request) {
    auto txs = DecodeHexTxArray(request.params[0]);
    auto results = GetDispatcher().SubmitBatchSync(txs);   // TopoSort + 环检测 + batch-budget 30s
    // results[i].status ∈ {accepted, rejected, timeout, batch-rejected-cycle, ...}
    return BuildBatchResult(results);
}
```

**为什么不做真 ACID**：
- mempool 不支持事务回滚（无 undo log）
- 跨 worker / 跨 BatchWrite 边界做事务会反向锁 cs_main
- 真 ACID 跟 RPC 短超时（30s）不兼容
- 现行 BSV / TBC 节点没有节点接受 atomic batch 语义；改 ACID 会让 v2.6 跟生态不互通

**RPC 文档明文**：调用方需准备处理"前 N 笔成功、第 N+1 笔失败、剩余 timeout"的混合返回。

**RPC 兼容性矩阵**：

| RPC | 行为 | v2.5 后 |
|----|----|----|
| `sendrawtransaction` 单笔 | 同步等结果 | ✅ 不变（走 SubmitSync） |
| `sendrawtransactions` 批 | 部分成功 | ✅ **不变**，逐笔语义保留 |
| `submitrawtransactions` 批 | ordered + topo-sorted, best-effort | ✅ 新增；C-C：非 ACID，已 commit 不回滚 |
| `getrawmempool` | 读 mempool | ✅ 不变（shared_lock(smtx)） |
| `getblocktemplate` | GBT | ⚠️ 长轮询最长 15s（详见 §8.4） |
| `waitformempoolentry` | 等 mempool 入池 | ✅ 新增（详见 §8.7） |

### 8.3 P2P 入口（异步）

```cpp
void ProcessTxMessage(CNode* pfrom, ...) {
    TxRef tx = ReadTxFromMessage(...);
    GetDispatcher().SubmitAsync(tx);
}
```

### 8.4 getblocktemplate（GBT 长轮询）

```cpp
UniValue getblocktemplate(const Config& config, const JSONRPCRequest& request) {
    uint256 current_tip;
    {
        LOCK(cs_main);
        current_tip = chainActive.Tip()->GetBlockHash();
    }

    auto snap = g_gbt_snapshot.WaitFresh(current_tip, std::chrono::seconds(15));
    if (!snap || snap->tip_hash != current_tip) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "template-rebuilding");
    }
    // 用 snap 构建模板
    return BuildBlockTemplate(snap);
}
```

### 8.5 废除的代码

```
src/txn_validator.cpp
  - processValidation 单笔/批量两个版本
  - threadNewTxnHandler
  - mStdTxns / mNonStdTxns / mProcessingQueue
  - mMainMtx / mMainCV / mTxnsProcessedCV

src/net/validation_scheduler.h/cpp
  - ValidationScheduler / TOPO_SORT 整个

src/net.h
  - ParallelTxnValidation 模板
```

### 8.6 保留的代码

```
src/orphan_txns.h/cpp         保留，dispatcher 集成（P5.4）
src/txn_recent_rejects.h/cpp  保留
src/txn_double_spend_detector 保留（仅 mempool 内 in-flight）
src/txmempool.*               保留（只用 mempool.smtx）
src/script/*                  保留
src/consensus/*               保留
```

### 8.7 waitformempoolentry（MED-3 修复，给交易所同步语义）

```cpp
UniValue waitformempoolentry(const Config& config, const JSONRPCRequest& request) {
    TxId txid = ParseHashV(request.params[0], "txid");
    int timeout_ms = request.params[1].get_int();

    // 注册 txid 监听 g_signal_dispatcher 的 TransactionAddedToMempool
    auto handle = g_signal_dispatcher.Subscribe(txid);
    if (handle.WaitFor(std::chrono::milliseconds(timeout_ms))) {
        return UniValue(true);
    }
    throw JSONRPCError(RPC_MISC_ERROR, "timeout");
}
```

---

## 9. 阶段子任务详细计划

### P0.0a：基础设施 spike（5 周，v2.6 H-G +1 周）

| 子任务 | 工时 |
|-------|-----|
| P0.0a.1 libcuckoo 集成 + 5000 万 entry × 24h soak | 1 周 |
| P0.0a.2 TSan / helgrind 24h（基础并发原语） | 1 周 |
| P0.0a.3 lock-hierarchy.md v0.1 + LEVEL_* 常量 | 1 周 |
| **P0.0a.4 AssertLockOrder hook 改造 sync.h（H-G 非破坏性增量：默认 LEVEL_DEFAULT，401 callsite 零修改 + 新 mutex 显式标 level + DEBUG_LOCKORDER 启用） + P0.0a.5 boost::recursive_mutex try_lock 语义 spike + H-F 异常注入测试** | **2 周** |

**P0.0a.5（N3 新增 + H-F 扩展）**：F2 单写者运行时守卫依赖 `boost::recursive_mutex::try_lock` 在「同线程已持锁」返回 true、「跨线程」返回 false 的语义。需在 4 个 boost 版本（1.65 / 1.71 / 1.74 / 1.83）上各跑一组单元测试：

1. 主线程持锁 → 主线程 try_lock → 期望 true，count 递增 1，RAII unique_lock 析构后 count 平衡
2. 主线程持锁 → 副线程 try_lock → 期望 false（核心断言）
3. 主线程持锁 N 次（递归）→ 主线程 try_lock → 期望 true，仍可正确 unlock 平衡
4. 高频路径 baseline：try_lock + unlock 100 万次单线程开销 → 期望 < 10ns/次
5. **H-F 异常注入**：try_lock 成功后、unique_lock 析构前注入异常（模拟 LogPrintf OOM 等）→ 期望 cs_main count 平衡（unique_lock RAII 析构保证）；用 ASan + helgrind 验证无锁泄漏

任一版本不符合预期 → F2 守卫塌方，必须改用其它机制（候选方案：`std::atomic<std::thread::id> writer_owner` + `compare_exchange`，性能略低但语义完全可控）。**KPI 写入 M-1a 决策门**。

### P0.0b：正确性 + sign-off（6 周，v2.5 F4 上调）

| 子任务 | 工时 |
|-------|-----|
| P0.0b.1 seqlock memory-model 文档（writer/reader fence 完整证明）| 1 周 |
| P0.0b.2 BatchWrite p99 ≤ 200ms（10 万 UTXO 块）| 1 周 |
| **P0.0b.3 共识等价性 baseline（采样窗口：关键激活高度 ±1000 块 + 每 5000 块全量；4-8 实例并发）** | **3 周**（F4 上调） |
| P0.0b.4 v1 ↔ vN binary **smoke** round-trip（仅验证 sync.h + lock-hierarchy 改造不破坏 disk format）+ 业务方 sign-off | 1 周 |

**F4 baseline 采样策略**：TBC 链当前 ~16 万块（自 824190 起），全 replay + UTXO hash + getrawmempool 比对在 4-8 实例并发下经验值 3-4 周（单实例 reindex BSV 全链 15-30 天）。改采样窗口：

- 关键激活高度（schnorrMultisigHeight、kycV1/V2、Genesis 等）±1000 块全量 replay + 每块 `gettxoutsetinfo`
- 其余高度每 5000 块采样全量
- 整体覆盖率 > 30%，敏感区段 100%
- 风险：极小概率漏过非激活高度的边缘 case；mitigated by P6 shadow node 4 周全量验证

### P0：CCoinsViewCache 并发改造（14-18 周，v2.5 F3 +2 周）

| 子任务 | 工时 |
|-------|-----|
| P0.1 改造 cacheCoins 数据结构 + batchWriteMtx + metaMtx | 2 周 |
| P0.2 BatchWrite insert + update_fn fallback + erase_fn 路径（K2）| 2 周 |
| P0.3 GetCoinConcurrent L1/L2/L3 实现（K4）+ 二级 LRU | 2 周 |
| **P0.4a `pcoinsTip` shared_ptr lifetime sweep（F3 新增）** | **2 周** |
| P0.4b cachedCoinsUsage atomic + IsBatchWriteInProgress + 1h 漂移压测 | 1 周 |
| P0.5 老接口（GetCoin/HaveCoin/AddCoin）保持兼容 | 1 周 |
| P0.6 单元测试 + TSan/helgrind 72 小时压测 | 2-3 周 |
| P0.7 性能 benchmark：32 worker 并发 ≥ 16x 单线程（无 BatchWrite 窗口）| 2-3 周 |

**P0.4a 范围**（F3）：

- `extern CCoinsViewCache *pcoinsTip;` → `extern std::shared_ptr<CCoinsViewCache> pcoinsTip;`（src/validation.h:1094）
- `init.cpp` 启动序：构造改 `pcoinsTip = std::make_shared<...>(pcoinsdbview.get())`
- `init.cpp` Shutdown 序重排：worker pool stop → dispatcher stop → `pcoinsTip.reset()`（避免 worker 持 raw `pcoinsTip->` 后被 reset）
- 全代码库 raw `pcoinsTip->` / `pcoinsTip.get()` / `*pcoinsTip` 调用点扫描（grep + AST），在 worker hot path 改持 `std::shared_ptr<CCoinsViewCache>` 局部副本，验证完释放
- 与 `pcoinsdbview`、`pblocktree` 等同生命周期组件的 stop 顺序联调
- TSan + ASan 测试覆盖 worker 持 shared_ptr 跨 Shutdown 的 race

### P1：删除 ChainContext + chainActive 替换（4-6 周）

| 子任务 | 工时 |
|-------|-----|
| P1.1 Chainstate seqlock 实现（K1 fence 完整）| 1 周 |
| P1.2 worker 直读 g_chainstate.Capture()（删除 ChainContext）| 1 周 |
| P1.3 IsXxxEnabled / GetBlockScriptFlags 接受 Snapshot& | 1 周 |
| P1.4 TxnValidation 13 处 chainActive 直接读 → snap | 1 周 |
| P1.5 CheckSequenceLocks 改造（TBC Genesis 后等价 return true）| 0.5 周 |
| P1.6 AST grep 穷举 260 处 chainActive 替换 | 0.5-1 周 |
| P1.7 单元测试 + 回归 | 1 周 |

### P2：ChainDispatcher + Worker pool（8-12 周）

| 子任务 | 工时 |
|-------|-----|
| P2.1 ChainDispatcher 数据结构 + 16-shard inflight 状态机 | 2 周 |
| P2.2 路由策略（投票 + power-of-two-choices + input cap） | 1-2 周 |
| P2.3 PerChainWorker thread loop + queue + cv | 2 周 |
| P2.4 reorg 独立队列 + token bucket + ReorgWorkerLoop | 1-2 周 |
| P2.5 SubmitBatchSync TopoSort（环检测 + 重复 txid）| 1 周 |
| P2.6 GC 线程 + 错峰 try_lock + 饥饿保护 | 1 周 |
| P2.7 异常捕获 + worker watchdog（软停语义）| 1 周 |
| P2.8 单元测试 + 多 worker 并发压测 | 2-3 周 |

### P3：worker 内 commit + 4 项 doubleCheck + AsyncTrim + RPC busy + GBT（6-8 周）

| 子任务 | 工时 |
|-------|-----|
| P3.1 doubleCheck 4 项实现（统一 unique_lock 内）| 1-2 周 |
| P3.2 Resubmit 双类策略（race 10 / hard 1）+ 30s 超时 | 1 周 |
| P3.3 worker 内 commit（AddUncheckedNL + dsDetector + 真 RAII rollback） | 1-2 周 |
| P3.4 AsyncTrim 专用线程 | 0.5 周 |
| P3.5 RPC busy → REJECT_OVERLOADED (0x45) 路径 + 客户端兼容性测试 | 0.5 周 |
| P3.6 GbtSnapshotProvider + condvar 长轮询 | 1-2 周 |
| P3.7 SignalDispatcher per-tx FIFO | 1 周 |

### P4：ConnectBlock 切细粒度锁 + 子系统验证（14-18 周，v2.5 F10 +2 周）

| 子任务 | 工时 |
|-------|-----|
| P4.1 ConnectBlock 提交阶段三锁原子提交 | 2 周 |
| P4.2 worker 协调测试（reorg 期间 worker 行为）| 2-3 周 |
| P4.3 BatchWrite 阻塞时长 benchmark + 分片优化预研 | 1-2 周 |
| P4.4 RemoveForBlock 跟 worker commit 同步 | 1 周 |
| **P4.5a 子系统影响验证（wallet/ZMQ/REST/GBT/reorg/orphan/pruning/indexer）** | 3-4 周 |
| **P4.5b subscriber 反向锁审计（F10 新增 + H-H 扩展） — 7+ 已知订阅点全量审计** | **4 周** |
| P4.6 lock-hierarchy DEBUG_LOCKORDER + AssertLockOrder 全量验证 + 新文件 TSA 注解 | 4-6 周 |

**P4.5b 范围（F10 + H-H 扩展）**：worker 完成 commit 后异步信号经 `g_signal_dispatcher` 派发给 wallet / ZMQ / REST / GBT / indexer。这些 subscriber 老代码常假设可重入 cs_main。signal_dispatcher 单线程异步派发跟 worker 不同帧栈——不会跟 worker 自己持锁冲突，但 signal 线程持 cs_main 后再调 mempool/pcoinsTip 必须按 lock-hierarchy 顺序。

**已知 7+ 订阅点（H-H 列出，每条产出"锁顺序矩阵"）**：

| # | 订阅点 | 已知/疑似锁路径 | 修复方案候选 |
|--|------|---------|-----|
| 1 | `src/wallet/wallet.cpp::TransactionAddedToMempool` | LOCK(cs_wallet) → 调 ChainstateManager / mempool（旧路径）| 重排：先 chainstate 后 cs_wallet；或拆 worker thread |
| 2 | `src/wallet/wallet.cpp::TransactionRemovedFromMempool` | 同上 | 同上 |
| 3 | `src/wallet/wallet.cpp::BlockConnected2 / BlockDisconnected` | LOCK(cs_wallet) → 调 mempool（reorg 路径）| 异步化（wallet 内部 queue） |
| 4 | `src/zmq/zmqnotificationinterface.cpp::TransactionAddedToMempool / BlockConnected2` | 通过 ZMQ socket send（无锁），但内部可能 LOCK(zmqpub_mtx) | 验证 zmq mtx 不参与 lock-hierarchy（叶子锁） |
| 5 | `src/index/txindex.cpp::SyncTransaction / BlockConnected2` | LOCK(cs_main) → m_db Write | 已是 cs_main 内串行；保留 |
| 6 | `src/mining/journaling_block_assembler.cpp` 信号订阅 | LOCK(cs_assembler) → 读 mempool（shared）| 验证锁顺序 cs_assembler 是叶子层级 |
| 7 | `src/validationinterface.cpp::CMainSignals` 全部 `RegisterValidationInterface` 实现 | grep `class.*: public CValidationInterface` | 逐一过 |
| 8 | `src/rpc/blockchain.cpp::waitfornewblock / waitformempoolentry` | condvar 等 signal | 已是叶子，无反向 |
| 9 | `src/index/...`（如有未来 indexer 扩展） | 同 #5 模式 | 同 #5 |

**每条产出**："实际锁顺序"+"是否跟 lock-hierarchy 一致"+"修复方案（异步化 or 顺序重排 or 拆 worker thread）"+ 单元测试 / 多线程压测覆盖。

**工时 4 周分配**：
- 1 周：grep + 阅读 9 处代码、写"锁顺序矩阵"
- 2 周：实现修复（异步化最重，wallet 路径预计占大部分）
- 1 周：DEBUG_LOCKORDER 全量回归 + helgrind 72h

### P5：RPC 入口替换 + 废除现有 PTV（4-6 周）

| 子任务 | 工时 |
|-------|-----|
| P5.1 sendrawtransaction / sendrawtransactions 改走 dispatcher | 1 周 |
| P5.2 P2P SubmitAsync 集成 net_processing | 1 周 |
| P5.3 废除 processValidation / mNewTxnsThread / ValidationScheduler | 1-2 周 |
| P5.4 orphan_txns / txn_recent_rejects 跟 dispatcher 集成（含 3 层孤儿链 test）| 1 周 |
| P5.5 waitformempoolentry RPC 实现 | 0.5 周 |
| P5.6 集成测试 | 1 周 |

### P6：测试 + 灰度（14-20 周）

| 子任务 | 工时 |
|-------|-----|
| P6.1 单元测试 + functional test 100% 通过 | 1-2 周 |
| P6.2 TSan / ASan / helgrind 72 小时压测 | 1-2 周 |
| P6.3 regtest reorg 注入测试 + reorg 风暴 | 1-2 周 |
| P6.4 共识等价性测试（v2.4 vs origin/main UTXO hash + getrawmempool diff）| 2 周 |
| P6.5 retry 风暴回归测试 | 1 周 |
| P6.6 mempool diff 监控（接受性差异）| 1 周 |
| P6.7 开发网部署 + 4 周稳定运行观察 | 4 周 |
| P6.8 真主网 shadow node 4 周 | 4 周 |
| P6.9 真主网 canary + 渐进发布 | 4 周 |

---

## 10. 测试矩阵

### 10.1 单元测试

```bash
./build/src/test/test_bitcoin --run_test=*
./build/src/test/test_bitcoin --run_test=tbc_*
./build/src/test/test_bitcoin --run_test=filled_miner_bill_v2_*
./build/src/test/test_bitcoin --run_test=x_only_pubkey_*
```

### 10.2 Functional 测试

```bash
test/functional/test_runner.py --extended

# 重点必过
mempool_packages
mempool_resurrect
mempool_reorg
mempool_persist
rpc_getrawtransactiondata
```

### 10.3 Sanitizer

```bash
cmake -B build-tsan -S . -Denable_tsan=ON -DBUILD_BITCOIN_WALLET=OFF
cmake -B build-asan -S . -Denable_asan=ON
valgrind --tool=helgrind ./build/src/test/test_bitcoin
```

### 10.4 Stress harness

```python
# test/functional/stress_dispatcher.py
# 100 客户端并发 + reorg 注入 + 逆序批次 + 持续 1 小时
```

### 10.5 共识等价性

```bash
# v2.4 节点和 reference 节点跑同样块
# 每块连接后对比 gettxoutsetinfo hash_serialized_2
# 必须 100% 一致
# 采样：每 1000 块 + 关键激活高度（schnorrMultisigHeight, kycV1/V2）全量
```

### 10.6 v1 ↔ vN binary 互换演练（F5 拆两次）

**P0.0b smoke**（11 周决策门，5w P0.0a + 6w P0.0b）：

```bash
# 仅验证 sync.h + lock-hierarchy 改造、Chainstate seqlock 早期实现不破坏 disk format
# vN-smoke binary 跑 1000 regtest 块 → 关停 → v1 启动同步无 reindex
# 此时 cacheCoins / dispatcher / worker 都没接入，演练范围有限
```

**P3 末 full**（M2 决策门）：

```bash
# 完整方案接入后再演练一次
# vN binary 跑 1000 regtest 块（含 dispatcher 派发、libcuckoo cacheCoins、seqlock chainstate、worker commit）
#   → 关停 → v1 binary 启动同步无 reindex
# 反向：v1 跑 1000 块 → vN 启动 → 同样无 reindex
# 两路验证 chainstate / mempool.dat / block file 全格式真兼容
```

**理由**：P0.0b 时方案 90%+ 还没动，smoke round-trip 只是 sanity check；真兼容性要在 P3 末验。两次演练都过才进 P6 灰度。

### 10.7 mempool diff 监控（MED-4 + M-K 修订）

```python
# 周期性对比 vN vs reference 节点的 getrawmempool
# M-K: 现实主网每秒 10-30 笔正常分歧（fee policy / mempool eviction 时序差异常态）
#      v2.5 阈值 "10 笔" 在主网会持续误报
#
# v2.6 改两类阈值：
#   - 瞬态差异（time-bounded）：60s 累计 diff > 200 笔 → WARNING
#   - 持久差异（permanent）：单一 txid 在 vN 缺失但在 reference 存在 > 10 分钟 → ALERT
#                          这是真"接受性差异"信号
# 报警分级让运维区分"主网正常波动"和"v2.6 真共识 bug"
```

### 10.8 开发网灰度

```bash
# 部署 v2.4 节点到开发网
# 跑 4 周
# 监控：TPS、p99 延迟、错误率、chain tip lag、ban score、mempool diff
# 跟 reference 节点 chain tip 必须一致
```

---

## 11. 子系统影响矩阵

| 子系统 | 影响 | 回归测试 |
|-------|-----|---------|
| **Wallet** | `TransactionAddedToMempool` 信号改异步分发；`getbalance` 在 RPC 返回成功后短暂可能未更新（per-tx FIFO 序号保证最终一致）；提供 `waitformempoolentry` 同步 RPC；**F10：subscriber 内部 cs_main 反向锁路径需 P4.5b 全量审计** | wallet_basic.py / wallet_listsinceblock.py |
| **ZMQ** | `hashtx` / `rawtx` topic 通过 `g_signal_dispatcher` 异步发；顺序由 global_seq 保证 | zmq_test.py |
| **REST** | 只读，依赖 mempool.smtx shared_lock，行为一致 | interface_rest.py |
| **GBT (mining)** | `getblocktemplate` 走 GbtSnapshotProvider condvar 长轮询；BatchWrite 期间最多等 15s | mining_basic.py |
| **reorg** | DisconnectTip → RemoveForReorg → TriggerReorgResubmit（独立队列）；reorg 风暴下不消耗 normal token bucket；F6 动态上限（按断链深度，10000-100000）+ 溢出落专用 ReorgStash（200k 容量、10 分钟 TTL，不污染 orphan_txns）| mempool_reorg.py / reorg_stress.py（新增）|
| **orphan_txns** | dispatcher 在父 commit 后 trigger orphan replay；3 层孤儿链测试覆盖；token bucket / reorg 队列溢出落 orphan | orphan_test.py（新增 3 层）|
| **pruning** | 不受影响（chainstate format 不变） | pruning.py |
| **indexer (txindex)** | 信号顺序异步保证；rebuild 路径不变 | feature_txindex.py |

---

## 12. Race 处理一览

| Race | 处理 |
|-----|------|
| tip 变了（reorg / 新块）| doubleCheck #1 (snap2.tip_hash) → Resubmit(TipChanged) |
| 激活高度穿越 | doubleCheck #2 (snap2.script_flags) → Resubmit(FlagsChanged) |
| UTXO 被并发花（mempool）| doubleSpendDetector (#3a) → reject |
| chain UTXO 被并发花 | #3b GetCoinConcurrent always 重读 + #3a dsDetector + ScopeExit RAII rollback 三重保证 |
| mempool 父被驱逐 | doubleCheck #4 → reject 或重查链上 |
| sigcache 中毒 | sigcache key 含 script_flags（已有）；perInputScriptFlags ≠ 0 时跳过 cache |
| worker use-after-free pcoinsTip | shared_ptr + libcuckoo |
| dispatcher inflight race | 16-shard shared_mutex + 状态机 |
| worker queue race | mutex + cv |
| ConnectBlock 期间 worker 跑一半 | unique_lock 阻塞 + doubleCheck 兜底 |
| 多 worker 同时 commit 抢 mempool.smtx | unique_lock 串行（毫秒级）|
| Worker 验证完 commit 时父被另一 worker 已抢花 | doubleSpendDetector → reject |
| 父刚 COMMITTED 子来路由（GC 窗口）| 5ms 延迟 GC 覆盖窗口 |
| 父被 ABORTED（worker crash）子来路由 | 状态机 ABORTED 不计投票 + GC 立即清理 |
| 逆序批次 sendrawtransactions | SubmitBatchSync 入口 TopoSort + 环检测 + 重复 txid 检测 |
| GetCoinConcurrent 同 key 并发 cache miss | insert 返回 bool 区分 inserted，仅 inserted 才 fetch_add（K2/C5/C6）|
| BatchWrite 中途 worker 读半态 | batchWriteMtx 读写锁阻塞 worker（块级原子）|
| eviction 重入 mempool.smtx | 异步 trim 专用线程 |
| LevelDB 慢 worker 饿死 ConnectBlock | 二级 LRU + worker 数 cap |
| Resubmit 风暴 reorg | reorg 独立队列（不消耗 token bucket）+ F6 动态上限 10000-100000 + 溢出专用 ReorgStash（200k / 10min TTL，不污染 orphan_txns）|
| seqlock writer 撕裂读 | reader fence + memcpy + seq 重试 + writer fence 完整保证 |
| ScopeGuard 析构再抛异常 | 无条件 noexcept + 内部 try/catch |
| InflightAbortedGuard 误判 | 显式 commit_succeeded 标志 |
| GBT 100ms 重试风暴 | condvar 长轮询 15s |

---

## 相关文档

- [概要设计](./cs_main-refactor-plan.md)
- [审核记录](./cs_main-refactor-audit-log.md)
- [锁层级](./lock-hierarchy.md)（P0.0a 同步交付）
- [开发网纪律](../learn/24-dev-mainnet-parity.md)
