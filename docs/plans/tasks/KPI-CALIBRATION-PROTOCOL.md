# KPI 校准协议

**适用于**：所有 61 张任务卡 §4 验收 KPI、6 个 GATE 必过 KPI 矩阵。

---

## 1. 问题

任务卡 §4 写"32 worker 并发吞吐 ≥ 16x"、"BatchWrite p99 ≤ 200ms"、"libcuckoo 24h 漂移 < 100MB"。这些数字**今天没有实测依据**，是基于经验估算的目标值。

如果 spike 阶段实测 BatchWrite p99 是 230ms（不是 200ms）：
- 严格按现行 GATE 协议 → KPI 红 → 整套放弃 → **可能误杀好方案**
- 不按 GATE → KPI 失去强制力 → **风险兜底机制失效**

两个极端都不对。需要中间机制。

---

## 2. 协议

每个 KPI 数值分**三类**：

### 2.1 硬阈值（由共识/物理决定，不可调整）

| 例 | 不可调原因 |
|---|----|
| TSan/helgrind 0 race | race = correctness bug，0 是唯一正确值 |
| 共识等价性 100% hash 一致 | 共识不能 99.99% |
| 0 use-after-free / 0 leak | 内存安全唯一正确值 |
| AST grep 0 未改造且未持 cs_main | 漏一处 = race |

**硬阈值不达标 = GATE 失败，直接放弃或 revert，无校准空间**。

### 2.2 软阈值（性能/资源类，spike 实测后允许校准 ±50%）

| 例 | 校准协议 |
|---|----|
| 32 worker 16x 单线程 | spike 实测 12-24x 进入校准；< 12x → GATE 失败 |
| BatchWrite p99 ≤ 200ms | 实测 100-300ms 进入校准；> 300ms → GATE 失败 |
| libcuckoo 24h 漂移 < 100MB | 实测 50-150MB 进入校准；> 150MB → GATE 失败 |
| TPS ≥ 600 | 实测 400-900 进入校准；< 400 → 触发 trade-off 评审 |
| RPC p99 < 100ms | 实测 50-150ms 进入校准；> 150ms → 评审 |

**校准动作**：
1. spike 实测数字写入 `docs/plans/spike-results/<task-id>-actual.md`
2. 业务方 1 周内决定接受 / 不接受
3. 接受 → 该任务卡 §4 KPI 改写为实测值 + 一行说明 "校准自 [日期]，原目标 [原值]"
4. 不接受 → 任务卡进入 KPI-revision phase 重新设计

### 2.3 探索阈值（spike 阶段才能给出，今天预设占位）

| 例 | 现状 |
|---|----|
| boost::recursive_mutex try_lock 跨线程语义 | P0.0a.5 spike 决定（4 boost 版本） |
| seqlock 在 ARM/RISC-V 是否需调整 fence | P0.0b.1 spike 决定 |
| 主网采样窗口最优策略 | P0.0b.3 spike 决定 |
| dispatcher 16-shard 是否够用 | P2.1 实测决定 sharding 数 |

**探索阈值**：spike 失败 → 切备选方案（每张卡 §7 已写）；spike 成功 → 数字写入正式 KPI。

---

## 3. 每张任务卡的标注义务

任务卡 §4 表格新增"类型"列：

| KPI | 目标 | 类型 | 测量 |
|-----|------|-----|------|
| TSan 24h | 0 race | **硬** | sanitizer |
| 32 worker 吞吐 | ≥ 16x | **软** | bench |
| boost try_lock 同线程 true | 通过 | **探索** | spike |

**强制规则**：
- §4 表格中如果出现"软"类型 KPI，§7 回滚条件必须写明校准 / 失败两条路径
- §4 表格中如果出现"探索"类型 KPI，§7 必须明确指向备选方案任务卡

---

## 4. KPI 校准记录模板

```markdown
# spike-results/P0.0a.1-actual.md

## 任务卡：P0.0a.1 libcuckoo soak

### 实测 KPI（YYYY-MM-DD）

| KPI | 原目标 | 实测 | 校准结果 |
|-----|--------|------|---------|
| 24h 内存漂移 | < 100MB | 78MB | **达标**（无需校准） |
| 24h 总 ops | > 10 亿 | 14.2 亿 | **超标** |
| 单 ops 延迟 | < 5µs | 6.2µs | **校准至 < 7µs**（业务方 2026-XX-XX 同意）|

### 业务方签字

业务方：________ 日期：________
主开发：________
```

---

## 5. 决策门 KPI 矩阵的应用

GATE-M-1a / M-1b / M0 / M1 / M2 / M3 必过 KPI 矩阵中：

- **硬 KPI** 不达标 → GATE 红 → 整套放弃
- **软 KPI** 不达标 → 校准协议执行：业务方签字接受新值 → GATE 绿；拒绝 → GATE 红
- **探索 KPI** 不达标 → 切备选方案 → 备选方案过 → GATE 绿；备选方案也不过 → GATE 红

---

## 6. 强制原则

- **任务卡 §4 KPI 表如果不标 "硬/软/探索"，PR 不允许 merge**
- **GATE 评审会前一周必须把所有"软/探索"的实测值汇总到 spike-results/ 目录**
- **业务方 GATE 评审会上签字接受所有"软"KPI 校准值**
