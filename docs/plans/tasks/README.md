# cs_main 重构方案 v2.6.1 任务卡索引

> 配套主文档：
> - [`../cs_main-refactor-plan.md`](../cs_main-refactor-plan.md) 概要设计
> - [`../cs_main-refactor-detailed-design.md`](../cs_main-refactor-detailed-design.md) 详细设计
> - [`../cs_main-refactor-audit-log.md`](../cs_main-refactor-audit-log.md) 9 轮审核记录

## 目录约定

每张任务卡是一个独立 markdown 文件，命名 `P{phase}.{seq}-{slug}.md`。

每张卡必含 9 个区段：

1. **目标** — 一句话说改什么
2. **影响文件** — 绝对路径 + 行号
3. **代码改造** — 完整代码 / 伪代码 / diff 草图
4. **验收 KPI** — 可量化、可自动化测试的指标
5. **测试用例** — 单元 / functional / sanitizer / benchmark 全列出
6. **风险登记** — 钉到 R1-R7 的具体哪条 + 缓解动作
7. **回滚条件** — KPI 不达标怎么回到上一稳定态
8. **审核 checklist** — code-review 时必过的项
9. **依赖 / 阻塞** — 上下游任务卡引用

## 风险登记表

R1-R7 见 `R-risk-register.md`，每张任务卡的"风险登记"段引用 R 编号。

## 阶段索引

| Phase | 工时 | 任务卡数 | 入口 |
|-------|------|---------|------|
| P0.0a | 5 周 | 5 张 | [P0.0a-INDEX.md](./P0.0a-INDEX.md) |
| P0.0b | 6 周 | 4 张 | [P0.0b-INDEX.md](./P0.0b-INDEX.md) |
| P0 | 14-18 周 | 8 张 | [P0-INDEX.md](./P0-INDEX.md) |
| P1 | 4-6 周 | 7 张 | [P1-INDEX.md](./P1-INDEX.md) |
| P2 | 8-12 周 | 8 张 | [P2-INDEX.md](./P2-INDEX.md) |
| P3 | 6-8 周 | 7 张 | [P3-INDEX.md](./P3-INDEX.md) |
| P4 | 16-20 周 | 6 张 | [P4-INDEX.md](./P4-INDEX.md) |
| P5 | 4-6 周 | 6 张 | [P5-INDEX.md](./P5-INDEX.md) |
| P6 | 11-13 周（简化版）| 8 张 | [P6-INDEX.md](./P6-INDEX.md) |

总 **60 张任务卡**（P0.0a 6 + P0.0b 4 + P0 8 + P1 7 + P2 8 + P3 7 + P4 7 + P5 6 + P6 8 - shadow/canary 删除 - canary 删除 = 60；含 P0.0a.6 dependency-pin）。每张卡平均 1-3 周工时。3 人并行。

**简化策略**：开发网 ≡ 主网（仅 chainparams 字段不同），开发网 4 周稳定 = 真主网兼容验证完成；P6 删除原 shadow node 4w + canary 4w + 渐进 4w 共 12 周流程，节省 3-7 周。详见 [P6-INDEX.md](./P6-INDEX.md)。

## 决策门 KPI 索引

| 门 | 阶段末 | 文档 |
|---|------|------|
| M-1a | P0.0a 完成 | [GATE-M-1a.md](./GATE-M-1a.md) |
| M-1b | P0.0b 完成 | [GATE-M-1b.md](./GATE-M-1b.md) |
| M0 | P0 完成 | [GATE-M0.md](./GATE-M0.md) |
| M1 | P1+P2 完成 | [GATE-M1.md](./GATE-M1.md) |
| M2 | P3+P4+P5 完成 | [GATE-M2.md](./GATE-M2.md) |
| M3 | P6 完成 | [GATE-M3.md](./GATE-M3.md) |

## 审核协议

每张卡完成后**必须走两轮独立审核**（架构 + 安全 + cpp 三选二）才能 merge。审核 checklist 在卡内 §8。每张卡的"风险登记"必须钉到 R1-R7 至少一条；不能钉的卡 = 这张卡不解决任何已知风险，需重新审视是否必要。

## R1 强制机制（长周期人员流失缓解）

R1 是 24 月项目最大的非技术风险（单人专注 2 年失败概率 > 50%）。所有 61 张任务卡**强制**遵守：

### R1.1 双人 ownership（强制）

每张任务卡 §owner 必须填两人：

```
**owner**: primary={姓名}, secondary={姓名}
```

- primary 主导实现
- secondary 必须独立看完代码 + KPI + 测试，能在 primary 离职 / 病假时 1 周内接手
- 任一人未填 → PR 不允许 merge

### R1.2 ARCHITECTURE-NOTE 强制交付

每个 phase 末（P0.0a / P0.0b / P0 / P1 / P2 / P3 / P4 / P5 / P6）的最后一张任务卡必须包含 ARCHITECTURE-NOTE-{phase}.md 交付：

- 决策理由（为什么这样设计而不是别的方案）
- 实测 KPI 跟原目标的偏差 + 校准记录
- 已知 trade-off + 残余风险
- 新人接手所需的入门路径
- 该 phase 内**所有跟原详细设计 detailed-design.md 偏离的实现选择**

文档 ≥ 2000 字。GATE 评审会前 1 周交付。

### R1.3 Hand-off 测试（每 GATE 必做）

每个 GATE 评审会前 1 周做一次 hand-off 测试：

- 选 1 个不参与本 phase 的工程师作为"new owner"
- 给他 ARCHITECTURE-NOTE + 该 phase 全部任务卡 + 决策门 KPI 矩阵
- 1 周内他必须能：(a) 解释该 phase 做了什么；(b) 列出所有未解决问题；(c) 提出至少 1 个改进建议

通过 → GATE 评审会照常进行
不通过 → 文档不充分，原 owner 补 ARCHITECTURE-NOTE 后再做

### R1.4 任务卡 §6 强制钉 R1

每张任务卡 §6 风险登记表新增一行：

| R | 缓解 | 证据 |
|---|------|------|
| R1 长周期人员流失 | 双人 ownership + ARCHITECTURE-NOTE + hand-off 测试 | README §R1 |

**这一行所有卡都必填，跟卡的内容无关**。R1 是项目级风险，每张卡都受影响。

## 任务卡模板

见 [TEMPLATE.md](./TEMPLATE.md)。
