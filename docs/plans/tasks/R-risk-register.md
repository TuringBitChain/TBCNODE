# R 风险登记表

> 把第十轮审核暴露的 R1-R7 全部钉到具体任务卡 + 测试用例 + 决策门 KPI。每条风险都必须有"证据型缓解"，不是"声称缓解"。

---

## R1：18-22 月长周期人员流失

**性质**：项目管理风险，单人专注 2 年失败率 > 50%（v2.6.1 §7 自陈）

**钉死方式**：
- 把 60 张任务卡每张做成"独立可交付单元"——任一任务卡完成即合入 main，不依赖整体方案最终成败
- 每张卡含完整审核 checklist（§8），新人接手能 1 周内上手
- 每个 phase 末必须出 ARCHITECTURE-NOTE 文档，记录决策理由（不是仅记录改了什么）
- 决策门 M-1a/M-1b/M0/M1/M2/M3 失败 → 每个门都可以独立回滚到上一稳态

**对应任务卡**：每张卡的 §8 审核 checklist 第 4 项（"至少 2 人 review"强制双人 ownership）；ARCHITECTURE-NOTE 在每 phase 末写一份；详细机制见 README §R1（双人 owner + ARCHITECTURE-NOTE ≥ 2000 字 + 每 GATE hand-off 测试）

**KPI**：
- 每张任务卡 wall-clock 时长 ≤ 卡内估算的 1.5×（超出触发 reviewer 介入）
- 任一 phase 末 hand-off 测试：让一个不参与本 phase 的人 1 周内能继续

---

## R2：libcuckoo / seqlock 跨平台 + 跨 boost 版本未覆盖

**性质**：基础设施依赖风险，spike 4 周覆盖不到所有 (OS × arch × boost × glibc) 组合

**钉死方式**：
- P0.0a.1（libcuckoo soak）：5000 万 entry × 24h × {Ubuntu 20.04/22.04/24.04} × {x86_64/ARM64}
- P0.0a.5（boost::recursive_mutex try_lock spike）：4 个 boost 版本（1.65/1.71/1.74/1.83）× try_lock 5 项语义测试
- P0.0b.1（seqlock memory-model 文档）：writer/reader fence 完整证明 + ARM64 + RISC-V64 实机交叉验证
- 任一组合失败 → 备选方案：libcuckoo 退回 `std::shared_mutex + std::unordered_map`（30-40% 性能损失但行为已知）；try_lock 退回 `std::atomic<thread::id> writer_owner`

**对应任务卡**：P0.0a.1 / P0.0a.5 / P0.0b.1 / GATE-M-1a / GATE-M-1b

**KPI**：
- 5000 万 entry × 24h soak 内存增长 < 100MB
- TSan / helgrind 24h 0 race
- ARM64 + RISC-V64 交叉编译通过且在 emulator 上跑过 seqlock 单元测试

---

## R3：32 worker 并发状态空间爆炸

**性质**：并发设计风险，测试覆盖率永远 < 100%

**钉死方式**：
- 每张涉并发的卡（P0.4 / P2.* / P3.* / P4.*）必含 TSan 72h + helgrind 24h KPI
- P6.5（retry 风暴回归测试）：100 客户端 × 1000 笔 × 1h，注入 reorg、BatchWrite、worker crash
- **P6.7（开发网 4 周）+ P6.8（真主网 1 周）= 5 周生产环境压测**（v2.6.1 简化：开发网 ≡ 主网，删除原 shadow + canary 8w）
- 每个 race 类（R-table 详细设计 §12）必须有对应单元测试

**对应任务卡**：P0.6 / P2.7 / P2.8 / P3.* 全部 / P4.2 / P6.2 / P6.5 / P6.7-P6.9

**KPI**：
- TSan 72h 0 race（含 helgrind 交叉验证）
- 100 客户端 1h 压测下 mempool diff < 阈值（M-K：60s 累计 200 笔）
- shadow node 4 周内 chain tip + UTXO hash 跟 reference 100% 一致

---

## R4：共识等价性 baseline 采样 30% 覆盖率

**性质**：覆盖率风险，70% 区段没全量验证

**钉死方式**：
- P0.0b.3（baseline 采样窗口）：关键激活高度 ±1000 块全量 + 每 5000 块全量
- P6.4（共识等价性测试）：4 周 shadow node 全量验证（每块 gettxoutsetinfo + getrawmempool diff）
- P6.6（mempool diff 监控）：60s 累计 200 笔 / 持久 txid 10min 双阈值告警
- 任一 diff 出现 → 立即停 + 回滚 v1 binary

**对应任务卡**：P0.0b.3 / P6.4 / P6.6 / GATE-M-1b / GATE-M3

**KPI**：
- baseline 30% 区段 + 关键激活高度 ±1000 块全量 100% 一致
- shadow node 4 周 0 持久 diff（瞬态可接受）
- canary 4 周 0 共识 hash 偏离

---

## R5：reorg 6 块风暴 + BatchWrite + RaceStash 叠加场景

**性质**：极端场景测试覆盖不足

**钉死方式**：
- P6.3（regtest reorg 注入测试 + reorg 风暴）：6 块连续断链 + 30 万 tx 涌入 + ReorgStash 满载验证
- 任务卡 P2.4（reorg 独立队列 + ReorgStash）单元测试覆盖溢出路径
- 任务卡 P3.2（Resubmit 双类策略）含风暴场景下 token bucket 限速 + RaceStash 回灌

**对应任务卡**：P2.4 / P3.2 / P6.3

**KPI**：
- 6 块 reorg + 30 万 tx 涌入：mempool 最终态跟 v1 一致；ReorgStash 溢出落 stash 不丢
- TPS ≥ 200（reorg 6 块连续 KPI，M2 决策门）

---

## R6：260 处 chainActive AST grep 漏一处即 BUG

**性质**：完整性风险，工具覆盖率不能 100% 但漏一处会埋 race

**钉死方式**：
- P1.6（AST grep 穷举）：用 clang AST 工具（不是文本 grep）穷举 `chainActive.` 调用点
- 每处替换 commit 单独走 review
- DEBUG_LOCKORDER build 启用，剩余 chainActive 调用点必须仍持 cs_main，否则编译期 / 运行时报错
- P6.1 functional test 全套覆盖

**对应任务卡**：P1.6 / P4.6（lock-hierarchy 全量验证）

**KPI**：
- AST 工具输出 0 处未替换且未持 cs_main 的 chainActive 调用点
- DEBUG_LOCKORDER build 跑 functional test 0 abort

---

## R7：std::abort 持锁退出（Flush 失败 / 单写者 try_lock 失败）

**性质**：进程退出后状态恢复风险

**钉死方式**：
- P-2 已写明：abort 不调析构 → OS 释放虚拟地址空间 / fd / LevelDB LOCK 文件
- P0.0a.5 异常注入测试覆盖
- 启动后从 LevelDB 重建 chainstate（format 不变，无需 reindex）
- 启动期 init.cpp 重建 pcoinsTip 时必须验证 LevelDB 状态一致性（已有逻辑保留）

**对应任务卡**：P0.0a.5（H-F 异常注入）/ P5.6（集成测试含异常路径恢复）

**KPI**：
- abort 后从 datadir 重启 5 秒内启动完成
- 重启后 chain tip + UTXO hash 跟 abort 前一致

---

## 风险矩阵：每条卡引用哪条风险

| Phase | 主要 R 缓解 |
|-------|-----------|
| P0.0a | R2, R7 |
| P0.0b | R2, R4 |
| P0 | R2, R3 |
| P1 | R3, R6 |
| P2 | R3, R5 |
| P3 | R3, R5 |
| P4 | R3, R6 |
| P5 | R3, R7 |
| P6 | R3, R4, R5 |
| 全程 | R1（双人 ownership / ARCHITECTURE-NOTE）|

---

## R8（v2.6.1 P6 简化新增）：开发网共识跟主网分化

**性质**：假设依赖风险，v2.6.1 P6 简化策略依赖"开发网 ≡ 主网"假设；如果未来 TBC 开发网跟主网共识字段开始分化（如不同 fork 高度激活、不同 script 限制），简化策略失效。

**钉死方式**：
- 当前事实（业务方 2026-04-28 确认）：开发网跟主网共识规则 100% 一样，仅 chainparams 字段不同（netMagic / fork heights / seeds）。验证：`src/chainparams.cpp:107` `TBCFirstBlockHeight` 跨 dev/main 一致；激活高度跨 dev/main 一致
- 监控：在 chainparams.cpp 加 commit hook，任何 commit 修改 dev 跟 main 的 `consensus::Params` 不一致字段 → 触发 RFC 评审（v2.6.2 patch 待写）
- 兜底：如果分化发生 → 补回 shadow node 4w + canary 4w 阶段（重新走 P6.8 / P6.9 流程）

**对应任务卡**：P6.7 / P6.8 / 待加 commit hook 任务卡

**KPI**：
- v2.6.1 启动时手工核对：dev/main `consensus::Params` 字段 100% 一致（除 netMagic / fork heights / seeds 等非共识字段）
- 24 月内 chainparams.cpp 任一 commit 触发分化检查 PR review

---

## 强制原则

**任一任务卡 §6 风险登记不能为空**——必须钉到至少 1 条 R（R1-R8）。
钉不上 = 这张卡不解决任何已知风险 → 重新审视必要性。
