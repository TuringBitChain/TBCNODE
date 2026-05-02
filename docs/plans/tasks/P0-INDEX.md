# P0 CCoinsViewCache 并发改造（14-18 周，硬决策门 M0）

**目标**：把 cacheCoins 从 std::mutex + std::unordered_map → libcuckoo + batchWriteMtx + metaMtx 三层结构；pcoinsTip 由全局裸指针改 shared_ptr。完成后 32 worker 并发吞吐 ≥ 16x 单线程（无 BatchWrite 窗口）。

| 卡 | 主题 | 工时 | 风险钉死 |
|----|------|------|---------|
| [P0.1](./P0.1-cachecoins-libcuckoo.md) | cacheCoins 改 libcuckoo + batchWriteMtx + metaMtx 三结构 | 2 周 | R2 R3 |
| [P0.2](./P0.2-batchwrite-impl.md) | BatchWrite insert + update_fn fallback + erase_fn 路径 | 2 周 | R3 |
| [P0.3](./P0.3-getcoinconcurrent.md) | GetCoinConcurrent L1/L2/L3 + 二级 LRU 64MB | 2 周 | R3 |
| [P0.4a](./P0.4a-pcoinstip-shared-ptr.md) | pcoinsTip 改 shared_ptr + Shutdown 序重排（F3）| 2 周 | R7 |
| [P0.4b](./P0.4b-cachedusage-isbatchwrite.md) | cachedCoinsUsage atomic + IsBatchWriteInProgress + 1h 漂移 | 1 周 | R3 |
| [P0.5](./P0.5-legacy-api-compat.md) | 老 GetCoin/HaveCoin/AddCoin 接口保持兼容 | 1 周 | R6 |
| [P0.6](./P0.6-tsan-72h.md) | 单元测试 + TSan/helgrind 72 小时压测 | 2-3 周 | R3 |
| [P0.7](./P0.7-bench-32worker.md) | 性能 benchmark：32 worker 并发 ≥ 16x 单线程 | 2-3 周 | R3 |

**M0 决策门 KPI**：见 [GATE-M0.md](./GATE-M0.md)。

**回滚**：M0 任一失败 → 整套放弃，沉没 11 + (8-9 月) ≈ 大量人月。是 v2.6.1 最大决策门。
