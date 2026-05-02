# GATE M3 决策门（P6 末，启动 22 月，项目结束）

**性质**：最终决策门，**任一 hash 偏离 → 全网回滚 v1**。否则 v2.6.1 上线，TPS 30 → 600+ 实测。

**简化逻辑**（业务方澄清）：开发网 ≡ 主网（共识规则 100% 一样，仅 chainparams 字段不同），所以无 shadow node / canary / 渐进 12 周流程。

---

## Wall-clock 硬上限

**estimate**：5 周（P6.7 4w + P6.8 1w）+ P6.1-P6.6 6-9w = 11-13 周
**hard limit**：**20 周**（estimate 上限 × 1.5），累计 26 月

超过 → 强制评审会：
- 开发网 4 周已通过 → 可决定继续等真主网 1 周观察 / 终止项目
- 业务方决策权

---

## 必过 KPI 矩阵

| 来源 | KPI | 类型 | 阈值 |
|------|-----|------|------|
| P6.1 | 单元 + functional --extended 全过 | 硬 | 100% |
| P6.2 | TSan / ASan / helgrind 72h 0 新增 | 硬 | 通过 |
| P6.3 | 30 万 tx + 6 块 reorg 0 丢失 | 硬 | 通过 |
| P6.4 | 共识等价性 baseline 全采样 | 硬 | 100% 一致 |
| P6.5 | 1h 风暴 0 死锁 / 0 丢 tx | 硬 | 通过 |
| P6.6 | mempool diff 监控双阈值告警机制 | 硬 | 通过 |
| **P6.7** | **开发网 4 周连续运行 0 panic / 0 fork（核心）** | **硬** | **通过** |
| P6.7 | 4 周 chain tip + UTXO hash 跟 v1 节点 100% 一致 | 硬 | 100% |
| P6.7 | TPS 开发网压测 | 软 | ≥ 600 |
| P6.7 | mempool diff 0 持久阈值告警 | 硬 | 通过 |
| P6.7 | 至少 3 个 v2.6.1 + 至少 2 个 v1 节点混跑 4 周 | 硬 | 通过 |
| P6.7 | 4 周内自然或注入 reorg ≥ 3 次 | 硬 | 通过 |
| P6.8 | Day 1 回滚演练成功（真做，不是 dry-run）| 硬 | 通过 |
| P6.8 | 真主网部署 1 周 0 panic / 0 fork | 硬 | 通过 |
| P6.8 | 重要 RPC 客户端（钱包/交易所/矿池）0 兼容性异常 | 硬 | 通过 |
| P6.8 | 真主网 1 周 mempool diff 0 持久告警 | 硬 | 通过 |

## 强制回滚演练

GATE-M3 评审会前已经在 P6.8 Day 1 做过 1 次真回滚演练（不是 dry-run）。归档 `docs/plans/spike-results/P6.8-rollback-readiness.md`。

---

## 决策流程

1. P6.1-P6.8 各 PR + 部署
2. P6.7 完成时（4 周开发网稳定）→ GATE-M3 预评审会，业务方决策是否进 P6.8 真主网部署
3. P6.8 完成时（1 周观察期通过）→ GATE-M3 正式评审会
4. 任一 KPI 红 → 立即执行 `mainnet-rollback.sh`（< 5 分钟回滚）→ 项目失败
5. 全绿 → v2.6.1 上线，项目结束

## 沉没成本

启动至此 ≈ 22 月（约 26-40 人月）

## 输出物（成功时）

- v2.6.1 主网全网升级
- TPS 30 → 600+ 实测验证
- 共识 hash 跟其他节点 100% 一致
- 老 RPC 100% 兼容 + 新 submitrawtransactions / waitformempoolentry
- 完整 ARCHITECTURE-NOTE-P0.0a/P0.0b/P0/P1/P2/P3/P4/P5/P6.md 文档链
- R1-R7 全部钉死的证据链
- Day 1 真回滚演练成功记录

## 输出物（失败时）

- 完整 post-mortem 报告
- v1 binary 全网恢复（5 分钟内）
- chainstate / mempool / block file 0 损坏
- 开发网 4 周日报（说明为什么开发网过了主网仍出问题——这本身是科研价值）
