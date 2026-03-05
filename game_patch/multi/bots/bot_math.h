#pragma once

#include "../../rf/entity.h"
#include <algorithm>
#include <cmath>

inline float normalize_angle(const float angle)
{
    constexpr float pi = 3.14159265358979323846f;
    constexpr float two_pi = 2.0f * pi;
    float value = angle;
    while (value > pi) {
        value -= two_pi;
    }
    while (value < -pi) {
        value += two_pi;
    }
    return value;
}

inline float approach_angle(const float current, const float target, const float max_step)
{
    const float delta = std::clamp(
        normalize_angle(target - current),
        -max_step,
        max_step
    );
    return normalize_angle(current + delta);
}

inline rf::Vector3 forward_from_non_linear_yaw_pitch(const float yaw, const float pitch)
{
    rf::Vector3 fvec;
    fvec.y = std::sin(pitch);
    const float factor = 1.0f - std::abs(fvec.y);
    fvec.x = factor * std::sin(yaw);
    fvec.z = factor * std::cos(yaw);
    fvec.normalize_safe();
    return fvec;
}

inline float non_linear_pitch_from_forward_vector(rf::Vector3 fvec)
{
    if (fvec.len_sq() < 0.000001f) {
        return 0.0f;
    }

    fvec.normalize_safe();

    if (fvec.y == 0.0f) {
        return 0.0f;
    }

    const float yaw = std::atan2(fvec.x, fvec.z);
    const float fvec_y_2 = fvec.y * fvec.y;
    const float y_sin = std::sin(yaw);
    const float y_cos = std::cos(yaw);
    const float y_sin_2 = y_sin * y_sin;
    const float y_cos_2 = y_cos * y_cos;
    const float p_sgn = std::signbit(fvec.y) ? -1.f : 1.f;

    const float a = 1.f / fvec_y_2 - y_sin_2 - 1.f - y_cos_2;
    if (std::abs(a) < 0.000001f) {
        return 0.0f;
    }

    const float b = 2.f * p_sgn * y_sin_2 + 2.f * p_sgn * y_cos_2;
    const float c = -y_sin_2 - y_cos_2;
    const float delta = b * b - 4.f * a * c;
    const float delta_sqrt = std::sqrt(std::max(delta, 0.0f));

    const float p_sin_1 = (-b - delta_sqrt) / (2.f * a);
    const float p_sin_2 = (-b + delta_sqrt) / (2.f * a);
    const float result_sin =
        (std::abs(p_sin_1) < std::abs(p_sin_2)) ? p_sin_1 : p_sin_2;

    return std::asin(std::clamp(result_sin, -1.0f, 1.0f));
}
