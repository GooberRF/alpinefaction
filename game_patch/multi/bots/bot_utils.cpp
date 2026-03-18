#include "bot_utils.h"

#include "bot_internal.h"
#include "../../misc/waypoints.h"
#include "../../rf/collide.h"
#include "../../rf/entity.h"
#include "../../rf/weapon.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

bool bot_has_unobstructed_level_los(
    const rf::Vector3& from,
    const rf::Vector3& to,
    const rf::Object* ignore1,
    const rf::Object* ignore2)
{
    rf::Vector3 p0 = from;
    rf::Vector3 p1 = to;
    rf::LevelCollisionOut collision{};
    collision.obj_handle = -1;
    collision.face = nullptr;
    const bool hit = rf::collide_linesegment_level_for_multi(
        p0,
        p1,
        const_cast<rf::Object*>(ignore1),
        const_cast<rf::Object*>(ignore2),
        &collision,
        0.05f,
        false,
        1.0f
    );
    return !hit;
}

int bot_find_closest_waypoint_with_fallback(const rf::Vector3& pos)
{
    float radius = kWaypointSearchRadius;
    for (int pass = 0; pass < 6; ++pass) {
        if (const int waypoint = waypoints_closest(pos, radius); waypoint > 0) {
            return waypoint;
        }
        radius *= 2.0f;
    }
    return 0;
}

int bot_find_closest_waypoint_with_los_fallback(const rf::Entity& entity)
{
    const bool can_use_cache =
        g_client_bot_state.closest_los_waypoint_cache_timer.valid()
        && !g_client_bot_state.closest_los_waypoint_cache_timer.elapsed();
    if (can_use_cache && g_client_bot_state.closest_los_waypoint_cache > 0) {
        rf::Vector3 moved = entity.pos - g_client_bot_state.closest_los_waypoint_cache_pos;
        moved.y = 0.0f;
        if (moved.len_sq()
            <= kClosestLosWaypointCacheMoveDist * kClosestLosWaypointCacheMoveDist) {
            return g_client_bot_state.closest_los_waypoint_cache;
        }
    }

    const int waypoint_total = waypoints_count();
    if (waypoint_total <= 1) {
        return 0;
    }

    const int nearest_waypoint = bot_find_closest_waypoint_with_fallback(entity.pos);
    const rf::Vector3 eye_offset = entity.eye_pos - entity.pos;
    if (nearest_waypoint > 0) {
        rf::Vector3 nearest_pos{};
        if (waypoints_get_pos(nearest_waypoint, nearest_pos)) {
            const rf::Vector3 nearest_eye_pos = nearest_pos + eye_offset;
            if (bot_has_unobstructed_level_los(
                    entity.eye_pos,
                    nearest_eye_pos,
                    &entity,
                    nullptr)) {
                g_client_bot_state.closest_los_waypoint_cache = nearest_waypoint;
                g_client_bot_state.closest_los_waypoint_cache_pos = entity.pos;
                g_client_bot_state.closest_los_waypoint_cache_timer.set(
                    kClosestLosWaypointCacheMs
                );
                return nearest_waypoint;
            }
        }
    }

    // Fallback: collect all waypoints sorted by distance, then check LOS for the closest ones.
    // This avoids expensive raycasts against distant waypoints.
    constexpr int kMaxLosCheckCandidates = 32;

    struct WaypointDist {
        int index;
        float dist_sq;
    };
    std::vector<WaypointDist> candidates;
    candidates.reserve(std::min(waypoint_total, 256));

    for (int waypoint = 1; waypoint < waypoint_total; ++waypoint) {
        rf::Vector3 waypoint_pos{};
        if (!waypoints_get_pos(waypoint, waypoint_pos)) {
            continue;
        }
        const float dist_sq = rf::vec_dist_squared(&entity.pos, &waypoint_pos);
        candidates.push_back({waypoint, dist_sq});
    }

    // Partial sort to get only the closest N candidates
    const int check_count = std::min(static_cast<int>(candidates.size()), kMaxLosCheckCandidates);
    std::partial_sort(candidates.begin(), candidates.begin() + check_count, candidates.end(),
        [](const WaypointDist& a, const WaypointDist& b) { return a.dist_sq < b.dist_sq; });

    float best_dist_sq = std::numeric_limits<float>::max();
    int best_waypoint = 0;
    for (int i = 0; i < check_count; ++i) {
        rf::Vector3 waypoint_pos{};
        if (!waypoints_get_pos(candidates[i].index, waypoint_pos)) {
            continue;
        }

        const rf::Vector3 waypoint_eye_pos = waypoint_pos + eye_offset;
        if (!bot_has_unobstructed_level_los(entity.eye_pos, waypoint_eye_pos, &entity, nullptr)) {
            continue;
        }

        if (candidates[i].dist_sq < best_dist_sq) {
            best_dist_sq = candidates[i].dist_sq;
            best_waypoint = candidates[i].index;
        }
    }

    if (best_waypoint > 0) {
        g_client_bot_state.closest_los_waypoint_cache = best_waypoint;
        g_client_bot_state.closest_los_waypoint_cache_pos = entity.pos;
        g_client_bot_state.closest_los_waypoint_cache_timer.set(
            kClosestLosWaypointCacheMs
        );
        return best_waypoint;
    }

    g_client_bot_state.closest_los_waypoint_cache = nearest_waypoint;
    g_client_bot_state.closest_los_waypoint_cache_pos = entity.pos;
    g_client_bot_state.closest_los_waypoint_cache_timer.set(
        kClosestLosWaypointCacheMs
    );
    return nearest_waypoint;
}

int bot_resolve_weapon_type_cached(const char* weapon_name)
{
    struct CacheEntry
    {
        const char* name = nullptr;
        int weapon_type = -2;
    };
    static std::array<CacheEntry, 6> cache_entries{{
        {"rail_gun", -2},
        {"Shotgun", -2},
        {"Riot Stick", -2},
        {"shoulder_cannon", -2},
        {"Rocket Launcher", -2},
        {"Remote Charge", -2},
    }};

    for (CacheEntry& entry : cache_entries) {
        if (std::strcmp(entry.name, weapon_name) != 0) {
            continue;
        }
        if (entry.weapon_type == -2) {
            entry.weapon_type = rf::weapon_lookup_type(entry.name);
        }
        return entry.weapon_type;
    }

    return rf::weapon_lookup_type(weapon_name);
}

bool bot_entity_has_weapon_type(const rf::Entity& entity, const int weapon_type)
{
    return weapon_type >= 0
        && weapon_type < rf::num_weapon_types
        && entity.ai.has_weapon[weapon_type];
}
