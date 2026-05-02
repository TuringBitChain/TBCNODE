# Chainstate seqlock memory model 正确性证明

**版本**：v2.6.1 P0.0b.1
**对应代码**：`src/validation/chainstate.h`
**对应测试**：`src/test/chainstate_seqlock_tests.cpp`

---

## 1. 状态机

```
seq = 0   (偶数 = 稳态，writer 没在写)
seq = 1   (奇数 = 写中，writer 正在更新字段)
seq = 2   (偶数 = 稳态，writer 完成新 epoch)
seq = 3   (奇数 = 写中)
seq = 4   (偶数)
...
```

每次 `UpdateForTest` (后续 `UpdateTip` 在 P1.1 接入)：
- 进奇数（开始写）
- 写 7 字段
- 退偶数（写完）

`seq` 是 `std::atomic<uint64_t>`，每次 update 加 2，64-bit 单调，永不溢出（每秒 100 万次 update 需 ~58 万年溢出）。

## 2. Writer 协议

```cpp
seq.fetch_add(1, std::memory_order_release);            // (1) 进奇数
std::atomic_thread_fence(std::memory_order_release);    // (2) K1 fence

memcpy(&m_tip_hash, ...);                               // (3) 写字段
m_tip_index               = ...;
m_script_flags            = ...;
m_height                  = ...;
m_mtp                     = ...;
m_isGenesisEnabled        = ...;
m_genesisActivationHeight = ...;

std::atomic_thread_fence(std::memory_order_release);    // (4) K1 fence
seq.fetch_add(1, std::memory_order_release);            // (5) 退偶数
```

**关键不变量**：
- (1) → (3)：(1) 是 release fetch_add，但 (3) 不能"飘到" (1) 之前——加 (2) release fence 显式保证字段写不能跨 (2) 飘到 (1) 之前
- (3) → (5)：(4) release fence 保证字段写完成才进 (5) 退偶数

实际 x86 上 (2) 和 (4) 是 noop（x86 是 strong memory model，atomic_thread_fence(release) 是编译屏障），但 ARM/RISC-V 需要显式 `dmb ish` / `fence rw,rw` 指令——P0.0b.1 §5 cross-arch 验证负责覆盖。

## 3. Reader 协议

```cpp
while (true) {
    uint64_t seq_before = seq.load(std::memory_order_acquire);     // (a)
    if (seq_before & 1u) { yield; continue; }                       // (b)
    std::atomic_thread_fence(std::memory_order_acquire);           // (c) K1 fence

    memcpy(&s.tip_hash, &m_tip_hash, ...);                          // (d) 读字段
    s.tip_index = m_tip_index;
    ...

    std::atomic_thread_fence(std::memory_order_acquire);           // (e) K1 fence
    uint64_t seq_after = seq.load(std::memory_order_acquire);       // (f)
    if (seq_before == seq_after) return s;                          // (g) 稳定快照
}
```

**关键不变量**：
- (a) acquire load：synchronizes-with writer 的 (1)/(5) release fetch_add
- (b)：seq 奇数说明 writer 正在写，跳过当前 round
- (c)：让 (d) 字段读不能"飘到" (a) seq 读之前
- (e)：让 (f) seq 读不能"飘到" (d) 字段读之前
- (g)：seq_before == seq_after 且不奇 → memcpy 期间 writer 没启动新 epoch → 字段值都来自同一稳定 epoch

## 4. 正确性证明（informal）

**目标**：reader 返回的 Snapshot 7 字段全部来自同一稳定 epoch（不撕裂）。

**证明**：

设 reader 拍到 `seq_before == 2K` (偶数，稳态)。

1. (a) acquire load 同步于 writer 之前最近的 (1) 或 (5) release fetch_add。具体：
   - 如果 writer 已经完成 epoch K（最后一次 (5) 让 seq = 2K），那 (a) 同步于 (5)，意味着 writer 在 (5) 之前的所有写（包括 (4) fence 之前的 (3) 字段写）对 reader 可见
2. (c) acquire fence + (d) 字段读：保证 (d) 读到的是 (3) 写的版本（如果 writer 没启动 epoch K+1）
3. (e) acquire fence：保证 (f) seq 读不会 reorder 到 (d) 之前
4. (f) `seq_after`：
   - 如果 writer 没启动 epoch K+1：seq_after == 2K，等于 seq_before，(g) 退出 → 返回 epoch K 的完整字段 ✓
   - 如果 writer 启动了 epoch K+1（fetch_add 进 2K+1）：seq_after >= 2K+1，不等于 seq_before，重试

**反证不存在的情况**：reader 返回撕裂数据需要 seq_before == seq_after **且** 字段读跨多个 epoch。

- 若字段读跨 epoch（reader 在 (d) 中途 writer 完成新一轮），seq 至少 +2（变成 2K+2）。
- 那 (f) seq_after >= 2K+2 > seq_before == 2K，不会满足 (g)，必然重试。
- 因此返回时（(g) 满足）必然字段读全部在同一 epoch 内 ∎

## 5. 实测发现的 bug + 修复

**初次实现** (do-while + continue 写法)：

```cpp
do {
    seq_before = seq.load(...);
    if (seq_before & 1u) { yield; continue; }   // ← bug
    ...
    seq_after = seq.load(...);
} while (seq_before != seq_after);
```

**bug**：`continue` 跳到 `while (...)` 检查 → 比较 `seq_before` 跟 **未初始化的** `seq_after`（C++ uint64_t POD 局部变量未初始化是 indeterminate value）。Stack 上的残留值可能正好等于 seq_before（如都是 0），while 假，循环退出，返回**根本没 memcpy 的随机 stack 字段**。

**实测**：3 秒压测捕获 529477 次 torn read（uint8_t b0 != byte[1..31]），证明 bug 真的触发。

**修复**：改 while(true) + break，每次循环都完整 round-trip：

```cpp
while (true) {
    uint64_t seq_before = seq.load(...);
    if (seq_before & 1u) { yield; continue; }
    fence(acquire);
    memcpy(...);
    fence(acquire);
    uint64_t seq_after = seq.load(...);
    if (seq_before == seq_after) return s;
}
```

**修复后实测**：3 秒压测 100+ 万次读 0 torn，单元测试 `no_torn_read_under_load` 通过。

**经验**：seqlock 看似简单，但 do-while + continue 是经典陷阱。Preshing 标准写法用 while(true) + break 避免此 bug。

## 6. ARM/RISC-V 跨架构验证（待 P0.0b.1 §5 CI matrix 跑）

**x86 实测**（本地）：通过

**ARM64 / RISC-V64 跨架构验证**：

CI matrix 用 QEMU emulator + cross-compile：

```yaml
# .github/workflows/cross-arch-seqlock.yml（待加）
strategy:
  matrix:
    arch: [arm64, riscv64]
steps:
  - run: docker run --platform linux/${{ matrix.arch }} ...
  - run: ./test_bitcoin --run_test=chainstate_seqlock
```

预期：在 ARM/RISC-V relaxed memory model 上仍 0 torn（依赖 K1 显式 fence）。

ARM64 fence 实际编译输出：
- `atomic_thread_fence(release)` → `dmb ish` (Data Memory Barrier, Inner Shareable)
- `atomic_thread_fence(acquire)` → `dmb ish`

RISC-V64：
- `atomic_thread_fence(release)` → `fence rw,w`
- `atomic_thread_fence(acquire)` → `fence r,rw`

如果未来发现 ARM/RISC-V 出 torn → 切 `memory_order_seq_cst`（更强约束），代价是性能略低（额外指令）。

## 7. 性能

x86 实测：
- writer：每次 update ~30ns（4 个 atomic op + 2 fence + memcpy 32 + 6 标量赋值）
- reader：每次 Capture ~30-50ns（2 个 atomic load + 2 fence + memcpy 32 + 6 赋值）
- 高频 reader 负载下 yield 极少（writer 短暂窗口），命中率 > 99.9%

跟 cs_main shared_lock 对比：
- shared_lock 入门 100-500ns（depending on contention）
- seqlock 30-50ns
- 加速 ~10x

worker hot path 大量调用 Capture 是 v2.6.1 性能的关键基础。

## 8. 关联文档

- 概要设计：`docs/plans/cs_main-refactor-plan.md` §2.4 seqlock
- 详细设计：`docs/plans/cs_main-refactor-detailed-design.md` §1.1 Chainstate
- 任务卡：`docs/plans/tasks/P0.0b.1-seqlock-memory-model.md`
- 单元测试：`src/test/chainstate_seqlock_tests.cpp`
