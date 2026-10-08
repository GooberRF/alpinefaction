#pragma once

#include <type_traits>
#include <utility>

template<class F>
struct ScopeGuard
{
    F f_;
    explicit ScopeGuard(F&& f) noexcept(std::is_nothrow_move_constructible<F>::value) :
        f_(std::forward<F>(f))
    {}
    ~ScopeGuard() noexcept
    {
        f_();
    }
    ScopeGuard(const ScopeGuard&) = delete;
    ScopeGuard& operator=(const ScopeGuard&) = delete;
};
