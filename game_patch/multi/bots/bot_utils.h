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

// Whether a bot at from can walk straight to to: nothing solid (glass and fences included) at step height, floor along
// the way, no ledge to climb, and no drop deeper than the movement edge probe allows.
bool bot_can_walk_direct(const rf::Vector3& from, const rf::Vector3& to);
// A walking bot stops at a ledge from about this high: anything higher it has to jump or go round.
inline constexpr float kBotLedgeMinStep = 0.4f;
// The highest single rise in the floor from from to to, and how far along it comes as a share of the way.
float bot_link_floor_step(const rf::Vector3& from, const rf::Vector3& to, float* out_share = nullptr);
bool bot_ctf_can_capture(bool own_flag_in_base);
int bot_semi_auto_min_interval_ms(int weapon_type);

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
