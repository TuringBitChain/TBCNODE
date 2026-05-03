# Mempool Ancestor Cleanup Plan

## 目的

删除 BSV 1.0.5 RC 遗留的 4 个 cached ancestor aggregate 字段 + LegacyBlockAssembler，修复 TBC ccf31423c 自加 `ancestorsHeight` 字段在 reorg 路径漏维护的潜在隐患，达到 BSV master Step 1-3 的清理水平（不改 mempool 架构）。

**预期收益**：链 500 入池时间 73ms → ~5ms（500× 减少 boost::multi_index reindex 操作）。

**不动**：consensus 规则、mempool 架构（CPFPGroup / Primary-Secondary / EvictionCandidateTracker 是 BSV master Step 4-5，工作量 3-5 天，留 future work）、TrimToSize 算法、`ancestorsHeight` 字段（这是 TBC ccf31423c 设计核心，保留）。

## 背景

### 4 个 cached ancestor aggregate 字段

| 字段 | 维护时机 | 维护代价 |
|---|---|---|
| `nCountWithAncestors` | 入池 +1 / RemoveForBlock -1 | O(祖先集合) |
| `nSizeWithAncestors` | 同上 | O(祖先集合) |
| `nModFeesWithAncestors` | 同上 + UpdateFeeDelta | O(祖先集合) |
| `nSigOpCountWithAncestors` | 同上 | O(祖先集合) |

每入池一个 tx 走 BFS 全祖先集，per-ancestor 调 `mapTx.modify(update_ancestor_state)`，每次 modify 触发 boost::multi_index 7 个索引 reindex。链 500 入池 = 499 modify × 7 reindex ≈ 3500 op，**这就是 73ms 慢的根因**。

TBC 实际**入池热路径**只用其中之一（chain limit 检查），其余 5 处 sort 用法都是 policy 层（journal/RPC/eviction），可换字段或删。

### `ancestorsHeight` 字段（TBC ccf31423c 自加）

定义：`height(tx) = max(height(p) for p in parents(tx)) + 1`，无父则 0。

维护代价 O(直接父数量) — 入池时只看直接父取 max+1，1 次 modify。

**当前用途**：
- `CalculateMemPoolAncestorsNL` 入池时 chain depth 限制
- `chainLimit` 检查
- `Check()` invariant 断言（`-checkmempool=1` 模式）
- 调试日志

**5 处 sort 用法（当前用 nCountWithAncestors，本次清理换 ancestorsHeight）**：

| # | 位置 | 拓扑序约束 |
|---|---|---|
| 1 | `journal_change_set.cpp:120` REORG/RESET sort | 真拓扑序（父必须排子前） |
| 2 | `txmempool.cpp:1395` `CompareDepthAndScoreNL`（dead code，0 caller） | 删 |
| 3 | `txmempool.cpp:1415` `DepthAndScoreComparator` → `getrawmempool` 输出 | 仅展示约束 |
| 4 | `txmempool.cpp:1845` `topoSortedTxFromSet` BlockMinTxFee CPFP-debt | 真拓扑序 |
| 5 | `txmempool.cpp:466` `UpdateAncestorState` method（不是 sort） | 删整 method |

## 跨模块共享数据结构 — `AncestorDescendantCounts`

**关键发现**：`AncestorDescendantCounts` 不是 mempool 内部 struct，是跨 mempool / mining 模块的**共享引用对象**。

```
mempool entry.ancestorDescendantCounts: shared_ptr<AncestorDescendantCounts>
                    ↓ 同一 shared_ptr 跨模块共享
journal entry.mAncestorCount: shared_ptr<AncestorDescendantCounts>
                    ↓
journal_change_set REORG sort 通过 journal entry 拿 shared_ptr 读
```

**当前 struct 内容**：
```cpp
struct AncestorDescendantCounts {
    std::atomic_uint64_t nCountWithAncestors{0};
    std::atomic_uint64_t nCountWithDescendants{0};
};
```

**Phase 2 删 `nCountWithAncestors` 的连带影响**：
- `journal_entry.h` 持的 shared_ptr 后续无法读到 mempool 的拓扑深度
- `journal_change_set.cpp:118-122` REORG sort 拿不到 sort key
- 必须把 `ancestorsHeight` 也搬进共享 struct（否则 journal 拿不到）

**修法（Phase 2 含）**：

```cpp
struct AncestorDescendantCounts {
    std::atomic<size_t> ancestorsHeight{0};        // ← 从 mempool entry 直接成员搬进来
    std::atomic_uint64_t nCountWithDescendants{0}; // 保留
};
```

- `src/txmempool.h:176` mempool entry 删独立 `size_t ancestorsHeight` 字段
- `src/txmempool.h:228-229` `GetAncestorsHeight()/SetAncestorsHeight()` 内部改读写 `ancestorDescendantCounts->ancestorsHeight`（API 不变，外部 caller 0 改）
- `src/txmempool.cpp:75` ctor 改 `make_shared<AncestorDescendantCounts>(0, 1)`（height=0, descendantCount=1）
- `src/txmempool.cpp:583-593` 入池 `SetAncestorsHeight` 透传到 shared 对象
- `src/txmempool.cpp:1996` `UpdateAncestorsHeightNL` 透传到 shared 对象
- `src/mining/journal_change_set.cpp:118-122` 改读 `count1->ancestorsHeight`
- `src/test/journal_tests.cpp:23` 改 `make_shared<AncestorDescendantCounts>(0, 1)`

## 关键发现：reorg 路径 ancestorsHeight stale 问题

### Site 0 — `UpdateTransactionsFromBlock` 漏更新 `ancestorsHeight`

**问题描述**：

DisconnectBlock 回流 tx 时，`UpdateTransactionsFromBlock` (`txmempool.cpp:182`) 给 disconnect 回流 tx 的 children（已在 mempool 但当时父在 block）：
- 重建 `mapLinks` ✓
- 调 `update_ancestor_state` 更新 4 cached aggregates ✓
- **不调 `UpdateAncestorsHeightNL` 重算 children 的 `ancestorsHeight`** ✗

**stale state 示例**：
```
chain X 上 block N 含 A → B → C
mempool 已有 D（D.parent=C, D.height=0 因 C 当时在 block）
        ↓ chain Y 抢
DisconnectBlock N → A.height=0, B.height=1, C.height=2 回流
UpdateTransactionsFromBlock 跑 → mapLinks/cached 全对，但 D.height 仍 = 0（实际应 3）
```

### prod 当前实际影响

**prod 用 nCountWithAncestors 排序，5 处 sort 不依赖 height** → 当前**没有**共识 / 矿工 / RPC 影响。

**有一个用户感知 0 的 policy 隐患**：reorg 后 mempool 中 disconnect tx 的 children 的 chain depth limit 检查偏松（字段值偏小，能继续接收新 child 超出 limit）。触发条件：reorg + 跨 reorg 的 mempool 长链 + 踩 limit，**线上极罕见**。

### Site 0 修法

`UpdateTransactionsFromBlock` 末尾追加：

```cpp
// 在 line 229 (for 循环结束后)、line 230 (CEnsureNonNullChangeSet) 之前：
setEntriesTopoSorted heightsToRefresh;
for (const uint256 &hash : vHashesToUpdate) {
    txiter it = mapTx.find(hash);
    if (it == mapTx.end()) continue;
    for (txiter child : GetMemPoolChildrenNL(it)) {
        heightsToRefresh.insert(child);
    }
}
UpdateAncestorsHeightNL(heightsToRefresh);
```

`UpdateAncestorsHeightNL` (`txmempool.cpp:1961`) 已经按拓扑序 BFS 重算 height（与 `RemoveForBlock` line 1076 用法一致），加这 6 行即可。

**估计**：+10 行代码（含注释）+ 1 单测（`mempool_reorg.py` 触发 disconnect block，验 children height 重算正确）。

## Phase 排序修正（审计发现的循环依赖）

**初版排序问题**：

| 问题 | 等级 | 描述 |
|---|---|---|
| Phase 1 site 1 跨模块依赖 | HIGH | journal_change_set REORG sort 通过 shared_ptr 拿 count，shared struct 当前没 ancestorsHeight 字段。Phase 1 单独无法完成此 task |
| Phase 2 → Phase 3 中间 build broken | HIGH | Phase 2 删 nCountWithAncestors + update_ancestor_state struct → LegacyBlockAssembler 编不过。Phase 3 必须在 Phase 2 之前或同 PR |

**修正后排序**：Phase 0 → **Phase 3 (LegacyBlockAssembler 删，提前)** → Phase 1 + Phase 2 (合并，shared struct 加 height + 删旧字段 + 5 处 sort 换) → Phase 4 (RPC + docs)。

合理性：
- Phase 3 提前后，ancestor_score 索引无人用（LegacyBlockAssembler 是唯一 caller），可在 Phase 1+2 合并里安全删
- Phase 1 + Phase 2 合并避免循环依赖（shared struct 加 height 是 Phase 1 site 1 的前置，删 nCountWithAncestors 是 Phase 2 的核心，必须同 PR）

## 计划阶段

### Phase 0 — 前置 Site 0 修复（独立小 PR）

**这一步可以独立 merge，不依赖后续清理。**

| 任务 | 文件 | 工作量 |
|---|---|---|
| 0.1 | 修 `UpdateTransactionsFromBlock` 末尾加 `UpdateAncestorsHeightNL` 调用 | `src/txmempool.cpp:182` (+10 行) |
| 0.2 | 加 `Check()` invariant 注释说明 reorg 后 height 必须刷新 | `src/txmempool.cpp:1188` (+3 行) |
| 0.3 | 加 reorg 单测 `MempoolReorgHeightRefresh` | `src/test/mempool_tests.cpp` (+50 行) |
| 0.4 | 跑 `test/functional/mempool_reorg.py` 验证 | 0 改动，验功能测试通过 |
| 0.5 | dev 节点 reorg soak（chain Y 抢占 chain X 含长链 mempool） | 0 改动，soak 24h |
| 0.6 | **reorg perf baseline 对比**（消除 Risk 1）— 同 reorg 场景下测**改前 vs 改后**`UpdateTransactionsFromBlock` 耗时；**acceptance**: 改后 ≤ 改前 × 1.5（增量 ≤ 50%）。超阈值 → 查算法不是加限制 | 0 代码改动，bench 验证 |

**Phase 0 工作量：1.5h 编码 + 4h 测试**

**Phase 0 收益（独立）**：
- 修复 chain depth limit 检查偏松隐患
- 修复 `-checkmempool=1` 模式下 reorg 后 assert 可能炸
- 为 Phase 1 换字段铺路

### Phase 3 (新顺序 — 提前) — 删 `LegacyBlockAssembler`

**前置**：Phase 0 已 merge。Phase 3 独立可 merge（不影响其他字段）。

| 任务 | 改动 |
|---|---|
| 3.1 | 删 `src/mining/legacy.h` + `src/mining/legacy.cpp`（共 ~1100 行） |
| 3.2 | `src/mining/factory.cpp:28-30` LEGACY case 改 `throw "LEGACY assembler removed since v3.3.0, use JOURNALING"` |
| 3.3 | 删 `src/mining/factory.h:8` `#include <mining/legacy.h>` |
| 3.4 | 删 `src/CMakeLists.txt:250` `mining/legacy.cpp` 列表项 |
| 3.5 | 删 `src/test/txvalidationcache_tests.cpp:10` `#include "mining/legacy.h"` |
| 3.6 | `src/init.cpp:1075` help 文本去掉 LEGACY 提及 |
| 3.7 | 删 `src/test/miner_tests.cpp` `miner_tests_legacy` suite + `LegacyTestingSetup` class |

**注意**：MempoolAncestorIndexingTest 依赖 ancestor_score 索引，要在 Phase 1+2 合并 PR 中删（不在 Phase 3）。

**Phase 3 工作量：2h 编码 + 1h 测试**

### Phase 1 + Phase 2 合并 — Sort 换字段 + 删 4 个 cached ancestor 字段（一个 PR）

**前置**：Phase 0 + Phase 3 已 merge。

**合并理由**：
- Phase 1 site 1（journal sort 换 height）需要 shared struct 含 ancestorsHeight，这是 Phase 2 的任务
- 拆分会让中间状态有死代码，不可独立 review

#### Sub-section A：共享 struct 重构（site 1 前置）

| 任务 | 文件 | 改动 |
|---|---|---|
| A.1 | `AncestorDescendantCounts` struct 加 `atomic<size_t> ancestorsHeight` 字段（同步删 `nCountWithAncestors`） | `src/txmempool.h:73-83` |
| A.2 | mempool entry 删独立 `size_t ancestorsHeight` 字段，`GetAncestorsHeight()/SetAncestorsHeight()` 内部改读写共享 struct（API 不变，外部 caller 0 改） | `src/txmempool.h:176, 228-229` |
| A.3 | ctor 创建共享 struct 改 `make_shared<AncestorDescendantCounts>(0, 1)`（height=0, desc=1） | `src/txmempool.cpp:75` |
| A.4 | 入池 `SetAncestorsHeight` + `UpdateAncestorsHeightNL` 透传共享 struct（API 不变） | `src/txmempool.cpp:583-593, 1996` |
| A.5 | `journal_tests.cpp:23` 测试构造 journal entry 时 `make_shared<AncestorDescendantCounts>(0, 1)` | `src/test/journal_tests.cpp:23` |

#### Sub-section B：5 处 sort 换 ancestorsHeight

| 任务 | 文件 | 改动 |
|---|---|---|
| B.1 | journal REORG/RESET sort 改**两阶段 snapshot-based**（消除 Risk 3）— 把 atomic load 进 local Indexed vector，stable_sort local，最后按 orig_idx rebuild | `src/mining/journal_change_set.cpp:99-130`（重写 sort 块约 25 行） |
| B.2 | 删 `CompareDepthAndScoreNL` + `CompareDepthAndScore`（dead code，0 caller） | `src/txmempool.{h,cpp}:1383-1412` + header 声明 |
| B.3 | `DepthAndScoreComparator` 换 `GetAncestorsHeight()`（在 smtx shared_lock 下天然安全，不需 snapshot） | `src/txmempool.cpp:1415-1428` |
| B.4 | `topoSortedTxFromSet` lambda 改**两阶段 snapshot-based**（消除 Risk 3）— 跟 B.1 同模式 | `src/txmempool.cpp:1840-1849`（重写 lambda 约 15 行） |
| B.5 | **sort 输出 diff 验证**（消除 Risk 2）— 旧/新 binary 各跑 stress 100× `getrawmempool true` JSON dump，diff 验 tx 集合 + 字段值相等，仅允许顺序差 | bench script，0 代码改动 |
| B.6 | **reorg 拓扑 fuzz**（消除 Risk 2）— 50× 随机 reorg（深度 1-10）× mempool 长链 50，每次后 assert ancestorsHeight invariant + journal 拓扑序合法 | 新建 `src/test/mempool_reorg_fuzz_tests.cpp` (+150 行) |

#### Sub-section C：删 4 个 cached ancestor 字段 + 相关代码

| 任务 | 文件 | 改动 |
|---|---|---|
| C.1 | 删 `nSizeWithAncestors`, `nModFeesWithAncestors`, `nSigOpCountWithAncestors` 字段 + 3 getters | `src/txmempool.h:165-167, 222-227` |
| C.2 | 删 `update_ancestor_state` struct + `UpdateAncestorState` method | `src/txmempool.h:254-270`, `src/txmempool.cpp:460-470` |
| C.3 | 删 ctor 中 4 字段写入（`nSizeWithAncestors`/`nModFeesWithAncestors`/`nSigOpCountWithAncestors`/`nCountWithAncestors` 初始化） | `src/txmempool.cpp:83-85`（line 75 ctor + line 83-85 是 setup） |
| C.4 | 删 `UpdateFeeDelta` 中 `nModFeesWithAncestors += newFeeDelta - feeDelta` | `src/txmempool.cpp:102` |
| C.5 | **删整个 `updateEntryForAncestorsNL` 函数** + 调用点 | `src/txmempool.cpp:347-360` 函数 + line 596 调用 |
| C.6 | **只删** `updateForRemoveFromMempoolNL` 中 `if (updateDescendants)` 整块（line 377-395，对 4 ancestor cached 操作） + `bool updateDescendants` 参数本身（5 caller 全传 false：line 784, 855, 984, 2049, 2202） + log line 445 中 `updateDescendants ? "true" : "false"` 引用。**保留函数主体（line 397-447）** — 这部分调 `update_descendant_state`（line 342 via `updateAncestorsOfNL`）跟 link sever（line 428, 435），跟 4 cached ancestor 无关。 | `src/txmempool.cpp:369, 377-395, 445, 784, 855, 984, 2049, 2202` + `src/txmempool.h:979`/`1019` 函数 + `removeStagedNL` 声明 |
| C.7 | 删 `updateForDescendantsNL` 中 `update_ancestor_state` 调用（line 150-153） | `src/txmempool.cpp:150-153` |
| C.8 | 删 issue #12 fix 中整个 for 块（line 1006-1019）— 这块只减 4 cached ancestor | `src/txmempool.cpp:1006-1019` |
| C.9 | 删 `prioritisetransaction` 中 `update_ancestor_state(0, nFeeDelta, 0, 0)` 调用 | `src/txmempool.cpp:1596` |
| C.10 | **删 `Check()` 中 line 1190-1213**（CalculateMemPoolAncestorsNL 调用 + setAncestors 累加 + 3 个 cached invariant assert）— 保留 line 1188 `assert(ancestorsHeight == GetAncestorsHeight())` invariant | `src/txmempool.cpp:1190-1213`（约 24 行） |
| C.11 | 删 `CompareTxMemPoolEntryByAncestorFee` struct + `ancestor_score` tag + multi_index 声明 | `src/txmempool.h:368-387, 392, 562-565` |
| C.12 | 删 `MempoolAncestorIndexingTest` 测试（依赖 ancestor_score 索引） | `src/test/mempool_tests.cpp` |

**update_ancestor_state 真实调用点（5 处，文档之前说 6 错）**：
- line 151 `updateForDescendantsNL`（C.7 删）
- line 358 `updateEntryForAncestorsNL`（C.5 删整函数）
- line 391 `updateForRemoveFromMempoolNL`（C.6 删整 if 块）
- line 1018 RemoveForBlockNL issue #12 fix（C.8 删）
- line 1596 prioritisetransaction（C.9 删）

`updateAncestorsOfNL` (line 330-345) **不调 `update_ancestor_state`**（只调 `update_descendant_state` line 342，保留），全函数保留。

**合并 Phase 1+2 工作量：6h 编码 + 5h 测试**

**收益**：链 500 入池 73ms → ~5ms（核心性能修复）。

**行为差异**：
- `prioritisetransaction` RPC 不再调整 descendants 的 ancestor fee 聚合（删 line 1596）
  - 实际影响 0：JournalingBlockAssembler 不依赖 ancestor fee 聚合做 mining 排序（直接用 entry 的 mModifiedFee）
  - prioritise 直接改 entry 的 mModifiedFee 仍生效

### Phase 4 — RPC 字段清理 + 文档 + dead config 文档化

**重要修正**：审计后发现 `entryToJSONNL` + `EntryDescriptionString` 是**共享 helper**，**4 个 RPC 都受影响**：
- `getrawmempool` (verbose=true) — line 349 调 entryToJSONNL
- `getmempoolancestors` (verbose=true) — line 491
- `getmempooldescendants` (verbose=true) — line 563
- `getmempoolentry` — line 604

实际改动只 2 处（共享 helper），但 breaking change 范围是 **4 个 RPC**。

| 任务 | 改动 |
|---|---|
| 4.1 | 删 `src/rpc/blockchain.cpp:315-318` `entryToJSONNL` 中 `ancestorcount/ancestorsize/ancestorfees` 3 行（影响 4 个 RPC 的 verbose 输出） |
| 4.2 | 删 `src/rpc/blockchain.cpp:290-295` `EntryDescriptionString` 中 ancestor 字段 6 行 doc 描述 |
| 4.3 | 改 `test/functional/mempool_packages.py:104-112` 删 `ancestorfees` 断言段（这测试用 getrawmempool 验 ancestorfees） |
| 4.4 | `doc/release-notes-v3.3.0.md` 加 ancestor cleanup 章节 + 4 RPC breaking change 列表 + dead config 文档化 |
| 4.5 | 跑 `bsv-mempool_ancestorsizelimit.py` 确认现状（疑似僵尸测试，验 dead config）。如长期 fail → 删测试 + release-notes 列；如 pass → 文档化此 false positive |

**Phase 4 工作量：1.5h 编码 + 1.5h 文档/调研**

**RPC breaking changes**（release-notes 必列）：
- `getrawmempool true` 输出移除 `ancestorcount/ancestorsize/ancestorfees` 字段
- `getmempoolentry` 输出移除同上 3 字段
- `getmempoolancestors true` 输出移除同上 3 字段（每个 ancestor 的 verbose 信息中）
- `getmempooldescendants true` 输出移除同上 3 字段（每个 descendant 的 verbose 信息中）

**dead config 文档化**：
- `-limitancestorsize`、`-limitdescendantcount`、`-limitdescendantsize` 自 TBC ccf31423c 起 `CalculateMemPoolAncestorsNL` 不再使用（只看 ancestorsHeight vs limitAncestorCount）
- 实际删除 API 签名 / CLI 选项注销是 separate cleanup，不在本次范围（外部 caller validation.cpp:1376, 1869 / wallet.cpp:3057 / 测试 仍传实参，删 API 需级联改）

## 总工作量（修正排序后）

| Phase | 顺序 | 编码 | 测试 | 累计 |
|---|---|---|---|---|
| 0（含 0.6 perf baseline 验证） | 1 | 1.5h | 6h | 7.5h |
| 3（提前） | 2 | 2h | 1h | 11h |
| 1+2（合并，含 B.5/B.6 风险消除 + snapshot sort 改造） | 3 | 8h | 7h | 26h |
| 4 | 4 | 1.5h | 1.5h | 28.5h |

**净 diff 估计：-1300 行 / +80 行 = 净删 1220 行**

**比初版工作量降 4h**：合并 Phase 1+2 省去重复测试时间。

## 决策点（执行前确认）

| # | 决策 | 推荐 | 备选 |
|---|---|---|---|
| 1 | `mempool_packages.py` 中 `ancestorfees` 断言怎么处理 | 删断言段（TBC 不再保证 fee 聚合） | 重算（违背删字段初衷） |
| 2 | `BlockAssemblerType::LEGACY` enum value | 保 enum value，factory throw（明确报错让运维知道） | 删 enum value 静默升级到 JOURNALING |
| 3 | `AncestorDescendantCounts` struct 怎么改 | 保 struct 名，加 `atomic<size_t> ancestorsHeight`（journal 跨模块共享需要，diff 最小，API 不变） | 改名 `MempoolEntryRefCounters`（语义更准但 diff 大） |
| 4 | 跟 v2.6.1 cs_main refactor PR 的关系 | **独立 PR，等 cs_main PR merge 后再开** | 合并到 cs_main PR（review 不可读） |

## 风险评估 + 完全消除方案

### Risk 1 (HIGH) — UpdateAncestorsHeightNL 性能压力 → **消除**

**消除手段**：算法分析 + perf baseline 对比，**不加任何硬上限**。

1. **算法复杂度分析**：
   - 每 entry 处理 = `GetMemPoolChildrenNL` (O(1)) + `GetMemPoolParentsNL` (O(直接父数量)) + `mapTx.modify(SetAncestorsHeight)` (Phase 2 删 ancestor_score 索引后 ancestorsHeight 不是任何 multi_index key → **O(1) 无 reindex**)
   - 总代价：受影响 entries × O(1)
   - 上限：天然 bounded by mempool 容量（`-maxmempool` 已限制）
2. **perf baseline 对比**（Phase 0.6）：
   - 同 reorg 场景测改前 vs 改后 `UpdateTransactionsFromBlock` 耗时
   - **acceptance**: 改后 ≤ 改前 × 1.5（增量 ≤ 50%，因为多了一次 BFS children 集合）
   - 超阈值 → **查算法 bug 或 worst case 比预期糟，不是加限制**
3. **不加硬上限的理由**：
   - mempool 满 + 全跨 reorg 长链是合法场景（含恶意攻击场景）
   - 加 `assert(false)` 会让节点 crash → 给攻击者一个 DoS trigger
   - 现有代码没限制，加进去是 silent behavior change

### Risk 2 (HIGH) — Sort tie-break 行为变 → **消除**

**消除手段**：
1. **保 sort key 形状不变**：
   - 现状：`(nCountWithAncestors, score)` 复合
   - 改后：`(ancestorsHeight, score)` 复合
   - 第二维 score tie-break **完全保留**（policy 一致）
   - 拓扑序保证：父 height < 子 height（数学严格成立）
2. **Phase 1+2.13 (新增)：sort 输出 diff 验证**
   - 旧 binary stress dump 100× `getrawmempool true` JSON
   - 新 binary 同样 stress dump
   - **acceptance**：tx 集合相等 + 字段值相等 + 仅允许"顺序差"
3. **Phase 1+2.14 (新增)：reorg 拓扑 fuzz**
   - 50× 随机 reorg（深度 1-10）× mempool 长链 50
   - 每次后验 `assert(ancestorsHeight invariant)` + journal 拓扑序合法
   - **0 tolerance** — 任何 fail block ship

### Risk 3 (MED) — atomic shared_ptr 跨锁 sort race → **消除**

**消除手段**：journal_change_set REORG sort 改两阶段（snapshot-based）：

```cpp
// Phase 1: snapshot atomic.load() 到 local immutable vector
struct Indexed { size_t height; Amount fee; uint64_t size; size_t orig_idx; };
std::vector<Indexed> snap;
for (size_t i = 0; i < mChangeSet.size(); ++i) {
    const auto& count = mChangeSet[i].second.getAncestorCount();
    snap.push_back({
        count->ancestorsHeight.load(std::memory_order_acquire),
        mChangeSet[i].second.getFee(),
        /*tx size from journal entry*/,
        i
    });
}
// Phase 2: sort snapshot (all values immutable, < transitive 数学保证)
std::stable_sort(snap.begin(), snap.end(), [](const Indexed& a, const Indexed& b){
    if (a.height != b.height) return a.height < b.height;
    // score tie-break：跟 CompareTxMemPoolEntryByScore 同公式
    return a.fee.GetSatoshis() * b.size < b.fee.GetSatoshis() * a.size;
});
// Phase 3: rebuild mChangeSet by snap[i].orig_idx
```

**消除原理**：
- atomic.load() 一次性 copy 到 local size_t，sort 读 local 不可变值
- `std::stable_sort` 数学要求 `<` 严格弱序（包括传递性）— local 值不变 ⇒ 数学保证
- 即使 reorg 中段 atomic 改值，sort 按 snapshot 跑完，**0 race**

`topoSortedTxFromSet` (Phase B.4) 同样模式改造。`DepthAndScoreComparator` (Phase B.3) **不需要** — 它在 `mempool.smtx` shared_lock 下跑，写者拿 unique_lock 阻塞，天然安全。

### 其他 Risk（保留 mitigation）

| 等级 | 项 | 缓解 |
|---|---|---|
| MED | Phase 2 删 cached 后 `Check()` invariant 部分断言失效 | C.10 改为只保 ancestorsHeight invariant |
| MED | Phase 4 4 RPC breaking change | release-notes 明列 |
| LOW | Phase 3 用户配置 `-blockassembler=LEGACY` 节点起不来 | factory throw 明确错误信息 |

## 全树扫描结果（穷尽审计）

通过 grep 全部相关符号（`nXxxWithAncestors`/`update_ancestor_state`/`ancestor_score`/`AncestorDescendantCounts`/`LegacyBlockAssembler`/`mining/legacy`），**确认涉及文件 16 个**：

| 模块 | 文件 | Phase |
|---|---|---|
| mempool 核心 | `src/txmempool.{h,cpp}` | 0+1+2 |
| mining journal | `src/mining/journal_change_set.cpp` | 1+2 (B.1) |
| mining journal | `src/mining/journal_entry.h` | 1+2 (A) 间接 |
| mining legacy 整删 | `src/mining/legacy.{h,cpp}` | 3 |
| mining factory | `src/mining/factory.{h,cpp}` | 3 |
| RPC | `src/rpc/blockchain.cpp` | 4 |
| build | `src/CMakeLists.txt` | 3 |
| init | `src/init.cpp` | 3（仅 LEGACY 路径，line 1908 `SetLimitAncestorCount` **留**） |
| 测试 mempool | `src/test/mempool_tests.cpp` | 1+2 + 3 |
| 测试 miner | `src/test/miner_tests.cpp` | 3 |
| 测试 journal | `src/test/journal_tests.cpp` | 1+2 (A.5) |
| 测试 txvalidationcache | `src/test/txvalidationcache_tests.cpp` | 3 |

**未触及的模块（全树 grep 0 命中）**：
- ZMQ (`src/zmq/`)、P2P (`src/net/`)、HTTP/RPC transport (`src/http*.cpp`)
- bench (`src/bench/`)、script (`src/script/`)、consensus (`src/consensus/`)、chainparams
- 钱包 wallet 主体（仅 `wallet/wallet.cpp` 传 dead config 实参，0 改动；`wallet/test/` 也 0 命中）

**`CJournalEntry` 跨模块消费方审计（共 4 处，只 1 处真读 ancestor 字段）**：

| caller | 位置 | 读 `getAncestorCount()`？ | Phase 影响 |
|---|---|---|---|
| `journal_change_set.cpp:118-119` REORG sort | journal sort | ✓ 读 | B.1 改读 `ancestorsHeight` |
| `journaling_block_assembler.cpp:331` | 矿工出块迭代 | ✗ 只读 tx/fee/sigops | 0 改 |
| `journal.cpp:252,259` invariant 检查 | journal 拓扑测试 | ✗ 只比较 tx id | 0 改 |
| `journal_tests.cpp` 多处 | 测试构造 | 仅构造 | A.5 改 ctor 实参 |

## 并发 / 锁分析（atomic shared_ptr 跨锁安全性）

Phase A 改 `AncestorDescendantCounts.ancestorsHeight` 为 `atomic<size_t>` 后：
- **写者**：mempool entry 入池路径（`addUnchecked` line 591）+ Phase 0 修复路径（`UpdateTransactionsFromBlock` 末尾调 `UpdateAncestorsHeightNL`）+ 现存 `RemoveForBlock` 路径（line 1076）。全在 `mempool.smtx` `unique_lock` 下。
- **读者**：journal sort（`journal_change_set.cpp:120` 在自己的 `mMtx` 下，**不持** mempool smtx）。

**结论**：跨锁但 atomic load/store 保证不撕裂（C++ `memory_order_seq_cst` 默认）。journal sort 读到的 height 是 mempool 中**当前**值（shared_ptr 共享更新），跟现状用 `nCountWithAncestors` 一致 — 这是设计意图（reorg 后 journal 看到 mempool 最新拓扑）。

**确认 0 LEGACY caller**：grep `TestingSetup.*LEGACY` + `blockassembler.*LEGACY` 全树 0 hit。Phase 3 删 LegacyBlockAssembler **0 真依赖**。

## 功能测试 `-limit*` 配置全分布（确认 0 破坏）

| 测试 | 用法 | 删字段后 |
|---|---|---|
| `abc-p2p-compactblocks.py:65-66` | `-limitancestorcount=999999 -limitancestorsize=999999`（无限） | ✓ 无影响 |
| `pruning.py:39` | 4 个 limit 全用，测试核心不依赖 size 生效 | ✓ 无影响 |
| `bsv-ptv-txn-chains.py:119,132` | `-limitancestorcount + -limitdescendantcount` | ✓ 无影响 |
| `bsv-ptv-rpc-sendrawtransactions.py:270-416` | `-limitancestorcount=100` | ✓ 无影响 |
| `wallet.py:387-389, 446` | `-limitancestorcount + -walletrejectlongchains` | ✓ wallet 走 `-walletrejectlongchains` 路径 |
| `bsv-ptv-p2p.py:255` | `-limitancestorcount=50` | ✓ 无影响 |
| `bsv-mempool_ancestorheightlimit.py` | TBC ccf31423c 关键回归测试（验 height limit） | ✓ **留**，关键 |
| `bsv-mempool_ancestorsizelimit.py` | 整个测试验 size limit 生效 | ⚠ **僵尸测试**（自 ccf31423c 起 size limit 已 dead），Phase 4.5 处理 |
| `mempool_packages.py:108` | `assert_equal(mempool[x]['ancestorfees'], ...)` | ⚠ Phase 4.3 改 |
| `test_mempool_recursive_descendant_removal.py:121-122` | `entry.get('ancestorfees', 'N/A')` 兜底 | ✓ 输出 'N/A'，**不断言失败**，无需改测试 |
| `bsv-highsigopsdensitymempool.py:18` | `-blockassembler=JOURNALING` 显式 | ✓ 无影响 |
| `dbcrash.py` (only -limit ref) | `-limitdescendantsize`（已 dead） | ✓ 无影响 |

## RPC 影响清单（审计后修正）

| RPC | verbose 模式 | 受影响字段 | 调用 helper |
|---|---|---|---|
| `getrawmempool` | `true` | ancestorcount/ancestorsize/ancestorfees 删除 | entryToJSONNL |
| `getmempoolentry` | 一直 verbose | 同上 | entryToJSONNL |
| `getmempoolancestors` | `true` | 每个 ancestor 的 verbose 信息中删除 | entryToJSONNL |
| `getmempooldescendants` | `true` | 每个 descendant 的 verbose 信息中删除 | entryToJSONNL |

**保留**（descendant 字段不动）：descendantcount / descendantsize / descendantfees。

## 跨模块依赖矩阵（审计后整理）

| 改动 | 影响的模块 | 文件 |
|---|---|---|
| 删 `nCountWithAncestors` | mempool + mining | `txmempool.{h,cpp}`, `journal_change_set.cpp`, `journal_entry.h`, `journal_tests.cpp` |
| 删 `nSize/nModFees/nSigOpCount WithAncestors` | mempool + RPC + functional test | `txmempool.{h,cpp}`, `rpc/blockchain.cpp`, `mempool_packages.py` |
| 删 `update_ancestor_state` struct | mempool 内部 | `txmempool.{h,cpp}`（含 prioritisetransaction 路径） |
| 删 `ancestor_score` multi_index | mempool + mining | `txmempool.h`, `mining/legacy.{h,cpp}`（连带删整 LegacyBlockAssembler） |
| 加共享 `ancestorsHeight` 进 struct | mempool + mining | `txmempool.{h,cpp}`, `journal_change_set.cpp`, `journal_entry.h`（API 不变，内部改） |
| 修 `UpdateTransactionsFromBlock` 加 height 刷新 | mempool 内部 | `txmempool.cpp:182-238`（Phase 0） |
| 删 LegacyBlockAssembler | mining + init + test + build | `mining/legacy.{h,cpp}`, `mining/factory.{h,cpp}`, `init.cpp`, `CMakeLists.txt`, `test/{miner,txvalidationcache}_tests.cpp` |
| RPC `getmempoolentry` 删 ancestor 字段 | RPC + 功能测试 | `rpc/blockchain.cpp`, `test/functional/mempool_packages.py` |

**未触及的 cached 字段（保留）**：
- `nCountWithDescendants` - 在共享 struct 中保留
- `nSizeWithDescendants` - mempool entry 直接成员，TrimToSize/eviction 用
- `nModFeesWithDescendants` - 同上
- `descendant_score` multi_index - `txmempool.cpp:2175` TrimToSize 实路径用
- `update_descendant_state` struct - 维护 3 个 descendant cached 用，独立于 `update_ancestor_state`

## 验证清单（每 Phase 结束跑）

| 项 | 命令 |
|---|---|
| 全套单测 | `cmake --build build --target check -j$(nproc)` |
| 关键功能测试 | `test/functional/mempool_reorg.py`、`test/functional/mempool_packages.py`、`test/functional/bsv-mempool_ancestorheightlimit.py`、`test/functional/abc-p2p-compactblocks.py` |
| dev 节点 soak | 24h 跑 + 监控 ERROR / ConnectBlock fail / queue full |
| 性能验证 | 链深 500 入池压测，验 73ms → ~5ms |
| `-checkmempool=1` invariant | 跑 reorg 测试 in `-checkmempool=1` 模式，无 assert 炸 |

## Baseline 改变全清单（release-notes 草稿源）

下列是用户可感知或可能感知的行为变化。每项必须在 `doc/release-notes-v3.3.0.md` 中文档化。

### 用户可感知（HIGH / MED）

| # | 等级 | 改变 | Phase |
|---|---|---|---|
| A | HIGH | **reorg 后 chain depth limit 检查变严**：当前 prod reorg 后 children 的 height stale 偏小，limit 检查偏松；Phase 0 修复后 height 准确，limit 严格。用户可能在 reorg 后某些 tx submission 突然被 reject (error: `too many unconfirmed ancestors`) | 0 |
| B | HIGH | **4 个 RPC 输出删 ancestor 字段**（getrawmempool true / getmempoolentry / getmempoolancestors true / getmempooldescendants true）— 删 ancestorcount/ancestorsize/ancestorfees | 1+2, 4 |
| C | MED | **getrawmempool 输出 tx 顺序变**：sort key 从 `(nCountWithAncestors, score)` → `(ancestorsHeight, score)`，外部脚本若依赖顺序稳定性会感知 | 1+2 |
| D | MED | **reorg 后 journal sort 兄弟/无关 tx 间顺序变**：父子拓扑序数学严格保留（父 height < 子 height 严格成立），仅兄弟 tx + 无关 tx 之间的相对位置可能不同（同 height 内按 score tie-break）。不影响 block 合法性 / consensus / 矿工 ConnectBlock 通过 | 1+2 |
| E | HIGH（但 0 真用户）| **`-blockassembler=LEGACY` 节点启动失败**：factory throw 替代之前的静默 fallback。全树 grep 0 真 caller，但 release-notes 必列 | 3 |

### 仅开发者感知（LOW）

| # | 等级 | 改变 |
|---|---|---|
| F | LOW | `-checkmempool=1` 模式 invariant 覆盖度变化：删 4 个 ancestor cached aggregate invariant；保 3 个 descendant cached invariant；新增 1 个 ancestorsHeight invariant（Phase 0 修复后稳定） |

### 0 用户感知（INFO，不必 release-notes）

| 改变 | 用户感知 |
|---|---|
| `prioritisetransaction` 不再调整 descendants 的 nModFeesWithAncestors | 字段被删，用户看不见 |
| `bool updateDescendants` 参数移除 | 内部 API |
| `update_ancestor_state` struct 删除 | 内部 |
| `ancestorsHeight` 字段从独立成员搬进共享 struct | API 不变 |
| 入池 / RemoveForBlock / Phase 0 reorg 路径 atomic load/store overhead | per-op ns 级，prod 不可感知 |
| `bsv-mempool_ancestorsizelimit.py` 僵尸测试可能删除 | 已是 dead 状态 |
| journal_change_set / topoSortedTxFromSet sort 改两阶段 snapshot 实现（消除 atomic race）| 内部实现，行为不变 |

## Out-of-scope（留 future work）

- BSV master Step 4-5：CPFPGroup + Primary/Secondary mempool + EvictionCandidateTracker（架构改写，3-5 天）
- 删 3 个 descendant cached aggregate 字段（保留，evict 路径还在用）
- 删 `descendant_score` multi_index（同上）
- mempool `multi_index` 主索引降级（BSV master 标 `// FIXME: DEPRECATED`，TBC 当前架构未触及此重写）
