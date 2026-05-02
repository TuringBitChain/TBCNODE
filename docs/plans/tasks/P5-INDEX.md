# P5 RPC 入口替换 + 废 PTV + 集成（4-6 周）

| 卡 | 主题 | 工时 | 风险钉死 |
|----|------|------|---------|
| [P5.1](./P5.1-rpc-entry.md) | sendrawtransaction / sendrawtransactions 改 dispatcher（保留逐笔语义）| 1 周 | R3 R6 |
| [P5.2](./P5.2-p2p-async.md) | P2P SubmitAsync 集成 net_processing | 1 周 | R3 |
| [P5.3](./P5.3-deprecate-ptv.md) | 废除 processValidation / mNewTxnsThread / ValidationScheduler | 1-2 周 | R6 |
| [P5.4](./P5.4-orphan-recent-rejects.md) | orphan_txns / txn_recent_rejects 跟 dispatcher 集成（含 3 层孤儿链 test）| 1 周 | R3 R5 |
| [P5.5](./P5.5-waitformempoolentry.md) | waitformempoolentry RPC + submitrawtransactions 新 RPC | 0.5-1 周 | R7 |
| [P5.6](./P5.6-integration-fullroundtrip.md) | 集成测试 + P3 末 full round-trip | 1 周 | R7 |
