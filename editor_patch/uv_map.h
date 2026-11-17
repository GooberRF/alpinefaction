#pragma once

#include <optional>
#include <span>
#include "mfc_types.h"

// A face's UVs as an affine function of world position on its plane: u = u0 + grad_u . (p - origin)
struct UvMap
{
    Vector3 origin;
    Vector3 grad_u;
    Vector3 grad_v;
    float u0;
    float v0;

    float u(const Vector3& p) const
    {
        return u0 + dot(grad_u, p - origin);
    }

    float v(const Vector3& p) const
    {
        return v0 + dot(grad_v, p - origin);
    }
};

struct UvCorner
{
    Vector3 p;
    float u;
    float v;
};

// Through the first corner and the two others spanning the largest triangle; none for a degenerate face
inline std::optional<UvMap> uv_map_through(std::span<const UvCorner> corners)
{
    if (corners.size() < 3) {
        return std::nullopt;
    }
    const UvCorner& c0 = corners[0];
    size_t best_i = 0, best_j = 0;
    float best_area = 0.0f;
    for (size_t i = 1; i < corners.size(); ++i) {
        for (size_t j = i + 1; j < corners.size(); ++j) {
            const float area = length(cross(corners[i].p - c0.p, corners[j].p - c0.p));
            if (area > best_area) {
                best_area = area;
                best_i = i;
                best_j = j;
            }
        }
    }
    if (!(best_area >= 1e-6f)) {
        return std::nullopt;
    }
    // grad = a e1 + b e2p with grad . e1 = d1 and grad . e2 = d2, e2p the part of e2 square to e1, which keeps a
    // long thin face from cancelling the solve
    const Vector3 e1 = corners[best_i].p - c0.p;
    const Vector3 e2 = corners[best_j].p - c0.p;
    const float g11 = dot(e1, e1), g12 = dot(e1, e2);
    const Vector3 e2p = e2 - e1 * (g12 / g11);
    const float g2p = dot(e2p, e2p);
    auto gradient = [&](float d1, float d2) {
        const float a = d1 / g11;
        const float b = (d2 - a * g12) / g2p;
        return e1 * a + e2p * b;
    };
    return UvMap{
        c0.p,
        gradient(corners[best_i].u - c0.u, corners[best_j].u - c0.u),
        gradient(corners[best_i].v - c0.v, corners[best_j].v - c0.v),
        c0.u,
        c0.v,
    };
}
