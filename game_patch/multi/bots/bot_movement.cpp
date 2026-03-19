#include "bot_movement.h"

#include "bot_internal.h"
#include "bot_math.h"
#include "bot_navigation_routes.h"
#include "../../main/main.h"
#include "../../rf/collide.h"
#include "../../rf/level.h"
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <random>
#include <unordered_map>

namespace
{
constexpr int kBotMovementTraceFlags = rf::CF_ANY_HIT | rf::CF_PROCESS_INVISIBLE_FACES;
constexpr float kMaxWaypointStepJumpHeight = 1.5f;
constexpr float kJumpPadFinalApproachStopDistance = 0.05f;
constexpr float kLadderClimbVerticalThreshold = 0.12f;
constexpr int kJumpBlockedEdgeCooldownMs = 9000;
constexpr int kWallAvoidSoftRepathMs = 100;
constexpr int kForwardObstacleRerouteCooldownMs = 1200;
constexpr int kForwardObstacleRerouteSamples = 3;
constexpr float kLinkGuidanceBlendWeight = 0.80f;
constexpr float kLinkGuidanceBlendRelaxedWeight = 0.65f;
constexpr float kLinkGuidanceLevelRelaxThreshold = 0.40f;
constexpr float kLinkGuidanceRelaxEdgeProbeDistance = 2.0f;
constexpr float kForwardProbeOriginYOffset = -0.75f;
constexpr float kForwardEdgeProbeStartYOffset = 0.10f;
constexpr float kForwardEdgeProbeDownDistance = 2.0f;
constexpr float kForwardEdgeUnsafeDropHeight = 1.0f;
constexpr float kLinkFloorProbeStartYOffset = 0.10f;
constexpr float kLinkFloorProbeDownDistance = 2.0f;
constexpr float kLinkFloorLinkTraceYOffset = 0.05f;
constexpr int kUpwardLinkObstacleCacheMaxEntries = 4096;

std::unordered_map<uint64_t, bool> g_upward_link_obstacle_cache{};
int g_upward_link_obstacle_cache_waypoint_total = 0;

uint64_t make_directed_link_key(const int from_waypoint, const int to_waypoint)
{
    return (static_cast<uint64_t>(static_cast<uint32_t>(from_waypoint)) << 32)
        | static_cast<uint32_t>(to_waypoint);
}

bool is_forward_probe_over_edge(
    const rf::Entity& entity,
    const rf::Vector3& direction,
    const float distance);
bool is_forward_probe_blocked(
    const rf::Entity& entity,
    const rf::Vector3& direction,
    const float distance);

bool trace_floor_below_waypoint_pos(
    const rf::Vector3& waypoint_pos,
    rf::Vector3& out_floor_pos)
{
    rf::Vector3 p0 = waypoint_pos;
    rf::Vector3 p1 = waypoint_pos;
    p0.y += kLinkFloorProbeStartYOffset;
    p1.y -= kLinkFloorProbeDownDistance;

    rf::GCollisionOutput collision{};
    if (!rf::collide_linesegment_level_solid(
            p0,
            p1,
            kBotMovementTraceFlags,
            &collision)) {
        return false;
    }

    out_floor_pos = collision.hit_point;
    return true;
}

bool compute_upward_link_requires_jump_uncached(
    const int from_waypoint,
    const int to_waypoint)
{
    rf::Vector3 from_pos{};
    rf::Vector3 to_pos{};
    if (!waypoints_get_pos(from_waypoint, from_pos)
        || !waypoints_get_pos(to_waypoint, to_pos)
        || to_pos.y <= from_pos.y + kWaypointLinkRadiusEpsilon) {
        return false;
    }

    rf::Vector3 from_floor{};
    rf::Vector3 to_floor{};
    if (!trace_floor_below_waypoint_pos(from_pos, from_floor)
        || !trace_floor_below_waypoint_pos(to_pos, to_floor)
        || to_floor.y <= from_floor.y + kWaypointLinkRadiusEpsilon) {
        return false;
    }

    rf::Vector3 p0 = from_floor;
    rf::Vector3 p1 = to_floor;
    p0.y += kLinkFloorLinkTraceYOffset;
    p1.y += kLinkFloorLinkTraceYOffset;
    rf::GCollisionOutput collision{};
    return rf::collide_linesegment_level_solid(
        p0,
        p1,
        kBotMovementTraceFlags,
        &collision);
}

bool upward_link_requires_jump_for_obstruction(
    const int from_waypoint,
    const int to_waypoint)
{
    if (from_waypoint <= 0
        || to_waypoint <= 0
        || !bot_nav_is_link_traversable_for_route(from_waypoint, to_waypoint)) {
        return false;
    }

    const int waypoint_total = waypoints_count();
    if (g_upward_link_obstacle_cache_waypoint_total != waypoint_total) {
        g_upward_link_obstacle_cache_waypoint_total = waypoint_total;
        g_upward_link_obstacle_cache.clear();
    }
    else if (static_cast<int>(g_upward_link_obstacle_cache.size())
        > kUpwardLinkObstacleCacheMaxEntries) {
        bool keep = false;
        for (auto it = g_upward_link_obstacle_cache.begin();
             it != g_upward_link_obstacle_cache.end(); ) {
            if (keep) { ++it; } else { it = g_upward_link_obstacle_cache.erase(it); }
            keep = !keep;
        }
    }

    const uint64_t edge_key = make_directed_link_key(from_waypoint, to_waypoint);
    if (auto it = g_upward_link_obstacle_cache.find(edge_key);
        it != g_upward_link_obstacle_cache.end()) {
        return it->second;
    }

    const bool requires_jump =
        compute_upward_link_requires_jump_uncached(from_waypoint, to_waypoint);
    g_upward_link_obstacle_cache.emplace(edge_key, requires_jump);
    return requires_jump;
}

bool get_active_waypoint_link(int& out_from_waypoint, int& out_to_waypoint)
{
    out_from_waypoint = 0;
    out_to_waypoint = 0;
    if (g_client_bot_state.waypoint_path.empty()
        || g_client_bot_state.waypoint_next_index <= 0
        || g_client_bot_state.waypoint_next_index
            >= static_cast<int>(g_client_bot_state.waypoint_path.size())) {
        return false;
    }

    out_from_waypoint =
        g_client_bot_state.waypoint_path[g_client_bot_state.waypoint_next_index - 1];
    out_to_waypoint =
        g_client_bot_state.waypoint_path[g_client_bot_state.waypoint_next_index];
    return out_from_waypoint > 0 && out_to_waypoint > 0;
}

void request_soft_repath_around_waypoint(const int avoid_waypoint)
{
    // Skip recovery for roam/none goals — just clear the route and let normal
    // navigation pick a new path without the recovery anchor system.
    if (g_client_bot_state.active_goal == BotGoalType::roam
        || g_client_bot_state.active_goal == BotGoalType::none) {
        g_client_bot_state.recovery_pending_reroute = false;
        g_client_bot_state.recovery_avoid_waypoint = 0;
        g_client_bot_state.recovery_anchor_waypoint = 0;
        g_client_bot_state.repath_timer.set(kWallAvoidSoftRepathMs);
        return;
    }
    g_client_bot_state.recovery_pending_reroute = true;
    g_client_bot_state.recovery_avoid_waypoint = avoid_waypoint;
    g_client_bot_state.recovery_anchor_waypoint = 0;
    g_client_bot_state.repath_timer.set(kWallAvoidSoftRepathMs);
}

void reroute_around_forward_obstacle()
{
    bot_internal_set_last_heading_change_reason("forward_obstacle_stuck");
    int from_waypoint = 0;
    int to_waypoint = 0;
    if (get_active_waypoint_link(from_waypoint, to_waypoint)) {
        const int blacklist_waypoint =
            bot_nav_choose_blacklist_waypoint_for_failed_link(from_waypoint, to_waypoint);
        if (blacklist_waypoint > 0) {
            bot_nav_blacklist_waypoint_temporarily(
                blacklist_waypoint,
                kFailedWaypointBlacklistMs
            );
        }
        if (!bot_nav_is_failed_edge_cooldown_active_no_prune(from_waypoint, to_waypoint)) {
            bot_nav_register_failed_edge_cooldown(
                from_waypoint,
                to_waypoint,
                kFailedEdgeCooldownMs
            );
        }
        request_soft_repath_around_waypoint(
            blacklist_waypoint > 0 ? blacklist_waypoint : to_waypoint
        );
        return;
    }

    request_soft_repath_around_waypoint(g_client_bot_state.waypoint_goal);
}

bool compute_link_guidance_direction(
    const rf::Entity& local_entity,
    rf::Vector3& out_world_dir)
{
    int from_waypoint = 0;
    int to_waypoint = 0;
    if (!get_active_waypoint_link(from_waypoint, to_waypoint)) {
        return false;
    }

    rf::Vector3 from_pos{};
    rf::Vector3 to_pos{};
    if (!waypoints_get_pos(from_waypoint, from_pos)
        || !waypoints_get_pos(to_waypoint, to_pos)) {
        return false;
    }

    rf::Vector3 segment = to_pos - from_pos;
    segment.y = 0.0f;
    const float segment_len_sq = segment.len_sq();
    if (segment_len_sq <= 0.0001f) {
        return false;
    }
    const float segment_len = std::sqrt(segment_len_sq);
    segment.normalize_safe();

    rf::Vector3 from_to_bot = local_entity.pos - from_pos;
    from_to_bot.y = 0.0f;
    const float t = std::clamp(from_to_bot.dot_prod(segment), 0.0f, segment_len);
    rf::Vector3 closest = from_pos + segment * t;
    rf::Vector3 right{segment.z, 0.0f, -segment.x};
    right.normalize_safe();
    const float lateral_offset = (local_entity.pos - closest).dot_prod(right);
    const float lateral_correction = std::clamp(lateral_offset * 0.65f, -0.80f, 0.80f);

    const float lookahead = std::clamp(segment_len * 0.30f, 0.60f, 1.60f);
    const float t_lookahead = std::clamp(t + lookahead, 0.0f, segment_len);
    rf::Vector3 guidance_point = from_pos + segment * t_lookahead - right * lateral_correction;
    guidance_point.y = local_entity.pos.y;

    out_world_dir = guidance_point - local_entity.pos;
    out_world_dir.y = 0.0f;
    return out_world_dir.len_sq() > 0.0001f;
}

bool active_link_is_approximately_level(
    const int from_waypoint,
    const int to_waypoint)
{
    if (from_waypoint <= 0 || to_waypoint <= 0) {
        return false;
    }

    rf::Vector3 from_pos{};
    rf::Vector3 to_pos{};
    if (!waypoints_get_pos(from_waypoint, from_pos)
        || !waypoints_get_pos(to_waypoint, to_pos)) {
        return false;
    }

    return std::fabs(to_pos.y - from_pos.y) <= kLinkGuidanceLevelRelaxThreshold;
}

bool open_terrain_edge_probes_safe(
    const rf::Entity& entity,
    const rf::Vector3& desired_world_dir)
{
    rf::Vector3 probe_forward = desired_world_dir;
    probe_forward.y = 0.0f;
    if (probe_forward.len_sq() < 0.001f) {
        probe_forward = forward_from_non_linear_yaw_pitch(
            entity.control_data.phb.y,
            0.0f
        );
        probe_forward.y = 0.0f;
    }
    if (probe_forward.len_sq() < 0.001f) {
        return false;
    }
    probe_forward.normalize_safe();

    rf::Vector3 probe_right{probe_forward.z, 0.0f, -probe_forward.x};
    if (probe_right.len_sq() < 0.001f) {
        return false;
    }
    probe_right.normalize_safe();

    rf::Vector3 forward_right = probe_forward + probe_right;
    rf::Vector3 forward_left = probe_forward - probe_right;
    if (forward_right.len_sq() < 0.001f || forward_left.len_sq() < 0.001f) {
        return false;
    }
    forward_right.normalize_safe();
    forward_left.normalize_safe();

    const auto direction_has_safe_ground = [&](const rf::Vector3& dir) {
        if (is_forward_probe_blocked(entity, dir, kLinkGuidanceRelaxEdgeProbeDistance)) {
            return true;
        }
        return !is_forward_probe_over_edge(entity, dir, kLinkGuidanceRelaxEdgeProbeDistance);
    };

    return direction_has_safe_ground(probe_forward)
        && direction_has_safe_ground(forward_right)
        && direction_has_safe_ground(forward_left);
}

bool is_forward_probe_blocked(
    const rf::Entity& entity,
    const rf::Vector3& direction,
    const float distance)
{
    if (distance <= 0.0f || direction.len_sq() < 0.0001f) {
        return false;
    }

    rf::Vector3 dir = direction;
    dir.y = 0.0f;
    if (dir.len_sq() < 0.0001f) {
        return false;
    }
    dir.normalize_safe();

    rf::Vector3 p0 = entity.pos + rf::Vector3{0.0f, kForwardProbeOriginYOffset, 0.0f};
    rf::Vector3 p1 = p0 + dir * distance;
    rf::GCollisionOutput collision{};
    return rf::collide_linesegment_level_solid(
        p0,
        p1,
        kBotMovementTraceFlags,
        &collision);
}

bool is_forward_probe_over_edge(
    const rf::Entity& entity,
    const rf::Vector3& direction,
    const float distance)
{
    if (distance <= 0.0f || direction.len_sq() < 0.0001f) {
        return false;
    }

    rf::Vector3 dir = direction;
    dir.y = 0.0f;
    if (dir.len_sq() < 0.0001f) {
        return false;
    }
    dir.normalize_safe();

    const rf::Vector3 endpoint =
        entity.pos + rf::Vector3{0.0f, kForwardProbeOriginYOffset, 0.0f} + dir * distance;
    rf::Vector3 p0 = endpoint;
    rf::Vector3 p1 = endpoint;
    p0.y += kForwardEdgeProbeStartYOffset;
    p1.y -= kForwardEdgeProbeDownDistance;
    rf::GCollisionOutput collision{};
    if (!rf::collide_linesegment_level_solid(
            p0,
            p1,
            kBotMovementTraceFlags,
            &collision)) {
        return true;
    }

    const float drop_height = p0.y - collision.hit_point.y;
    return drop_height > kForwardEdgeUnsafeDropHeight;
}

void reset_forward_obstacle_progress_tracking()
{
    g_client_bot_state.forward_obstacle_progress_timer.invalidate();
    g_client_bot_state.forward_obstacle_progress_origin = {};
}

bool is_forward_obstacle_stuck(const rf::Entity& entity)
{
    if (!g_client_bot_state.forward_obstacle_progress_timer.valid()) {
        g_client_bot_state.forward_obstacle_progress_timer.set(
            kNavigationBlockedProgressWindowMs
        );
        g_client_bot_state.forward_obstacle_progress_origin = entity.pos;
        return false;
    }

    if (!g_client_bot_state.forward_obstacle_progress_timer.elapsed()) {
        return false;
    }

    rf::Vector3 moved = entity.pos - g_client_bot_state.forward_obstacle_progress_origin;
    moved.y = 0.0f;
    g_client_bot_state.forward_obstacle_progress_timer.set(
        kNavigationBlockedProgressWindowMs
    );
    g_client_bot_state.forward_obstacle_progress_origin = entity.pos;
    return moved.len_sq()
        < kNavigationBlockedProgressDistance * kNavigationBlockedProgressDistance;
}

bool waypoint_requires_crouch(const int waypoint)
{
    int movement_subtype = static_cast<int>(WaypointDroppedSubtype::normal);
    if (!waypoints_get_movement_subtype(waypoint, movement_subtype)) {
        return false;
    }
    return movement_subtype == static_cast<int>(WaypointDroppedSubtype::crouch_needed);
}

bool waypoint_is_without_crouch_requirement(const int waypoint)
{
    int movement_subtype = static_cast<int>(WaypointDroppedSubtype::normal);
    if (!waypoints_get_movement_subtype(waypoint, movement_subtype)) {
        return false;
    }
    return movement_subtype != static_cast<int>(WaypointDroppedSubtype::crouch_needed);
}

bool waypoint_is_unsafe_terrain(const int waypoint)
{
    int movement_subtype = static_cast<int>(WaypointDroppedSubtype::normal);
    if (!waypoints_get_movement_subtype(waypoint, movement_subtype)) {
        return false;
    }
    return movement_subtype == static_cast<int>(WaypointDroppedSubtype::unsafe_terrain);
}

bool should_jump_to_cross_gap_link(
    const rf::Entity& local_entity,
    const int from_waypoint,
    const int to_waypoint)
{
    if (!bot_nav_is_link_traversable_for_route(from_waypoint, to_waypoint)
        || bot_nav_link_midpoint_has_support(from_waypoint, to_waypoint)) {
        return false;
    }

    rf::Vector3 from_pos{};
    rf::Vector3 to_pos{};
    if (!waypoints_get_pos(from_waypoint, from_pos)
        || !waypoints_get_pos(to_waypoint, to_pos)) {
        return false;
    }

    rf::Vector3 segment = to_pos - from_pos;
    segment.y = 0.0f;
    const float segment_len_sq = segment.len_sq();
    if (segment_len_sq <= 0.04f) {
        return false;
    }

    rf::Vector3 from_to_entity = local_entity.pos - from_pos;
    from_to_entity.y = 0.0f;
    const float progress_t = std::clamp(
        from_to_entity.dot_prod(segment) / segment_len_sq,
        0.0f,
        1.0f
    );

    const rf::Vector3 closest = from_pos + segment * progress_t;
    const float dx = local_entity.pos.x - closest.x;
    const float dz = local_entity.pos.z - closest.z;
    const float lateral_dist_sq = dx * dx + dz * dz;
    const float corridor =
        std::max(kWaypointLinkCorridorRadius, 2.25f) + 0.75f;
    const float corridor_sq = corridor * corridor;
    if (lateral_dist_sq > corridor_sq) {
        return false;
    }

    return progress_t >= 0.08f && progress_t <= 0.72f;
}

bool should_jump_to_clear_obstructed_upward_link(
    const rf::Entity& local_entity,
    const int from_waypoint,
    const int to_waypoint)
{
    if (!upward_link_requires_jump_for_obstruction(from_waypoint, to_waypoint)) {
        return false;
    }

    rf::Vector3 from_pos{};
    rf::Vector3 to_pos{};
    if (!waypoints_get_pos(from_waypoint, from_pos)
        || !waypoints_get_pos(to_waypoint, to_pos)) {
        return false;
    }

    rf::Vector3 segment = to_pos - from_pos;
    segment.y = 0.0f;
    const float segment_len_sq = segment.len_sq();
    if (segment_len_sq <= 0.04f) {
        return false;
    }

    rf::Vector3 from_to_entity = local_entity.pos - from_pos;
    from_to_entity.y = 0.0f;
    const float progress_t = std::clamp(
        from_to_entity.dot_prod(segment) / segment_len_sq,
        0.0f,
        1.0f
    );

    return progress_t >= 0.10f && progress_t <= 0.72f;
}

void update_traversal_crouch_state_for_waypoint_path(
    const rf::Entity& local_entity,
    const rf::Vector3& move_target)
{
    const bool has_valid_path_target =
        !g_client_bot_state.waypoint_path.empty()
        && g_client_bot_state.waypoint_next_index >= 0
        && g_client_bot_state.waypoint_next_index
            < static_cast<int>(g_client_bot_state.waypoint_path.size());
    if (!has_valid_path_target) {
        g_client_bot_state.traversal_crouch_active = false;
        return;
    }

    // waypoint_next_index is the destination of the currently traversed link.
    // If that destination requires crouching, start crouching while traversing in.
    const int target_waypoint =
        g_client_bot_state.waypoint_path[g_client_bot_state.waypoint_next_index];
    if (waypoint_requires_crouch(target_waypoint)) {
        g_client_bot_state.traversal_crouch_active = true;
        return;
    }

    if (!waypoint_is_without_crouch_requirement(target_waypoint)) {
        return;
    }

    const float reach_dist_sq = kWaypointReachRadius * kWaypointReachRadius;
    const float dist_sq_to_target = rf::vec_dist_squared(&local_entity.pos, &move_target);
    if (dist_sq_to_target <= reach_dist_sq) {
        g_client_bot_state.traversal_crouch_active = false;
    }
}

void apply_corner_centering_steer(
    const rf::Entity& local_entity,
    const rf::Vector3& desired_world_dir,
    const float desired_move_z,
    const bool suppress_edge_checks,
    float& in_out_move_x)
{
    const float corner_centering_bias = std::clamp(
        get_active_bot_personality().corner_centering_bias,
        0.0f,
        3.0f
    );
    if (corner_centering_bias <= 0.01f || desired_move_z <= 0.25f) {
        g_client_bot_state.corner_steer_probe_timer.invalidate();
        return;
    }

    if (!g_client_bot_state.corner_steer_probe_timer.valid()) {
        g_client_bot_state.corner_steer_probe_timer.set(kCornerSteerProbeIntervalMs);
    }
    if (!g_client_bot_state.corner_steer_probe_timer.elapsed()) {
        return;
    }
    g_client_bot_state.corner_steer_probe_timer.set(kCornerSteerProbeIntervalMs);

    rf::Vector3 probe_forward = forward_from_non_linear_yaw_pitch(
        local_entity.control_data.phb.y,
        0.0f
    );
    probe_forward.y = 0.0f;
    if (probe_forward.len_sq() < 0.001f) {
        probe_forward = desired_world_dir;
        probe_forward.y = 0.0f;
    }
    if (probe_forward.len_sq() < 0.001f) {
        return;
    }
    probe_forward.normalize_safe();

    rf::Vector3 probe_right{probe_forward.z, 0.0f, -probe_forward.x};
    if (probe_right.len_sq() < 0.001f) {
        return;
    }
    probe_right.normalize_safe();

    rf::Vector3 forward_right = probe_forward + probe_right;
    rf::Vector3 forward_left = probe_forward - probe_right;
    if (forward_right.len_sq() < 0.001f || forward_left.len_sq() < 0.001f) {
        return;
    }
    forward_right.normalize_safe();
    forward_left.normalize_safe();

    const bool forward_blocked = is_forward_probe_blocked(
        local_entity,
        probe_forward,
        kCornerSteerProbeDistance
    );
    const bool forward_over_edge =
        !forward_blocked
        && !suppress_edge_checks
        && is_forward_probe_over_edge(
            local_entity,
            probe_forward,
            kCornerSteerProbeDistance
        );
    if (!forward_blocked && !forward_over_edge) {
        // Forward path is currently safe: do not side-steer just for side-wall proximity.
        return;
    }

    const bool forward_right_blocked = is_forward_probe_blocked(
        local_entity,
        forward_right,
        kCornerSteerProbeDistance
    );
    const bool forward_left_blocked = is_forward_probe_blocked(
        local_entity,
        forward_left,
        kCornerSteerProbeDistance
    );
    const bool forward_right_over_edge =
        !suppress_edge_checks
        && !forward_right_blocked
        && is_forward_probe_over_edge(
            local_entity,
            forward_right,
            kCornerSteerProbeDistance
        );
    const bool forward_left_over_edge =
        !suppress_edge_checks
        && !forward_left_blocked
        && is_forward_probe_over_edge(
            local_entity,
            forward_left,
            kCornerSteerProbeDistance
        );

    const bool right_safe = !forward_right_blocked && !forward_right_over_edge;
    const bool left_safe = !forward_left_blocked && !forward_left_over_edge;

    bool steer_right = false;
    bool steer_left = false;
    if (left_safe != right_safe) {
        steer_right = right_safe;
        steer_left = left_safe;
    }

    if (!steer_right && !steer_left) {
        return;
    }

    const float steer_strength = std::clamp(
        0.55f * corner_centering_bias,
        0.0f,
        kCornerSteerMaxStrafe
    );
    if (steer_strength <= 0.0f) {
        return;
    }

    if (steer_right) {
        in_out_move_x = std::clamp(in_out_move_x + steer_strength, -1.0f, 1.0f);
    }
    else if (steer_left) {
        in_out_move_x = std::clamp(in_out_move_x - steer_strength, -1.0f, 1.0f);
    }
}

void issue_movement_actions(
    rf::Player& player,
    const float local_move_x,
    const float local_move_y,
    const float local_move_z,
    const bool jump)
{
    g_client_bot_state.movement_override_active = true;
    const float target_x = std::clamp(local_move_x, -1.0f, 1.0f);
    const float target_y = std::clamp(local_move_y, -1.0f, 1.0f);
    const float target_z = std::clamp(local_move_z, -1.0f, 1.0f);
    constexpr float deadzone = 0.05f;
    g_client_bot_state.move_input_x = (std::fabs(target_x) > deadzone) ? target_x : 0.0f;
    g_client_bot_state.move_input_y = (std::fabs(target_y) > deadzone) ? target_y : 0.0f;
    g_client_bot_state.move_input_z = (std::fabs(target_z) > deadzone) ? target_z : 0.0f;

    if (jump) {
        rf::player_execute_action(&player, rf::CC_ACTION_JUMP, true);
    }
}
}

void bot_process_movement(
    rf::Player& local_player,
    const rf::Entity& local_entity,
    const rf::Vector3& move_target,
    const bool pursuing_enemy_goal,
    const bool enemy_has_los,
    const float skill_factor)
{
    int active_from_waypoint = 0;
    int active_to_waypoint = 0;
    const bool has_active_link =
        get_active_waypoint_link(active_from_waypoint, active_to_waypoint);
    int active_from_type_raw = 0;
    int active_from_subtype = 0;
    int active_to_type_raw = 0;
    int active_to_subtype = 0;
    const bool has_active_from_type =
        has_active_link
        && waypoints_get_type_subtype(
            active_from_waypoint,
            active_from_type_raw,
            active_from_subtype);
    const bool has_active_to_type =
        has_active_link
        && waypoints_get_type_subtype(
            active_to_waypoint,
            active_to_type_raw,
            active_to_subtype);
    const bool active_link_changed =
        has_active_link
        && (active_from_waypoint != g_client_bot_state.jump_target_link_from_waypoint
            || active_to_waypoint != g_client_bot_state.jump_target_link_to_waypoint);
    if (has_active_link && active_link_changed) {
        g_client_bot_state.jump_target_link_from_waypoint = active_from_waypoint;
        g_client_bot_state.jump_target_link_to_waypoint = active_to_waypoint;
    }
    else if (!has_active_link) {
        g_client_bot_state.jump_target_link_from_waypoint = 0;
        g_client_bot_state.jump_target_link_to_waypoint = 0;
    }
    const bool active_to_is_jump_pad =
        has_active_to_type
        && static_cast<WaypointType>(active_to_type_raw) == WaypointType::jump_pad;
    const bool active_link_involves_ladder =
        (has_active_from_type
            && static_cast<WaypointType>(active_from_type_raw) == WaypointType::ladder)
        || (has_active_to_type
            && static_cast<WaypointType>(active_to_type_raw) == WaypointType::ladder);
    const bool active_link_route_traversable =
        has_active_link
        && bot_nav_is_link_traversable_for_route(active_from_waypoint, active_to_waypoint);
    const bool active_link_is_gap =
        has_active_link
        && !bot_nav_link_midpoint_has_support(active_from_waypoint, active_to_waypoint);
    const bool active_link_from_unsafe_terrain =
        has_active_link
        && waypoint_is_unsafe_terrain(active_from_waypoint);

    const rf::Vector3 to_target = move_target - local_entity.pos;
    rf::Vector3 climb_region_probe = local_entity.pos;
    const bool in_climb_region = rf::level_point_in_climb_region(&climb_region_probe) != nullptr;
    const bool ladder_traversal_active =
        g_client_bot_state.has_waypoint_target
        && has_active_link
        && active_link_involves_ladder
        && in_climb_region;
    float ladder_move_y = 0.0f;
    if (ladder_traversal_active) {
        if (to_target.y > kLadderClimbVerticalThreshold) {
            ladder_move_y = 1.0f;
        }
        else if (to_target.y < -kLadderClimbVerticalThreshold) {
            ladder_move_y = -1.0f;
        }
    }
    rf::Vector3 desired_world_dir = to_target;
    desired_world_dir.y = 0.0f;
    const float horizontal_dist = desired_world_dir.len();
    if (g_client_bot_state.has_waypoint_target
        && bot_internal_is_following_waypoint_link(local_entity)) {
        rf::Vector3 link_guidance_dir{};
        if (compute_link_guidance_direction(local_entity, link_guidance_dir)) {
            float link_guidance_blend = kLinkGuidanceBlendWeight;
            if (has_active_link) {
                const bool level_link =
                    active_link_is_approximately_level(active_from_waypoint, active_to_waypoint);
                const bool not_near_ledge =
                    !active_link_is_gap
                    && !active_link_from_unsafe_terrain
                    && open_terrain_edge_probes_safe(local_entity, desired_world_dir);
                if (level_link || not_near_ledge) {
                    link_guidance_blend = kLinkGuidanceBlendRelaxedWeight;
                }
            }
            // Favor the active link centerline so turning doesn't cut corners into walls.
            desired_world_dir =
                desired_world_dir * (1.0f - link_guidance_blend)
                + link_guidance_dir * link_guidance_blend;
        }
    }
    update_traversal_crouch_state_for_waypoint_path(local_entity, move_target);
    const bool collecting_item =
        bot_goal_is_item_collection(g_client_bot_state.active_goal)
        || g_client_bot_state.fsm_state == BotFsmState::collect_pickup
        || g_client_bot_state.fsm_state == BotFsmState::seek_weapon
        || g_client_bot_state.fsm_state == BotFsmState::replenish_health_armor;
    const bool activating_bridge =
        g_client_bot_state.active_goal == BotGoalType::activate_bridge
        || g_client_bot_state.fsm_state == BotFsmState::activate_bridge;
    const bool creating_crater =
        g_client_bot_state.active_goal == BotGoalType::create_crater
        || g_client_bot_state.fsm_state == BotFsmState::create_crater;
    const bool shattering_glass =
        g_client_bot_state.active_goal == BotGoalType::shatter_glass
        || g_client_bot_state.fsm_state == BotFsmState::shatter_glass;
    const bool ctf_objective =
        bot_goal_is_ctf_objective(g_client_bot_state.active_goal)
        || g_client_bot_state.fsm_state == BotFsmState::ctf_objective;
    const bool control_point_objective =
        bot_goal_is_control_point_objective(g_client_bot_state.active_goal)
        || g_client_bot_state.fsm_state == BotFsmState::control_point_objective;

    float stop_distance = 2.5f;
    if (pursuing_enemy_goal && enemy_has_los) {
        const BotPersonality& personality = get_active_bot_personality();
        // LOS maintenance has priority over holding preferred engagement distance.
        if (g_client_bot_state.has_waypoint_target) {
            stop_distance = 1.0f;
        }
        else {
            stop_distance = std::lerp(
                personality.preferred_engagement_far,
                personality.preferred_engagement_near,
                skill_factor
            );
        }
    }
    else if (pursuing_enemy_goal) {
        stop_distance = 1.0f;
    }
    else if (collecting_item) {
        // While following waypoint links, keep normal traversal tolerance to
        // avoid oscillating around exact waypoint centers. Only force near-zero
        // stop distance on the final direct approach to the pickup itself.
        stop_distance = g_client_bot_state.has_waypoint_target ? 0.85f : 0.05f;
    }
    else if (activating_bridge) {
        // Bridge activation requires the entity origin to enter trigger volume.
        stop_distance = 0.10f;
    }
    else if (creating_crater || shattering_glass) {
        // Crater targets should be approached closely for stable aim.
        stop_distance = 0.35f;
    }
    else if (ctf_objective) {
        if (g_client_bot_state.active_goal == BotGoalType::ctf_hold_enemy_flag) {
            // Keep moving through patrol anchors while holding enemy flag.
            stop_distance = g_client_bot_state.has_waypoint_target ? 0.85f : 0.05f;
        }
        else {
            // Use normal tolerance while following route waypoints, and only
            // force run-through distance on the final direct approach.
            stop_distance = g_client_bot_state.has_waypoint_target ? 0.85f : 0.05f;
        }
    }
    else if (control_point_objective) {
        // CP objective traversal should keep momentum across waypoint links and
        // run through the direct objective center once close.
        stop_distance = g_client_bot_state.has_waypoint_target ? 0.85f : 0.10f;
    }
    else if (g_client_bot_state.active_goal == BotGoalType::roam
        || g_client_bot_state.active_goal == BotGoalType::none) {
        // Roam should always keep moving through waypoints, never stop near them.
        stop_distance = g_client_bot_state.has_waypoint_target ? 0.65f : 0.10f;
    }
    if (active_to_is_jump_pad) {
        stop_distance = kJumpPadFinalApproachStopDistance;
    }

    float desired_move_x = 0.0f;
    float desired_move_z = 0.0f;
    if (horizontal_dist > stop_distance) {
        desired_world_dir.normalize_safe();
        const rf::Vector3 view_forward = forward_from_non_linear_yaw_pitch(
            local_entity.control_data.phb.y,
            0.0f
        );
        const rf::Vector3 view_right{view_forward.z, 0.0f, -view_forward.x};

        desired_move_z = std::clamp(desired_world_dir.dot_prod(view_forward), -1.0f, 1.0f);
        desired_move_x = std::clamp(desired_world_dir.dot_prod(view_right), -1.0f, 1.0f);
    }
    else if (pursuing_enemy_goal && enemy_has_los && horizontal_dist < stop_distance * 0.45f) {
        desired_move_z = -0.45f;
    }

    const BotPersonality& personality = get_active_bot_personality();
    const bool allow_navigation_strafe =
        !pursuing_enemy_goal
        && !activating_bridge
        && !creating_crater
        && !shattering_glass
        && !ladder_traversal_active
        && horizontal_dist > stop_distance
        && desired_move_z > 0.15f;
    if (allow_navigation_strafe) {
        // 1.0 means neutral split between direct forward and strafe-oriented navigation.
        const float nav_strafe_ratio = std::clamp(
            std::clamp(personality.navigation_strafe_bias, 0.0f, 2.0f) * 0.5f,
            0.0f,
            1.0f
        );
        if (nav_strafe_ratio > 0.001f) {
            float strafe_sign = 0.0f;
            if (std::fabs(desired_move_x) > 0.08f) {
                strafe_sign = desired_move_x > 0.0f ? 1.0f : -1.0f;
            }
            else {
                const int sign_basis = (has_active_link && active_to_waypoint > 0)
                    ? active_to_waypoint
                    : std::max(g_client_bot_state.waypoint_goal, 1);
                strafe_sign = (sign_basis & 1) == 0 ? 1.0f : -1.0f;
            }

            const float proximity_scale = std::clamp(
                (horizontal_dist - std::max(stop_distance, 0.2f)) / 3.0f,
                0.0f,
                1.0f
            );
            const float forward_mag = std::clamp(desired_move_z, 0.0f, 1.0f);
            const float target_strafe_mag = std::clamp(
                forward_mag
                    * std::lerp(0.10f, 0.80f, nav_strafe_ratio)
                    * proximity_scale,
                0.0f,
                0.95f
            );
            const float blend = std::clamp(0.25f + nav_strafe_ratio * 0.45f, 0.0f, 0.90f);
            desired_move_x = std::clamp(
                std::lerp(desired_move_x, strafe_sign * target_strafe_mag, blend),
                -1.0f,
                1.0f
            );
            desired_move_z = std::clamp(
                std::lerp(
                    desired_move_z,
                    std::max(0.22f, forward_mag * std::lerp(1.0f, 0.60f, nav_strafe_ratio)),
                    blend * 0.65f
                ),
                -1.0f,
                1.0f
            );
        }
    }

    const bool respawn_gearup_active =
        g_client_bot_state.respawn_gearup_timer.valid()
        && !g_client_bot_state.respawn_gearup_timer.elapsed();

    bool maneuver_jump = false;
    bool combat_crouch_hold = false;
    if (pursuing_enemy_goal && enemy_has_los && !respawn_gearup_active) {
        const BotSkillProfile& skill_profile = get_active_bot_skill_profile();

        if (!g_client_bot_state.combat_maneuver_timer.valid()
            || g_client_bot_state.combat_maneuver_timer.elapsed()) {
            const float dodge_chance = std::clamp(
                skill_profile.dodge_likelihood
                    * std::clamp(personality.dodge_combat_bias, 0.25f, 2.5f),
                0.0f,
                1.0f
            );
            const float crouch_chance = std::clamp(
                skill_profile.crouch_likelihood
                    * std::clamp(personality.crouch_combat_bias, 0.25f, 2.5f),
                0.0f,
                1.0f
            );
            const float jump_chance = std::clamp(
                skill_profile.jump_likelihood
                    * std::clamp(personality.jump_combat_bias, 0.25f, 2.5f),
                0.0f,
                1.0f
            );
            std::uniform_real_distribution<float> roll_dist(0.0f, 1.0f);

            if (roll_dist(g_rng) <= dodge_chance) {
                std::uniform_int_distribution<int> strafe_sign(0, 1);
                const float strafe_strength = std::lerp(0.35f, 0.9f, skill_factor);
                g_client_bot_state.combat_maneuver_strafe =
                    strafe_sign(g_rng) == 0 ? -strafe_strength : strafe_strength;
            }
            else {
                g_client_bot_state.combat_maneuver_strafe = 0.0f;
            }

            if (roll_dist(g_rng) <= crouch_chance) {
                const int crouch_ms = static_cast<int>(std::lround(
                    std::lerp(220.0f, 520.0f, std::clamp(crouch_chance, 0.0f, 1.0f))
                ));
                g_client_bot_state.combat_crouch_timer.set(crouch_ms);
            }

            if (roll_dist(g_rng) <= jump_chance
                && (!g_client_bot_state.jump_variation_timer.valid()
                    || g_client_bot_state.jump_variation_timer.elapsed())
                && (!g_client_bot_state.jump_timer.valid()
                    || g_client_bot_state.jump_timer.elapsed())) {
                maneuver_jump = true;
                g_client_bot_state.jump_variation_timer.set(650);
                g_client_bot_state.jump_timer.set(550);
            }

            const int maneuver_ms = static_cast<int>(std::lround(
                std::lerp(460.0f, 180.0f, std::clamp(skill_factor, 0.0f, 1.0f))
            ));
            g_client_bot_state.combat_maneuver_timer.set(maneuver_ms);
        }

        desired_move_x = std::clamp(
            desired_move_x + g_client_bot_state.combat_maneuver_strafe,
            -1.0f,
            1.0f
        );
        combat_crouch_hold =
            g_client_bot_state.combat_crouch_timer.valid()
            && !g_client_bot_state.combat_crouch_timer.elapsed();
    }
    else {
        g_client_bot_state.combat_maneuver_strafe = 0.0f;
        g_client_bot_state.combat_maneuver_timer.invalidate();
        g_client_bot_state.combat_crouch_timer.invalidate();
    }

    apply_corner_centering_steer(
        local_entity,
        desired_world_dir,
        desired_move_z,
        active_link_is_gap || active_link_from_unsafe_terrain,
        desired_move_x
    );

    const bool suppress_forward_obstacle_checks_for_upward_link =
        active_link_route_traversable
        && g_client_bot_state.has_waypoint_target
        && to_target.y > 0.25f
        && horizontal_dist > stop_distance;
    const bool suppress_forward_obstacle_checks =
        suppress_forward_obstacle_checks_for_upward_link
        || ladder_traversal_active;
    const bool suppress_forward_edge_checks =
        active_link_is_gap || active_link_from_unsafe_terrain;

    if (desired_move_z > 0.25f && !suppress_forward_obstacle_checks) {
        if (!g_client_bot_state.forward_obstacle_guard_timer.valid()) {
            g_client_bot_state.forward_obstacle_guard_timer.set(
                kForwardObstacleGuardIntervalMs
            );
        }

        if (g_client_bot_state.forward_obstacle_guard_timer.elapsed()) {
            g_client_bot_state.forward_obstacle_guard_timer.set(
                kForwardObstacleGuardIntervalMs
            );
            rf::Vector3 probe_forward = desired_world_dir;
            probe_forward.y = 0.0f;
            if (probe_forward.len_sq() < 0.001f) {
                probe_forward = forward_from_non_linear_yaw_pitch(
                    local_entity.control_data.phb.y,
                    0.0f
                );
                probe_forward.y = 0.0f;
            }

            const bool forward_blocked = is_forward_probe_blocked(
                local_entity,
                probe_forward,
                kForwardObstacleProbeDistance
            );
            const bool can_sample_edge_drop = !suppress_forward_edge_checks;
            const bool forward_over_edge =
                !forward_blocked
                && can_sample_edge_drop
                && is_forward_probe_over_edge(
                    local_entity,
                    probe_forward,
                    kForwardObstacleProbeDistance);
            if (forward_blocked) {
                rf::Vector3 probe_right{probe_forward.z, 0.0f, -probe_forward.x};
                if (probe_right.len_sq() > 0.001f) {
                    probe_right.normalize_safe();
                    rf::Vector3 forward_right = probe_forward + probe_right;
                    rf::Vector3 forward_left = probe_forward - probe_right;
                    const bool can_sample_diagonals =
                        forward_right.len_sq() > 0.001f && forward_left.len_sq() > 0.001f;
                    bool forward_right_blocked = true;
                    bool forward_left_blocked = true;
                    if (can_sample_diagonals) {
                        forward_right.normalize_safe();
                        forward_left.normalize_safe();
                        forward_right_blocked = is_forward_probe_blocked(
                            local_entity,
                            forward_right,
                            kForwardObstacleProbeDistance
                        );
                        forward_left_blocked = is_forward_probe_blocked(
                            local_entity,
                            forward_left,
                            kForwardObstacleProbeDistance
                        );
                    }

                    if (can_sample_diagonals && forward_left_blocked != forward_right_blocked) {
                        // Prefer steering toward the open diagonal and keep the current objective.
                        const float steer_strength = std::clamp(
                            0.35f + 0.35f * std::clamp(
                                get_active_bot_personality().corner_centering_bias,
                                0.0f,
                                3.0f),
                            0.2f,
                            0.9f
                        );
                        if (forward_left_blocked) {
                            desired_move_x = std::clamp(desired_move_x + steer_strength, -1.0f, 1.0f);
                        }
                        else {
                            desired_move_x = std::clamp(desired_move_x - steer_strength, -1.0f, 1.0f);
                        }
                        desired_move_z = std::max(desired_move_z, 0.35f);
                        g_client_bot_state.forward_obstacle_blocked_samples = 0;
                        reset_forward_obstacle_progress_tracking();
                    }
                    else {
                        ++g_client_bot_state.forward_obstacle_blocked_samples;
                        const bool stuck_for_reroute = is_forward_obstacle_stuck(local_entity);
                        const bool reroute_ready =
                            !g_client_bot_state.forward_obstacle_reroute_timer.valid()
                            || g_client_bot_state.forward_obstacle_reroute_timer.elapsed();
                        if (stuck_for_reroute
                            && g_client_bot_state.forward_obstacle_blocked_samples >= kForwardObstacleRerouteSamples
                            && reroute_ready) {
                            reroute_around_forward_obstacle();
                            g_client_bot_state.forward_obstacle_reroute_timer.set(
                                kForwardObstacleRerouteCooldownMs
                            );
                            g_client_bot_state.forward_obstacle_blocked_samples = 0;
                            reset_forward_obstacle_progress_tracking();
                        }

                        // Softly sidestep while preserving forward intent; don't immediately reverse.
                        float side_sign = 0.0f;
                        if (desired_move_x > 0.05f) {
                            side_sign = 1.0f;
                        }
                        else if (desired_move_x < -0.05f) {
                            side_sign = -1.0f;
                        }
                        else {
                            side_sign =
                                (g_client_bot_state.forward_obstacle_blocked_samples % 2 == 0)
                                    ? 1.0f
                                    : -1.0f;
                        }
                        const float steer_strength = std::clamp(
                            0.28f + 0.24f * std::clamp(
                                get_active_bot_personality().corner_centering_bias,
                                0.0f,
                                3.0f),
                            0.2f,
                            0.85f
                        );
                        desired_move_x = std::clamp(
                            desired_move_x + side_sign * steer_strength,
                            -1.0f,
                            1.0f
                        );
                        desired_move_z = std::max(desired_move_z, 0.20f);
                    }
                }
                else {
                    ++g_client_bot_state.forward_obstacle_blocked_samples;
                    const bool stuck_for_reroute = is_forward_obstacle_stuck(local_entity);
                    const bool reroute_ready =
                        !g_client_bot_state.forward_obstacle_reroute_timer.valid()
                        || g_client_bot_state.forward_obstacle_reroute_timer.elapsed();
                    if (stuck_for_reroute
                        && g_client_bot_state.forward_obstacle_blocked_samples >= kForwardObstacleRerouteSamples
                        && reroute_ready) {
                        reroute_around_forward_obstacle();
                        g_client_bot_state.forward_obstacle_reroute_timer.set(
                            kForwardObstacleRerouteCooldownMs
                        );
                        g_client_bot_state.forward_obstacle_blocked_samples = 0;
                        reset_forward_obstacle_progress_tracking();
                    }

                    const float side_sign =
                        (g_client_bot_state.forward_obstacle_blocked_samples % 2 == 0)
                            ? 1.0f
                            : -1.0f;
                    desired_move_x = std::clamp(
                        desired_move_x + side_sign * 0.35f,
                        -1.0f,
                        1.0f
                    );
                    desired_move_z = std::max(desired_move_z, 0.20f);
                }
            }
            else if (forward_over_edge) {
                rf::Vector3 probe_right{probe_forward.z, 0.0f, -probe_forward.x};
                if (probe_right.len_sq() > 0.001f) {
                    probe_right.normalize_safe();
                    rf::Vector3 forward_right = probe_forward + probe_right;
                    rf::Vector3 forward_left = probe_forward - probe_right;
                    const bool can_sample_diagonals =
                        forward_right.len_sq() > 0.001f && forward_left.len_sq() > 0.001f;
                    if (can_sample_diagonals) {
                        forward_right.normalize_safe();
                        forward_left.normalize_safe();
                        const bool right_blocked = is_forward_probe_blocked(
                            local_entity,
                            forward_right,
                            kForwardObstacleProbeDistance
                        );
                        const bool left_blocked = is_forward_probe_blocked(
                            local_entity,
                            forward_left,
                            kForwardObstacleProbeDistance
                        );
                        const bool right_over_edge = !right_blocked
                            && is_forward_probe_over_edge(
                                local_entity,
                                forward_right,
                                kForwardObstacleProbeDistance);
                        const bool left_over_edge = !left_blocked
                            && is_forward_probe_over_edge(
                                local_entity,
                                forward_left,
                                kForwardObstacleProbeDistance);

                        const bool right_safe = !right_blocked && !right_over_edge;
                        const bool left_safe = !left_blocked && !left_over_edge;
                        if (left_safe != right_safe) {
                            const float steer_strength = std::clamp(
                                0.30f + 0.30f * std::clamp(
                                    get_active_bot_personality().corner_centering_bias,
                                    0.0f,
                                    3.0f),
                                0.20f,
                                0.90f
                            );
                            if (left_safe) {
                                desired_move_x = std::clamp(desired_move_x - steer_strength, -1.0f, 1.0f);
                            }
                            else {
                                desired_move_x = std::clamp(desired_move_x + steer_strength, -1.0f, 1.0f);
                            }
                            desired_move_z = std::max(desired_move_z, 0.35f);
                            g_client_bot_state.forward_obstacle_blocked_samples = 0;
                            reset_forward_obstacle_progress_tracking();
                        }
                        else {
                            ++g_client_bot_state.forward_obstacle_blocked_samples;
                            const bool stuck_for_reroute = is_forward_obstacle_stuck(local_entity);
                            const bool reroute_ready =
                                !g_client_bot_state.forward_obstacle_reroute_timer.valid()
                                || g_client_bot_state.forward_obstacle_reroute_timer.elapsed();
                            if (stuck_for_reroute
                                && g_client_bot_state.forward_obstacle_blocked_samples >= kForwardObstacleRerouteSamples
                                && reroute_ready) {
                                reroute_around_forward_obstacle();
                                g_client_bot_state.forward_obstacle_reroute_timer.set(
                                    kForwardObstacleRerouteCooldownMs
                                );
                                g_client_bot_state.forward_obstacle_blocked_samples = 0;
                                reset_forward_obstacle_progress_tracking();
                            }
                        }
                    }
                }
            }
            else {
                g_client_bot_state.forward_obstacle_blocked_samples = 0;
                reset_forward_obstacle_progress_tracking();
            }
        }
    }
    else {
        g_client_bot_state.forward_obstacle_guard_timer.invalidate();
        g_client_bot_state.forward_obstacle_blocked_samples = 0;
        reset_forward_obstacle_progress_tracking();
    }

    const bool should_hold_crouch =
        !respawn_gearup_active
        && (combat_crouch_hold || g_client_bot_state.traversal_crouch_active);
    const bool currently_crouched = rf::entity_is_crouching(const_cast<rf::Entity*>(&local_entity));
    g_client_bot_state.traversal_crouch_toggled_on = currently_crouched;
    if (currently_crouched != should_hold_crouch) {
        rf::player_execute_action(&local_player, rf::CC_ACTION_CROUCH, true);
        g_client_bot_state.traversal_crouch_toggled_on = should_hold_crouch;
    }

    bool should_jump = maneuver_jump;
    if (!should_jump
        && has_active_link
        && active_link_changed
        && waypoints_link_has_target_type(
            active_from_waypoint,
            active_to_waypoint,
            WaypointTargetType::jump)) {
        should_jump = true;
        g_client_bot_state.jump_timer.set(650);
    }

    const bool target_jump_height_allowed =
        !g_client_bot_state.has_waypoint_target
        || to_target.y <= kMaxWaypointStepJumpHeight;
    const bool near_vertical_waypoint_target =
        g_client_bot_state.has_waypoint_target
        && horizontal_dist <= std::max(stop_distance, 0.35f)
        && to_target.y > 0.0f;
    const bool jump_blocked_by_height =
        (desired_move_z > 0.25f || near_vertical_waypoint_target)
        && to_target.y > kMaxWaypointStepJumpHeight
        && g_client_bot_state.has_waypoint_target;
    if (jump_blocked_by_height && !active_link_route_traversable) {
        int from_waypoint = 0;
        int to_waypoint = 0;
        const bool jump_check_has_active_link =
            get_active_waypoint_link(from_waypoint, to_waypoint);
        if (jump_check_has_active_link
            && !bot_nav_is_failed_edge_cooldown_active_no_prune(from_waypoint, to_waypoint)) {
            const int blacklist_waypoint =
                bot_nav_choose_blacklist_waypoint_for_failed_link(from_waypoint, to_waypoint);
            if (blacklist_waypoint > 0) {
                bot_nav_blacklist_waypoint_temporarily(
                    blacklist_waypoint,
                    kFailedWaypointBlacklistMs
                );
            }
            // This edge requires a jump the bot cannot make; bias routing away from it.
            bot_nav_register_failed_edge_cooldown(
                from_waypoint,
                to_waypoint,
                kJumpBlockedEdgeCooldownMs
            );
            bot_internal_start_recovery_anchor_reroute(
                local_entity,
                blacklist_waypoint > 0 ? blacklist_waypoint : to_waypoint
            );
        }
        else if (!jump_check_has_active_link) {
            bot_internal_start_recovery_anchor_reroute(local_entity, g_client_bot_state.waypoint_goal);
        }

        desired_move_z = std::min(desired_move_z, 0.0f);
        should_jump = false;
    }

    if ((desired_move_z > 0.25f || near_vertical_waypoint_target)
        && to_target.y > 1.25f
        && target_jump_height_allowed
        && (!g_client_bot_state.jump_timer.valid()
            || g_client_bot_state.jump_timer.elapsed())) {
        should_jump = true;
        g_client_bot_state.jump_timer.set(650);
    }

    if (!should_jump
        && has_active_link
        && should_jump_to_clear_obstructed_upward_link(
            local_entity,
            active_from_waypoint,
            active_to_waypoint)
        && (!g_client_bot_state.jump_timer.valid()
            || g_client_bot_state.jump_timer.elapsed())) {
        should_jump = true;
        g_client_bot_state.jump_timer.set(650);
    }

    if (!should_jump
        && has_active_link
        && should_jump_to_cross_gap_link(
            local_entity,
            active_from_waypoint,
            active_to_waypoint)
        && (!g_client_bot_state.jump_timer.valid()
            || g_client_bot_state.jump_timer.elapsed())) {
        should_jump = true;
        g_client_bot_state.jump_timer.set(650);
    }

    issue_movement_actions(
        local_player,
        desired_move_x,
        ladder_move_y,
        desired_move_z,
        should_jump
    );
}
