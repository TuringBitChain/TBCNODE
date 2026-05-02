# P2 ChainDispatcher + Per-Chain Worker pool（8-12 周）

**目标**：实现 ChainDispatcher（16-shard inflight 状态机 + 拓扑排序 + ReorgStash/RaceStash + token bucket）+ PerChainWorker。

| 卡 | 主题 | 工时 | 风险钉死 |
|----|------|------|---------|
| [P2.1](./P2.1-chaindispatcher-skeleton.md) | ChainDispatcher 数据结构 + 16-shard inflight 状态机 | 2 周 | R3 |
| [P2.2](./P2.2-routing-power-of-two.md) | 路由策略（投票 + power-of-two-choices + input cap）| 1-2 周 | R3 |
| [P2.3](./P2.3-perchainworker.md) | PerChainWorker thread loop + queue + cv | 2 周 | R3 |
| [P2.4](./P2.4-reorg-stash-tokenbucket.md) | ReorgStash + RaceStash + token bucket + ReorgWorkerLoop | 1-2 周 | R5 R7 |
| [P2.5](./P2.5-toposort-batch.md) | SubmitBatchSync TopoSort（环检测 + 重复 txid + batch-budget 30s）| 1 周 | R3 |
| [P2.6](./P2.6-gc-thread.md) | GC 线程 + 错峰 try_lock + 饥饿保护 | 1 周 | R3 |
| [P2.7](./P2.7-watchdog.md) | 异常捕获 + worker watchdog（软停语义）| 1 周 | R3 |
| [P2.8](./P2.8-multiworker-tsan.md) | 单元测试 + 多 worker 并发压测 | 2-3 周 | R3 |

**M1 决策门**（P1+P2 一起）：见 [GATE-M1.md](./GATE-M1.md)。
