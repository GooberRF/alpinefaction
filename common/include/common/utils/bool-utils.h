#pragma once

#include <functional>
#include <optional>

template <std::invocable F>
[[nodiscard]] constexpr auto then(const bool cond, F&& f) -> std::conditional_t<
    std::is_void_v<std::invoke_result_t<F>>,
    std::optional<std::monostate>,
    std::optional<std::decay_t<std::invoke_result_t<F>>>
> {
    if (cond) {
        if constexpr (std::is_void_v<std::invoke_result_t<F>>) {
            std::invoke(std::forward<F>(f));
            return std::optional{std::monostate{}};
        } else {
            return std::optional{std::invoke(std::forward<F>(f))};
        }
    }
    return std::nullopt;
}

template <typename T>
[[nodiscard]] constexpr std::optional<std::decay_t<T>> then_some(
    const bool cond,
    T&& value
) {
    if (cond) {
        return std::optional{std::forward<T>(value)};
    }
    return std::nullopt;
}
