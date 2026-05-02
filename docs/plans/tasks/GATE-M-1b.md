# GATE M-1b 决策门（P0.0b 末，启动 11 周）

**性质**：硬决策门，任一 KPI 不达标 + 业务方未 sign-off → **整套 v2.6.1 放弃，沉没 11 周**

---

## Wall-clock 硬上限

**estimate**：6 周（累计 11 周自启动）
**hard limit**：**10 周**（累计 15 周自启动）

超过 → 强制评审会，业务方决策延期 / 缩 scope / 放弃。

---

## 必过 KPI 矩阵

| 来源 | KPI | 阈值 |
|------|-----|------|
| P0.0b.1 | seqlock memory-model 文档 review | 至少 1 架构 + 1 cpp + 1 安全 LGTM |
| P0.0b.1 | x86 / ARM64 / RISC-V64 长压 0 torn | 全过 |
| P0.0b.2 | 10 万 UTXO BatchWrite p99 | ≤ 200ms |
| P0.0b.2 | 含 reader 排队 BatchWrite p99 | ≤ 300ms |
| P0.0b.3 | 共识等价性 baseline 覆盖率 | > 30% + 关键激活 ±1000 全量 |
| P0.0b.3 | baseline spot check（跨机器 100 高度）| 100% 一致 |
| P0.0b.4 | smoke round-trip 双向 | 启动 < 30s 且 tip hash 一致 |
| P0.0b.4 | 业务方 sign-off | 签字归档 |
| P0.0b.4 | ARCHITECTURE-NOTE-P0.0b.md | 已写 hand-off 决策理由 |

## 决策流程

1. P0.0b.1-4 各自 PR merge 到 main
2. 11 周末提交 GATE-M-1b 评审会
3. 任一 KPI 红 / 业务方拒签 → close 所有 PR → 项目结束
4. 全绿 → 进 P0（CCoinsViewCache 改造）

## 强制回滚演练

- v1↔vN smoke round-trip 已经在 P0.0b.4 任务卡内强制执行
- GATE-M-1b 评审会前再做 1 次"全 P0.0a + P0.0b PR revert + datadir 重启"演练
- 验证：v1 binary 启动同 datadir 0 reindex 立即同步
- 耗时 ≤ 8 小时，归档 `docs/plans/spike-results/GATE-M-1b-rollback-drill.md`

---

## 沉没成本

11 周（5w P0.0a + 6w P0.0b ≈ 13-17 人月）

## 输出物（全过时）

- seqlock memory-model 完整证明文档
- 跨架构（x86/ARM/RISC-V）seqlock 0 torn 验证
- BatchWrite p99 量化报告
- 共识等价性 baseline JSON（外部存储 200+GB）
- v1↔vN smoke round-trip 测试通过
- 业务方签字
- ARCHITECTURE-NOTE-P0.0b.md
