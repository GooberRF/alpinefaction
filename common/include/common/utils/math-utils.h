#pragma once

#include <algorithm>
#include <cmath>
#include <numbers>

// A non-finite serialized or typed value falls back instead of carrying a NaN through the clamp
// (every comparison against a NaN is false, so std::clamp would return it unchanged).
inline float clamp_finite(float v, float lo, float hi, float fallback)
{
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}

// Angle in radians, wrapped into [-pi, pi]
inline float wrap_pi(float a)
{
    return std::remainder(a, 2.0f * std::numbers::pi_v<float>);
}
