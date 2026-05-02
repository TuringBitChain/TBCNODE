# GATE M2 决策门（P3+P4+P5 末，启动 20-21 月）

**性质**：硬决策门，**TPS 600 / 共识等价 / round-trip / reorg KPI 任一不达标 → 重做 P3-P4 或放弃**

---

## Wall-clock 硬上限

**estimate**：26-34 周（P3+P4+P5），累计 20-21 月自启动
**hard limit**：**51 周**（estimate 上限 × 1.5），累计 25-26 月

超过 → 强制评审会，业务方决策。

---

## 必过 KPI 矩阵

| 来源 | KPI | 阈值 |
|------|-----|------|
| P3.1 | H-D perInputScriptFlags 5 项等价矩阵 | 全过 |
| P3.6 | GBT 单 worker + 合并队列 + N4 锁顺序 | 通过 |
| P4.1 | C-B phantom-tip 100 万次注入 0 共识漂移 | 通过 |
| P4.1 | P-2 Flush 失败 abort + 重启恢复 | 通过 |
| P4.5b | 9 处 subscriber 反向锁审计 + 修复 | 100% |
| P4.6 | -Werror=thread-safety 0 警告 + DEBUG_LOCKORDER functional 0 abort | 通过 |
| P5.6 | full round-trip 双向通过 | 通过 |
| 综合 | functional --extended 100% 通过 | 100% |
| 综合 | 开发网部署 4 周稳定 | 0 panic / 0 fork |
| 综合 | TPS（开发网压测） | ≥ 600 |
| 综合 | 重要 RPC p99 | < 100ms |
| 综合 | reorg 6 块 KPI | TPS ≥ 200 |
| 综合 | 共识等价性（P0.0b.3 baseline 对比） | 4 周 0 diff |

## 决策流程

1. P3+P4+P5 各 PR merge
2. 20-21 月末 GATE-M2 评审
3. 任一 KPI 红 → revert 或重做 P3-P4
4. 全绿 → 进 P6（真主网灰度）

## 强制回滚演练

P5.6 已经强制 full round-trip。GATE-M2 评审会前再做 1 次：

1. v2.6.1 完整版（dispatcher + worker + RaceStash 非空 + ReorgStash 非空）跑 24h 真规模负载
2. 关停 → v1 binary 启动同 datadir
3. 验证：0 reindex / tip 一致 / mempool 内容（non-stash 部分）一致
4. **额外**：模拟开发网部署节点关停 + 主网 v1 替换 + 节点继续工作

归档 `docs/plans/spike-results/GATE-M2-rollback-drill.md`，业务方签字。

---

## 沉没成本

启动至此 ≈ 20-21 月（约 38-50 人月）

## 输出物

- v2.6.1 完整 PTV → ChainDispatcher 替换
- 老 RPC 100% 兼容 + 新 submitrawtransactions / waitformempoolentry
- C-B 锁次序 + P-2 Flush abort 验证通过
- 9 处 subscriber 反向锁审计 + 修复
- DEBUG_LOCKORDER + TSA 全量验证
- vN ↔ v1 full round-trip 通过
- 开发网 4 周稳定 + TPS 600 达标
- ARCHITECTURE-NOTE-P3.md / -P4.md / -P5.md hand-off
