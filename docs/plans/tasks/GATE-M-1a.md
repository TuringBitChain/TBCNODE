# GATE M-1a 决策门（P0.0a 末，启动 5 周）

**性质**：硬决策门，任一 KPI 不达标 → **整套 v2.6.1 放弃，沉没 5 周**

---

## Wall-clock 硬上限

**estimate**：5 周
**hard limit**：**8 周**（estimate × 1.5 + 1 周缓冲）

超过 8 周仍未提交 GATE-M-1a 评审 → **强制评审会**：
- 进度评估 + 业务方决策（接受延期 / 缩 scope / 放弃）
- 不允许"再拖一拖"无止境

---

## 必过 KPI 矩阵

| 来源任务卡 | KPI | 阈值 |
|-----------|-----|-----|
| P0.0a.1 | libcuckoo 5000 万 entry × 24h soak 内存漂移 | < 100MB |
| P0.0a.1 | libcuckoo 24h ops 总量 | > 10 亿 |
| P0.0a.1 | TSan / helgrind 24h | 0 race |
| P0.0a.1 | 跨平台（Ubuntu 20.04/22.04/24.04 + ARM64 emulator）| 全过 |
| P0.0a.2 | TSan / ASan / UBSan baseline 建立 | suppression list 100% 已知项 |
| P0.0a.2 | 编译矩阵覆盖完整 | TSan + ASan + UBSan + helgrind 4 build 全过 |
| P0.0a.3 | `lock-hierarchy.md` v0.1 review | 至少 1 架构 + 1 cpp LGTM |
| P0.0a.3 | `lock_hierarchy.h` 静态断言 | 编译期通过 |
| P0.0a.4 | sync.h hook 改造 401 callsite | 编译期 0 error |
| P0.0a.4 | DEBUG_LOCKORDER build functional test | 0 LOCK ORDER VIOLATION abort |
| P0.0a.4 | release build 性能影响 | < 1% 退化 |
| P0.0a.5 | boost::recursive_mutex try_lock 5 项语义 × 4 版本 × 2 OS | 全过 |
| P0.0a.5 | H-F 异常注入 RAII 平衡测试 | 4 版本全过 |

## 决策流程

1. P0.0a.1 / P0.0a.2 / P0.0a.3 / P0.0a.4 / P0.0a.5 各自 PR merge 到 main
2. 5 周末提交 GATE-M-1a 评审会
3. 任一 KPI 红 → close 所有任务卡 PR → main 分支无变化 → 项目结束
4. 全绿 → 进 P0.0b

## 强制回滚演练

GATE-M-1a 评审会前必须完成 1 次回滚演练：
- 在演练机器上 apply P0.0a 全部 PR
- 模拟 KPI 失败场景（任一硬 KPI 红）
- 执行 close 所有 PR + main 分支无变化的回滚命令
- 验证 datadir / build artifacts / CI state 100% 回到启动前
- 演练耗时 ≤ 4 小时
- 记录归档 `docs/plans/spike-results/GATE-M-1a-rollback-drill.md`

**真演练**，不是 dry-run。失败 → 修复回滚机制 → 再演练。

---

## 沉没成本

5 周（1 主开发 + 1 reviewer + 1 QA × 5 周 ≈ 6-8 人月）

## 输出物（全过时）

- libcuckoo 集成进 build 系统
- TSan/ASan/UBSan/helgrind CI 24h 长跑环境
- `lock-hierarchy.md v0.1`（含 6 个新 mutex level + 401 callsite 兼容性证明）
- `src/sync.h` 改造完成，401 callsite 编译期零修改
- boost spike 报告 + 备选方案就绪
