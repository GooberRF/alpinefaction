#pragma once

#include <cmath>
#include <numbers>

// Angle in radians, wrapped into [-pi, pi]
inline float wrap_pi(float a)
{
    return std::remainder(a, 2.0f * std::numbers::pi_v<float>);
}
