# P4 ConnectBlock 切细粒度锁 + 子系统验证（16-20 周）

| 卡 | 主题 | 工时 | 风险钉死 |
|----|------|------|---------|
| [P4.1](./P4.1-connectblock-locks.md) | ConnectBlock 提交阶段三锁原子提交（含 C-B 锁次序 + P-2 Flush abort）| 2 周 | R3 R6 |
| [P4.2](./P4.2-worker-coord.md) | worker 协调测试（reorg 期间 worker 行为）| 2-3 周 | R5 |
| [P4.3](./P4.3-batchwrite-bench-shard.md) | BatchWrite 阻塞时长 benchmark + 分片优化预研 | 1-2 周 | R3 |
| [P4.4](./P4.4-removeforblock-sync.md) | RemoveForBlock 跟 worker commit 同步 | 1 周 | R3 |
| [P4.5a](./P4.5a-subsystem-verify.md) | 子系统影响验证（wallet/ZMQ/REST/GBT/reorg/orphan/pruning/indexer）| 3-4 周 | R3 R6 |
| [P4.5b](./P4.5b-subscriber-lock-audit.md) | subscriber 反向锁审计 7+ 已知订阅点（H10）| 4 周 | R3 |
| [P4.6](./P4.6-debug-lockorder-tsa.md) | DEBUG_LOCKORDER + AssertLockOrder 全量验证 + 新文件 TSA 注解 | 4-6 周 | R3 R6 |
