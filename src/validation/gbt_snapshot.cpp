// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.

#include "validation/gbt_snapshot.h"

#include "logging.h"
#include "utiltime.h"

#include <chrono>

namespace tbc {
namespace validation {

void GbtSnapshotProvider::Start(RefreshFunc fn) {
    if (running.exchange(true, std::memory_order_acq_rel)) {
        return;  // already running
    }
    refresh_fn = std::move(fn);
    thread = std::thread([this] { Run(); });
}

void GbtSnapshotProvider::Stop() {
    if (!running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    refresh_cv.notify_all();
    if (thread.joinable()) {
        thread.join();
    }
    // 释放所有挂着的 promise（避免调用方 future 死等）
    std::lock_guard lock(pending_mtx);
    for (auto& p : pending) {
        try { p->set_value(nullptr); } catch (...) {}
    }
    pending.clear();
}

bool GbtSnapshotProvider::IsRunning() const noexcept {
    return running.load(std::memory_order_acquire);
}

void GbtSnapshotProvider::NotifyTipChanged() noexcept {
    tip_changed.store(true, std::memory_order_release);
    refresh_cv.notify_one();
}

GbtSnapshotProvider::SnapshotPtr GbtSnapshotProvider::GetSnapshot(int64_t timeout_ms) {
    // Phase G (task #164): 死锁防护契约文档化
    //   ⚠️ 调用方 MUST NOT hold cs_main when calling GetSnapshot()
    //   refresh_fn 内部取 cs_main shared（参考 init.cpp:3007-3022 注册时的契约）；
    //   若 caller 持 cs_main 调 GetSnapshot → refresh_fn 卡 cs_main → 死锁。
    //   sync.h 当前无 AssertLockNotHeld 宏（仅 AssertLockHeld）；契约靠 caller 守约。
    if (!running.load(std::memory_order_acquire)) {
        return nullptr;
    }

    // 检查 cached：如果不老（<5s）+ tip 没变 → 直接返回
    {
        std::lock_guard lock(cache_mtx);
        if (cached) {
            int64_t age_us = GetTimeMicros() - cached->computed_at_us;
            const int64_t FRESH_AGE_US = 5'000'000;
            if (age_us < FRESH_AGE_US && !tip_changed.load(std::memory_order_acquire)) {
                return cached;
            }
        }
    }

    // 排队等 refresh worker 算
    auto promise = std::make_shared<std::promise<SnapshotPtr>>();
    auto future = promise->get_future();
    {
        std::lock_guard lock(pending_mtx);
        pending.push_back(promise);
    }
    refresh_cv.notify_one();

    auto status = future.wait_for(std::chrono::milliseconds(timeout_ms));
    if (status != std::future_status::ready) {
        return nullptr;
    }
    return future.get();
}

void GbtSnapshotProvider::Run() {
    while (running.load(std::memory_order_acquire)) {
        std::vector<std::shared_ptr<std::promise<SnapshotPtr>>> batch;
        {
            std::unique_lock lock(pending_mtx);
            refresh_cv.wait_for(lock, std::chrono::seconds(5), [this] {
                return !pending.empty()
                    || tip_changed.load(std::memory_order_acquire)
                    || !running.load(std::memory_order_acquire);
            });
            if (!running.load(std::memory_order_acquire)) break;
            batch.swap(pending);
        }

        // 计算一次新 snapshot（同时给 batch 里所有 promise 用）
        SnapshotPtr snap;
        try {
            if (refresh_fn) {
                snap = refresh_fn();
                if (snap) snap->computed_at_us = GetTimeMicros();
            }
        } catch (const std::exception& e) {
            LogPrintf("v2.6.1 GbtSnapshot: refresh exception: %s\n", e.what());
            snap = nullptr;
        } catch (...) {
            snap = nullptr;
        }

        if (snap) {
            std::lock_guard lock(cache_mtx);
            cached = snap;
            tip_changed.store(false, std::memory_order_release);
        }

        // 给所有 batch promise 回写
        for (auto& p : batch) {
            try { p->set_value(snap); }
            catch (const std::future_error&) {}
        }
    }
}

GbtSnapshotProvider g_gbt_snapshot;

} // namespace validation
} // namespace tbc
