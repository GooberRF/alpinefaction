#pragma once

#include <type_traits>

template <class F>
struct ScopeGuard {
    F _f;
    constexpr explicit ScopeGuard(F&& f)
        noexcept(std::is_nothrow_move_constructible<F>::value)
        : _f(std::forward<F>(f)) {
    }
    ~ScopeGuard() noexcept { _f(); }
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
};
