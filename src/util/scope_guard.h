// Copyright (c) 2026 The TuringBitChain developers
// Distributed under the Open BSV software license, see the accompanying file LICENSE.
//
// v2.6.1 P3.3 / §7.1 ScopeGuard (H8 修复)
//
// 析构无条件 noexcept + 内部 try/catch 防 std::terminate
// 不依赖 uncaught_exceptions()（H5 修复）

#ifndef BITCOIN_UTIL_SCOPE_GUARD_H
#define BITCOIN_UTIL_SCOPE_GUARD_H

#include <type_traits>
#include <utility>

namespace tbc {

template<typename F>
class ScopeGuard {
    F f;
    bool armed = true;
public:
    explicit ScopeGuard(F&& f_) noexcept(std::is_nothrow_move_constructible_v<F>)
        : f(std::move(f_)) {}

    // H8: 无条件 noexcept，内部 try/catch 防 terminate
    ~ScopeGuard() noexcept {
        if (armed) {
            try { f(); }
            catch (...) {
                // swallow（生产可改成 LogPrintf）
            }
        }
    }

    void Disarm() noexcept { armed = false; }

    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
    ScopeGuard(ScopeGuard&&) = delete;
    ScopeGuard& operator=(ScopeGuard&&) = delete;
};

template<typename F>
ScopeGuard<std::decay_t<F>> MakeScopeGuard(F&& f) {
    return ScopeGuard<std::decay_t<F>>(std::forward<F>(f));
}

} // namespace tbc

#endif // BITCOIN_UTIL_SCOPE_GUARD_H
