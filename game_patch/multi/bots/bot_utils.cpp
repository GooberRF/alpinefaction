#include "bot_utils.h"

#include "bot_context.h"
#include "bot_internal.h"
#include "../vehicles/vehicle.h"
#include "../../misc/waypoints.h"
#include "../../rf/collide.h"
#include "../../rf/entity.h"
#include "../../rf/weapon.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
constexpr int kWalkDirectTraceFlags = rf::CF_ANY_HIT | rf::CF_PROCESS_INVISIBLE_FACES;
constexpr float kWalkDirectSampleSpacing = 1.5f;
// Only the stretch ahead is checked; direct moves re-check it every think as the bot advances.
constexpr float kWalkDirectMaxDistance = 12.0f;
constexpr float kWalkDirectStepHeight = 0.6f;
// Matches the drop the movement edge probe refuses to walk off.
constexpr float kWalkDirectMaxStep = 1.0f;
constexpr float kWalkDirectFloorProbeDepth = 3.0f;
constexpr float kWalkDirectProbeMargin = 0.1f;
constexpr int kWalkDirectCacheMs = 250;
constexpr float kWalkDirectCacheTolerance = 0.25f;
constexpr int kLosWaypointWalkCheckLimit = 6;
constexpr int kLosMaxTraces = 3;
// At least the humanoid box 0x0049C690 tests every entity with, whatever the entity is.
constexpr float kLosRiderBoxHalfWidth = 0.3f;
constexpr float kLosRiderBoxHalfHeight = 0.9f;
constexpr float kLosRiderBoxExitMargin = 0.05f;

bool level_los_blocked(
    rf::Vector3 p0,
    rf::Vector3 p1,
    const rf::Object* ignore1,
    const rf::Object* ignore2,
    rf::LevelCollisionOut& collision)
{
    collision.obj_handle = -1;
    collision.face = nullptr;
    return rf::collide_linesegment_level_for_multi(
        p0,
        p1,
        const_cast<rf::Object*>(ignore1),
        const_cast<rf::Object*>(ignore2),
        &collision,
        0.05f,
        false,
        1.0f
    );
}

float rider_box_exit_distance(const rf::Vector3& origin, const rf::Vector3& dir, const rf::Vector3& center)
{
    const float origin_axes[3] = {origin.x, origin.y, origin.z};
    const float dir_axes[3] = {dir.x, dir.y, dir.z};
    const float center_axes[3] = {center.x, center.y, center.z};
    const float half_axes[3] = {kLosRiderBoxHalfWidth, kLosRiderBoxHalfHeight, kLosRiderBoxHalfWidth};
    float exit = std::numeric_limits<float>::infinity();
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(dir_axes[axis]) < 1e-6f) {
            continue;
        }
        const float t0 = (center_axes[axis] - half_axes[axis] - origin_axes[axis]) / dir_axes[axis];
        const float t1 = (center_axes[axis] + half_axes[axis] - origin_axes[axis]) / dir_axes[axis];
        exit = std::min(exit, std::max(t0, t1));
    }
    return std::max(exit, 0.0f);
}

bool trace_walk_floor(const rf::Vector3& pos, const float top_y, const float bottom_y, rf::Vector3& out_floor)
{
    rf::Vector3 p0{pos.x, top_y, pos.z};
    rf::Vector3 p1{pos.x, bottom_y, pos.z};
    rf::GCollisionOutput collision{};
    if (!rf::collide_linesegment_level_solid(p0, p1, kWalkDirectTraceFlags, &collision)) {
        return false;
    }
    out_floor = collision.hit_point;
    return true;
}

bool walk_direct_trace_clear(const rf::Vector3& from, const rf::Vector3& to)
{
    rf::Vector3 p0 = from;
    rf::Vector3 p1 = to;
    rf::GCollisionOutput collision{};
    return !rf::collide_linesegment_level_solid(p0, p1, kWalkDirectTraceFlags, &collision);
}

bool compute_walk_direct(const rf::Vector3& from, const rf::Vector3& to)
{
    rf::Vector3 floor{};
    if (!trace_walk_floor(from, from.y + kWalkDirectProbeMargin, from.y - kWalkDirectFloorProbeDepth, floor)) {
        // Swimming, airborne or on a mover: floor support cannot be judged, so only require a clear line.
        return walk_direct_trace_clear(from, to);
    }

    const float horizontal_dist = std::sqrt(bot_horizontal_dist_sq(from, to));
    const float checked_share =
        horizontal_dist > kWalkDirectMaxDistance ? kWalkDirectMaxDistance / horizontal_dist : 1.0f;
    const int num_samples =
        std::max(static_cast<int>(std::ceil(horizontal_dist * checked_share / kWalkDirectSampleSpacing)), 1);
    // Lets a 45 degree slope pass however far apart the samples are.
    const float max_slope_step = std::max(kWalkDirectMaxStep, horizontal_dist * checked_share / num_samples);
    const rf::Vector3 step_offset{0.0f, kWalkDirectStepHeight, 0.0f};
    for (int sample = 1; sample <= num_samples; ++sample) {
        const rf::Vector3 point = from + (to - from) * (checked_share * sample / num_samples);
        const float top_y = std::max(point.y, floor.y + max_slope_step) + kWalkDirectProbeMargin;
        rf::Vector3 next_floor{};
        if (!trace_walk_floor(point, top_y, floor.y - max_slope_step - kWalkDirectProbeMargin, next_floor)
            || next_floor.y > floor.y + max_slope_step) {
            return false;
        }
        if (std::abs(next_floor.y - floor.y) > kWalkDirectMaxStep) {
            const rf::Vector3 midpoint = (floor + next_floor) * 0.5f;
            rf::Vector3 mid_floor{};
            if (!trace_walk_floor(
                    midpoint,
                    std::max(floor.y, next_floor.y) + kWalkDirectProbeMargin,
                    std::min(floor.y, next_floor.y) - kWalkDirectProbeMargin,
                    mid_floor)
                || std::abs(mid_floor.y - floor.y) > kWalkDirectMaxStep
                || std::abs(next_floor.y - mid_floor.y) > kWalkDirectMaxStep) {
                return false;
            }
        }
        if (!walk_direct_trace_clear(floor + step_offset, next_floor + step_offset)) {
            return false;
        }
        floor = next_floor;
    }
    return true;
}
}

bool bot_can_walk_direct(const rf::Vector3& from, const rf::Vector3& to)
{
    constexpr float tolerance_sq = kWalkDirectCacheTolerance * kWalkDirectCacheTolerance;
    auto& cache = bot_state().walk_direct_cache;
    for (const BotWalkDirectCacheEntry& entry : cache) {
        if (entry.timer.valid()
            && !entry.timer.elapsed()
            && rf::vec_dist_squared(&entry.from, &from) <= tolerance_sq
            && rf::vec_dist_squared(&entry.to, &to) <= tolerance_sq) {
            return entry.walkable;
        }
    }

    const bool walkable = compute_walk_direct(from, to);
    int& next_slot = bot_state().walk_direct_cache_next;
    BotWalkDirectCacheEntry& entry = cache[next_slot];
    next_slot = (next_slot + 1) % static_cast<int>(cache.size());
    entry.from = from;
    entry.to = to;
    entry.walkable = walkable;
    entry.timer.set(kWalkDirectCacheMs);
    return walkable;
}

bool bot_has_unobstructed_level_los(
    const rf::Vector3& from,
    const rf::Vector3& to,
    const rf::Object* ignore1,
    const rf::Object* ignore2)
{
    rf::LevelCollisionOut collision{};
    const rf::Entity* const self = ignore1 ? rf::entity_from_handle(ignore1->handle) : nullptr;
    const rf::Entity* const target = ignore2 ? rf::entity_from_handle(ignore2->handle) : nullptr;
    const rf::Entity* const own_hull = vehicle_ridden_hull(self);
    const rf::Entity* const target_hull =
        target && vehicle_is_synced_entity_type(target) ? target : vehicle_ridden_hull(target);
    if (!own_hull && !target_hull) {
        return !level_los_blocked(from, to, ignore1, ignore2, collision);
    }

    // An entity hit clears the primitive's face, so the level is tested on its own, with 0x0049C690's own flags.
    rf::Vector3 level_p0 = from;
    rf::Vector3 level_p1 = to;
    rf::PCollisionOut level_collision{};
    level_collision.obj_handle = -1;
    if (rf::collide_linesegment_world(
            level_p0, level_p1, rf::CF_SKIP_SHOOT_THRU | rf::CF_SKIP_DESTRUCTIBLE_GLASS, &level_collision)) {
        return false;
    }

    // A seated viewer's own hull stands round his eye, so it takes the second ignore slot.
    const rf::Object* const second_ignore = own_hull ? own_hull : ignore2;
    rf::Vector3 p0 = from;
    for (int trace = 0; trace < kLosMaxTraces; ++trace) {
        if (!level_los_blocked(p0, to, ignore1, second_ignore, collision)) {
            return true;
        }
        const rf::Entity* const hit = rf::entity_from_handle(collision.obj_handle);
        if (!hit) {
            return false;
        }
        if (target
            && (hit == target || hit == target_hull || (target_hull && hit->host_handle == target_hull->handle))) {
            return true;
        }
        if (!own_hull || hit->host_handle != own_hull->handle) {
            return false;
        }
        rf::Vector3 dir = to - p0;
        const float remaining = dir.len();
        dir.normalize_safe();
        const float skip = rider_box_exit_distance(p0, dir, hit->pos) + kLosRiderBoxExitMargin;
        if (skip >= remaining) {
            return true;
        }
        p0 += dir * skip;
    }
    return false;
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
        bot_state().closest_los_waypoint_cache_timer.valid()
        && !bot_state().closest_los_waypoint_cache_timer.elapsed();
    if (can_use_cache && bot_state().closest_los_waypoint_cache > 0) {
        rf::Vector3 moved = entity.pos - bot_state().closest_los_waypoint_cache_pos;
        moved.y = 0.0f;
        if (moved.len_sq()
            <= kClosestLosWaypointCacheMoveDist * kClosestLosWaypointCacheMoveDist) {
            return bot_state().closest_los_waypoint_cache;
        }
    }

    const int waypoint_total = waypoints_count();
    if (waypoint_total <= 1) {
        return 0;
    }

    const int nearest_waypoint = bot_find_closest_waypoint_with_fallback(entity.pos);
    const rf::Vector3 eye_offset = entity.eye_pos - entity.pos;
    const auto remember = [&](const int waypoint) {
        bot_state().closest_los_waypoint_cache = waypoint;
        bot_state().closest_los_waypoint_cache_pos = entity.pos;
        bot_state().closest_los_waypoint_cache_timer.set(kClosestLosWaypointCacheMs);
        return waypoint;
    };
    const auto visible = [&](const rf::Vector3& waypoint_pos) {
        return bot_has_unobstructed_level_los(entity.eye_pos, waypoint_pos + eye_offset, &entity, nullptr);
    };
    rf::Vector3 nearest_pos{};
    if (nearest_waypoint > 0 && waypoints_get_pos(nearest_waypoint, nearest_pos) && visible(nearest_pos)) {
        if (bot_can_walk_direct(entity.pos, nearest_pos)) {
            return remember(nearest_waypoint);
        }
        // In sight but not walkable (glass, a fence, a ledge): prefer a linked waypoint the bot can walk to.
        std::array<int, kMaxWaypointLinks> links{};
        const int link_count = waypoints_get_links(nearest_waypoint, links);
        for (int i = 0; i < link_count; ++i) {
            rf::Vector3 link_pos{};
            if (links[i] > 0
                && waypoints_get_pos(links[i], link_pos)
                && visible(link_pos)
                && bot_can_walk_direct(entity.pos, link_pos)) {
                return remember(links[i]);
            }
        }
        return remember(nearest_waypoint);
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

    int best_waypoint = 0;
    int nearest_visible_waypoint = 0;
    int walk_checks = 0;
    for (int i = 0; i < check_count; ++i) {
        rf::Vector3 waypoint_pos{};
        if (!waypoints_get_pos(candidates[i].index, waypoint_pos) || !visible(waypoint_pos)) {
            continue;
        }
        if (nearest_visible_waypoint <= 0) {
            nearest_visible_waypoint = candidates[i].index;
        }
        if (walk_checks++ >= kLosWaypointWalkCheckLimit) {
            break;
        }
        if (bot_can_walk_direct(entity.pos, waypoint_pos)) {
            best_waypoint = candidates[i].index;
            break;
        }
    }
    if (best_waypoint <= 0) {
        best_waypoint = nearest_visible_waypoint;
    }

    return remember(best_waypoint > 0 ? best_waypoint : nearest_waypoint);
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
