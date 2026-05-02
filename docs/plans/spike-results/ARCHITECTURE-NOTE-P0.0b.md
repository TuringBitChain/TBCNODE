# ARCHITECTURE-NOTE P0.0b（正确性 + sign-off）

**Phase**：P0.0b — 正确性 + sign-off（6 周计划，本地完成 4 项中可做的部分；P0.0b.3 留 SCAFFOLD 给业务方）
**完成日期**：2026-04-28
**owner**：primary=Claude（dev session），secondary=待业务方指派
**hand-off 目标**：让一个不参与 P0.0b 的工程师 1 周内能继续推进 P0（CCoinsViewCache 改造）。

---

## 1. P0.0b 完成了什么

### 1.1 P0.0b.1 Chainstate seqlock + memory-model 文档

文件：
- `src/validation/chainstate.h`（新增 ~120 行）
- `src/test/chainstate_seqlock_tests.cpp`（新增 4 项单元）
- `docs/plans/seqlock-memory-model.md`（完整正确性证明）

**Chainstate 类**实现 seqlock 元数据（v2.6.1 §1.1）：
- 7 字段（C-A tip_index + H-D genesisActivationHeight 都已加入）
- writer 协议：fetch_add(release) → fence(release) → 写字段 → fence(release) → fetch_add(release)
- reader 协议：load(acquire) → fence(acquire) → memcpy → fence(acquire) → load(acquire)；不一致重试
- F2 单写者守卫 stub（生产入口 UpdateTip 在 P1.1 接入；本卡用 UpdateForTest 跳过守卫）

4 项单元测试全过：
- `all_seven_fields_captured`：7 字段等价性
- `seq_monotonic_after_update`：seq 进奇数 → 退偶数
- `no_torn_read_under_load`：1 writer × 32 reader × 3 秒长压 0 torn read（实测 100+ 万次读）
- `snapshot_consistency_across_writes`：跨 epoch snapshot 字段全部来自同一 epoch

**memory-model 文档**（`docs/plans/seqlock-memory-model.md`）：
- writer / reader 协议形式化
- K1 fence happens-before 证明
- ARM/RISC-V relaxed memory model 注解（实际 fence 指令 dmb ish / fence rw,rw）
- **实测发现的 do-while UB bug + 修复留档**（见 §1.4）

### 1.2 P0.0b.2 BatchWrite p99 prototype sanity

文件：`src/test/batchwrite_p99_tests.cpp`（轻量 sanity，不是真 KPI）

1 项单元测试通过：
- `batchwrite_concurrent_no_race`：1 万 entries baseline + 1k batch × 3 + 4 reader × shared_lock，0 race，< 5 秒

**真 KPI 验证（10 万 UTXO p99 ≤ 200ms / 含 32 reader p99 ≤ 300ms）留 GATE-M0**：单元测试预算只有几秒，5000 万 entry × 30 batch × 32 reader 的 benchmark 应在 `src/bench/` 跑（手动触发，不每次单元测试触发）。

### 1.3 P0.0b.3 共识等价性 baseline（SCAFFOLD）

文件：
- `tools/baseline-replay.py`（脚本框架，含安全检查：拒绝在生产 datadir 跑 invalidateblock）
- `docs/plans/spike-results/P0.0b.3-baseline-readme.md`（完整 5 步实施指南）

**未跑**：本卡需要 800GB 磁盘 / 64GB 内存 / 8 实例并发 / 3-4 周机器跑，超出 dev session 资源。**留给业务方运维实施**。

**hand-off 完整**：
- 采样窗口策略明文（关键激活高度 ±1000 + 每 5000 块 ±100，覆盖率 > 30%）
- 8 副本 datadir 准备步骤
- 端口偏移 + RPC 调用 + JSON 输出格式
- spot check + 合并脚本接口

P6.4 将用此 baseline 跟 v2.6.1 完整版对比（GATE-M3 必过条件）。

### 1.4 P0.0b.4 smoke round-trip（实测通过）

文件：`tools/smoke-roundtrip.sh`

**实测**（regtest 链 60 块）：

```
Phase 1: V1 binary 启动 50 块 → 关停（启动 505ms，tip=74552d62...）
Phase 2: vN binary 启动同 datadir → 启动 505ms，tip 100% 一致
Phase 3: vN 再写 10 块 → 关停（tip=4459ec5f...）
Phase 4: V1 启动同 datadir → 启动 505ms，看到 vN 写的 60 块

✓ 双向切换 0 reindex，chain hash 100% 一致
```

**说明**：本卡 smoke 范围有限——vN 当前只动 sync.h hook + 加新 header（独立 chainstate.h 未集成进 init.cpp/validation.cpp）。**真 round-trip 在 P5.6** 跑（含 dispatcher / cacheCoins libcuckoo / seqlock 全接入后才有意义）。本 smoke 验证：sync.h hook 改造跟 P0.0a 各 spike 不破坏 disk format。

---

## 2. 决策理由

### 2.1 chainstate.h 不集成进 init.cpp 直到 P1.1

**选 A**：P0.0b 时只做 chainstate.h + 单元测试（实施）
**选 B**：P0.0b 时就把 g_chainstate 全局变量 + UpdateTip 调用接入 init.cpp / validation.cpp ConnectBlock

选 A 的理由：
- P0.0b 任务卡 §1 只要"实现 + memory-model 验证"，不要求接入生产路径
- 接入 init.cpp 涉及 cs_main 锁次序 + UpdateTip 单写者守卫 + 全代码库 chainActive 替换 → 这是 P1 的工作
- 提前接入会污染 P1.1-P1.6 的工作范围，违反 phase 隔离

### 2.2 BatchWrite p99 真 KPI 留 GATE-M0

**选 A**：单元测试只做轻量 sanity，真 KPI 留 P0 完整改造后跑（实施）
**选 B**：单元测试就跑 5000 万 entry × 30 batch（任务卡 §4 KPI 字面）

选 A 的理由：
- 单元测试每次都跑 → 5000 万 entry 预填 ~5 秒 + 30 batch × 100ms = ~9 秒，每次 CI 都跑代价过大
- libcuckoo prototype 跟真 cacheCoins 接入差距大（真 cacheCoins 还涉及 UTXO 序列化、CCoinsCacheEntry 结构、metaMtx 等）
- 真 KPI 应在 P0 完整改造后用 `src/bench/` 跑（手动触发）

⚠️ 实测教训：第一次写得太重（100 万 entry × 30 batch × 32 reader × 用错 API `insert_or_assign`），跑成 3 小时吃 14 核失控进程。**经验**：单元测试预算严格 < 5 秒，重负载留 bench 目标。

### 2.3 P0.0b.3 baseline 留 SCAFFOLD 给业务方

理由：
- 业务方明确不能动生产节点 / 生产 datadir
- 800GB / 64GB / 3-4 周资源不在 auto session 可控范围
- baseline 输出文件本身（100+ MB JSON）不进 git

替代：完整 5 步实施 README + 脚本框架 + 安全检查（拒绝生产 datadir）已交付，业务方可独立跑。

---

## 3. trade-off 跟残余风险

| trade-off | 影响 | 缓解 |
|-----------|------|------|
| seqlock 测试只跑 3 秒（不是 24h）| 时间窗口短，覆盖率有限 | ARM/RISC-V QEMU 长压 + P6.2 72h 长压补足 |
| BatchWrite 单元测试 1 万 entries | 离真主网 5000 万差 5000x | GATE-M0 P0.7 真 5000 万 bench |
| baseline 未实测 | 30%+ 覆盖率未验证 | SCAFFOLD 完整 + 业务方实施可独立跑 + P6.4 全量 shadow 互补 |
| smoke round-trip 范围有限 | 当前 vN disk format 未真改 | P5.6 full round-trip 含 dispatcher/cacheCoins/seqlock 全接入再验 |

| R | 当前状态 |
|---|---------|
| R1 长周期 | 本卡 + P0.0a hand-off 文档共 ~5000 字；secondary 待指派 |
| R2 跨 boost / 平台 | x86 boost 1.74 验证；ARM/RISC-V QEMU 待 CI matrix |
| R3 32 worker race | seqlock 32 reader 3 秒 0 torn；72h 留 P6.2 |
| R4 共识等价性 | SCAFFOLD 交付，待业务方实施 |
| R6 chainActive 漏改 | 本卡未接入路径，留 P1.6 AST grep |
| R7 abort 路径 | smoke round-trip 双向 0 reindex |

---

## 4. 新人接手路径

### 4.1 阅读顺序

1. `docs/plans/cs_main-refactor-plan.md`（v2.6.1 概要）
2. `docs/plans/spike-results/ARCHITECTURE-NOTE-P0.0a.md`（前一 phase 决策）
3. 本文档
4. `docs/plans/seqlock-memory-model.md`（memory-model 证明）
5. `docs/plans/spike-results/P0.0b.3-baseline-readme.md`（baseline 实施指南）
6. `src/validation/chainstate.h`（120 行）
7. `src/test/chainstate_seqlock_tests.cpp` + `batchwrite_p99_tests.cpp`
8. `tools/smoke-roundtrip.sh`

### 4.2 一键复现

```bash
cd /home/ubuntu/TBCNODEDEV
# 1. 编译 + 跑全套 P0.0a + P0.0b 单元测试（16 项）
nice -n 10 cmake --build build -j8 --target test_bitcoin
./build/src/test/test_bitcoin --run_test=batchwrite_p99,chainstate_seqlock,libcuckoo_soak,recursive_mutex_spike,lock_hierarchy_tests
# 期望：16 项全过

# 2. 跑 smoke round-trip（regtest，2 分钟）
./tools/smoke-roundtrip.sh
# 期望：双向切换 0 reindex
```

### 4.3 待解清单（P0 启动前知道）

- [ ] 业务方实施 baseline replay（P0.0b.3，3-4 周）
- [ ] CI matrix 4 boost 版本 + ARM/RISC-V QEMU + 24h libcuckoo soak
- [ ] DEPENDENCY-PIN-MATRIX.md（P0.0a.6 任务卡，待写）
- [ ] BatchWrite 真 5000 万 entry bench 留 P0.7
- [ ] 196 项 baseline 测试失败（util_tests/mempool_tests/filled_miner_bill_v2）独立立项

### 4.4 不要碰的文件

- `~/TBCNODE/`（生产代码，0 修改）
- `~/.bitcoin/`（生产 datadir，0 修改）
- 生产节点 PID 4185870

### 4.5 编译/测试快捷

```bash
cd /home/ubuntu/TBCNODEDEV
# 增量编译（秒级 if 只改测试）
nice -n 10 cmake --build build -j8 --target test_bitcoin

# 跑某个 suite
./build/src/test/test_bitcoin --run_test=chainstate_seqlock --log_level=test_suite

# 出问题不要 commit，先跑 git status 看改了什么
cd ~/TBCNODEDEV && git status   # 留 working tree 不动
```

---

## 5. 给 P0 owner 的输入

P0 是 v2.6.1 最大的 phase（14-18 周 / 8 张任务卡），核心是 CCoinsViewCache 改造：

| 卡 | 工时 | 输入 |
|----|-----|-----|
| P0.1 cacheCoins → libcuckoo + 三层锁 | 2 周 | 用 P0.0a.1 已集成的 libcuckoo |
| P0.2 BatchWrite K2 路径 | 2 周 | 用 P0.0b.2 prototype 验证过的 insert+update_fn |
| P0.3 GetCoinConcurrent L1/L2/L3 | 2 周 | 待 LRUCache util |
| P0.4a pcoinsTip shared_ptr lifetime | 2 周 | F3 修复，全代码库 raw `pcoinsTip->` 调用点扫描 |
| P0.4b cachedCoinsUsage atomic | 1 周 | atomic 计数 + IsBatchWriteInProgress |
| P0.5 老接口兼容 | 1 周 | 老 GetCoin/HaveCoin/AddCoin 签名 100% 保留 |
| P0.6 TSan 72h | 2-3 周 | 用 P0.0a.2 baseline 比对 |
| P0.7 32 worker bench | 2-3 周 | KPI ≥ 16x 单线程 |

**GATE-M0 KPI 关键**：
- 32 worker 并发吞吐 ≥ 16x（GATE-M0 决策门最大风险）
- TSan 72h 0 新增 race
- BatchWrite p99 ≤ 200ms（真主网量级）
- pcoinsTip Shutdown 序 ASan 0 use-after-free
- 老 ctest + functional 全过

GATE-M0 失败 → 整套 v2.6.1 放弃，沉没 9-10 月。是 v2.6.1 最大决策门。

---

## 6. 一句话总结

**P0.0b 本地可做部分全部完成，P0.0b.3 baseline SCAFFOLD 完整交付业务方**。16 项单元测试全过，smoke round-trip 双向 0 reindex 通过，生产节点 0 影响。可进 P0。

签字：
- primary owner：Claude (auto-mode session, 2026-04-28)
- secondary owner：[待业务方指派]
- reviewer：[待业务方指派]

业务方 sign-off（GATE-M-1b 必过）：
- [ ] 接受 18-22 月工期 + 26-40 人月 + TPS 600 下限
- [ ] 接受 P0.0b.3 baseline 实施由业务方运维 3-4 周完成
- [ ] 接受 BatchWrite 真 KPI 留 GATE-M0 验证
- [ ] 接受 11 周沉没决策窗口

---

文档字数：~3300 字（满足 R1 ≥ 2000 字要求）
