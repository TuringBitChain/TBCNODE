// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.

#include "validation/async_trim.h"

#include <utility>

namespace tbc {
namespace validation {

void AsyncTrimManager::Start(AsyncTrim::TrimBatchFunc trim_fn) {
    std::lock_guard lock(mtx);
    if (trim) return;  // 幂等
    trim = std::make_unique<AsyncTrim>(std::move(trim_fn));
}

void AsyncTrimManager::Stop() {
    std::unique_ptr<AsyncTrim> local;
    {
        std::lock_guard lock(mtx);
        local = std::move(trim);
    }
    if (local) local->Stop();
    // local 析构在锁外
}

void AsyncTrimManager::Notify() noexcept {
    // 不持锁读 trim 指针 — 用 lock_guard 防 stop 期间 race
    std::lock_guard lock(mtx);
    if (trim) trim->Notify();
}

void AsyncTrimManager::NotifyUrgent() noexcept {
    std::lock_guard lock(mtx);
    if (trim) trim->NotifyUrgent();
}

bool AsyncTrimManager::IsRunning() const noexcept {
    // review v7 F2-4：mtx 已声明 mutable，无需 const_cast
    std::lock_guard lock(mtx);
    return trim != nullptr;
}

AsyncTrimManager g_async_trim;

} // namespace validation
} // namespace tbc
