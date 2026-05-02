// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P1.1: Chainstate::UpdateTip 生产入口实现
//
// 依赖 CBlockIndex 完整声明（chain.h），所以放 .cpp（chainstate.h 只前向声明）

#include "validation/chainstate.h"

#include "chain.h"     // CBlockIndex
#include "logging.h"   // LogPrintf
#include "sync.h"      // CCriticalSection / cs_main

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <thread>

extern CCriticalSection cs_main;

namespace tbc {
namespace validation {

// v2.6.1 C4 修补（设计 P0.0a.5 + H-F 备选）：
//   F2 单写者守卫从"cs_main.try_lock"改 atomic<thread::id> writer_owner CAS。
//   原 try_lock 在 boost::recursive_mutex 上"无人持锁"也成功 → 守卫失效。
//
//   语义：
//   - 首次进入：CAS expected=空 -> desired=self → 成功标记 self 为 writer
//   - 嵌套进入（如 PublishTipEarly 内调 UpdateTip）：CAS 失败但 owner==self → 放行，
//     by_self_count++；嵌套层退出不写回空（外层退出统一清）
//   - 跨线程并发：CAS 失败且 owner != self → abort（违反单写者协议）
static std::atomic<std::thread::id> g_chainstate_writer_owner{ std::thread::id{} };
static thread_local int g_chainstate_writer_depth = 0;

// L3 (post-Teranode-audit)：WriterGuard 嵌套 depth 跟 owner CAS 协议设计注意：
//   - g_chainstate_writer_depth 是 thread_local，仅当前 writer 线程见到 — 嵌套层
//     析构 (--depth) 不会影响 owner，正确。
//   - outermost 析构时清 owner，让下一波 writer 能重新 CAS 进入。
//   - 异常路径：UpdateTip 内若抛异常，WriterGuard 析构正确清 depth 跟 owner（RAII）。
//   - 严格前提：UpdateTip 调用方持 cs_main 排他，所以单线程进入；G 若未来出现
//     多 writer 并发（如 ChainstateManager 化），这套 CAS 直接 abort 起到保护作用。
class WriterGuard {
public:
    WriterGuard() {
        const std::thread::id self = std::this_thread::get_id();
        std::thread::id expected{};
        if (g_chainstate_writer_owner.compare_exchange_strong(
                expected, self,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            // 首次进入：是 writer
            outermost = true;
            g_chainstate_writer_depth = 1;
            return;
        }
        // CAS 失败：owner 是别人（expected 已被填回当前 owner）
        if (expected == self) {
            // 嵌套调用：放行
            outermost = false;
            ++g_chainstate_writer_depth;
            return;
        }
        // 跨线程并发 → 严重违反单写者协议
        std::ostringstream oss;
        oss << "FATAL: Chainstate::UpdateTip called concurrently. "
            << "current_owner=" << expected
            << " self=" << self;
        LogPrintf("%s\n", oss.str());
        fprintf(stderr, "%s\n", oss.str().c_str());
        std::abort();
    }
    ~WriterGuard() {
        if (outermost) {
            g_chainstate_writer_depth = 0;
            g_chainstate_writer_owner.store(std::thread::id{},
                                            std::memory_order_release);
        } else {
            --g_chainstate_writer_depth;
        }
    }
    WriterGuard(const WriterGuard&) = delete;
    WriterGuard& operator=(const WriterGuard&) = delete;
private:
    bool outermost = false;
};

void Chainstate::UpdateTip(const CBlockIndex* new_tip,
                           uint32_t script_flags,
                           int32_t genesisActivationHeight_,
                           bool isGenesisEnabled_)
{
    WriterGuard guard;   // C4: atomic<thread::id> CAS 单写者保护

    if (new_tip == nullptr) {
        // 允许 nullptr：Shutdown 路径或 reindex 早期
        seq.fetch_add(1, std::memory_order_release);
        std::atomic_thread_fence(std::memory_order_release);
        m_tip_hash_w0.store(0, std::memory_order_relaxed);
        m_tip_hash_w1.store(0, std::memory_order_relaxed);
        m_tip_hash_w2.store(0, std::memory_order_relaxed);
        m_tip_hash_w3.store(0, std::memory_order_relaxed);
        m_tip_index.store(nullptr, std::memory_order_relaxed);
        m_script_flags.store(0, std::memory_order_relaxed);
        m_height.store(0, std::memory_order_relaxed);
        m_mtp.store(0, std::memory_order_relaxed);
        m_isGenesisEnabled.store(false, std::memory_order_relaxed);
        m_genesisActivationHeight.store(genesisActivationHeight_, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        seq.fetch_add(1, std::memory_order_release);
        return;
    }

    // writer 协议（K1 完整 fence）
    seq.fetch_add(1, std::memory_order_release);            // 进奇数（写中）
    std::atomic_thread_fence(std::memory_order_release);    // K1

    uint256 hash = new_tip->GetBlockHash();
    uint64_t w[4];
    std::memcpy(w, &hash, sizeof(uint256));
    m_tip_hash_w0.store(w[0], std::memory_order_relaxed);
    m_tip_hash_w1.store(w[1], std::memory_order_relaxed);
    m_tip_hash_w2.store(w[2], std::memory_order_relaxed);
    m_tip_hash_w3.store(w[3], std::memory_order_relaxed);
    m_tip_index.store(new_tip, std::memory_order_relaxed);
    m_script_flags.store(script_flags, std::memory_order_relaxed);
    m_height.store(new_tip->nHeight, std::memory_order_relaxed);
    m_mtp.store(new_tip->GetMedianTimePast(), std::memory_order_relaxed);
    m_isGenesisEnabled.store(isGenesisEnabled_, std::memory_order_relaxed);
    m_genesisActivationHeight.store(genesisActivationHeight_, std::memory_order_relaxed);

    std::atomic_thread_fence(std::memory_order_release);    // K1
    seq.fetch_add(1, std::memory_order_release);            // 退偶数（稳定）
}

// 全局 Chainstate 实例（v2.6.1 P1.1 引入；P4.1 接入 ConnectBlock）
Chainstate g_chainstate;

// v2.6.1 P4.1 (架构 C-2)：reorg epoch counter（偶数=稳定，奇数=半态）
std::atomic<uint64_t> g_reorg_epoch{0};
// sub-A4 review v2 CRITICAL (task #238)：嵌套 reorg guard depth
std::atomic<uint32_t> g_reorg_nesting_depth{0};

} // namespace validation
} // namespace tbc
