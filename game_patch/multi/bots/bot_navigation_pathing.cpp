#include "bot_navigation_pathing.h"

#include "bot_decision_eval.h"
#include "bot_internal.h"
#include "bot_math.h"
#include "bot_navigation_routes.h"
#include "bot_utils.h"
#include "bot_waypoint_route.h"
#include "../../main/main.h"
#include "../../rf/collide.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <unordered_set>
#include <vector>

namespace
{
constexpr int kBotNavProbeTraceFlags = rf::CF_ANY_HIT | rf::CF_PROCESS_INVISIBLE_FACES;
constexpr float kWaypointTurnRelaxHeightThreshold = 0.40f;
constexpr float kJumpPadWaypointReachRadius = 0.35f;
constexpr float kPathFallFastForwardRadius = kWaypointReachRadius * 1.75f;
constexpr float kPathFallFastForwardVerticalSlack = 1.0f;
constexpr float kPathFallFastForwardLevelTolerance = 0.25f;
constexpr int kFallFastForwardStuckTimeoutMs = 2000;
constexpr float kRecoveryAnchorHorizontalDeadlockRadius = 0.85f;
constexpr float kRecoveryAnchorVerticalDeadlockHeight = 1.6f;

int current_waypoint_progress_timeout_ms()
{
    if (g_client_bot_state.fsm_state == BotFsmState::recover_navigation) {
        return std::max(kWaypointProgressTimeoutMs, 1400);
    }
    if (g_client_bot_state.active_goal == BotGoalType::control_point_objective
        || g_client_bot_state.fsm_state == BotFsmState::control_point_objective) {
        return std::max(kWaypointProgressTimeoutMs, 1250);
    }
    if (g_client_bot_state.active_goal != BotGoalType::none
        && g_client_bot_state.active_goal != BotGoalType::roam) {
        return kWaypointObjectiveProgressTimeoutMs;
    }
    return kWaypointProgressTimeoutMs;
}


bool waypoint_has_viable_route_neighbor(const int waypoint)
{
    if (waypoint <= 0) {
        return false;
    }

    std::array<int, kMaxWaypointLinks> links{};
    const int link_count = waypoints_get_links(waypoint, links);
    if (link_count <= 0) {
        return false;
    }

    for (int i = 0; i < link_count; ++i) {
        const int neighbor = links[i];
        if (neighbor <= 0 || neighbor == waypoint) {
            continue;
        }
        if (!bot_nav_is_link_traversable_for_route(waypoint, neighbor)) {
            continue;
        }
        if (!waypoints_has_direct_link(waypoint, neighbor)
            || !waypoints_link_is_clear(waypoint, neighbor)) {
            continue;
        }
        return true;
    }
    return false;
}

bool is_recovery_anchor_vertical_deadlock(
    const rf::Entity& entity,
    const rf::Vector3& anchor_pos)
{
    const float horizontal_sq = bot_horizontal_dist_sq(entity.pos, anchor_pos);
    const float vertical_delta = std::abs(anchor_pos.y - entity.pos.y);
    return horizontal_sq
            <= kRecoveryAnchorHorizontalDeadlockRadius * kRecoveryAnchorHorizontalDeadlockRadius
        && vertical_delta >= kRecoveryAnchorVerticalDeadlockHeight;
}

int choose_recovery_anchor_escape_neighbor(
    const rf::Entity& entity,
    const int anchor_waypoint)
{
    if (anchor_waypoint <= 0) {
        return 0;
    }

    const rf::Vector3 eye_offset = entity.eye_pos - entity.pos;
    std::array<int, kMaxWaypointLinks> links{};
    const int link_count = waypoints_get_links(anchor_waypoint, links);
    if (link_count <= 0) {
        return 0;
    }

    int best_neighbor = 0;
    float best_score = -std::numeric_limits<float>::infinity();
    for (int i = 0; i < link_count; ++i) {
        const int neighbor = links[i];
        if (neighbor <= 0 || neighbor == anchor_waypoint) {
            continue;
        }
        if (!bot_nav_is_link_traversable_for_route(anchor_waypoint, neighbor)) {
            continue;
        }
        if (!waypoints_has_direct_link(anchor_waypoint, neighbor)
            || !waypoints_link_is_clear(anchor_waypoint, neighbor)) {
            continue;
        }

        rf::Vector3 neighbor_pos{};
        if (!waypoints_get_pos(neighbor, neighbor_pos)) {
            continue;
        }
        if (is_recovery_anchor_vertical_deadlock(entity, neighbor_pos)) {
            continue;
        }

        const float horizontal_sq = bot_horizontal_dist_sq(entity.pos, neighbor_pos);
        const float vertical_delta = std::abs(neighbor_pos.y - entity.pos.y);
        float score = std::clamp(horizontal_sq, 0.0f, 256.0f) - vertical_delta * 2.2f;

        const rf::Vector3 neighbor_eye_pos = neighbor_pos + eye_offset;
        if (bot_has_unobstructed_level_los(entity.eye_pos, neighbor_eye_pos, &entity, nullptr)) {
            score += 24.0f;
        }

        if (score > best_score) {
            best_score = score;
            best_neighbor = neighbor;
        }
    }

    return best_neighbor;
}

int find_recovery_anchor_alternative_waypoint(
    const rf::Entity& entity,
    const int disallow_waypoint)
{
    const int waypoint_total = waypoints_count();
    if (waypoint_total <= 1) {
        return 0;
    }

    const rf::Vector3 eye_offset = entity.eye_pos - entity.pos;
    auto choose_best = [&](const bool require_los) -> int {
        int best_waypoint = 0;
        float best_score = std::numeric_limits<float>::infinity();
        for (int waypoint = 1; waypoint < waypoint_total; ++waypoint) {
            if (waypoint == disallow_waypoint) {
                continue;
            }
            if (!waypoint_has_viable_route_neighbor(waypoint)) {
                continue;
            }

            rf::Vector3 waypoint_pos{};
            if (!waypoints_get_pos(waypoint, waypoint_pos)) {
                continue;
            }
            if (is_recovery_anchor_vertical_deadlock(entity, waypoint_pos)) {
                continue;
            }

            if (require_los) {
                const rf::Vector3 waypoint_eye_pos = waypoint_pos + eye_offset;
                if (!bot_has_unobstructed_level_los(
                        entity.eye_pos,
                        waypoint_eye_pos,
                        &entity,
                        nullptr)) {
                    continue;
                }
            }

            const float dist_sq = rf::vec_dist_squared(&entity.pos, &waypoint_pos);
            const float vertical_delta = std::abs(waypoint_pos.y - entity.pos.y);
            const float score =
                dist_sq + std::max(0.0f, vertical_delta - 3.0f) * 18.0f;
            if (score < best_score) {
                best_score = score;
                best_waypoint = waypoint;
            }
        }
        return best_waypoint;
    };

    if (int waypoint = choose_best(true); waypoint > 0) {
        return waypoint;
    }
    return choose_best(false);
}

int choose_recovery_anchor_waypoint(
    const rf::Entity& entity,
    const int seed_waypoint)
{
    if (seed_waypoint <= 0) {
        return 0;
    }

    rf::Vector3 seed_pos{};
    const bool seed_pos_valid = waypoints_get_pos(seed_waypoint, seed_pos);
    const bool seed_deadlock = !seed_pos_valid || is_recovery_anchor_vertical_deadlock(entity, seed_pos);
    const bool seed_viable = !seed_deadlock && waypoint_has_viable_route_neighbor(seed_waypoint);
    if (seed_viable) {
        return seed_waypoint;
    }

    if (!seed_deadlock) {
        if (const int escape_neighbor =
                choose_recovery_anchor_escape_neighbor(entity, seed_waypoint);
            escape_neighbor > 0) {
            return escape_neighbor;
        }
    }

    if (const int alternative =
            find_recovery_anchor_alternative_waypoint(entity, seed_waypoint);
        alternative > 0) {
        return alternative;
    }

    return seed_deadlock ? 0 : seed_waypoint;
}

bool set_recovery_anchor_direct_target(const rf::Entity& entity)
{
    if (g_client_bot_state.recovery_anchor_waypoint <= 0) {
        return false;
    }

    int anchor_waypoint = g_client_bot_state.recovery_anchor_waypoint;
    rf::Vector3 anchor_pos{};
    if (!waypoints_get_pos(anchor_waypoint, anchor_pos)) {
        return false;
    }

    if (is_recovery_anchor_vertical_deadlock(entity, anchor_pos)) {
        int escape_waypoint =
            choose_recovery_anchor_escape_neighbor(entity, anchor_waypoint);
        if (escape_waypoint <= 0) {
            escape_waypoint =
                find_recovery_anchor_alternative_waypoint(entity, anchor_waypoint);
        }
        if (escape_waypoint <= 0 || !waypoints_get_pos(escape_waypoint, anchor_pos)) {
            return false;
        }
        anchor_waypoint = escape_waypoint;
        g_client_bot_state.recovery_anchor_waypoint = anchor_waypoint;
    }

    g_client_bot_state.waypoint_target_pos = anchor_pos;
    g_client_bot_state.waypoint_goal = anchor_waypoint;
    g_client_bot_state.has_waypoint_target = true;
    return true;
}

bool is_probe_direction_blocked(
    const rf::Entity& entity,
    const rf::Vector3& origin,
    const rf::Vector3& direction,
    const float distance)
{
    (void)entity;

    if (distance <= 0.0f || direction.len_sq() < 0.0001f) {
        return false;
    }

    rf::Vector3 dir = direction;
    dir.normalize_safe();
    const rf::Vector3 endpoint = origin + dir * distance;
    rf::Vector3 p0 = origin;
    rf::Vector3 p1 = endpoint;
    rf::GCollisionOutput collision{};
    return rf::collide_linesegment_level_solid(
        p0,
        p1,
        kBotNavProbeTraceFlags,
        &collision);
}

// find_closest_waypoint_with_los_fallback is defined in bot_utils.cpp

bool has_visibility_to_target_position(
    const rf::Vector3& origin,
    const rf::Vector3& target_pos,
    const rf::Vector3& target_eye_pos,
    const rf::Object* ignore1,
    const rf::Object* ignore2)
{
    if (bot_has_unobstructed_level_los(origin, target_eye_pos, ignore1, ignore2)) {
        return true;
    }

    rf::Vector3 chest_pos = target_pos;
    chest_pos.y = std::lerp(target_pos.y, target_eye_pos.y, 0.6f);
    return bot_has_unobstructed_level_los(origin, chest_pos, ignore1, ignore2);
}

bool waypoint_has_visibility_to_target(
    const rf::Vector3& waypoint_pos,
    const rf::Vector3& eye_offset,
    const rf::Vector3& target_pos,
    const rf::Vector3& target_eye_pos,
    const rf::Object* target_obj)
{
    const rf::Vector3 waypoint_eye_pos = waypoint_pos + eye_offset;
    return has_visibility_to_target_position(
        waypoint_eye_pos,
        target_pos,
        target_eye_pos,
        target_obj,
        nullptr
    );
}

float compute_waypoint_path_length(const std::vector<int>& path)
{
    if (path.size() < 2) {
        return 0.0f;
    }

    rf::Vector3 prev_pos{};
    if (!waypoints_get_pos(path[0], prev_pos)) {
        return std::numeric_limits<float>::infinity();
    }

    float total_length = 0.0f;
    for (size_t index = 1; index < path.size(); ++index) {
        rf::Vector3 waypoint_pos{};
        if (!waypoints_get_pos(path[index], waypoint_pos)) {
            return std::numeric_limits<float>::infinity();
        }
        total_length += (waypoint_pos - prev_pos).len();
        prev_pos = waypoint_pos;
    }
    return total_length;
}

constexpr int kRouteChoiceLockMsBase = 1100;
constexpr int kRouteChoiceLockMsRecover = 1800;

bool route_choice_lock_context_matches(const int goal_waypoint)
{
    if (!g_client_bot_state.route_choice_lock_timer.valid()
        || g_client_bot_state.route_choice_lock_timer.elapsed()) {
        return false;
    }
    if (g_client_bot_state.route_choice_lock_next_waypoint <= 0) {
        return false;
    }
    if (g_client_bot_state.route_choice_lock_goal_type != g_client_bot_state.active_goal) {
        return false;
    }
    if (g_client_bot_state.route_choice_lock_target_handle != g_client_bot_state.goal_target_handle
        || g_client_bot_state.route_choice_lock_target_identifier
            != g_client_bot_state.goal_target_identifier) {
        return false;
    }

    const bool recover_eliminate =
        g_client_bot_state.active_goal == BotGoalType::eliminate_target
        && (g_client_bot_state.fsm_state == BotFsmState::recover_navigation
            || g_client_bot_state.recovery_pending_reroute);
    if (!recover_eliminate
        && goal_waypoint > 0
        && g_client_bot_state.route_choice_lock_goal_waypoint > 0
        && g_client_bot_state.route_choice_lock_goal_waypoint != goal_waypoint) {
        return false;
    }
    return true;
}

int get_locked_route_next_waypoint_if_valid(const int goal_waypoint)
{
    return route_choice_lock_context_matches(goal_waypoint)
        ? g_client_bot_state.route_choice_lock_next_waypoint
        : 0;
}

void set_route_choice_lock_from_path(const std::vector<int>& path, const int goal_waypoint)
{
    if (path.size() <= 1) {
        g_client_bot_state.route_choice_lock_timer.invalidate();
        g_client_bot_state.route_choice_lock_goal_type = BotGoalType::none;
        g_client_bot_state.route_choice_lock_target_handle = -1;
        g_client_bot_state.route_choice_lock_target_identifier = -1;
        g_client_bot_state.route_choice_lock_goal_waypoint = 0;
        g_client_bot_state.route_choice_lock_next_waypoint = 0;
        return;
    }

    const bool recover_eliminate =
        g_client_bot_state.active_goal == BotGoalType::eliminate_target
        && (g_client_bot_state.fsm_state == BotFsmState::recover_navigation
            || g_client_bot_state.recovery_pending_reroute);
    g_client_bot_state.route_choice_lock_goal_type = g_client_bot_state.active_goal;
    g_client_bot_state.route_choice_lock_target_handle = g_client_bot_state.goal_target_handle;
    g_client_bot_state.route_choice_lock_target_identifier =
        g_client_bot_state.goal_target_identifier;
    g_client_bot_state.route_choice_lock_goal_waypoint = goal_waypoint;
    g_client_bot_state.route_choice_lock_next_waypoint = path[1];
    g_client_bot_state.route_choice_lock_timer.set(
        recover_eliminate ? kRouteChoiceLockMsRecover : kRouteChoiceLockMsBase
    );
}

int choose_weighted_top_rank(const int candidate_count)
{
    if (candidate_count <= 1) {
        return 0;
    }

    float total_weight = 0.0f;
    for (int rank = 0; rank < candidate_count; ++rank) {
        total_weight += 1.0f / static_cast<float>(rank + 1);
    }

    std::uniform_real_distribution<float> roll_dist(0.0f, total_weight);
    const float roll = roll_dist(g_rng);
    float accumulated = 0.0f;
    for (int rank = 0; rank < candidate_count; ++rank) {
        accumulated += 1.0f / static_cast<float>(rank + 1);
        if (roll <= accumulated) {
            return rank;
        }
    }

    return candidate_count - 1;
}

float compute_route_score_tie_epsilon(const float best_score)
{
    return std::max(0.25f, std::abs(best_score) * 0.004f);
}

template<typename RouteCandidateT>
int choose_route_candidate_index(
    const std::vector<RouteCandidateT>& route_candidates,
    const int pool_size,
    const int goal_waypoint)
{
    if (route_candidates.empty()) {
        return 0;
    }

    const int bounded_pool_size = std::clamp(
        pool_size,
        1,
        static_cast<int>(route_candidates.size())
    );
    if (bounded_pool_size <= 1) {
        return 0;
    }

    const float best_score = route_candidates[0].score;
    const float tie_epsilon = compute_route_score_tie_epsilon(best_score);
    const bool recover_eliminate =
        g_client_bot_state.active_goal == BotGoalType::eliminate_target
        && (g_client_bot_state.fsm_state == BotFsmState::recover_navigation
            || g_client_bot_state.recovery_pending_reroute);
    float sticky_margin = tie_epsilon * (recover_eliminate ? 12.0f : 4.5f);
    sticky_margin = std::max(sticky_margin, recover_eliminate ? 4.0f : 1.5f);

    int sticky_next_waypoint = 0;
    if (goal_waypoint > 0
        && g_client_bot_state.waypoint_goal == goal_waypoint
        && g_client_bot_state.waypoint_next_index > 0
        && g_client_bot_state.waypoint_next_index
            < static_cast<int>(g_client_bot_state.waypoint_path.size())) {
        sticky_next_waypoint =
            g_client_bot_state.waypoint_path[g_client_bot_state.waypoint_next_index];
    }
    if (sticky_next_waypoint <= 0 && recover_eliminate
        && g_client_bot_state.waypoint_next_index > 0
        && g_client_bot_state.waypoint_next_index
            < static_cast<int>(g_client_bot_state.waypoint_path.size())) {
        sticky_next_waypoint =
            g_client_bot_state.waypoint_path[g_client_bot_state.waypoint_next_index];
    }
    if (sticky_next_waypoint <= 0) {
        sticky_next_waypoint = get_locked_route_next_waypoint_if_valid(goal_waypoint);
    }
    if (sticky_next_waypoint > 0) {
        for (int index = 0; index < bounded_pool_size; ++index) {
            const auto& candidate_path = route_candidates[index].path;
            if (candidate_path.size() <= 1 || candidate_path[1] != sticky_next_waypoint) {
                continue;
            }
            if (route_candidates[index].score + sticky_margin >= best_score) {
                return index;
            }
        }
    }

    int tie_count = 1;
    while (tie_count < bounded_pool_size
           && std::abs(route_candidates[tie_count].score - best_score) <= tie_epsilon) {
        ++tie_count;
    }

    if (tie_count > 1) {
        if (sticky_next_waypoint > 0) {
            for (int index = 0; index < tie_count; ++index) {
                const auto& candidate_path = route_candidates[index].path;
                if (candidate_path.size() > 1 && candidate_path[1] == sticky_next_waypoint) {
                    return index;
                }
            }
        }

        std::uniform_int_distribution<int> tie_dist(0, tie_count - 1);
        return tie_dist(g_rng);
    }

    return choose_weighted_top_rank(bounded_pool_size);
}

bool is_long_route_escape_active()
{
    if (g_client_bot_state.long_route_escape_timer.valid()
        && g_client_bot_state.long_route_escape_timer.elapsed()) {
        g_client_bot_state.long_route_escape_timer.invalidate();
    }
    return g_client_bot_state.long_route_escape_timer.valid();
}

void trigger_loop_escape_mode()
{
    std::uniform_int_distribution<int> long_route_dist(kLoopEscapeMinMs, kLoopEscapeMaxMs);
    g_client_bot_state.long_route_escape_timer.set(long_route_dist(g_rng));
    g_client_bot_state.repath_timer.invalidate();
}

void set_last_heading_change_reason(const char* reason)
{
    const char* text = (reason && reason[0]) ? reason : "none";
    std::snprintf(
        g_client_bot_state.last_heading_change_reason,
        sizeof(g_client_bot_state.last_heading_change_reason),
        "%s",
        text
    );
}
}

bool continue_recovery_anchor_move(const rf::Entity& entity)
{
    if (!g_client_bot_state.recovery_pending_reroute
        || g_client_bot_state.recovery_anchor_waypoint <= 0
        || !g_client_bot_state.has_waypoint_target) {
        return false;
    }

    const float horizontal_sq = bot_horizontal_dist_sq(
        entity.pos,
        g_client_bot_state.waypoint_target_pos
    );
    const float vertical_delta = std::abs(
        g_client_bot_state.waypoint_target_pos.y - entity.pos.y
    );
    if (horizontal_sq > kWaypointReachRadius * kWaypointReachRadius) {
        return true;
    }
    if (vertical_delta >= kRecoveryAnchorVerticalDeadlockHeight
        && horizontal_sq
            <= kRecoveryAnchorHorizontalDeadlockRadius * kRecoveryAnchorHorizontalDeadlockRadius) {
        g_client_bot_state.has_waypoint_target = false;
        return false;
    }

    g_client_bot_state.has_waypoint_target = false;
    return false;
}

float link_segment_distance_sq_xz(
    const rf::Vector3& point,
    const rf::Vector3& from,
    const rf::Vector3& to);

void clear_waypoint_route()
{
    g_client_bot_state.waypoint_path.clear();
    g_client_bot_state.waypoint_next_index = 0;
    g_client_bot_state.waypoint_goal = 0;
    g_client_bot_state.has_waypoint_target = false;
    g_client_bot_state.waypoint_progress_timer.invalidate();
    g_client_bot_state.waypoint_progress_waypoint = 0;
    g_client_bot_state.waypoint_progress_best_dist_sq = 0.0f;
    g_client_bot_state.waypoint_progress_no_improvement_windows = 0;
    g_client_bot_state.waypoint_progress_origin = {};
    g_client_bot_state.los_route_solve_timer.invalidate();
    g_client_bot_state.validated_link_from_waypoint = 0;
    g_client_bot_state.validated_link_to_waypoint = 0;
    g_client_bot_state.validated_link_clear = false;
    g_client_bot_state.corridor_violation_from_waypoint = 0;
    g_client_bot_state.corridor_violation_to_waypoint = 0;
    g_client_bot_state.corridor_violation_count = 0;
    g_client_bot_state.combat_los_anchor_waypoint = 0;
    g_client_bot_state.combat_los_target_waypoint = 0;
    g_client_bot_state.combat_los_reposition_timer.invalidate();
    g_client_bot_state.combat_los_reposition_eval_timer.invalidate();
    g_client_bot_state.combat_los_waypoint_guard_timer.invalidate();
    g_client_bot_state.combat_los_waypoint_guard_waypoint = 0;
    g_client_bot_state.combat_los_waypoint_guard_target_handle = -1;
    g_client_bot_state.combat_los_waypoint_guard_visible = false;
    g_client_bot_state.forward_obstacle_progress_timer.invalidate();
    g_client_bot_state.forward_obstacle_progress_origin = {};
    g_client_bot_state.corner_probe_progress_timer.invalidate();
    g_client_bot_state.corner_probe_progress_origin = {};
    g_client_bot_state.geometry_stuck_sample_count = 0;
    g_client_bot_state.fall_fast_forward_pending = false;
    g_client_bot_state.fall_fast_forward_stuck_timer.invalidate();
    g_client_bot_state.fall_fast_forward_stuck_origin = {};
}

bool is_following_waypoint_link(const rf::Entity& entity)
{
    if (g_client_bot_state.waypoint_path.empty()
        || g_client_bot_state.waypoint_next_index <= 0
        || g_client_bot_state.waypoint_next_index
            >= static_cast<int>(g_client_bot_state.waypoint_path.size())) {
        return false;
    }

    const int from_waypoint =
        g_client_bot_state.waypoint_path[g_client_bot_state.waypoint_next_index - 1];
    const int to_waypoint =
        g_client_bot_state.waypoint_path[g_client_bot_state.waypoint_next_index];
    if (from_waypoint <= 0 || to_waypoint <= 0) {
        return false;
    }
    if (!waypoints_has_direct_link(from_waypoint, to_waypoint)) {
        return false;
    }

    rf::Vector3 from_pos{};
    rf::Vector3 to_pos{};
    if (!waypoints_get_pos(from_waypoint, from_pos) || !waypoints_get_pos(to_waypoint, to_pos)) {
        return false;
    }

    const float near_endpoint_sq = (kWaypointReachRadius * 1.35f) * (kWaypointReachRadius * 1.35f);
    const float dist_from_sq = rf::vec_dist_squared(&entity.pos, &from_pos);
    const float dist_to_sq = rf::vec_dist_squared(&entity.pos, &to_pos);
    if (dist_from_sq <= near_endpoint_sq || dist_to_sq <= near_endpoint_sq) {
        return true;
    }

    const float corridor_radius = kWaypointLinkCorridorRadius * 1.25f;
    const float corridor_dist_sq = link_segment_distance_sq_xz(entity.pos, from_pos, to_pos);
    return corridor_dist_sq <= corridor_radius * corridor_radius;
}

bool start_recovery_anchor_reroute(const rf::Entity& entity, const int avoid_waypoint)
{
    // Skip recovery for goals that don't need precise pathing — roam can just
    // pick a new nearby waypoint without the full anchor reroute system.
    if (g_client_bot_state.active_goal == BotGoalType::roam
        || g_client_bot_state.active_goal == BotGoalType::none) {
        clear_waypoint_route();
        g_client_bot_state.recovery_anchor_waypoint = 0;
        g_client_bot_state.recovery_avoid_waypoint = 0;
        g_client_bot_state.recovery_pending_reroute = false;
        return false;
    }

    int anchor_waypoint = bot_find_closest_waypoint_with_los_fallback(entity);
    anchor_waypoint = choose_recovery_anchor_waypoint(entity, anchor_waypoint);
    clear_waypoint_route();
    if (anchor_waypoint <= 0) {
        g_client_bot_state.recovery_anchor_waypoint = 0;
        g_client_bot_state.recovery_avoid_waypoint = 0;
        g_client_bot_state.recovery_pending_reroute = false;
        return false;
    }
    set_last_heading_change_reason("recovery_reroute");
    g_client_bot_state.recovery_anchor_waypoint = anchor_waypoint;
    g_client_bot_state.recovery_avoid_waypoint = avoid_waypoint;
    g_client_bot_state.recovery_pending_reroute = true;
    g_client_bot_state.repath_timer.invalidate();
    return false;
}

float link_segment_distance_sq_xz(
    const rf::Vector3& point,
    const rf::Vector3& from,
    const rf::Vector3& to)
{
    const float ab_x = to.x - from.x;
    const float ab_z = to.z - from.z;
    const float ab_len_sq = ab_x * ab_x + ab_z * ab_z;
    if (ab_len_sq <= 0.0001f) {
        const float dx = point.x - from.x;
        const float dz = point.z - from.z;
        return dx * dx + dz * dz;
    }

    const float ap_x = point.x - from.x;
    const float ap_z = point.z - from.z;
    const float t = std::clamp((ap_x * ab_x + ap_z * ab_z) / ab_len_sq, 0.0f, 1.0f);
    const float closest_x = from.x + ab_x * t;
    const float closest_z = from.z + ab_z * t;
    const float dx = point.x - closest_x;
    const float dz = point.z - closest_z;
    return dx * dx + dz * dz;
}

float compute_waypoint_advance_radius_for_turn(const int next_index)
{
    float reach_radius = kWaypointReachRadius;
    if (next_index <= 0
        || next_index >= static_cast<int>(g_client_bot_state.waypoint_path.size())) {
        return reach_radius;
    }

    const int current_waypoint = g_client_bot_state.waypoint_path[next_index];
    const int previous_waypoint = g_client_bot_state.waypoint_path[next_index - 1];
    const bool has_outgoing_waypoint =
        next_index + 1 < static_cast<int>(g_client_bot_state.waypoint_path.size());
    if (!has_outgoing_waypoint) {
        return reach_radius;
    }

    const int outgoing_waypoint = g_client_bot_state.waypoint_path[next_index + 1];
    rf::Vector3 prev_pos{};
    rf::Vector3 current_pos{};
    rf::Vector3 next_pos{};
    if (!waypoints_get_pos(previous_waypoint, prev_pos)
        || !waypoints_get_pos(current_waypoint, current_pos)
        || !waypoints_get_pos(outgoing_waypoint, next_pos)) {
        return reach_radius;
    }

    rf::Vector3 in_dir = current_pos - prev_pos;
    rf::Vector3 out_dir = next_pos - current_pos;
    in_dir.y = 0.0f;
    out_dir.y = 0.0f;

    const float in_len = in_dir.len();
    const float out_len = out_dir.len();
    if (in_len <= 0.001f || out_len <= 0.001f) {
        return reach_radius;
    }

    in_dir /= in_len;
    out_dir /= out_len;
    const float turn_cos = std::clamp(in_dir.dot_prod(out_dir), -1.0f, 1.0f);
    // 0 = straight, 1 = full reversal.
    const float turn_strength = std::clamp((1.0f - turn_cos) * 0.5f, 0.0f, 1.0f);
    const bool approximately_level_link =
        std::fabs(current_pos.y - prev_pos.y) <= kWaypointTurnRelaxHeightThreshold;

    if (approximately_level_link) {
        const float relaxed_turn_scaled_radius =
            std::lerp(kWaypointReachRadius, 2.6f, turn_strength);
        // In flatter/open traversal, ease back toward original behavior.
        const float relaxed_segment_cap = std::max(2.0f, std::min(in_len, out_len) * 0.58f);
        reach_radius = std::min(relaxed_turn_scaled_radius, relaxed_segment_cap);
        return std::clamp(reach_radius, 2.0f, kWaypointReachRadius);
    }

    const float strict_turn_scaled_radius = std::lerp(kWaypointReachRadius, 1.0f, turn_strength);
    // Keep switch-over near the corner vertex on shorter links.
    const float strict_segment_cap = std::max(1.0f, std::min(in_len, out_len) * 0.38f);
    reach_radius = std::min(strict_turn_scaled_radius, strict_segment_cap);
    return std::clamp(reach_radius, 1.0f, kWaypointReachRadius);
}

bool try_fast_forward_path_on_downward_fall(
    const rf::Entity& entity,
    int& in_out_current_waypoint,
    rf::Vector3& in_out_target_pos)
{
    if (!g_client_bot_state.fall_fast_forward_pending) {
        return false;
    }

    if (g_client_bot_state.waypoint_path.empty()
        || g_client_bot_state.waypoint_next_index < 0
        || g_client_bot_state.waypoint_next_index
            >= static_cast<int>(g_client_bot_state.waypoint_path.size())) {
        g_client_bot_state.fall_fast_forward_pending = false;
        return false;
    }

    const int next_index = g_client_bot_state.waypoint_next_index;
    if (next_index + 1 >= static_cast<int>(g_client_bot_state.waypoint_path.size())) {
        g_client_bot_state.fall_fast_forward_pending = false;
        return false;
    }

    const float max_dist_sq = kPathFallFastForwardRadius * kPathFallFastForwardRadius;
    int selected_index = -1;
    float selected_dist_sq = std::numeric_limits<float>::max();

    for (int path_index = next_index + 1;
         path_index < static_cast<int>(g_client_bot_state.waypoint_path.size());
         ++path_index) {
        const int waypoint = g_client_bot_state.waypoint_path[path_index];
        rf::Vector3 waypoint_pos{};
        if (waypoint <= 0 || !waypoints_get_pos(waypoint, waypoint_pos)) {
            continue;
        }
        if (waypoint_pos.y > entity.pos.y + kPathFallFastForwardVerticalSlack) {
            continue;
        }

        const float dist_sq = rf::vec_dist_squared(&entity.pos, &waypoint_pos);
        if (dist_sq > max_dist_sq) {
            continue;
        }

        if (path_index > selected_index
            || (path_index == selected_index && dist_sq < selected_dist_sq)) {
            selected_index = path_index;
            selected_dist_sq = dist_sq;
        }
    }

    if (selected_index <= next_index) {
        g_client_bot_state.fall_fast_forward_pending = false;
        return false;
    }

    g_client_bot_state.waypoint_path.erase(
        g_client_bot_state.waypoint_path.begin(),
        g_client_bot_state.waypoint_path.begin() + selected_index);
    g_client_bot_state.waypoint_next_index = 0;
    if (g_client_bot_state.waypoint_path.empty()) {
        return false;
    }

    in_out_current_waypoint = g_client_bot_state.waypoint_path[0];
    if (!waypoints_get_pos(in_out_current_waypoint, in_out_target_pos)) {
        return false;
    }

    g_client_bot_state.waypoint_progress_timer.invalidate();
    g_client_bot_state.waypoint_progress_waypoint = 0;
    g_client_bot_state.waypoint_progress_best_dist_sq = 0.0f;
    g_client_bot_state.waypoint_progress_no_improvement_windows = 0;
    g_client_bot_state.waypoint_progress_origin = {};
    g_client_bot_state.validated_link_from_waypoint = 0;
    g_client_bot_state.validated_link_to_waypoint = 0;
    g_client_bot_state.validated_link_clear = false;
    g_client_bot_state.fall_fast_forward_pending = false;
    g_client_bot_state.fall_fast_forward_stuck_origin = entity.pos;
    g_client_bot_state.fall_fast_forward_stuck_timer.set(kFallFastForwardStuckTimeoutMs);
    set_last_heading_change_reason("fall_path_fast_forward");
    return true;
}

void update_fall_fast_forward_transition_state(const rf::Entity& entity)
{
    const bool is_falling = rf::entity_is_falling(const_cast<rf::Entity*>(&entity));
    if (is_falling) {
        if (!g_client_bot_state.fall_fast_forward_was_falling) {
            g_client_bot_state.fall_fast_forward_was_falling = true;
            g_client_bot_state.fall_fast_forward_start_y = entity.pos.y;
            g_client_bot_state.fall_fast_forward_pending = false;
        }
        return;
    }

    if (g_client_bot_state.fall_fast_forward_was_falling) {
        g_client_bot_state.fall_fast_forward_was_falling = false;
        const float landing_height_delta = entity.pos.y - g_client_bot_state.fall_fast_forward_start_y;
        // Fast-forward on level/downward landings; skip only on clearly upward landings.
        g_client_bot_state.fall_fast_forward_pending =
            landing_height_delta <= kPathFallFastForwardLevelTolerance;
    }
}

bool handle_fall_fast_forward_stuck_guard(const rf::Entity& entity, const int current_waypoint)
{
    if (!g_client_bot_state.fall_fast_forward_stuck_timer.valid()
        || !g_client_bot_state.fall_fast_forward_stuck_timer.elapsed()) {
        return false;
    }

    rf::Vector3 moved = entity.pos - g_client_bot_state.fall_fast_forward_stuck_origin;
    moved.y = 0.0f;
    g_client_bot_state.fall_fast_forward_stuck_timer.invalidate();
    g_client_bot_state.fall_fast_forward_stuck_origin = {};
    if (moved.len_sq()
        >= kNavigationBlockedProgressDistance * kNavigationBlockedProgressDistance) {
        return false;
    }

    set_last_heading_change_reason("fall_fast_forward_stuck_repath");
    start_recovery_anchor_reroute(entity, current_waypoint);
    return true;
}

bool enforce_current_link_corridor(const rf::Entity& entity)
{
    if (g_client_bot_state.waypoint_path.empty()
        || g_client_bot_state.waypoint_next_index <= 0
        || g_client_bot_state.waypoint_next_index
            >= static_cast<int>(g_client_bot_state.waypoint_path.size())) {
        return true;
    }

    const int from_waypoint =
        g_client_bot_state.waypoint_path[g_client_bot_state.waypoint_next_index - 1];
    const int to_waypoint =
        g_client_bot_state.waypoint_path[g_client_bot_state.waypoint_next_index];
    if (from_waypoint <= 0 || to_waypoint <= 0) {
        return true;
    }

    if (g_client_bot_state.validated_link_from_waypoint != from_waypoint
        || g_client_bot_state.validated_link_to_waypoint != to_waypoint) {
        g_client_bot_state.validated_link_from_waypoint = from_waypoint;
        g_client_bot_state.validated_link_to_waypoint = to_waypoint;
        g_client_bot_state.validated_link_clear = waypoints_link_is_clear(from_waypoint, to_waypoint);
        g_client_bot_state.corridor_violation_from_waypoint = 0;
        g_client_bot_state.corridor_violation_to_waypoint = 0;
        g_client_bot_state.corridor_violation_count = 0;
    }

    const auto corridor_violation_should_fail = [&]() {
        if (g_client_bot_state.corridor_violation_from_waypoint != from_waypoint
            || g_client_bot_state.corridor_violation_to_waypoint != to_waypoint) {
            g_client_bot_state.corridor_violation_from_waypoint = from_waypoint;
            g_client_bot_state.corridor_violation_to_waypoint = to_waypoint;
            g_client_bot_state.corridor_violation_count = 1;
        }
        else {
            g_client_bot_state.corridor_violation_count = std::min(
                g_client_bot_state.corridor_violation_count + 1,
                16
            );
        }

        int required_violations = 2;
        if (bot_goal_is_control_point_objective(g_client_bot_state.active_goal)
            || bot_goal_is_item_collection(g_client_bot_state.active_goal)) {
            required_violations = 3;
        }
        return g_client_bot_state.corridor_violation_count >= required_violations;
    };

    if (!g_client_bot_state.validated_link_clear) {
        if (!corridor_violation_should_fail()) {
            return true;
        }
        bot_nav_register_failed_edge_cooldown(from_waypoint, to_waypoint, kFailedEdgeCooldownMs);
        start_recovery_anchor_reroute(entity, to_waypoint);
        return false;
    }

    rf::Vector3 from_pos{};
    rf::Vector3 to_pos{};
    if (!waypoints_get_pos(from_waypoint, from_pos) || !waypoints_get_pos(to_waypoint, to_pos)) {
        start_recovery_anchor_reroute(entity, to_waypoint);
        return false;
    }

    const float near_endpoint_sq = (kWaypointReachRadius * 1.1f) * (kWaypointReachRadius * 1.1f);
    const float dist_from_sq = rf::vec_dist_squared(&entity.pos, &from_pos);
    const float dist_to_sq = rf::vec_dist_squared(&entity.pos, &to_pos);
    if (dist_from_sq <= near_endpoint_sq || dist_to_sq <= near_endpoint_sq) {
        g_client_bot_state.corridor_violation_from_waypoint = 0;
        g_client_bot_state.corridor_violation_to_waypoint = 0;
        g_client_bot_state.corridor_violation_count = 0;
        return true;
    }

    float corridor_radius = kWaypointLinkCorridorRadius;
    if (bot_goal_is_control_point_objective(g_client_bot_state.active_goal)
        || bot_goal_is_item_collection(g_client_bot_state.active_goal)) {
        corridor_radius *= 1.35f;
    }
    if (g_client_bot_state.fsm_state == BotFsmState::recover_navigation) {
        corridor_radius *= 1.15f;
    }

    const float corridor_dist_sq = link_segment_distance_sq_xz(entity.pos, from_pos, to_pos);
    if (corridor_dist_sq <= corridor_radius * corridor_radius) {
        g_client_bot_state.corridor_violation_from_waypoint = 0;
        g_client_bot_state.corridor_violation_to_waypoint = 0;
        g_client_bot_state.corridor_violation_count = 0;
        return true;
    }

    if (!corridor_violation_should_fail()) {
        return true;
    }
    bot_nav_register_failed_edge_cooldown(from_waypoint, to_waypoint, kFailedEdgeCooldownMs);
    start_recovery_anchor_reroute(entity, to_waypoint);
    return false;
}

void clear_synthetic_movement_controls()
{
    g_client_bot_state.move_input_x = 0.0f;
    g_client_bot_state.move_input_y = 0.0f;
    g_client_bot_state.move_input_z = 0.0f;
    g_client_bot_state.nav_look_phase = 0.0f;
    g_client_bot_state.movement_override_active = false;
}

void reset_client_bot_state()
{
    clear_waypoint_route();
    g_client_bot_state.route_choice_lock_timer.invalidate();
    g_client_bot_state.route_choice_lock_goal_type = BotGoalType::none;
    g_client_bot_state.route_choice_lock_target_handle = -1;
    g_client_bot_state.route_choice_lock_target_identifier = -1;
    g_client_bot_state.route_choice_lock_goal_waypoint = 0;
    g_client_bot_state.route_choice_lock_next_waypoint = 0;
    clear_synthetic_movement_controls();
    g_client_bot_state.repath_timer.invalidate();
    g_client_bot_state.stuck_timer.invalidate();
    g_client_bot_state.geometry_stuck_sample_count = 0;
    g_client_bot_state.respawn_retry_timer.invalidate();
    g_client_bot_state.respawn_gearup_timer.invalidate();
    g_client_bot_state.respawn_uncrouch_retry_timer.invalidate();
    g_client_bot_state.retaliation_timer.invalidate();
    g_client_bot_state.retaliation_target_handle = -1;
    g_client_bot_state.last_recorded_health = -1.0f;
    g_client_bot_state.last_recorded_armor = -1.0f;
    g_client_bot_state.fire_decision_timer.invalidate();
    g_client_bot_state.auto_fire_release_guard_timer.invalidate();
    g_client_bot_state.ctf_threat_handle = -1;
    g_client_bot_state.ctf_threat_pos = {};
    g_client_bot_state.ctf_threat_visible = false;
    g_client_bot_state.ctf_objective_route_fail_timer.invalidate();
    g_client_bot_state.ctf_hold_goal_timer.invalidate();
    g_client_bot_state.control_point_route_fail_timer.invalidate();
    g_client_bot_state.control_point_patrol_waypoint = 0;
    g_client_bot_state.control_point_patrol_timer.invalidate();
    g_client_bot_state.firing.wants_fire = false;
    g_client_bot_state.firing.synthetic_primary_fire_down = false;
    g_client_bot_state.firing.synthetic_secondary_fire_down = false;
    g_client_bot_state.firing.synthetic_primary_fire_just_pressed = false;
    g_client_bot_state.firing.synthetic_secondary_fire_just_pressed = false;
    g_client_bot_state.jump_timer.invalidate();
    g_client_bot_state.jump_variation_timer.invalidate();
    g_client_bot_state.jump_target_link_from_waypoint = 0;
    g_client_bot_state.jump_target_link_to_waypoint = 0;
    g_client_bot_state.reload_timer.invalidate();
    g_client_bot_state.los_check_timer.invalidate();
    g_client_bot_state.los_route_solve_timer.invalidate();
    g_client_bot_state.los_target_handle = -1;
    g_client_bot_state.los_to_enemy = false;
    g_client_bot_state.preferred_enemy_handle = -1;
    g_client_bot_state.preferred_enemy_lock_timer.invalidate();
    g_client_bot_state.recovery_anchor_waypoint = 0;
    g_client_bot_state.recovery_avoid_waypoint = 0;
    g_client_bot_state.recovery_pending_reroute = false;
    g_client_bot_state.waypoint_progress_timer.invalidate();
    g_client_bot_state.waypoint_progress_waypoint = 0;
    g_client_bot_state.waypoint_progress_best_dist_sq = 0.0f;
    g_client_bot_state.waypoint_progress_no_improvement_windows = 0;
    g_client_bot_state.waypoint_progress_origin = {};
    g_client_bot_state.contextual_goal_eval_timer.invalidate();
    g_client_bot_state.contextual_goal_item_handle = -1;
    g_client_bot_state.contextual_goal_waypoint = 0;
    g_client_bot_state.contextual_goal_pos = {};
    g_client_bot_state.item_goal_contact_timer.invalidate();
    g_client_bot_state.item_goal_contact_handle = -1;
    g_client_bot_state.pursuit_target_handle = -1;
    g_client_bot_state.pursuit_route_failures = 0;
    g_client_bot_state.last_pursuit_route_was_fallback = false;
    g_client_bot_state.collect_route_failures = 0;
    g_client_bot_state.pursuit_recovery_timer.invalidate();
    g_client_bot_state.blind_progress_timer.invalidate();
    g_client_bot_state.blind_progress_origin = {};
    g_client_bot_state.blind_progress_max_dist_sq = 0.0f;
    g_client_bot_state.long_route_escape_timer.invalidate();
    g_client_bot_state.obstacle_probe_timer.invalidate();
    g_client_bot_state.corner_steer_probe_timer.invalidate();
    g_client_bot_state.forward_obstacle_guard_timer.invalidate();
    g_client_bot_state.forward_obstacle_reroute_timer.invalidate();
    g_client_bot_state.forward_obstacle_blocked_samples = 0;
    g_client_bot_state.forward_obstacle_progress_timer.invalidate();
    g_client_bot_state.forward_obstacle_progress_origin = {};
    g_client_bot_state.corner_probe_progress_timer.invalidate();
    g_client_bot_state.corner_probe_progress_origin = {};
    g_client_bot_state.closest_los_waypoint_cache_timer.invalidate();
    g_client_bot_state.closest_los_waypoint_cache_pos = {};
    g_client_bot_state.closest_los_waypoint_cache = 0;
    g_client_bot_state.fall_fast_forward_was_falling = false;
    g_client_bot_state.fall_fast_forward_pending = false;
    g_client_bot_state.fall_fast_forward_start_y = 0.0f;
    g_client_bot_state.fall_fast_forward_stuck_timer.invalidate();
    g_client_bot_state.fall_fast_forward_stuck_origin = {};
    g_client_bot_state.failed_edge_cooldowns.clear();
    g_client_bot_state.failed_waypoint_cooldowns.clear();
    g_client_bot_state.failed_item_goal_cooldowns.clear();
    g_client_bot_state.recent_item_goal_selections.clear();
    g_client_bot_state.alert_source_samples.clear();
    g_client_bot_state.alert_contacts.clear();
    g_client_bot_state.recent_waypoint_visits.clear();
    g_client_bot_state.combat_los_reposition_timer.invalidate();
    g_client_bot_state.combat_los_anchor_waypoint = 0;
    g_client_bot_state.combat_los_target_waypoint = 0;
    g_client_bot_state.combat_los_reposition_eval_timer.invalidate();
    g_client_bot_state.combat_los_waypoint_guard_timer.invalidate();
    g_client_bot_state.combat_los_waypoint_guard_waypoint = 0;
    g_client_bot_state.combat_los_waypoint_guard_target_handle = -1;
    g_client_bot_state.combat_los_waypoint_guard_visible = false;
    g_client_bot_state.firing.reset();
    g_client_bot_state.known_weapons.fill(false);
    g_client_bot_state.known_weapons_initialized = false;
    g_client_bot_state.weapon_switch_timer.invalidate();
    g_client_bot_state.combat_weapon_eval_timer.invalidate();
    g_client_bot_state.combat_maneuver_timer.invalidate();
    g_client_bot_state.combat_crouch_timer.invalidate();
    g_client_bot_state.traversal_crouch_active = false;
    g_client_bot_state.traversal_crouch_toggled_on = false;
    g_client_bot_state.combat_maneuver_strafe = 0.0f;
    g_client_bot_state.active_goal = BotGoalType::none;
    g_client_bot_state.goal_target_handle = -1;
    g_client_bot_state.goal_target_identifier = -1;
    g_client_bot_state.goal_target_waypoint = 0;
    g_client_bot_state.goal_target_pos = {};
    g_client_bot_state.goal_stuck_wd.goal = BotGoalType::none;
    g_client_bot_state.goal_stuck_wd.fsm = BotFsmState::inactive;
    g_client_bot_state.goal_stuck_wd.handle = -1;
    g_client_bot_state.goal_stuck_wd.identifier = -1;
    g_client_bot_state.goal_stuck_wd.waypoint = 0;
    g_client_bot_state.goal_stuck_wd.route_waypoint = 0;
    g_client_bot_state.goal_stuck_wd.route_next_index = 0;
    g_client_bot_state.goal_stuck_wd.recovery_anchor = 0;
    g_client_bot_state.goal_stuck_wd.timer.invalidate();
    g_client_bot_state.goal_stuck_wd.origin = {};
    g_client_bot_state.goal_stuck_wd.retry_window_timer.invalidate();
    g_client_bot_state.goal_stuck_wd.retry_count = 0;
    g_client_bot_state.eliminate_target_reacquire_timer.invalidate();
    g_client_bot_state.goal_eval_timer.invalidate();
    g_client_bot_state.goal_switch_lock_timer.invalidate();
    g_client_bot_state.bridge.zone_uid = -1;
    g_client_bot_state.bridge.trigger_uid = -1;
    g_client_bot_state.bridge.trigger_pos = {};
    g_client_bot_state.bridge.activation_radius = 0.0f;
    g_client_bot_state.bridge.requires_use = false;
    g_client_bot_state.bridge.use_press_timer.invalidate();
    g_client_bot_state.bridge.activation_abort_timer.invalidate();
    g_client_bot_state.bridge.activation_best_dist_sq = std::numeric_limits<float>::infinity();
    g_client_bot_state.bridge.post_open_zone_uid = -1;
    g_client_bot_state.bridge.post_open_target_waypoint = 0;
    g_client_bot_state.bridge.post_open_priority_timer.invalidate();
    g_client_bot_state.crater_goal_abort_timer.invalidate();
    g_client_bot_state.crater_goal_timeout_timer.invalidate();
    g_client_bot_state.shatter_goal_abort_timer.invalidate();
    g_client_bot_state.shatter_goal_timeout_timer.invalidate();
    g_client_bot_state.confusion_abort_timer.invalidate();
    g_client_bot_state.confusion_abort_pos = {};
    g_client_bot_state.confusion_abort_aim_dir = {};
    g_client_bot_state.confusion_abort_last_fired_timestamp = -1;
    set_last_heading_change_reason("none");
    g_client_bot_state.fsm_state = BotFsmState::inactive;
    g_client_bot_state.fsm_state_timer.invalidate();
    g_client_bot_state.recent_secondary_goal_type = BotGoalType::none;
    g_client_bot_state.recent_secondary_goal_timer.invalidate();
    g_client_bot_state.hud_status_timer.invalidate();
    g_client_bot_state.console_status_timer.invalidate();
    g_client_bot_state.no_move_target_watchdog_timer.invalidate();
    g_client_bot_state.no_move_target_watchdog_retries = 0;
    g_client_bot_state.position_stall_wd.timer.invalidate();
    g_client_bot_state.position_stall_wd.origin = {};
    g_client_bot_state.position_stall_wd.waypoint = 0;
    g_client_bot_state.position_stall_wd.retry_window_timer.invalidate();
    g_client_bot_state.position_stall_wd.goal = BotGoalType::none;
    g_client_bot_state.position_stall_wd.handle = -1;
    g_client_bot_state.position_stall_wd.identifier = -1;
    g_client_bot_state.position_stall_wd.retry_count = 0;
    g_client_bot_state.objective_progress_watchdog_timer.invalidate();
    g_client_bot_state.objective_progress_retry_window_timer.invalidate();
    g_client_bot_state.objective_progress_watchdog_goal = BotGoalType::none;
    g_client_bot_state.objective_progress_watchdog_fsm = BotFsmState::inactive;
    g_client_bot_state.objective_progress_watchdog_handle = -1;
    g_client_bot_state.objective_progress_watchdog_identifier = -1;
    g_client_bot_state.objective_progress_watchdog_waypoint = 0;
    g_client_bot_state.objective_progress_watchdog_route_waypoint = 0;
    g_client_bot_state.objective_progress_watchdog_route_next_index = 0;
    g_client_bot_state.objective_progress_watchdog_best_dist_sq = std::numeric_limits<float>::infinity();
    g_client_bot_state.objective_progress_watchdog_retry_count = 0;
    g_client_bot_state.global_failsafe_wd.timer.invalidate();
    g_client_bot_state.global_failsafe_wd.retry_window_timer.invalidate();
    g_client_bot_state.global_failsafe_wd.origin = {};
    g_client_bot_state.global_failsafe_wd.aim_dir = {};
    g_client_bot_state.global_failsafe_wd.goal = BotGoalType::none;
    g_client_bot_state.global_failsafe_wd.fsm = BotFsmState::inactive;
    g_client_bot_state.global_failsafe_wd.handle = -1;
    g_client_bot_state.global_failsafe_wd.identifier = -1;
    g_client_bot_state.global_failsafe_wd.last_fired_timestamp = -1;
    g_client_bot_state.global_failsafe_wd.retry_count = 0;
}

bool should_check_stuck_progress(const rf::Entity& entity)
{
    if (g_client_bot_state.waypoint_path.empty()
        && !g_client_bot_state.has_waypoint_target) {
        return false;
    }

    if (!g_client_bot_state.has_waypoint_target) {
        return true;
    }

    const float dist_sq = rf::vec_dist_squared(
        &entity.pos,
        &g_client_bot_state.waypoint_target_pos
    );
    const float min_progress_dist = kWaypointReachRadius * 1.35f;
    return dist_sq > min_progress_dist * min_progress_dist;
}

bool try_recover_from_stuck_on_geometry(const rf::Entity& entity)
{
    if (!g_client_bot_state.stuck_timer.valid()) {
        g_client_bot_state.stuck_timer.set(kWaypointStuckCheckMs);
        g_client_bot_state.last_pos = entity.pos;
        return false;
    }

    if (!g_client_bot_state.stuck_timer.elapsed()) {
        return false;
    }

    rf::Vector3 moved = entity.pos - g_client_bot_state.last_pos;
    moved.y = 0.0f;
    const float moved_sq = moved.len_sq();
    g_client_bot_state.last_pos = entity.pos;
    g_client_bot_state.stuck_timer.set(kWaypointStuckCheckMs);

    if (!should_check_stuck_progress(entity)
        || moved_sq >= kWaypointStuckDistance * kWaypointStuckDistance) {
        g_client_bot_state.geometry_stuck_sample_count = 0;
        return false;
    }

    const int required_samples = g_client_bot_state.active_goal == BotGoalType::control_point_objective
        ? 3
        : 2;
    ++g_client_bot_state.geometry_stuck_sample_count;
    if (g_client_bot_state.geometry_stuck_sample_count < required_samples) {
        return false;
    }
    g_client_bot_state.geometry_stuck_sample_count = 0;

    int blocked_waypoint = 0;
    int previous_waypoint = 0;
    if (!g_client_bot_state.waypoint_path.empty()
        && g_client_bot_state.waypoint_next_index >= 0
        && g_client_bot_state.waypoint_next_index
            < static_cast<int>(g_client_bot_state.waypoint_path.size())) {
        blocked_waypoint =
            g_client_bot_state.waypoint_path[g_client_bot_state.waypoint_next_index];
        if (g_client_bot_state.waypoint_next_index > 0) {
            previous_waypoint =
                g_client_bot_state.waypoint_path[g_client_bot_state.waypoint_next_index - 1];
        }
    }

    if (blocked_waypoint > 0) {
        const int blacklist_waypoint =
            bot_nav_choose_blacklist_waypoint_for_failed_link(
                previous_waypoint,
                blocked_waypoint
            );
        if (blacklist_waypoint > 0) {
            bot_nav_blacklist_waypoint_temporarily(
                blacklist_waypoint,
                kFailedWaypointBlacklistMs
            );
        }
        if (previous_waypoint <= 0) {
            previous_waypoint = bot_find_closest_waypoint_with_los_fallback(entity);
        }
        if (previous_waypoint > 0 && previous_waypoint != blocked_waypoint) {
            bot_nav_register_failed_edge_cooldown(
                previous_waypoint,
                blocked_waypoint,
                kFailedEdgeCooldownMs
            );
        }
    }

    const int recovery_avoid_waypoint =
        bot_nav_choose_blacklist_waypoint_for_failed_link(previous_waypoint, blocked_waypoint);
    start_recovery_anchor_reroute(
        entity,
        recovery_avoid_waypoint > 0 ? recovery_avoid_waypoint : blocked_waypoint
    );
    return false;
}

bool try_recover_from_corner_probe(
    const rf::Entity& entity,
    const rf::Vector3& move_target)
{
    if (!g_client_bot_state.obstacle_probe_timer.valid()) {
        g_client_bot_state.obstacle_probe_timer.set(kObstacleProbeIntervalMs);
        return false;
    }

    if (!g_client_bot_state.obstacle_probe_timer.elapsed()) {
        return false;
    }
    g_client_bot_state.obstacle_probe_timer.set(kObstacleProbeIntervalMs);

    rf::Vector3 aim_forward = forward_from_non_linear_yaw_pitch(
        entity.control_data.phb.y,
        0.0f
    );
    aim_forward.y = 0.0f;
    if (aim_forward.len_sq() < 0.001f) {
        aim_forward = move_target - entity.pos;
        aim_forward.y = 0.0f;
    }

    if (aim_forward.len_sq() < 0.25f) {
        return false;
    }

    aim_forward.normalize_safe();
    rf::Vector3 aim_right{aim_forward.z, 0.0f, -aim_forward.x};
    aim_right.normalize_safe();
    const rf::Vector3 origin = entity.eye_pos;

    const bool right_blocked = is_probe_direction_blocked(
        entity,
        origin,
        aim_right,
        kObstacleProbeDistance
    );
    const bool left_blocked = is_probe_direction_blocked(
        entity,
        origin,
        rf::Vector3{-aim_right.x, 0.0f, -aim_right.z},
        kObstacleProbeDistance
    );
    const bool back_blocked = is_probe_direction_blocked(
        entity,
        origin,
        rf::Vector3{-aim_forward.x, 0.0f, -aim_forward.z},
        kObstacleProbeDistance
    );
    const bool forward_blocked = is_probe_direction_blocked(
        entity,
        origin,
        aim_forward,
        kObstacleForwardProbeDistance
    );

    const int blocked_count = static_cast<int>(forward_blocked)
        + static_cast<int>(back_blocked)
        + static_cast<int>(left_blocked)
        + static_cast<int>(right_blocked);
    const bool corner_blocked =
        (left_blocked || right_blocked) && (forward_blocked || back_blocked);
    const bool trapped = corner_blocked || (forward_blocked && blocked_count >= 2);

    if (!trapped) {
        g_client_bot_state.corner_probe_progress_timer.invalidate();
        g_client_bot_state.corner_probe_progress_origin = {};
        return false;
    }

    if (!g_client_bot_state.corner_probe_progress_timer.valid()) {
        g_client_bot_state.corner_probe_progress_timer.set(kNavigationBlockedProgressWindowMs);
        g_client_bot_state.corner_probe_progress_origin = entity.pos;
        return false;
    }

    if (!g_client_bot_state.corner_probe_progress_timer.elapsed()) {
        return false;
    }

    rf::Vector3 moved = entity.pos - g_client_bot_state.corner_probe_progress_origin;
    moved.y = 0.0f;
    g_client_bot_state.corner_probe_progress_timer.set(kNavigationBlockedProgressWindowMs);
    g_client_bot_state.corner_probe_progress_origin = entity.pos;
    if (moved.len_sq()
        >= kNavigationBlockedProgressDistance * kNavigationBlockedProgressDistance) {
        return false;
    }

    start_recovery_anchor_reroute(entity, g_client_bot_state.waypoint_goal);
    return true;
}

bool pick_waypoint_route(const int start_waypoint)
{
    return bot_nav_pick_waypoint_route(start_waypoint);
}

bool pick_waypoint_route_to_goal(const int start_waypoint, const int goal_waypoint, const int repath_ms)
{
    return bot_nav_pick_waypoint_route_to_goal(
        start_waypoint,
        goal_waypoint,
        repath_ms
    );
}

bool pick_waypoint_route_to_los_goal(
    const int start_waypoint,
    const rf::Vector3& eye_offset,
    const rf::Vector3& target_pos,
    const rf::Vector3& target_eye_pos,
    const rf::Object* target_obj,
    const int repath_ms)
{
    if (start_waypoint <= 0) {
        return false;
    }

    struct RawCandidate
    {
        int waypoint = 0;
        rf::Vector3 pos{};
        float dist_sq_to_target = 0.0f;
    };

    struct Candidate
    {
        int waypoint = 0;
        float dist_sq_to_target = 0.0f;
    };

    const int waypoint_total = waypoints_count();
    std::vector<RawCandidate> raw_candidates;
    raw_candidates.reserve(waypoint_total);
    for (int waypoint = 1; waypoint < waypoint_total; ++waypoint) {
        if (waypoint == start_waypoint) {
            continue;
        }

        rf::Vector3 waypoint_pos{};
        if (!waypoints_get_pos(waypoint, waypoint_pos)) {
            continue;
        }

        raw_candidates.push_back(RawCandidate{
            waypoint,
            waypoint_pos,
            rf::vec_dist_squared(&waypoint_pos, &target_pos),
        });
    }

    if (raw_candidates.empty()) {
        return false;
    }

    std::sort(
        raw_candidates.begin(),
        raw_candidates.end(),
        [](const RawCandidate& lhs, const RawCandidate& rhs) {
            return lhs.dist_sq_to_target < rhs.dist_sq_to_target;
        }
    );

    int los_probe_limit = std::min<int>(static_cast<int>(raw_candidates.size()), 160);
    if (waypoint_total >= 1800) {
        los_probe_limit = std::min(los_probe_limit, 120);
    }
    if (waypoint_total >= 2500) {
        los_probe_limit = std::min(los_probe_limit, 96);
    }

    std::vector<Candidate> candidates;
    candidates.reserve(los_probe_limit);
    for (int probe_index = 0; probe_index < los_probe_limit; ++probe_index) {
        const RawCandidate& raw = raw_candidates[probe_index];
        if (!waypoint_has_visibility_to_target(
                raw.pos,
                eye_offset,
                target_pos,
                target_eye_pos,
                target_obj)) {
            continue;
        }
        candidates.push_back(Candidate{
            raw.waypoint,
            raw.dist_sq_to_target,
        });
    }

    if (candidates.empty()) {
        return false;
    }

    int max_candidates = std::min<int>(
        static_cast<int>(candidates.size()),
        std::min(kLosWaypointCandidateLimit, kWaypointPickAttempts)
    );
    if (waypoint_total >= 1800) {
        max_candidates = std::min(max_candidates, 12);
    }
    if (waypoint_total >= 2500) {
        max_candidates = std::min(max_candidates, 8);
    }

    struct RouteCandidate
    {
        int goal_waypoint = 0;
        float score = 0.0f;
        std::vector<int> path{};
    };

    std::vector<int> avoidset{};
    bot_nav_append_blacklisted_waypoints_to_avoidset(
        avoidset,
        start_waypoint,
        0
    );
    std::sort(avoidset.begin(), avoidset.end());
    const bool has_blacklisted_avoidance = !avoidset.empty();
    const std::vector<int> no_avoidset{};
    std::vector<RouteCandidate> route_candidates;
    auto collect_candidates = [&](const std::vector<int>& active_avoidset) {
        std::vector<int> path;
        for (int candidate_index = 0; candidate_index < max_candidates; ++candidate_index) {
            const Candidate& candidate = candidates[candidate_index];
            path.clear();
            if (!bot_waypoint_route(start_waypoint, candidate.waypoint, active_avoidset, path)
                || path.size() < 2
                || bot_nav_path_contains_failed_edge_cooldown(path)) {
                continue;
            }

            const float route_length = compute_waypoint_path_length(path);
            if (!std::isfinite(route_length)) {
                continue;
            }

            const float dist_to_target = std::sqrt(std::max(candidate.dist_sq_to_target, 0.0f));
            const float route_score = bot_nav_compute_route_score(path, dist_to_target);
            route_candidates.push_back(
                RouteCandidate{
                    candidate.waypoint,
                    route_score,
                    path,
                }
            );
        }
    };

    collect_candidates(avoidset);
    if (route_candidates.empty() && has_blacklisted_avoidance) {
        collect_candidates(no_avoidset);
    }

    if (route_candidates.empty()) {
        return false;
    }

    std::sort(
        route_candidates.begin(),
        route_candidates.end(),
        [](const RouteCandidate& lhs, const RouteCandidate& rhs) {
            return lhs.score > rhs.score;
        }
    );

    const int pool_size = std::min<int>(
        static_cast<int>(route_candidates.size()),
        kRouteSelectionPool
    );
    const int selected_rank = choose_route_candidate_index(
        route_candidates,
        pool_size,
        0
    );
    RouteCandidate& selected = route_candidates[selected_rank];

    set_route_choice_lock_from_path(selected.path, selected.goal_waypoint);
    g_client_bot_state.waypoint_path = std::move(selected.path);
    g_client_bot_state.waypoint_next_index = 1;
    g_client_bot_state.waypoint_goal = selected.goal_waypoint;
    g_client_bot_state.repath_timer.set(repath_ms);
    return true;
}

bool commit_single_hop_fallback_route(
    const int start_waypoint,
    const int route_goal_waypoint,
    const rf::Vector3& destination,
    const int repath_ms,
    const char* heading_change_reason)
{
    if (start_waypoint <= 0) {
        return false;
    }

    std::array<int, kMaxWaypointLinks> links{};
    const int link_count = waypoints_get_links(start_waypoint, links);
    if (link_count <= 0) {
        return false;
    }

    int best_neighbor = 0;
    float best_score = std::numeric_limits<float>::max();
    for (int i = 0; i < link_count; ++i) {
        const int neighbor = links[i];
        if (neighbor <= 0 || neighbor == start_waypoint) {
            continue;
        }
        if (bot_nav_is_failed_edge_cooldown_active_no_prune(start_waypoint, neighbor)) {
            continue;
        }
        if (bot_nav_is_waypoint_blacklisted_no_prune(neighbor)) {
            continue;
        }
        if (!bot_nav_is_link_traversable_for_route(start_waypoint, neighbor)) {
            continue;
        }
        if (!waypoints_has_direct_link(start_waypoint, neighbor)
            || !waypoints_link_is_clear(start_waypoint, neighbor)) {
            continue;
        }

        rf::Vector3 neighbor_pos{};
        if (!waypoints_get_pos(neighbor, neighbor_pos)) {
            continue;
        }

        const float score = rf::vec_dist_squared(&neighbor_pos, &destination);
        if (score < best_score) {
            best_score = score;
            best_neighbor = neighbor;
        }
    }

    if (best_neighbor <= 0) {
        return false;
    }

    g_client_bot_state.waypoint_path.clear();
    g_client_bot_state.waypoint_path.push_back(start_waypoint);
    g_client_bot_state.waypoint_path.push_back(best_neighbor);
    g_client_bot_state.waypoint_next_index = 1;
    g_client_bot_state.waypoint_goal =
        route_goal_waypoint > 0 ? route_goal_waypoint : best_neighbor;
    set_route_choice_lock_from_path(
        g_client_bot_state.waypoint_path,
        g_client_bot_state.waypoint_goal
    );
    g_client_bot_state.repath_timer.set(std::clamp(repath_ms, 125, 1500));
    set_last_heading_change_reason(heading_change_reason);
    return true;
}

bool update_waypoint_target_from_current_path(const rf::Entity& entity)
{
    if (g_client_bot_state.waypoint_path.empty()) {
        clear_waypoint_route();
        return false;
    }

    if (g_client_bot_state.waypoint_next_index < 0
        || g_client_bot_state.waypoint_next_index
            >= static_cast<int>(g_client_bot_state.waypoint_path.size())) {
        clear_waypoint_route();
        return false;
    }

    int current_waypoint =
        g_client_bot_state.waypoint_path[g_client_bot_state.waypoint_next_index];
    rf::Vector3 target_pos{};
    if (!waypoints_get_pos(current_waypoint, target_pos)) {
        clear_waypoint_route();
        return false;
    }
    update_fall_fast_forward_transition_state(entity);
    (void)try_fast_forward_path_on_downward_fall(entity, current_waypoint, target_pos);
    if (handle_fall_fast_forward_stuck_guard(entity, current_waypoint)) {
        return false;
    }

    if (g_client_bot_state.pursuit_target_handle >= 0 && g_client_bot_state.los_to_enemy) {
        rf::Entity* pursuit_target =
            rf::entity_from_handle(g_client_bot_state.pursuit_target_handle);
        if (pursuit_target) {
            const bool should_refresh_waypoint_los =
                !g_client_bot_state.combat_los_waypoint_guard_timer.valid()
                || g_client_bot_state.combat_los_waypoint_guard_timer.elapsed()
                || g_client_bot_state.combat_los_waypoint_guard_waypoint != current_waypoint
                || g_client_bot_state.combat_los_waypoint_guard_target_handle
                    != pursuit_target->handle;
            if (should_refresh_waypoint_los) {
                const rf::Vector3 eye_offset = entity.eye_pos - entity.pos;
                g_client_bot_state.combat_los_waypoint_guard_visible =
                    waypoint_has_visibility_to_target(
                        target_pos,
                        eye_offset,
                        pursuit_target->pos,
                        pursuit_target->eye_pos,
                        pursuit_target
                    );
                g_client_bot_state.combat_los_waypoint_guard_waypoint = current_waypoint;
                g_client_bot_state.combat_los_waypoint_guard_target_handle =
                    pursuit_target->handle;
                g_client_bot_state.combat_los_waypoint_guard_timer.set(
                    kCombatLosSampleIntervalMs
                );
            }

            if (!g_client_bot_state.combat_los_waypoint_guard_visible) {
                start_recovery_anchor_reroute(entity, current_waypoint);
                // LOS can flicker near corners; keep advancing this frame while recovery replans.
            }
        }
        else {
            g_client_bot_state.combat_los_waypoint_guard_timer.invalidate();
            g_client_bot_state.combat_los_waypoint_guard_waypoint = 0;
            g_client_bot_state.combat_los_waypoint_guard_target_handle = -1;
            g_client_bot_state.combat_los_waypoint_guard_visible = false;
        }
    }
    else {
        g_client_bot_state.combat_los_waypoint_guard_timer.invalidate();
        g_client_bot_state.combat_los_waypoint_guard_waypoint = 0;
        g_client_bot_state.combat_los_waypoint_guard_target_handle = -1;
        g_client_bot_state.combat_los_waypoint_guard_visible = false;
    }

    if (!enforce_current_link_corridor(entity)) {
        clear_waypoint_route();
        return false;
    }

    const float distance_sq = rf::vec_dist_squared(&entity.pos, &target_pos);
    if (g_client_bot_state.waypoint_progress_waypoint != current_waypoint) {
        g_client_bot_state.waypoint_progress_waypoint = current_waypoint;
        g_client_bot_state.waypoint_progress_best_dist_sq = distance_sq;
        g_client_bot_state.waypoint_progress_no_improvement_windows = 0;
        g_client_bot_state.waypoint_progress_timer.set(current_waypoint_progress_timeout_ms());
        g_client_bot_state.waypoint_progress_origin = entity.pos;
    }
    else {
        const float improvement =
            g_client_bot_state.waypoint_progress_best_dist_sq - distance_sq;
        if (improvement >= kWaypointProgressMinImprovement * kWaypointProgressMinImprovement) {
            g_client_bot_state.waypoint_progress_best_dist_sq = distance_sq;
            g_client_bot_state.waypoint_progress_no_improvement_windows = 0;
            g_client_bot_state.waypoint_progress_timer.set(current_waypoint_progress_timeout_ms());
            g_client_bot_state.waypoint_progress_origin = entity.pos;
        }
        else if (g_client_bot_state.waypoint_progress_timer.valid()
                 && g_client_bot_state.waypoint_progress_timer.elapsed()) {
            rf::Vector3 moved = entity.pos - g_client_bot_state.waypoint_progress_origin;
            moved.y = 0.0f;
            const bool has_world_progress = moved.len_sq()
                >= kNavigationBlockedProgressDistance * kNavigationBlockedProgressDistance;
            bool should_fail_progress = !has_world_progress;
            if (has_world_progress) {
                ++g_client_bot_state.waypoint_progress_no_improvement_windows;
                should_fail_progress =
                    g_client_bot_state.waypoint_progress_no_improvement_windows
                    >= kWaypointNoImprovementWindowLimit;
            }

            if (should_fail_progress) {
                int previous_waypoint = 0;
                if (g_client_bot_state.waypoint_next_index > 0
                    && g_client_bot_state.waypoint_next_index
                        < static_cast<int>(g_client_bot_state.waypoint_path.size())) {
                    previous_waypoint =
                        g_client_bot_state.waypoint_path[g_client_bot_state.waypoint_next_index - 1];
                }
                if (previous_waypoint > 0 && previous_waypoint != current_waypoint) {
                    bot_nav_register_failed_edge_cooldown(
                        previous_waypoint,
                        current_waypoint,
                        kFailedEdgeCooldownMs
                    );
                }
                const int blacklist_waypoint =
                    bot_nav_choose_blacklist_waypoint_for_failed_link(
                        previous_waypoint,
                        current_waypoint
                    );
                if (blacklist_waypoint > 0) {
                    bot_nav_blacklist_waypoint_temporarily(
                        blacklist_waypoint,
                        kFailedWaypointBlacklistMs
                    );
                }
                start_recovery_anchor_reroute(
                    entity,
                    blacklist_waypoint > 0 ? blacklist_waypoint : current_waypoint
                );
                return false;
            }

            // Movement happened, but we are not closing distance to the same waypoint.
            // Keep trying briefly before forcing a reroute.
            g_client_bot_state.waypoint_progress_timer.set(current_waypoint_progress_timeout_ms());
            g_client_bot_state.waypoint_progress_origin = entity.pos;
        }
    }

    float waypoint_reach_radius =
        compute_waypoint_advance_radius_for_turn(g_client_bot_state.waypoint_next_index);
    int current_type_raw = 0;
    int current_subtype = 0;
    if (waypoints_get_type_subtype(current_waypoint, current_type_raw, current_subtype)) {
        const auto current_type = static_cast<WaypointType>(current_type_raw);
        // Jump pads must be entered, not just approached.
        if (current_type == WaypointType::jump_pad) {
            waypoint_reach_radius = std::min(waypoint_reach_radius, kJumpPadWaypointReachRadius);
        }
    }
    if (distance_sq <= waypoint_reach_radius * waypoint_reach_radius) {
        bot_nav_record_waypoint_visit(current_waypoint);
        g_client_bot_state.waypoint_progress_timer.invalidate();
        g_client_bot_state.waypoint_progress_waypoint = 0;
        g_client_bot_state.waypoint_progress_best_dist_sq = 0.0f;
        g_client_bot_state.waypoint_progress_no_improvement_windows = 0;
        g_client_bot_state.waypoint_progress_origin = {};

        int loop_waypoint_a = 0;
        int loop_waypoint_b = 0;
        if (g_client_bot_state.pursuit_target_handle >= 0
            && !g_client_bot_state.los_to_enemy
            && bot_nav_detect_recent_waypoint_ping_pong_loop(loop_waypoint_a, loop_waypoint_b)) {
            const int loop_cooldown_ms = std::max(kLoopBreakerEdgeCooldownMs, 9000);
            bot_nav_register_failed_edge_cooldown_bidirectional(
                loop_waypoint_a,
                loop_waypoint_b,
                loop_cooldown_ms
            );
            trigger_loop_escape_mode();
            g_client_bot_state.recent_waypoint_visits.clear();
            const int avoid_waypoint = (current_waypoint == loop_waypoint_b)
                ? loop_waypoint_b
                : loop_waypoint_a;
            start_recovery_anchor_reroute(entity, avoid_waypoint);
            return false;
        }

        if (g_client_bot_state.waypoint_next_index + 1
            < static_cast<int>(g_client_bot_state.waypoint_path.size())) {
            ++g_client_bot_state.waypoint_next_index;
            current_waypoint =
                g_client_bot_state.waypoint_path[g_client_bot_state.waypoint_next_index];
            if (!waypoints_get_pos(current_waypoint, target_pos)) {
                clear_waypoint_route();
                return false;
            }
        }
        else {
            // Reached end of path. For roam, immediately pick a new route so the
            // bot keeps moving continuously without a stop-and-go pause.
            clear_waypoint_route();
            if (g_client_bot_state.active_goal == BotGoalType::roam
                || g_client_bot_state.active_goal == BotGoalType::none) {
                if (pick_waypoint_route(current_waypoint)) {
                    return update_waypoint_target_from_current_path(entity);
                }
            }
            return false;
        }
    }

    g_client_bot_state.waypoint_target_pos = target_pos;
    g_client_bot_state.has_waypoint_target = true;
    return true;
}

bool update_waypoint_target(const rf::Entity& entity)
{
    if (try_recover_from_stuck_on_geometry(entity)) {
        return true;
    }
    if (continue_recovery_anchor_move(entity)) {
        return true;
    }

    int closest_waypoint = bot_find_closest_waypoint_with_los_fallback(entity);
    if (g_client_bot_state.recovery_pending_reroute
        && g_client_bot_state.recovery_anchor_waypoint > 0) {
        closest_waypoint = g_client_bot_state.recovery_anchor_waypoint;
    }
    if (closest_waypoint <= 0) {
        clear_waypoint_route();
        return false;
    }

    // If the closest waypoint has no outgoing links (e.g., a crater hole waypoint
    // with stripped links), it's useless as a route start. Walk directly toward it
    // and let the next frame find a better starting waypoint once we've moved.
    {
        std::array<int, kMaxWaypointLinks> links{};
        if (waypoints_get_links(closest_waypoint, links) == 0) {
            rf::Vector3 wp_pos{};
            if (waypoints_get_pos(closest_waypoint, wp_pos)) {
                g_client_bot_state.waypoint_target_pos = wp_pos;
                g_client_bot_state.has_waypoint_target = true;
                return true;
            }
        }
    }

    const bool need_route =
        g_client_bot_state.waypoint_path.empty()
        || g_client_bot_state.waypoint_next_index < 0
        || g_client_bot_state.waypoint_next_index
            >= static_cast<int>(g_client_bot_state.waypoint_path.size())
        || (g_client_bot_state.repath_timer.valid()
            && g_client_bot_state.repath_timer.elapsed());

    if (need_route && !pick_waypoint_route(closest_waypoint)) {
        if (!commit_single_hop_fallback_route(
                closest_waypoint,
                0,
                entity.pos,
                kWaypointRecoveryRepathMs,
                "fallback_single_hop_roam")) {
            start_recovery_anchor_reroute(entity, 0);
            if (set_recovery_anchor_direct_target(entity)) {
                return true;
            }
            // Last resort: walk directly toward the nearest waypoint position.
            rf::Vector3 wp_pos{};
            if (waypoints_get_pos(closest_waypoint, wp_pos)) {
                g_client_bot_state.waypoint_target_pos = wp_pos;
                g_client_bot_state.has_waypoint_target = true;
                return true;
            }
            return false;
        }
    }
    const bool updated = update_waypoint_target_from_current_path(entity);
    if (updated) {
        g_client_bot_state.recovery_pending_reroute = false;
        g_client_bot_state.recovery_anchor_waypoint = 0;
        g_client_bot_state.recovery_avoid_waypoint = 0;
    }
    return updated;
}

bool update_waypoint_target_towards(
    const rf::Entity& entity,
    const rf::Vector3& destination,
    const rf::Vector3* los_target_eye_pos,
    const rf::Object* los_target_obj,
    const int repath_ms)
{
    constexpr float kGoalWaypointSwitchMinDist = kWaypointReachRadius * 1.5f;
    constexpr float kPreferredGoalWaypointSnapRadius = kWaypointReachRadius * 2.75f;
    constexpr int kEliminateRecoveryMinRepathMs = 900;
    constexpr float kEliminateRecoveryGoalWaypointRetargetDistance = 9.5f;
    g_client_bot_state.last_pursuit_route_was_fallback = false;

    const bool stabilize_eliminate_recovery_route_goal =
        g_client_bot_state.active_goal == BotGoalType::eliminate_target
        && (g_client_bot_state.fsm_state == BotFsmState::recover_navigation
            || g_client_bot_state.recovery_pending_reroute);

    int effective_repath_ms = repath_ms;
    if (stabilize_eliminate_recovery_route_goal) {
        effective_repath_ms = std::max(effective_repath_ms, kEliminateRecoveryMinRepathMs);
    }

    if (try_recover_from_stuck_on_geometry(entity)) {
        return true;
    }
    if (continue_recovery_anchor_move(entity)) {
        return true;
    }

    int goal_waypoint = 0;
    if (stabilize_eliminate_recovery_route_goal && g_client_bot_state.waypoint_goal > 0) {
        rf::Vector3 current_goal_pos{};
        if (waypoints_get_pos(g_client_bot_state.waypoint_goal, current_goal_pos)) {
            const float retarget_dist_sq = rf::vec_dist_squared(&current_goal_pos, &destination);
            if (retarget_dist_sq
                <= kEliminateRecoveryGoalWaypointRetargetDistance
                    * kEliminateRecoveryGoalWaypointRetargetDistance) {
                goal_waypoint = g_client_bot_state.waypoint_goal;
            }
        }
    }
    if (!los_target_eye_pos
        && goal_waypoint <= 0
        && g_client_bot_state.goal_target_waypoint > 0
        && (bot_goal_is_item_collection(g_client_bot_state.active_goal)
            || bot_goal_is_control_point_objective(g_client_bot_state.active_goal)
            || bot_goal_is_ctf_objective(g_client_bot_state.active_goal)
            || g_client_bot_state.active_goal == BotGoalType::activate_bridge
            || g_client_bot_state.active_goal == BotGoalType::create_crater
            || g_client_bot_state.active_goal == BotGoalType::shatter_glass)) {
        rf::Vector3 preferred_goal_pos{};
        if (waypoints_get_pos(g_client_bot_state.goal_target_waypoint, preferred_goal_pos)) {
            const float goal_snap_dist_sq = rf::vec_dist_squared(&preferred_goal_pos, &destination);
            if (goal_snap_dist_sq
                <= kPreferredGoalWaypointSnapRadius * kPreferredGoalWaypointSnapRadius) {
                goal_waypoint = g_client_bot_state.goal_target_waypoint;
            }
        }
    }
    if (goal_waypoint <= 0) {
        goal_waypoint = bot_find_closest_waypoint_with_fallback(destination);
    }
    if (goal_waypoint <= 0) {
        clear_waypoint_route();
        return false;
    }

    const bool path_invalid =
        g_client_bot_state.waypoint_path.empty()
        || g_client_bot_state.waypoint_next_index < 0
        || g_client_bot_state.waypoint_next_index
            >= static_cast<int>(g_client_bot_state.waypoint_path.size())
        || !g_client_bot_state.repath_timer.valid();
    const bool allow_opportunistic_repath =
        g_client_bot_state.recovery_pending_reroute
        && !stabilize_eliminate_recovery_route_goal;

    bool need_route = path_invalid;
    if (!need_route && allow_opportunistic_repath && !los_target_eye_pos
        && g_client_bot_state.waypoint_goal != goal_waypoint) {
        bool should_switch_anchor = g_client_bot_state.recovery_pending_reroute;
        if (!should_switch_anchor) {
            rf::Vector3 current_goal_pos{};
            rf::Vector3 new_goal_pos{};
            if (g_client_bot_state.waypoint_goal <= 0
                || !waypoints_get_pos(g_client_bot_state.waypoint_goal, current_goal_pos)
                || !waypoints_get_pos(goal_waypoint, new_goal_pos)) {
                should_switch_anchor = true;
            }
            else {
                const float anchor_shift_sq = rf::vec_dist_squared(
                    &current_goal_pos,
                    &new_goal_pos
                );
                should_switch_anchor =
                    anchor_shift_sq
                    > kGoalWaypointSwitchMinDist * kGoalWaypointSwitchMinDist;
            }
        }
        need_route = should_switch_anchor;
    }

    if (!need_route && allow_opportunistic_repath && los_target_eye_pos) {
        rf::Vector3 goal_waypoint_pos{};
        const rf::Vector3 eye_offset = entity.eye_pos - entity.pos;
        if (g_client_bot_state.waypoint_goal <= 0
            || !waypoints_get_pos(g_client_bot_state.waypoint_goal, goal_waypoint_pos)
            || !waypoint_has_visibility_to_target(
                goal_waypoint_pos,
                eye_offset,
                destination,
                *los_target_eye_pos,
                los_target_obj)) {
            need_route = true;
        }
    }

    const bool prefer_long_route =
        los_target_eye_pos && is_long_route_escape_active();
    if (need_route) {
        set_last_heading_change_reason("repath_needed");
        int closest_waypoint = 0;
        if (g_client_bot_state.recovery_pending_reroute
            && g_client_bot_state.recovery_anchor_waypoint > 0) {
            closest_waypoint = g_client_bot_state.recovery_anchor_waypoint;
        }
        else {
            closest_waypoint = bot_find_closest_waypoint_with_los_fallback(entity);
        }
        if (closest_waypoint <= 0) {
            clear_waypoint_route();
            return false;
        }

        if (closest_waypoint == goal_waypoint) {
            bot_nav_prune_failed_edge_cooldowns();
            bot_nav_prune_failed_waypoint_blacklist();
            std::array<int, kMaxWaypointLinks> links{};
            const int link_count = waypoints_get_links(closest_waypoint, links);
            int best_neighbor = 0;
            float best_score = std::numeric_limits<float>::max();
            const int last_visited =
                g_client_bot_state.recent_waypoint_visits.empty()
                    ? 0
                    : g_client_bot_state.recent_waypoint_visits.back();
            const rf::Vector3 eye_offset = entity.eye_pos - entity.pos;

            for (int i = 0; i < link_count; ++i) {
                const int link = links[i];
                if (link <= 0 || link == closest_waypoint) {
                    continue;
                }
                if (bot_nav_is_failed_edge_cooldown_active_no_prune(closest_waypoint, link)) {
                    continue;
                }
                if (bot_nav_is_waypoint_blacklisted_no_prune(link)) {
                    continue;
                }
                if (!bot_nav_is_link_traversable_for_route(closest_waypoint, link)) {
                    continue;
                }
                if (!waypoints_has_direct_link(closest_waypoint, link)
                    || !waypoints_link_is_clear(closest_waypoint, link)) {
                    continue;
                }

                rf::Vector3 link_pos{};
                if (!waypoints_get_pos(link, link_pos)) {
                    continue;
                }

                float score = rf::vec_dist_squared(&link_pos, &destination);
                if (link == last_visited) {
                    score *= 1.25f;
                }
                if (los_target_eye_pos
                    && waypoint_has_visibility_to_target(
                        link_pos,
                        eye_offset,
                        destination,
                        *los_target_eye_pos,
                        los_target_obj)) {
                    score *= 0.80f;
                }

                if (score < best_score) {
                    best_score = score;
                    best_neighbor = link;
                }
            }

            if (best_neighbor > 0) {
                g_client_bot_state.waypoint_path.clear();
                g_client_bot_state.waypoint_path.push_back(closest_waypoint);
                g_client_bot_state.waypoint_path.push_back(best_neighbor);
                g_client_bot_state.waypoint_next_index = 1;
                g_client_bot_state.waypoint_goal = goal_waypoint;
                set_route_choice_lock_from_path(
                    g_client_bot_state.waypoint_path,
                    g_client_bot_state.waypoint_goal
                );
                g_client_bot_state.repath_timer.set(
                    std::max(kWaypointRecoveryRepathMs, effective_repath_ms)
                );
                set_last_heading_change_reason("neighbor_link_repath");
                return update_waypoint_target_from_current_path(entity);
            }
        }

        const bool performance_sensitive_route =
            bot_goal_is_control_point_objective(g_client_bot_state.active_goal)
            || bot_goal_is_item_collection(g_client_bot_state.active_goal)
            || bot_goal_is_ctf_objective(g_client_bot_state.active_goal)
            || g_client_bot_state.active_goal == BotGoalType::activate_bridge
            || g_client_bot_state.active_goal == BotGoalType::create_crater
            || g_client_bot_state.active_goal == BotGoalType::shatter_glass;
        const bool large_waypoint_graph = waypoints_count() >= 1800;

        // Primary solve first (cheap/direct path preferred internally), then optionally
        // one specialized heavy solver. This avoids stacking several expensive solve
        // strategies in a single frame.
        bool routed = false;
        if (g_client_bot_state.recovery_pending_reroute) {
            routed = bot_nav_pick_waypoint_route_to_goal_randomized_after_stuck(
                closest_waypoint,
                goal_waypoint,
                g_client_bot_state.recovery_avoid_waypoint,
                effective_repath_ms
            );
        }
        else {
            routed = pick_waypoint_route_to_goal(
                closest_waypoint,
                goal_waypoint,
                effective_repath_ms
            );
        }

        const bool allow_secondary_heavy_solver =
            !routed
            && !g_client_bot_state.recovery_pending_reroute
            && !(performance_sensitive_route && large_waypoint_graph);
        if (allow_secondary_heavy_solver && los_target_eye_pos) {
            if (prefer_long_route) {
                routed = bot_nav_pick_waypoint_route_to_goal_long_detour(
                    closest_waypoint,
                    goal_waypoint,
                    effective_repath_ms
                );
            }
            else {
                const rf::Vector3 eye_offset = entity.eye_pos - entity.pos;
                const bool los_route_solve_ready =
                    !g_client_bot_state.los_route_solve_timer.valid()
                    || g_client_bot_state.los_route_solve_timer.elapsed();
                if (los_route_solve_ready) {
                    routed = pick_waypoint_route_to_los_goal(
                        closest_waypoint,
                        eye_offset,
                        destination,
                        *los_target_eye_pos,
                        los_target_obj,
                        effective_repath_ms
                    );
                    g_client_bot_state.los_route_solve_timer.set(kLosRouteSolveIntervalMs);
                }
            }
        }

        if (!routed) {
            g_client_bot_state.last_pursuit_route_was_fallback = true;
            if (!commit_single_hop_fallback_route(
                    closest_waypoint,
                    goal_waypoint,
                    destination,
                    std::max(kWaypointRecoveryRepathMs, effective_repath_ms),
                    "fallback_single_hop_goal")) {
                start_recovery_anchor_reroute(entity, goal_waypoint);
                clear_waypoint_route();
                if (set_recovery_anchor_direct_target(entity)) {
                    return true;
                }
                return false;
            }
        }
        set_last_heading_change_reason("route_selected");

        g_client_bot_state.recovery_pending_reroute = false;
        g_client_bot_state.recovery_anchor_waypoint = 0;
        g_client_bot_state.recovery_avoid_waypoint = 0;
    }

    const bool updated = update_waypoint_target_from_current_path(entity);
    if (updated) {
        g_client_bot_state.recovery_pending_reroute = false;
        g_client_bot_state.recovery_anchor_waypoint = 0;
        g_client_bot_state.recovery_avoid_waypoint = 0;
    }
    return updated;
}

bool update_waypoint_target_for_local_los_reposition(
    const rf::Entity& entity,
    const rf::Entity& enemy_target,
    const bool enemy_has_los)
{
    int anchor_waypoint = 0;
    const bool can_reuse_anchor =
        g_client_bot_state.combat_los_anchor_waypoint > 0
        && g_client_bot_state.combat_los_reposition_eval_timer.valid()
        && !g_client_bot_state.combat_los_reposition_eval_timer.elapsed();
    if (can_reuse_anchor) {
        anchor_waypoint = g_client_bot_state.combat_los_anchor_waypoint;
    }
    else {
        anchor_waypoint = bot_find_closest_waypoint_with_los_fallback(entity);
    }

    if (anchor_waypoint <= 0) {
        g_client_bot_state.combat_los_reposition_timer.invalidate();
        g_client_bot_state.combat_los_reposition_eval_timer.invalidate();
        g_client_bot_state.combat_los_anchor_waypoint = 0;
        g_client_bot_state.combat_los_target_waypoint = 0;
        return false;
    }

    const bool can_reuse_cached_target =
        g_client_bot_state.combat_los_anchor_waypoint == anchor_waypoint
        && g_client_bot_state.combat_los_target_waypoint > 0
        && g_client_bot_state.combat_los_reposition_eval_timer.valid()
        && !g_client_bot_state.combat_los_reposition_eval_timer.elapsed();
    if (can_reuse_cached_target) {
        const int target_waypoint = g_client_bot_state.combat_los_target_waypoint;
        g_client_bot_state.last_pursuit_route_was_fallback = false;
        if (target_waypoint == anchor_waypoint) {
            rf::Vector3 anchor_pos{};
            if (waypoints_get_pos(anchor_waypoint, anchor_pos)) {
                const float anchor_dist_sq = rf::vec_dist_squared(&entity.pos, &anchor_pos);
                if (anchor_dist_sq <= kWaypointReachRadius * kWaypointReachRadius) {
                    g_client_bot_state.waypoint_path.clear();
                    g_client_bot_state.waypoint_next_index = 0;
                    g_client_bot_state.waypoint_goal = anchor_waypoint;
                    g_client_bot_state.waypoint_target_pos = anchor_pos;
                    g_client_bot_state.has_waypoint_target = true;
                    g_client_bot_state.repath_timer.set(kCombatLosRepositionRepathMs);
                    return true;
                }
            }
        }
        else if (waypoints_has_direct_link(anchor_waypoint, target_waypoint)
                 && bot_nav_is_link_traversable_for_route(
                     anchor_waypoint,
                     target_waypoint)) {
            g_client_bot_state.waypoint_path.clear();
            g_client_bot_state.waypoint_path.push_back(anchor_waypoint);
            g_client_bot_state.waypoint_path.push_back(target_waypoint);
            g_client_bot_state.waypoint_next_index = 1;
            g_client_bot_state.waypoint_goal = target_waypoint;
            g_client_bot_state.repath_timer.set(kCombatLosRepositionRepathMs);
            return update_waypoint_target_from_current_path(entity);
        }

        g_client_bot_state.combat_los_target_waypoint = 0;
    }

    const bool los_eval_on_cooldown =
        g_client_bot_state.combat_los_anchor_waypoint == anchor_waypoint
        && g_client_bot_state.combat_los_target_waypoint <= 0
        && g_client_bot_state.combat_los_reposition_eval_timer.valid()
        && !g_client_bot_state.combat_los_reposition_eval_timer.elapsed();
    if (los_eval_on_cooldown) {
        // Don't stall while LOS-neighbor evaluation is throttled; continue current route if possible.
        return update_waypoint_target_from_current_path(entity);
    }

    const rf::Vector3 eye_offset = entity.eye_pos - entity.pos;
    struct Candidate
    {
        int waypoint = 0;
        rf::Vector3 pos{};
        bool is_anchor = false;
    };

    const bool anchor_in_instant_death_zone = waypoints_waypoint_has_zone_type(
        anchor_waypoint,
        WaypointZoneType::instant_death_zone
    );
    std::vector<Candidate> candidates;
    candidates.reserve(kMaxWaypointLinks + 1);
    auto add_candidate = [&](const int waypoint, const bool is_anchor) {
        if (waypoint <= 0) {
            return;
        }
        for (const Candidate& existing : candidates) {
            if (existing.waypoint == waypoint) {
                return;
            }
        }

        rf::Vector3 waypoint_pos{};
        if (!waypoints_get_pos(waypoint, waypoint_pos)) {
            return;
        }
        if (waypoints_waypoint_has_zone_type(waypoint, WaypointZoneType::instant_death_zone)
            && (!anchor_in_instant_death_zone || waypoint == anchor_waypoint)) {
            return;
        }
        if (!waypoint_has_visibility_to_target(
                waypoint_pos,
                eye_offset,
                enemy_target.pos,
                enemy_target.eye_pos,
                &enemy_target)) {
            return;
        }

        candidates.push_back(Candidate{waypoint, waypoint_pos, is_anchor});
    };

    add_candidate(anchor_waypoint, true);
    std::array<int, kMaxWaypointLinks> links{};
    const int link_count = waypoints_get_links(anchor_waypoint, links);
    for (int i = 0; i < link_count; ++i) {
        const int linked_waypoint = links[i];
        if (linked_waypoint <= 0 || linked_waypoint == anchor_waypoint) {
            continue;
        }
        add_candidate(linked_waypoint, false);
    }

    g_client_bot_state.combat_los_reposition_eval_timer.set(kCombatLosNeighborEvalIntervalMs);

    if (candidates.empty()) {
        g_client_bot_state.combat_los_reposition_timer.invalidate();
        g_client_bot_state.combat_los_anchor_waypoint = anchor_waypoint;
        g_client_bot_state.combat_los_target_waypoint = 0;
        return false;
    }

    auto find_candidate = [&](const int waypoint) -> const Candidate* {
        for (const Candidate& candidate : candidates) {
            if (candidate.waypoint == waypoint) {
                return &candidate;
            }
        }
        return nullptr;
    };

    bool need_new_target =
        g_client_bot_state.combat_los_anchor_waypoint != anchor_waypoint
        || !g_client_bot_state.combat_los_reposition_timer.valid()
        || g_client_bot_state.combat_los_reposition_timer.elapsed()
        || !find_candidate(g_client_bot_state.combat_los_target_waypoint);

    if (!need_new_target) {
        const Candidate* active_candidate =
            find_candidate(g_client_bot_state.combat_los_target_waypoint);
        if (!active_candidate) {
            need_new_target = true;
        }
        else {
            const float active_dist_sq =
                rf::vec_dist_squared(&entity.pos, &active_candidate->pos);
            if (active_dist_sq <= kWaypointReachRadius * kWaypointReachRadius) {
                need_new_target = true;
            }
        }
    }

    if (need_new_target) {
        int selected_waypoint = anchor_waypoint;
        const bool seeking_power_position =
            g_client_bot_state.fsm_state == BotFsmState::find_power_position;

        if (seeking_power_position) {
            const float weapon_readiness = std::clamp(
                bot_decision_compute_weapon_readiness_score(entity, &enemy_target),
                0.0f,
                1.0f
            );
            const float weapon_power_factor = std::lerp(0.85f, 1.75f, weapon_readiness);
            const int current_waypoint = g_client_bot_state.combat_los_target_waypoint;
            float best_score = -std::numeric_limits<float>::infinity();
            bool has_choice = false;
            for (const Candidate& candidate : candidates) {
                if (candidate.is_anchor && candidates.size() > 1) {
                    // Keep power-position movement active by preferring side-waypoint cycling.
                    continue;
                }

                float score = 0.0f;
                const float y_delta = candidate.pos.y - entity.pos.y;
                if (y_delta > 0.0f) {
                    score += std::clamp(y_delta / 6.0f, 0.0f, 3.0f) * weapon_power_factor;
                }
                else {
                    score += std::clamp(y_delta / 10.0f, -0.35f, 0.0f);
                }

                if (waypoints_waypoint_has_zone_type(
                        candidate.waypoint,
                        WaypointZoneType::high_power_zone)) {
                    score += std::lerp(1.0f, 2.1f, weapon_readiness);
                }

                if (!candidate.is_anchor) {
                    score += 0.35f;
                }
                if (current_waypoint > 0
                    && candidate.waypoint == current_waypoint
                    && candidates.size() > 2) {
                    score -= 0.45f;
                }

                const float enemy_dist = std::sqrt(std::max(
                    rf::vec_dist_squared(&candidate.pos, &enemy_target.pos),
                    0.0f
                ));
                const float range_bias = std::clamp(
                    (enemy_dist - kWaypointReachRadius) / 16.0f,
                    -0.5f,
                    0.75f
                );
                score += range_bias * std::lerp(0.20f, 0.75f, weapon_readiness);

                std::uniform_real_distribution<float> jitter_dist(-0.12f, 0.12f);
                score += jitter_dist(g_rng);

                if (!has_choice || score > best_score) {
                    best_score = score;
                    selected_waypoint = candidate.waypoint;
                    has_choice = true;
                }
            }
        }
        else if (enemy_has_los) {
            std::vector<int> side_candidates;
            side_candidates.reserve(candidates.size());
            for (const Candidate& candidate : candidates) {
                if (!candidate.is_anchor) {
                    side_candidates.push_back(candidate.waypoint);
                }
            }

            if (!side_candidates.empty()) {
                const int current_waypoint = g_client_bot_state.combat_los_target_waypoint;
                if (current_waypoint > 0 && side_candidates.size() > 1) {
                    side_candidates.erase(
                        std::remove(side_candidates.begin(), side_candidates.end(), current_waypoint),
                        side_candidates.end()
                    );
                }
                if (!side_candidates.empty()) {
                    std::uniform_int_distribution<size_t> pick_dist(
                        0,
                        side_candidates.size() - 1
                    );
                    selected_waypoint = side_candidates[pick_dist(g_rng)];
                }
            }
        }
        else {
            float best_dist_sq = std::numeric_limits<float>::max();
            for (const Candidate& candidate : candidates) {
                const float dist_sq = rf::vec_dist_squared(&entity.pos, &candidate.pos);
                if (dist_sq < best_dist_sq) {
                    best_dist_sq = dist_sq;
                    selected_waypoint = candidate.waypoint;
                }
            }
        }

        g_client_bot_state.combat_los_anchor_waypoint = anchor_waypoint;
        g_client_bot_state.combat_los_target_waypoint = selected_waypoint;
        g_client_bot_state.combat_los_reposition_timer.set(kCombatLosRepositionDecisionMs);
    }

    const int target_waypoint = g_client_bot_state.combat_los_target_waypoint;
    if (target_waypoint <= 0) {
        return false;
    }

    const Candidate* selected_candidate = find_candidate(target_waypoint);
    if (!selected_candidate) {
        return false;
    }

    g_client_bot_state.last_pursuit_route_was_fallback = false;

    if (target_waypoint == anchor_waypoint) {
        const float anchor_dist_sq = rf::vec_dist_squared(&entity.pos, &selected_candidate->pos);
        if (anchor_dist_sq <= kWaypointReachRadius * kWaypointReachRadius) {
            g_client_bot_state.waypoint_path.clear();
            g_client_bot_state.waypoint_next_index = 0;
            g_client_bot_state.waypoint_goal = anchor_waypoint;
            g_client_bot_state.waypoint_target_pos = selected_candidate->pos;
            g_client_bot_state.has_waypoint_target = true;
            g_client_bot_state.repath_timer.set(kCombatLosRepositionRepathMs);
            return true;
        }
        return false;
    }

    if (!waypoints_has_direct_link(anchor_waypoint, target_waypoint)
        || !bot_nav_is_link_traversable_for_route(anchor_waypoint, target_waypoint)
        || !waypoints_link_is_clear(anchor_waypoint, target_waypoint)) {
        g_client_bot_state.combat_los_target_waypoint = 0;
        g_client_bot_state.combat_los_reposition_timer.invalidate();
        return false;
    }

    g_client_bot_state.waypoint_path.clear();
    g_client_bot_state.waypoint_path.push_back(anchor_waypoint);
    g_client_bot_state.waypoint_path.push_back(target_waypoint);
    g_client_bot_state.waypoint_next_index = 1;
    g_client_bot_state.waypoint_goal = target_waypoint;
    set_route_choice_lock_from_path(
        g_client_bot_state.waypoint_path,
        g_client_bot_state.waypoint_goal
    );
    g_client_bot_state.repath_timer.set(kCombatLosRepositionRepathMs);
    return update_waypoint_target_from_current_path(entity);
}

bool bot_nav_try_emergency_direct_move_target(
    const rf::Entity& entity,
    rf::Vector3& out_move_target)
{
    out_move_target = {};

    rf::Vector3 forward = entity.eye_orient.fvec;
    if (forward.len_sq() < 0.0001f) {
        forward = entity.orient.fvec;
    }
    forward.y = 0.0f;
    if (forward.len_sq() < 0.0001f) {
        return false;
    }
    forward.normalize_safe();

    rf::Vector3 right = entity.eye_orient.rvec;
    if (right.len_sq() < 0.0001f) {
        right = entity.orient.rvec;
    }
    right.y = 0.0f;
    if (right.len_sq() < 0.0001f) {
        right = rf::Vector3{forward.z, 0.0f, -forward.x};
    }
    if (right.len_sq() < 0.0001f) {
        return false;
    }
    right.normalize_safe();

    struct ProbeCandidate
    {
        rf::Vector3 direction{};
        float distance = 0.0f;
    };

    const std::array<ProbeCandidate, 6> candidates{
        ProbeCandidate{forward, 6.0f},
        ProbeCandidate{forward + right * 0.65f, 5.5f},
        ProbeCandidate{forward - right * 0.65f, 5.5f},
        ProbeCandidate{right, 4.0f},
        ProbeCandidate{right * -1.0f, 4.0f},
        ProbeCandidate{forward * -1.0f, 3.0f},
    };

    const rf::Vector3 probe_origin = entity.pos + rf::Vector3{0.0f, 0.35f, 0.0f};
    for (const ProbeCandidate& candidate : candidates) {
        if (candidate.direction.len_sq() < 0.0001f || candidate.distance <= 0.1f) {
            continue;
        }

        rf::Vector3 probe_dir = candidate.direction;
        probe_dir.y = 0.0f;
        probe_dir.normalize_safe();
        if (probe_dir.len_sq() < 0.0001f) {
            continue;
        }

        if (is_probe_direction_blocked(entity, probe_origin, probe_dir, candidate.distance)) {
            continue;
        }

        out_move_target = entity.pos + probe_dir * candidate.distance;
        return true;
    }

    return false;
}

void bot_internal_set_last_heading_change_reason(const char* reason)
{
    set_last_heading_change_reason(reason);
}

const char* bot_internal_get_last_heading_change_reason()
{
    return g_client_bot_state.last_heading_change_reason;
}

