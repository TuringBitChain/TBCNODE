# GATE M1 决策门（P1+P2 末，启动 13-14 月）

**性质**：硬决策门，**dispatcher QPS / chainActive 替换完整性 / TSan** 任一不达标 → 重新设计或放弃

---

## Wall-clock 硬上限

**estimate**：12-18 周（P1+P2），累计 13-14 月自启动
**hard limit**：**27 周**（estimate 上限 × 1.5），累计 17-18 月

超过 → 强制评审会，业务方决策。

---

## 必过 KPI 矩阵

| 来源 | KPI | 阈值 |
|------|-----|------|
| P1.1 | Chainstate seqlock 7 字段全过 cross-arch 0 torn | 通过 |
| P1.6 | AST grep 0 处 chainActive 漏改且未持 cs_main | 通过 |
| P1.7 | DEBUG_LOCKORDER functional test 0 abort | 通过 |
| P2.1 | 16-shard 状态机 QPS | > 1000 |
| P2.4 | ReorgStash 6 块 reorg + 30 万 tx 0 丢失 | 通过 |
| P2.8 | 32 worker 1h 压测吞吐 | > 600 TPS |
| P2.8 | TSan 72h 新增 race | 0 |

## 决策流程

1. P1+P2 各 PR merge
2. 13-14 月末 GATE-M1 评审
3. 任一 KPI 红 → revert 重新设计或 close PR
4. 全绿 → 进 P3

## 强制回滚演练

GATE-M1 评审会前 1 周完成回滚演练：

1. v2.6.1（含 P1 seqlock + P2 dispatcher）跑 10000 regtest 块
2. 关停 → v1 binary 启动同 datadir
3. 验证 0 reindex + tip 一致
4. **额外**：模拟 dispatcher 满载时关停（in-flight 100 笔），重启 v2.6.1 验证 in-flight 持久化恢复
5. 反向：v1 → v2.6.1 切换

归档 `docs/plans/spike-results/GATE-M1-rollback-drill.md`，业务方签字。

---

## 沉没成本

启动至此 ≈ 13-14 月（约 25-35 人月）

## 输出物

- Chainstate seqlock 完整实现 + cross-arch 验证
- AST grep 报告（260+ 处 chainActive 替换完整）
- ChainDispatcher + 16 shard inflight + Worker pool
- ReorgStash + RaceStash + token bucket
- 32 worker 600 TPS 压测达标
- ARCHITECTURE-NOTE-P1.md + ARCHITECTURE-NOTE-P2.md hand-off
