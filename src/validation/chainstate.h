// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P0.0b.1: Chainstate seqlock 元数据
//
// 设计目标：worker 验证路径不持 cs_main，但需要读 chain tip / script_flags / mtp 等
// 元数据。用 seqlock（单写者多读者无锁）替代 cs_main 持锁读。
//
// 单写者：UpdateTip 持 cs_main 排他时单线程更新（F2 守卫 + EXCLUSIVE_LOCKS_REQUIRED）
// 多读者：worker 持局部 Snapshot，~30ns 拍快照，验证完丢弃
//
// K1 内存模型：
//   writer：seq.fetch_add(1, release) + atomic_thread_fence(release) + 写字段 +
//           atomic_thread_fence(release) + seq.fetch_add(1, release)
//   reader：seq.load(acquire) + atomic_thread_fence(acquire) + memcpy 读字段 +
//           atomic_thread_fence(acquire) + seq.load(acquire); seq 不一致重试
//
// C-A: Snapshot 包含 tip_index（CBlockIndex*）给 CheckSequenceLocks 等需要遍历祖先用
//      不变量：mapBlockIndex 节点对象在 Active 阶段不被 erase（详 §1.1 注释）
// H-D: Snapshot 包含 genesisActivationHeight 给 worker 阶段 3 推 perInputScriptFlags
// F2:  UpdateTip 用 std::unique_lock<CCriticalSection>(cs_main, std::try_to_lock)
//      RAII 守卫，跨线程调用直接 abort（DEBUG_LOCKORDER 关闭时 release build 也启用）

#ifndef BITCOIN_VALIDATION_CHAINSTATE_H
#define BITCOIN_VALIDATION_CHAINSTATE_H

#include "uint256.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>

class CBlockIndex;

namespace tbc {
namespace validation {

class Chainstate {
public:
    struct Snapshot {
        uint256 tip_hash;
        const CBlockIndex* tip_index;
        uint32_t script_flags;
        int32_t height;
        int64_t mtp;
        bool isGenesisEnabled;
        int32_t genesisActivationHeight;
    };

    Chainstate() = default;

    // worker 读 API（无锁，标准 seqlock with explicit fences — Preshing pattern）
    //
    // 修复（P0.0b.1 实测发现）：原 do-while + continue 写法在 continue 路径上跳过
    // seq_after 赋值，导致 while 比较读未初始化值（UB），实测会出大量 torn read。
    // 改 while(true) + break：每次循环都完整 round-trip。
    //
    // H3 (post-Teranode-audit) 修：原非原子字段（裸指针 / POD）+ atomic_thread_fence
    //   是 C++ 内存模型 UB（fence 不保 non-atomic 字段并发读写）。x86/ARM 实测 OK
    //   但编译器可能 dead-store elimination / loop unswitching 破坏。
    //   修法：所有字段改 std::atomic<T>，relaxed load/store（seq fence 仍做总序）。
    //   uint256 (32 字节) 拆 4× atomic<uint64_t>，relaxed 4 次 load 拼回。
    Snapshot Capture() const noexcept {
        Snapshot s;
        while (true) {
            uint64_t seq_before = seq.load(std::memory_order_acquire);
            if (seq_before & 1u) {
                std::this_thread::yield();
                continue;   // writer 写中，重试
            }
            std::atomic_thread_fence(std::memory_order_acquire);   // K1

            // 全部 atomic relaxed load — fence 提供 ordering，relaxed 足够
            uint64_t w[4];
            w[0] = m_tip_hash_w0.load(std::memory_order_relaxed);
            w[1] = m_tip_hash_w1.load(std::memory_order_relaxed);
            w[2] = m_tip_hash_w2.load(std::memory_order_relaxed);
            w[3] = m_tip_hash_w3.load(std::memory_order_relaxed);
            std::memcpy(&s.tip_hash, w, sizeof(uint256));
            s.tip_index               = m_tip_index.load(std::memory_order_relaxed);
            s.script_flags            = m_script_flags.load(std::memory_order_relaxed);
            s.height                  = m_height.load(std::memory_order_relaxed);
            s.mtp                     = m_mtp.load(std::memory_order_relaxed);
            s.isGenesisEnabled        = m_isGenesisEnabled.load(std::memory_order_relaxed);
            s.genesisActivationHeight = m_genesisActivationHeight.load(std::memory_order_relaxed);

            std::atomic_thread_fence(std::memory_order_acquire);   // K1
            uint64_t seq_after = seq.load(std::memory_order_acquire);
            if (seq_before == seq_after) {
                return s;   // 稳定快照
            }
            // writer 在 load 中途启动新 epoch，重试
        }
    }

    // 生产入口：从 CBlockIndex 接入 + F2 单写者守卫（P1.1 接入 ConnectBlock 在 P4.1 按 C-B 锁次序）
    // 调用者必须持 cs_main 排他（编译期 EXCLUSIVE_LOCKS_REQUIRED 在 P4 全量 TSA 注解时加）
    // F2 运行时守卫：cs_main.try_lock() + 立即 unlock —— boost::recursive_mutex 同线程已持锁返回 true，
    //                跨线程返回 false → abort（生产 release build 也启用，不依赖 DEBUG_LOCKORDER）
    void UpdateTip(const CBlockIndex* new_tip,
                   uint32_t script_flags,
                   int32_t genesisActivationHeight,
                   bool isGenesisEnabled);   // 实现在 chainstate.cpp（依赖 CBlockIndex 完整声明）

    // 测试用 setter（无 F2 守卫，绕开 cs_main，给 P0.0b.1 单元测试用）
    void UpdateForTest(const uint256& tip_hash,
                       const CBlockIndex* tip_index,
                       uint32_t script_flags,
                       int32_t height,
                       int64_t mtp,
                       bool isGenesisEnabled,
                       int32_t genesisActivationHeight) noexcept {
        // writer 协议（K1 完整 fence）
        seq.fetch_add(1, std::memory_order_release);            // 进奇数（写中）
        std::atomic_thread_fence(std::memory_order_release);    // K1

        uint64_t w[4];
        std::memcpy(w, &tip_hash, sizeof(uint256));
        m_tip_hash_w0.store(w[0], std::memory_order_relaxed);
        m_tip_hash_w1.store(w[1], std::memory_order_relaxed);
        m_tip_hash_w2.store(w[2], std::memory_order_relaxed);
        m_tip_hash_w3.store(w[3], std::memory_order_relaxed);
        m_tip_index.store(tip_index, std::memory_order_relaxed);
        m_script_flags.store(script_flags, std::memory_order_relaxed);
        m_height.store(height, std::memory_order_relaxed);
        m_mtp.store(mtp, std::memory_order_relaxed);
        m_isGenesisEnabled.store(isGenesisEnabled, std::memory_order_relaxed);
        m_genesisActivationHeight.store(genesisActivationHeight, std::memory_order_relaxed);

        std::atomic_thread_fence(std::memory_order_release);    // K1
        seq.fetch_add(1, std::memory_order_release);            // 退偶数（稳定）
    }

    // 当前 seq 值（测试 / 调试用）
    uint64_t SeqForTest() const noexcept {
        return seq.load(std::memory_order_acquire);
    }

private:
    // seqlock：偶数=稳定，奇数=写中
    std::atomic<uint64_t> seq{0};

    // 7 字段（全部 atomic — H3 修：消除 C++ 内存模型 UB）
    //   uint256 拆 4× atomic<uint64_t>，relaxed read/write，seq fence 提供总序
    std::atomic<uint64_t> m_tip_hash_w0{0};
    std::atomic<uint64_t> m_tip_hash_w1{0};
    std::atomic<uint64_t> m_tip_hash_w2{0};
    std::atomic<uint64_t> m_tip_hash_w3{0};
    std::atomic<const CBlockIndex*> m_tip_index{nullptr};
    std::atomic<uint32_t> m_script_flags{0};
    std::atomic<int32_t> m_height{0};
    std::atomic<int64_t> m_mtp{0};
    std::atomic<bool> m_isGenesisEnabled{false};
    std::atomic<int32_t> m_genesisActivationHeight{0};

    // H3：所有 atomic 必须 lock-free，否则 seqlock 协议被序列化
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "Chainstate seqlock requires lock-free atomic<uint64_t>");
    static_assert(std::atomic<int64_t>::is_always_lock_free,
                  "Chainstate seqlock requires lock-free atomic<int64_t>");
    static_assert(std::atomic<const CBlockIndex*>::is_always_lock_free,
                  "Chainstate seqlock requires lock-free atomic<const CBlockIndex*>");
};

// 全局实例（v2.6.1 P1.1；定义在 chainstate.cpp）
// 必须在 namespace tbc::validation 内，跟 chainstate.cpp 定义匹配（否则 linker error）
extern Chainstate g_chainstate;

// v2.6.1 P4.1 (架构 C-2)：reorg epoch counter（同 seqlock pattern）
//   UpdateMempoolForReorg 进入时 fetch_add(1) → 进奇数（reorg-in-progress）
//   全部完成 + stash push 完后 fetch_add(1) → 退偶数（稳定）
//   worker 拿快照前/后各读一次：奇 → 半态需 retry；偶 + 不变 → 稳定一致
extern std::atomic<uint64_t> g_reorg_epoch;
// review v7 a5：seqlock 设计假设 atomic uint64 是 lock-free。32-bit ABI 上若 fallback
//   到 mutex 实现，整个 reorg-epoch 投票协议会被序列化拖死。编译期保证。
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "g_reorg_epoch requires lock-free std::atomic<uint64_t> (64-bit ABI)");

// sub-A4 review v2 CRITICAL (task #238)：reorg guard 嵌套深度计数。
//   ActivateBestChain 内层调 InvalidateBlock / UpdateMempoolForReorg 时双 guard 嵌套，
//   不能各自独立 fetch_add(1) — 否则 epoch 中间变偶数（"reorg done"）让 worker
//   提前结束阻塞。
//   方案：用 nesting_depth ref-count，只在 0→1 转换时抬 epoch 进奇数，N→0 转换时
//   退到偶数。中间嵌套保持 epoch 不变（始终奇数）。
//
// L4 (post-Teranode-audit)：正确性前提 — 所有 ActivateReorgEpochGuard /
//   InvalidateReorgEpochGuard 激活/析构都在 cs_main 持锁线程内（两者都
//   AssertLockHeld(cs_main)）。cs_main 是 boost::recursive_mutex，同线程多次 LOCK
//   合法，所以同线程嵌套 guard 时 depth ref-count 正确。
//   未来若引入多线程 ActivateBestChain，CAS 路径需要重新审视（当前单线程 cs_main
//   排他写入是隐式不变量）。
extern std::atomic<uint32_t> g_reorg_nesting_depth;

inline bool ReorgInProgress() noexcept {
    return (g_reorg_epoch.load(std::memory_order_acquire) & 1u) != 0;
}

} // namespace validation
} // namespace tbc

#endif // BITCOIN_VALIDATION_CHAINSTATE_H
