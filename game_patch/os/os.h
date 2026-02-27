#pragma once

#include <chrono>
#include <optional>
#include <algorithm>

void os_apply_patch();
void frametime_render_ui();
float get_maximum_fps();
void apply_maximum_fps();
void wait_for(float ms);

class HighResTimer {
    using clock = std::chrono::high_resolution_clock;

    std::optional<clock::time_point> _start_time{};
    std::chrono::nanoseconds _duration{0};
public:
    [[nodiscard]] bool elapsed() const {
        return valid() && time_until() <= std::chrono::nanoseconds{0};
    }

    template <class Rep, class Period>
    void set(const std::chrono::duration<Rep, Period> duration) {
        _duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
            duration
        );
        _start_time.emplace(clock::now());
    }

    void set_sec(const float sec) {
        set(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<float>{sec}
        ));
    }

    void set_sec(const uint64_t sec) {
        set(std::chrono::seconds{sec});
    }

    void set_ms(const uint64_t ms) {
        set(std::chrono::milliseconds{ms});
    }

    void set_ns(const uint64_t ns) {
        set(std::chrono::nanoseconds{ns});
    }

    bool valid() const {
        return _start_time.has_value();
    }

    [[nodiscard]] std::chrono::nanoseconds time_until() const {
        if (!valid()) {
            return std::chrono::nanoseconds{0};
        }
        return (*_start_time + _duration) - clock::now();
    }

    [[nodiscard]] float time_until_sec() const {
        return std::chrono::duration<float>{time_until()}.count();
    }

    [[nodiscard]] int64_t time_until_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(time_until())
            .count();
    }

    [[nodiscard]] int64_t time_until_ns() const {
        return time_until().count();
    }

    [[nodiscard]] std::chrono::nanoseconds time_since() const {
        if (!valid()) {
            return std::chrono::nanoseconds{0};
        }
        return clock::now() - *_start_time;
    }

    [[nodiscard]] float time_since_sec() const {
        return std::chrono::duration<float>{time_since()}.count();
    }

    [[nodiscard]] int64_t time_since_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(time_since())
            .count();
    }

    [[nodiscard]] int64_t time_since_ns() const {
        return time_since().count();
    }

    void invalidate() {
        _start_time.reset();
        _duration = std::chrono::nanoseconds{0};
    }

    [[nodiscard]] float elapsed_frac() const {
        if (!valid()) {
            return 0.f;
        } else if (_duration == std::chrono::nanoseconds{0}) {
            return 1.f;
        }
        const double p = std::chrono::duration<double>{time_since()}
            / std::chrono::duration<double>{_duration};
        return static_cast<float>(std::clamp(p, 0., 1.));
    }

    std::chrono::nanoseconds duration() const {
        return _duration;
    }

    float duration_secs() const {
        return std::chrono::duration<float>{_duration}.count();
    }

    int64_t duration_ms() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(_duration)
            .count();
    }

    int64_t duration_ns() const {
        return _duration.count();
    }

    void restart() {
        _start_time.emplace(clock::now());
    }
};
