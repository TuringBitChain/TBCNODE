# 任务卡模板

```markdown
# P{phase}.{seq} {标题}

**工时**：{N 周}
**owner**：{TBD}
**前置**：{依赖任务卡 ID 列表，或"无"}
**阻塞**：{后续阻塞任务卡 ID}

---

## 1. 目标

{一句话说改什么。例："把 cacheCoins 从 std::unordered_map + std::mutex 换成 libcuckoo::cuckoohash_map + batchWriteMtx + metaMtx 三层结构"}

## 2. 影响文件

| 文件 | 行号 | 改动类型 |
|------|------|---------|
| src/coins.h | 30-340 | 修改 class CCoinsViewCache |
| src/coins.cpp | 1-450 | 修改全部方法实现 |
| src/test/coins_tests.cpp | 全文 | 新增并发测试 |
| {绝对路径} | {范围} | {新增/修改/删除} |

## 3. 代码改造

### 3.1 当前代码片段

\`\`\`cpp
// src/coins.h:329 现状
mutable std::mutex mCoinsViewCacheMtx{};
std::unordered_map<COutPoint, CCoinsCacheEntry, SaltedOutpointHasher> cacheCoins;
\`\`\`

### 3.2 改造后

\`\`\`cpp
// 改造后
libcuckoo::cuckoohash_map<COutPoint, CCoinsCacheEntry, SaltedOutpointHasher> cacheCoins;
mutable std::shared_mutex batchWriteMtx;
mutable std::shared_mutex metaMtx;
mutable LRUCache<COutPoint, Coin> levelTwoCache{64 * 1024 * 1024};
std::atomic<size_t> cachedCoinsUsage{0};
\`\`\`

### 3.3 关键算法

{如果需要，给出伪代码或 flow chart}

## 4. 验收 KPI（必须自动化）

| KPI | 目标 | 测量方法 |
|-----|------|---------|
| 单线程吞吐 | 不退化（≥ 当前 95%）| benchmark 脚本 `bench/coins_bench.cpp` |
| 32 worker 并发吞吐（无 BatchWrite 窗口）| ≥ 单线程 16× | benchmark + perf stat |
| 32 worker 并发吞吐（含 BatchWrite 窗口）| ≥ 单线程 8× | 同上 |
| TSan 72h 压测 | 0 race | `ctest -L tsan -R coins` |
| helgrind 24h | 0 race | helgrind script |
| 1h 内存漂移 | < 1% | `valgrind --tool=massif` |

## 5. 测试用例

### 5.1 单元测试（GoogleTest / Boost.Test）

\`\`\`cpp
TEST(CoinsViewCacheConcurrent, ConcurrentGetCoinSameKey) { ... }
TEST(CoinsViewCacheConcurrent, BatchWriteAtomicVisibility) { ... }
TEST(CoinsViewCacheConcurrent, CacheUsageAccountingNoDrift1h) { ... }
\`\`\`

### 5.2 functional test

\`\`\`bash
test/functional/coin_cache_concurrent.py    # 新增
\`\`\`

### 5.3 sanitizer

\`\`\`bash
cmake -B build-tsan -Denable_tsan=ON
ctest --test-dir build-tsan -L tsan -R coins
\`\`\`

### 5.4 benchmark

\`\`\`bash
./build/src/bench/coins_bench --benchmark_filter=Concurrent --benchmark_threads=32
\`\`\`

## 6. 风险登记

| 风险 | 缓解动作 | 责任卡 |
|-----|---------|-------|
| R3 32 worker race | TSan 72h + helgrind 24h + 5.1 单元测试覆盖 | 本卡 §5 |
| R2 libcuckoo 跨平台 | P0.0a libcuckoo soak 已先验证 | P0.0a.1 卡 |

## 7. 回滚条件

任一 KPI（§4）不达标 → 立即回滚 commit，回到上一通过 KPI 的 stable hash。
回滚命令：`git revert {commit-hash}` + `cmake --build build --target check`。

## 8. 审核 checklist（merge 前必过）

- [ ] 所有 KPI 自动化脚本进 CI
- [ ] §5 全部测试通过
- [ ] TSan 72h + helgrind 24h 输出附在 PR 描述
- [ ] code-review 至少 2 人 + 架构 / 安全 / cpp 三选二
- [ ] 涉及 lock-hierarchy 的改动 → DEBUG_LOCKORDER build 全量回归
- [ ] 涉及 共识 等价性 → gettxoutsetinfo hash 跟 reference 100% 一致
- [ ] 风险登记表 §6 引用的 R 编号有对应缓解证据

## 9. 依赖 / 阻塞

- **前置**：P0.0a.1（libcuckoo soak 通过）
- **阻塞后续**：P1.1（Chainstate seqlock 依赖 batchWriteMtx 已落地）、P3.1（doubleCheck 依赖 GetCoinConcurrent）
```
