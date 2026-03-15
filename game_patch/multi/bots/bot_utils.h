#pragma once

#include "../../rf/math/vector.h"

namespace rf
{
    struct Object;
}

bool bot_has_unobstructed_level_los(
    const rf::Vector3& from,
    const rf::Vector3& to,
    const rf::Object* ignore1,
    const rf::Object* ignore2);

int bot_find_closest_waypoint_with_fallback(const rf::Vector3& pos);

inline float bot_horizontal_dist_sq(const rf::Vector3& a, const rf::Vector3& b)
{
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return dx * dx + dz * dz;
}
