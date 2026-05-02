# P3 worker commit + doubleCheck + AsyncTrim + RPC busy + GBT + SignalDispatcher（6-8 周）

| 卡 | 主题 | 工时 | 风险钉死 |
|----|------|------|---------|
| [P3.1](./P3.1-doublecheck.md) | doubleCheck 4 项实现（统一 unique_lock 内）+ H-D perInputScriptFlags | 1-2 周 | R3 R6 |
| [P3.2](./P3.2-resubmit-strategy.md) | Resubmit 双类策略（race 10 / hard 1 + 30s）+ RaceStash 集成 | 1 周 | R5 R7 |
| [P3.3](./P3.3-worker-commit.md) | worker 内 commit + 真 RAII rollback（H8 ScopeGuard）| 1-2 周 | R3 R7 |
| [P3.4](./P3.4-async-trim.md) | AsyncTrim 专用线程 + evict 拆批 1000 + yield | 0.5 周 | R3 |
| [P3.5](./P3.5-reject-overloaded.md) | RPC busy → REJECT_OVERLOADED (0x45) 路径 + 客户端兼容性测试 | 0.5 周 | R3 |
| [P3.6](./P3.6-gbt-snapshot.md) | GbtSnapshotProvider 单 refresh worker + 合并队列 + N4 chain 用 seqlock | 1-2 周 | R3 R5 |
| [P3.7](./P3.7-signal-dispatcher.md) | SignalDispatcher per-tx FIFO（global_seq）| 1 周 | R3 |

完整代码见 detailed-design §3 / §6 / §7。
