#pragma once

#include <cmath>

// Finite unit axes, pairwise orthogonal, both within 0.01: a basis that cannot shear a derived box.
// Shared so the editor never writes a Vehicle Factory pose the game refuses.
template<typename Matrix>
bool vehicle_orient_is_orthonormal(const Matrix& orient)
{
    const auto is_unit = [](const auto& a) {
        return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z)
            && std::fabs(std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z) - 1.0f) <= 0.01f;
    };
    const auto dot = [](const auto& a, const auto& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    return is_unit(orient.rvec) && is_unit(orient.uvec) && is_unit(orient.fvec)
        && std::fabs(dot(orient.rvec, orient.uvec)) <= 0.01f
        && std::fabs(dot(orient.rvec, orient.fvec)) <= 0.01f
        && std::fabs(dot(orient.uvec, orient.fvec)) <= 0.01f;
}
