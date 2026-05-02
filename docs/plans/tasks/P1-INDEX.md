# P1 删除 ChainContext + chainActive 替换（4-6 周）

**目标**：实现 Chainstate seqlock，全代码库 chainActive 直接 + 间接读穷举改成 g_chainstate.Capture()。

| 卡 | 主题 | 工时 | 风险钉死 |
|----|------|------|---------|
| [P1.1](./P1.1-chainstate-seqlock.md) | Chainstate seqlock 完整实现（K1 fence + tip_index + genesisActivationHeight + F2 守卫）| 1 周 | R2 R7 |
| [P1.2](./P1.2-worker-direct-read.md) | worker 直读 Capture()，删除 ChainContext 类型 | 1 周 | R3 |
| [P1.3](./P1.3-isxxx-snapshot-arg.md) | IsXxxEnabled / GetBlockScriptFlags 接受 Snapshot& 入参 | 1 周 | R6 |
| [P1.4](./P1.4-txnvalidation-13points.md) | TxnValidation 13 处直接 chainActive 读 → snap | 1 周 | R6 |
| [P1.5](./P1.5-checksequencelocks.md) | CheckSequenceLocks Genesis 后 return true 简化 | 0.5 周 | R6 |
| [P1.6](./P1.6-ast-grep-260.md) | AST grep 穷举 260+ 处 chainActive 替换 | 0.5-1 周 | R6 |
| [P1.7](./P1.7-unit-regression.md) | 单元 + 回归测试 | 1 周 | R3 R6 |

**M1 决策门跟 P2 一起评审**：见 [GATE-M1.md](./GATE-M1.md)。
