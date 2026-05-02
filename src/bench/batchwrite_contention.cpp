// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P4.3: BatchWrite contention benchmark
//
// 测试 CCoinsViewCache::Flush（持 batchWriteMtx unique）跟并发 GetCoin 读
//（持 batchWriteMtx shared）之间的延迟分布。模拟 v2.6.1 三锁帧实际场景：
//   主线程：周期性持 batchWriteMtx unique 写入 N 个 coin
//   N 个 reader 线程：循环 try_lock_shared 读 GetCoin
//
// 衡量指标：
//   - 主线程 BatchWrite 平均时长（持锁窗口）
//   - reader 平均 GetCoin 时长（受 BatchWrite 阻塞影响）

#include "bench.h"
#include "coins.h"
#include "random.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {

constexpr size_t COIN_BATCH_SIZE = 1024;     // 每次 BatchWrite 写 N 个 coin
constexpr size_t READER_THREADS  = 8;
constexpr int    BENCH_ITERATIONS = 16;       // BatchWrite 调用次数

// 给 BatchWrite 准备一批随机 outpoint+coin
static void FillCoinMap(CCoinsMap& m, FastRandomContext& rng) {
    for (size_t i = 0; i < COIN_BATCH_SIZE; ++i) {
        TxId t(rng.rand256());
        COutPoint op(t, static_cast<uint32_t>(i & 0xffff));
        CTxOut txout(Amount(1000), CScript() << OP_TRUE);
        Coin coin(std::move(txout), /*nHeightIn=*/1, /*IsCoinbase=*/false);
        CCoinsCacheEntry entry;
        entry.coin = std::move(coin);
        entry.flags = CCoinsCacheEntry::DIRTY;
        m.emplace(op, std::move(entry));
    }
}

} // namespace

static void BatchWriteContention(benchmark::State& state) {
    // backing：用 CCoinsViewEmpty 做最小 stub（C-6 后接受 backing 必须显式实现 NoLock）
    CCoinsViewEmpty backing;
    CCoinsViewCache cache(&backing);

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> reader_ops{0};

    // 启 N 个 reader thread 模拟 worker GetCoin
    std::vector<std::thread> readers;
    readers.reserve(READER_THREADS);
    for (size_t i = 0; i < READER_THREADS; ++i) {
        readers.emplace_back([&]() {
            FastRandomContext rng;
            while (!stop.load(std::memory_order_acquire)) {
                COutPoint op(TxId(rng.rand256()), 0);
                Coin coin;
                (void)cache.GetCoin(op, coin);
                reader_ops.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    FastRandomContext rng;

    while (state.KeepRunning()) {
        for (int it = 0; it < BENCH_ITERATIONS; ++it) {
            CCoinsMap m;
            FillCoinMap(m, rng);
            uint256 hashBlock = rng.rand256();
            cache.BatchWrite(m, hashBlock);
        }
    }

    stop.store(true, std::memory_order_release);
    for (auto& t : readers) t.join();
}

BENCHMARK(BatchWriteContention);
