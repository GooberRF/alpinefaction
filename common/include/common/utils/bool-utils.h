#pragma once

#include <optional>

template <std::invocable F>
[[nodiscard]] std::optional<std::remove_cvref_t<std::invoke_result_t<F>>> then(
    const bool cond,
    F&& f
) {
    if (cond) {
        return std::optional{std::invoke(std::forward<F>(f))};
    }
    return std::nullopt;
}

template <typename T>
[[nodiscard]] std::optional<std::remove_cvref_t<T>> then_some(
    const bool cond,
    T&& value
) {
    if (cond) {
        return std::optional{std::forward<T>(value)};
    }
    return std::nullopt;
}
