# P6 测试 + 灰度（11-13 周，简化版）

**简化逻辑**（业务方澄清 2026-04-28）：

> TBC 开发网跟主网**共识规则 100% 一样**，仅 chainparams 字段（netMagic / fork heights / seeds）不同。
> → 开发网 4 周稳定 = 真主网兼容性已验证
> → **不需要** shadow node 4w + canary 4w + 渐进 4w（节省 8-12 周）

| 卡 | 主题 | 工时 | 风险钉死 |
|----|------|------|---------|
| [P6.1](./P6.1-unit-functional.md) | 单元测试 + functional test 100% 通过 | 1-2 周 | R3 R6 |
| [P6.2](./P6.2-tsan-asan-72h.md) | TSan / ASan / helgrind 72 小时压测 | 1-2 周 | R3 |
| [P6.3](./P6.3-reorg-injection.md) | regtest reorg 注入测试 + reorg 风暴 | 1-2 周 | R5 |
| [P6.4](./P6.4-consensus-equiv.md) | 共识等价性测试（v2.6.1 vs origin/main UTXO hash + getrawmempool diff）| 2 周 | R4 |
| [P6.5](./P6.5-retry-storm.md) | retry 风暴回归测试 | 1 周 | R5 |
| [P6.6](./P6.6-mempool-diff-monitor.md) | mempool diff 监控（接受性差异，60s/200 笔 + 持久 10min）| 1 周 | R4 |
| [P6.7](./P6.7-devnet-4w.md) | **开发网部署 + 4 周稳定运行观察（核心兜底）** | 4 周 | R3 R4 R5 |
| [P6.8](./P6.8-mainnet-deploy.md) | **真主网部署 + 1 周观察**（开发网通过即直接全网，无 shadow / 无 canary） | 1 周 | R1-R7 全部 |

**M3 决策门**：见 [GATE-M3.md](./GATE-M3.md)。

**总工期影响**：P6 由 14-20 周 → **11-13 周**（节省 3-7 周）。整套方案 19-24 月 → **18-22 月**。
