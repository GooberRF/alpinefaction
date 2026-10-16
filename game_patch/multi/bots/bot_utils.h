#pragma once

#include "../../rf/math/vector.h"

namespace rf
{
    struct Object;
    struct Entity;
}

bool bot_has_unobstructed_level_los(
    const rf::Vector3& from,
    const rf::Vector3& to,
    const rf::Object* ignore1,
    const rf::Object* ignore2);

// Whether a bot at from can walk straight to to: nothing solid (glass and fences included) at step height, and floor
// along the way with no ledge to climb or drop off.
bool bot_can_walk_direct(const rf::Vector3& from, const rf::Vector3& to);

int bot_find_closest_waypoint_with_fallback(const rf::Vector3& pos);

int bot_find_closest_waypoint_with_los_fallback(const rf::Entity& entity);

int bot_resolve_weapon_type_cached(const char* weapon_name);

bool bot_entity_has_weapon_type(const rf::Entity& entity, int weapon_type);

inline float bot_horizontal_dist_sq(const rf::Vector3& a, const rf::Vector3& b)
{
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return dx * dx + dz * dz;
}
