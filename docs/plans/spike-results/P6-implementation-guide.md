# P6 测试 + 灰度（实施指南）

**简化策略**：开发网 ≡ 主网（TBC 共识规则一致），P6 由 14-20 周 → **11-13 周**（删 shadow + canary + 渐进 12 周）。

**当前状态**：SCAFFOLD（指南 + 测试脚本框架）。**真灰度部署留业务方实施**。

---

## P6.1 单元 + functional --extended

```bash
cd ~/TBCNODEDEV
cmake --build build --target test_bitcoin
./build/src/test/test_bitcoin   # 全套
test/functional/test_runner.py --extended

# 重点必过
test/functional/mempool_packages.py
test/functional/mempool_resurrect.py
test/functional/mempool_reorg.py
test/functional/mempool_persist.py
```

KPI：100% 通过（v2.6.1 改动 0 破坏现有 functional test）

## P6.2 TSan / ASan / helgrind 72h

`tools/p0-tsan-72h.sh`（已交付）扩展跑 P3-P5 全套测试 + functional --extended 循环 24h × 3。

KPI：vs P0.0a.2 baseline 0 新增 race / 0 新增 leak

## P6.3 reorg 风暴注入

```python
# test/functional/reorg_storm_30w_tx.py（实施时写）
# 30 万 tx 入 mempool → 触发 6 块 reorg → 验证：
#   - mempool 最终态跟 v1 一致
#   - TPS ≥ 200（reorg 6 块 KPI）
#   - 0 panic / 0 crash
#   - ReorgStash metrics 合理
```

## P6.4 共识等价性 vs origin/main

`tools/baseline-replay.py`（已交付 SCAFFOLD）跑全采样高度，对比 v2.6.1 跟 reference 节点 UTXO hash 100% 一致。

## P6.5 retry 风暴 1h

100 客户端 × 1000 笔 × 1h，注入 reorg / BatchWrite / worker crash。验证：
- 0 死锁
- 0 tx 永久丢失
- TPS ≥ 600

## P6.6 mempool diff 双阈值监控

`tools/mempool-diff-monitor.py` 60s 累计 200 笔 → WARNING；持久 txid 10min → ALERT。

## P6.7 开发网 4 周稳定（核心兜底）

**这是 v2.6.1 上线前的核心 KPI**：

```bash
# 业务方实施步骤：
# 1. 至少 3 个 v2.6.1 节点 + 至少 2 个 v1 节点混跑
# 2. 监控 chain tip + UTXO hash + mempool diff + TPS + p99 RPC
# 3. 至少经历 3 次自然或注入 reorg
# 4. 4 周连续 0 panic / 0 fork
```

**简化逻辑（v2.6.1 P6 简化）**：开发网 ≡ 主网（仅 chainparams netMagic / fork heights / seeds 不同），开发网 4 周稳定 = 真主网兼容性已验证。

## P6.8 真主网部署 + 1 周观察（无 shadow / 无 canary）

```bash
# tools/mainnet-deploy.sh
# Day 1：备节点上做真回滚演练（不是 dry-run）
# 确认 mainnet-rollback.sh 5 分钟内回到 v1
# 然后 Day 2-7 真主网部署 + 监控

# tools/mainnet-rollback.sh
systemctl stop bitcoind-v26
ln -sf /usr/local/bin/bitcoind-v1 /usr/local/bin/bitcoind
systemctl start bitcoind
```

KPI：1 周 0 panic / 0 fork / chain hash 跟其他节点一致 / mempool diff 0 持久告警 / 重要客户端 0 异常

---

## GATE-M3 KPI 矩阵（最终决策门）

| 来源 | KPI | 类型 | 阈值 |
|------|-----|------|------|
| P6.1 | 单元 + functional --extended | 硬 | 100% |
| P6.2 | TSan / ASan / helgrind 72h 新增 | 硬 | 0 |
| P6.3 | 30 万 tx + 6 块 reorg 0 丢失 | 硬 | 通过 |
| P6.4 | baseline 全采样 UTXO hash 一致 | 硬 | 100% |
| P6.5 | 1h 风暴 0 死锁 / 0 丢 tx | 硬 | 通过 |
| P6.6 | mempool diff 双阈值机制 | 硬 | 通过 |
| **P6.7** | **开发网 4 周（核心）** | **硬** | **通过** |
| P6.7 | 开发网 TPS 实测 | 软 | ≥ 600 |
| P6.7 | 开发网 reorg ≥ 3 次 | 硬 | 通过 |
| P6.8 | Day 1 真回滚演练 | 硬 | < 5min 切回 v1 |
| P6.8 | 真主网 1 周 0 panic / 0 fork | 硬 | 通过 |
| P6.8 | 重要 RPC 客户端兼容 | 硬 | 通过 |

---

## 全 v2.6.1 完成时业务方交付物

- v2.6.1 主网全网升级
- TPS 30 → 600+ 实测验证
- 共识 hash 跟其他节点 100% 一致
- 老 RPC 100% 兼容 + 新 submitrawtransactions / waitformempoolentry
- 完整 ARCHITECTURE-NOTE-P0.0a/P0.0b/P0/P1/P2/P3/P4/P5/P6.md 文档链
- R1-R8 全部钉死的证据链
- Day 1 真回滚演练成功记录
