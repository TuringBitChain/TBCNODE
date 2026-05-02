# GATE M0 决策门（P0 末，启动 9-10 月）

**性质**：硬决策门，最大决策点之一，**任一 KPI 不达标 → 整套 v2.6.1 放弃**，沉没已大量人月

---

## Wall-clock 硬上限

**estimate**：14-18 周（P0），累计 8-9 月自启动
**hard limit**：**27 周**（estimate 上限 × 1.5），累计 11-12 月

超过 → 强制评审会，业务方决策。

---

## 必过 KPI 矩阵

| 来源 | KPI | 阈值 |
|------|-----|------|
| P0.6 | TSan 72h 新增 race vs baseline | 0 |
| P0.6 | helgrind 72h 新增 race vs baseline | 0 |
| P0.7 | 32 worker 并发 GetCoinConcurrent 吞吐 | ≥ 单线程 16x |
| P0.7 | 含 BatchWrite 窗口（10 min 周期）吞吐 | ≥ 单线程 8x |
| P0.7 | BatchWrite p99（10 万 UTXO 块） | ≤ 200ms |
| P0.4b | cachedCoinsUsage 1h 漂移 | < 1% |
| P0.5 | 老 ctest + functional 全过 | 100% |
| P0.4a | Shutdown 序 ASan 0 use-after-free | 通过 |

## 决策流程

1. P0.1-P0.7 各 PR merge 到 main
2. 9-10 月末提交 GATE-M0 评审会
3. 任一 KPI 红 → close P1+ 后续 PR 计划 → 项目结束
4. 全绿 → 进 P1（删 ChainContext + chainActive 替换）

## 强制回滚演练

GATE-M0 评审会前 1 周完成完整回滚演练：

1. 在演练环境跑 v2.6.1（带 P0 完整 cacheCoins 改造）跑 10000 regtest 块
2. 关停 → 启动 v1 binary 同 datadir
3. 验证：v1 启动 < 30s + tip hash 一致 + 0 reindex
4. v1 跑 1000 块 → 启动 v2.6.1 同 datadir
5. 验证：v2.6.1 启动 < 60s + tip hash 一致

任一步失败 → 修复 lifetime / shared_ptr / Shutdown 序 → 再演练。

归档 `docs/plans/spike-results/GATE-M0-rollback-drill.md`，业务方签字。

---

## 沉没成本

启动至此 ≈ 9-10 月（13w P0.0a + 6w P0.0b + 14-18w P0），相当于 18-25 人月。

## 输出物（全过时）

- CCoinsViewCache 完成 libcuckoo + batchWriteMtx + metaMtx 三结构
- pcoinsTip shared_ptr lifetime sweep 完成
- 32 worker 16x 吞吐验证
- 老 API 100% 兼容
- TSan/helgrind 72h 0 新增 race
- ARCHITECTURE-NOTE-P0.md hand-off 文档
