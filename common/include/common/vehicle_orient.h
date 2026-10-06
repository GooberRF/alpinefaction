#pragma once

#include <cmath>

// Finite unit axes, pairwise orthogonal, both within 0.01, and right-handed like the identity: a rotation,
// so it cannot shear or mirror a derived box.
// Shared so the editor never writes a Vehicle Factory pose the game refuses.
template<typename Matrix>
bool vehicle_orient_is_rotation(const Matrix& orient)
{
    const auto is_unit = [](const auto& a) {
        return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z)
            && std::fabs(std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z) - 1.0f) <= 0.01f;
    };
    const auto dot = [](const auto& a, const auto& b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
    const auto& r = orient.rvec;
    const auto& u = orient.uvec;
    const auto& f = orient.fvec;
    const float handedness = (r.y * u.z - r.z * u.y) * f.x + (r.z * u.x - r.x * u.z) * f.y
                           + (r.x * u.y - r.y * u.x) * f.z;
    return is_unit(r) && is_unit(u) && is_unit(f)
        && std::fabs(dot(r, u)) <= 0.01f
        && std::fabs(dot(r, f)) <= 0.01f
        && std::fabs(dot(u, f)) <= 0.01f
        && handedness > 0.0f;
}
