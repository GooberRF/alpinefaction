#include "bot_navigation_routes.h"

#include "bot_internal.h"
#include "bot_waypoint_route.h"
#include "../../main/main.h"
#include "../../rf/collide.h"
#include "../../rf/item.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
constexpr int kBotRouteProbeTraceFlags = rf::CF_ANY_HIT | rf::CF_PROCESS_INVISIBLE_FACES;
constexpr int kMaxGoalDetourAttempts = 8;
constexpr int kMaxLongDetourAttempts = 10;
constexpr int kMaxRoamRouteAttempts = 16;
constexpr float kRouteMaxUpwardStepHeight = 1.5f;
constexpr float kLadderEndpointTolerance = 0.75f;
constexpr float kRouteGapProbeStartYOffset = 0.10f;
constexpr float kRouteGapProbeDownDistance = 2.5f;
constexpr int kRouteGapEdgeCacheMaxEntries = 8192;
constexpr float kRouteDamageZonePenalty = 8.0f;
constexpr float kRouteDamageZoneEnemyGoalPenalty = 5.5f;
constexpr float kRouteInstantDeathZonePenalty = 120.0f;
constexpr float kPowerPositionElevationScale = 8.0f;
constexpr float kPowerPositionElevationClamp = 2.5f;
constexpr float kPowerPositionZoneBonus = 1.35f;

std::unordered_map<uint64_t, bool> g_route_gap_support_cache{};
int g_route_gap_support_cache_waypoint_total = 0;

struct LadderBounds
{
    float min_y = 0.0f;
    float max_y = 0.0f;
    int count = 0;
};

std::unordered_map<int, LadderBounds> g_ladder_bounds_cache{};
int g_ladder_bounds_cache_waypoint_total = 0;

uint64_t make_link_key(const int waypoint_a, const int waypoint_b)
{
    const uint32_t min_uid = static_cast<uint32_t>(std::min(waypoint_a, waypoint_b));
    const uint32_t max_uid = static_cast<uint32_t>(std::max(waypoint_a, waypoint_b));
    return (static_cast<uint64_t>(min_uid) << 32) | static_cast<uint64_t>(max_uid);
}

bool compute_link_midpoint_support_uncached(const int from_waypoint, const int to_waypoint)
{
    rf::Vector3 from_pos{};
    rf::Vector3 to_pos{};
    if (!waypoints_get_pos(from_waypoint, from_pos)
        || !waypoints_get_pos(to_waypoint, to_pos)) {
        return true;
    }

    rf::Vector3 midpoint = (from_pos + to_pos) * 0.5f;
    rf::Vector3 p0 = midpoint;
    rf::Vector3 p1 = midpoint;
    p0.y += kRouteGapProbeStartYOffset;
    p1.y -= kRouteGapProbeDownDistance;
    rf::GCollisionOutput collision{};
    return rf::collide_linesegment_level_solid(
        p0,
        p1,
        kBotRouteProbeTraceFlags,
        &collision
    );
}

bool waypoint_type_allows_special_vertical_route(const WaypointType type)
{
    switch (type) {
        case WaypointType::jump_pad:
        case WaypointType::tele_entrance:
        case WaypointType::lift_entrance:
        case WaypointType::lift_body:
        case WaypointType::ladder:
            return true;
        default:
            return false;
    }
}

bool waypoint_is_ladder_with_identifier(
    const int waypoint,
    int& out_identifier,
    rf::Vector3& out_pos)
{
    out_identifier = -1;
    int type_raw = 0;
    int subtype = 0;
    if (waypoint <= 0
        || !waypoints_get_type_subtype(waypoint, type_raw, subtype)
        || static_cast<WaypointType>(type_raw) != WaypointType::ladder
        || !waypoints_get_identifier(waypoint, out_identifier)
        || !waypoints_get_pos(waypoint, out_pos)) {
        return false;
    }
    return true;
}

void refresh_ladder_bounds_cache_if_needed()
{
    const int waypoint_total = waypoints_count();
    if (g_ladder_bounds_cache_waypoint_total == waypoint_total) {
        return;
    }

    g_ladder_bounds_cache_waypoint_total = waypoint_total;
    g_ladder_bounds_cache.clear();

    for (int waypoint = 1; waypoint < waypoint_total; ++waypoint) {
        int ladder_identifier = -1;
        rf::Vector3 waypoint_pos{};
        if (!waypoint_is_ladder_with_identifier(
                waypoint,
                ladder_identifier,
                waypoint_pos)
            || ladder_identifier < 0) {
            continue;
        }

        LadderBounds& bounds = g_ladder_bounds_cache[ladder_identifier];
        if (bounds.count <= 0) {
            bounds.min_y = waypoint_pos.y;
            bounds.max_y = waypoint_pos.y;
            bounds.count = 1;
            continue;
        }

        bounds.min_y = std::min(bounds.min_y, waypoint_pos.y);
        bounds.max_y = std::max(bounds.max_y, waypoint_pos.y);
        ++bounds.count;
    }
}

bool ladder_waypoint_is_endpoint(const int waypoint)
{
    int ladder_identifier = -1;
    rf::Vector3 waypoint_pos{};
    if (!waypoint_is_ladder_with_identifier(waypoint, ladder_identifier, waypoint_pos)) {
        return false;
    }

    if (ladder_identifier < 0) {
        return true;
    }

    refresh_ladder_bounds_cache_if_needed();
    const auto it = g_ladder_bounds_cache.find(ladder_identifier);
    if (it == g_ladder_bounds_cache.end() || it->second.count <= 1) {
        return true;
    }

    const LadderBounds& bounds = it->second;
    return std::fabs(waypoint_pos.y - bounds.min_y) <= kLadderEndpointTolerance
        || std::fabs(waypoint_pos.y - bounds.max_y) <= kLadderEndpointTolerance;
}

bool is_ladder_boundary_transition_valid(const int from_waypoint, const int to_waypoint)
{
    int from_type_raw = 0;
    int from_subtype = 0;
    int to_type_raw = 0;
    int to_subtype = 0;
    if (!waypoints_get_type_subtype(from_waypoint, from_type_raw, from_subtype)
        || !waypoints_get_type_subtype(to_waypoint, to_type_raw, to_subtype)) {
        return false;
    }

    const WaypointType from_type = static_cast<WaypointType>(from_type_raw);
    const WaypointType to_type = static_cast<WaypointType>(to_type_raw);
    const bool from_ladder = from_type == WaypointType::ladder;
    const bool to_ladder = to_type == WaypointType::ladder;
    if (!from_ladder && !to_ladder) {
        return true;
    }

    if (from_ladder && to_ladder) {
        int from_identifier = -1;
        int to_identifier = -1;
        rf::Vector3 from_pos{};
        rf::Vector3 to_pos{};
        const bool has_from_identifier =
            waypoint_is_ladder_with_identifier(from_waypoint, from_identifier, from_pos);
        const bool has_to_identifier =
            waypoint_is_ladder_with_identifier(to_waypoint, to_identifier, to_pos);
        if (has_from_identifier && has_to_identifier && from_identifier == to_identifier) {
            return true;
        }
        return ladder_waypoint_is_endpoint(from_waypoint)
            && ladder_waypoint_is_endpoint(to_waypoint);
    }

    if (from_ladder) {
        return ladder_waypoint_is_endpoint(from_waypoint);
    }

    return ladder_waypoint_is_endpoint(to_waypoint);
}

bool is_link_unclimbable_upward_for_route(const int from_waypoint, const int to_waypoint)
{
    int from_type_raw = 0;
    int from_subtype = 0;
    int to_type_raw = 0;
    int to_subtype = 0;
    if (!waypoints_get_type_subtype(from_waypoint, from_type_raw, from_subtype)
        || !waypoints_get_type_subtype(to_waypoint, to_type_raw, to_subtype)) {
        return true;
    }

    const auto from_type = static_cast<WaypointType>(from_type_raw);
    const auto to_type = static_cast<WaypointType>(to_type_raw);
    (void)from_subtype;
    (void)to_subtype;
    if (waypoint_type_allows_special_vertical_route(from_type)
        || waypoint_type_allows_special_vertical_route(to_type)) {
        return false;
    }

    rf::Vector3 from_pos{};
    rf::Vector3 to_pos{};
    if (!waypoints_get_pos(from_waypoint, from_pos)
        || !waypoints_get_pos(to_waypoint, to_pos)) {
        return true;
    }

    const float upward_delta = to_pos.y - from_pos.y;
    return upward_delta > (kRouteMaxUpwardStepHeight + 0.01f);
}

float compute_waypoint_path_gap_penalty(const std::vector<int>& path)
{
    if (path.size() < 2) {
        return 0.0f;
    }

    float unsupported_edges = 0.0f;
    int samples = 0;
    for (size_t i = 1; i < path.size(); ++i) {
        const int from = path[i - 1];
        const int to = path[i];
        if (!bot_nav_is_link_traversable_for_route(from, to)) {
            continue;
        }
        unsupported_edges += bot_nav_link_midpoint_has_support(from, to) ? 0.0f : 1.0f;
        ++samples;
    }

    if (samples <= 0) {
        return 0.0f;
    }
    return unsupported_edges / static_cast<float>(samples);
}

float point_to_segment_dist_sq_xz(
    const rf::Vector3& point,
    const rf::Vector3& start,
    const rf::Vector3& end)
{
    const float ab_x = end.x - start.x;
    const float ab_z = end.z - start.z;
    const float ab_len_sq = ab_x * ab_x + ab_z * ab_z;
    if (ab_len_sq <= 0.0001f) {
        const float dx = point.x - start.x;
        const float dz = point.z - start.z;
        return dx * dx + dz * dz;
    }

    const float ap_x = point.x - start.x;
    const float ap_z = point.z - start.z;
    const float t = std::clamp((ap_x * ab_x + ap_z * ab_z) / ab_len_sq, 0.0f, 1.0f);
    const float closest_x = start.x + ab_x * t;
    const float closest_z = start.z + ab_z * t;
    const float dx = point.x - closest_x;
    const float dz = point.z - closest_z;
    return dx * dx + dz * dz;
}

int compute_attempt_budget(
    const int base_attempts,
    const int waypoint_total,
    const int min_attempts)
{
    if (base_attempts <= 0) {
        return 0;
    }

    const float decision_skill = bot_get_decision_skill_factor();
    const float efficiency =
        std::clamp(get_active_bot_personality().decision_efficiency_bias, 0.35f, 2.25f);
    const float graph_scale =
        std::clamp(900.0f / std::max(static_cast<float>(waypoint_total), 1.0f), 0.22f, 1.0f);
    const float skill_scale = std::lerp(0.55f, 1.0f, decision_skill);
    const float efficiency_scale = std::clamp(std::sqrt(efficiency), 0.65f, 1.5f);
    const int attempts = static_cast<int>(std::lround(
        static_cast<float>(base_attempts) * graph_scale * skill_scale * efficiency_scale));
    return std::clamp(attempts, min_attempts, base_attempts);
}

void optimize_noisy_route_path(std::vector<int>& path)
{
    if (path.size() < 3) {
        return;
    }

    // Remove immediate duplicates first.
    path.erase(std::unique(path.begin(), path.end()), path.end());
    if (path.size() < 3) {
        return;
    }

    bot_nav_prune_failed_edge_cooldowns();
    constexpr int kLookahead = 6;
    for (size_t i = 0; i + 2 < path.size();) {
        int best_jump = -1;
        const size_t max_jump = std::min(path.size() - 1, i + kLookahead);
        for (size_t j = max_jump; j >= i + 2; --j) {
            const int from = path[i];
            const int to = path[j];
            if (from <= 0 || to <= 0) {
                if (j == i + 2) {
                    break;
                }
                continue;
            }
            if (!waypoints_has_direct_link(from, to)) {
                if (j == i + 2) {
                    break;
                }
                continue;
            }
            if (!is_ladder_boundary_transition_valid(from, to)) {
                if (j == i + 2) {
                    break;
                }
                continue;
            }
            if (is_link_unclimbable_upward_for_route(from, to)) {
                if (j == i + 2) {
                    break;
                }
                continue;
            }
            if (bot_nav_is_failed_edge_cooldown_active_no_prune(from, to)) {
                if (j == i + 2) {
                    break;
                }
                continue;
            }
            if (!waypoints_link_is_clear(from, to)) {
                if (j == i + 2) {
                    break;
                }
                continue;
            }
            best_jump = static_cast<int>(j);
            break;
        }

        if (best_jump > static_cast<int>(i + 1)) {
            path.erase(path.begin() + static_cast<std::ptrdiff_t>(i + 1), path.begin() + best_jump);
        }
        else {
            ++i;
        }
    }
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

void commit_route_path(std::vector<int>&& path, const int goal_waypoint, const int repath_ms)
{
    optimize_noisy_route_path(path);
    set_route_choice_lock_from_path(path, goal_waypoint);
    g_client_bot_state.waypoint_path = std::move(path);
    g_client_bot_state.waypoint_next_index =
        (g_client_bot_state.waypoint_path.size() > 1) ? 1 : 0;
    g_client_bot_state.waypoint_goal = goal_waypoint;
    g_client_bot_state.repath_timer.set(repath_ms);
}

struct PathGeometryMetrics
{
    float route_length = std::numeric_limits<float>::infinity();
    float turn_penalty = 0.0f;
    float density_penalty = 0.0f;
    float detour_penalty = 0.0f;
};

PathGeometryMetrics compute_path_geometry_metrics(const std::vector<int>& path)
{
    PathGeometryMetrics metrics{};
    if (path.size() < 2) {
        metrics.route_length = 0.0f;
        return metrics;
    }

    rf::Vector3 start_pos{};
    rf::Vector3 prev_pos{};
    if (!waypoints_get_pos(path[0], start_pos) || !waypoints_get_pos(path[0], prev_pos)) {
        return metrics;
    }

    rf::Vector3 prev_dir{};
    bool has_prev_dir = false;
    int turn_samples = 0;
    int density_samples = 0;
    float route_length = 0.0f;
    float turn_penalty = 0.0f;
    float density_penalty = 0.0f;
    rf::Vector3 current_pos{};
    for (size_t index = 1; index < path.size(); ++index) {
        if (!waypoints_get_pos(path[index], current_pos)) {
            return metrics;
        }

        rf::Vector3 segment = current_pos - prev_pos;
        const float segment_len = segment.len();
        route_length += segment_len;
        if (segment_len < (kWaypointRadius * 0.95f)) {
            const float shortness = std::clamp(
                (kWaypointRadius * 0.95f - segment_len) / std::max(kWaypointRadius, 0.001f),
                0.0f,
                1.0f
            );
            density_penalty += 0.5f + shortness;
        }
        ++density_samples;

        segment.y = 0.0f;
        if (segment.len_sq() > 0.01f) {
            segment.normalize_safe();
            if (has_prev_dir) {
                const float cosine = std::clamp(prev_dir.dot_prod(segment), -1.0f, 1.0f);
                const float bend = std::max(0.0f, (1.0f - cosine) - 0.08f);
                turn_penalty += bend;
                ++turn_samples;
            }
            prev_dir = segment;
            has_prev_dir = true;
        }
        prev_pos = current_pos;
    }

    const rf::Vector3 endpoint_vec = current_pos - start_pos;
    const float direct_len = endpoint_vec.len();
    float detour_penalty = 0.0f;
    if (direct_len > 0.25f) {
        detour_penalty = std::max(0.0f, route_length / direct_len - 1.0f);
    }

    metrics.route_length = route_length;
    metrics.turn_penalty = turn_samples > 0 ? (turn_penalty / static_cast<float>(turn_samples)) : 0.0f;
    metrics.density_penalty = density_samples > 0 ? (density_penalty / static_cast<float>(density_samples)) : 0.0f;
    metrics.detour_penalty = detour_penalty;
    return metrics;
}

float compute_waypoint_path_length(const std::vector<int>& path)
{
    return compute_path_geometry_metrics(path).route_length;
}

float get_waypoint_item_value(const int waypoint_index)
{
    int type = 0;
    int subtype = 0;
    if (!waypoints_get_type_subtype(waypoint_index, type, subtype) || type != 2) {
        return 0.0f;
    }

    if (subtype < 0 || subtype >= rf::num_item_types) {
        return 0.0f;
    }

    const rf::ItemInfo& item = rf::item_info[subtype];
    if (item.flags & rf::IIF_NO_PICKUP) {
        return 0.0f;
    }

    float value = bot_get_item_weight(subtype);
    if (item.gives_weapon_id >= 0) {
        const float pickup_weight = bot_get_weapon_pickup_weight(item.gives_weapon_id);
        const float preference_weight = bot_get_weapon_preference_weight(item.gives_weapon_id);
        value += 2.8f * pickup_weight * preference_weight;
    }
    if (item.ammo_for_weapon_id >= 0) {
        value += 1.4f * bot_get_weapon_pickup_weight(item.ammo_for_weapon_id);
    }

    const int count = std::max(item.count_multi, item.count);
    value += std::clamp(static_cast<float>(count) * 0.06f, 0.0f, 2.2f);

    const float respawn_seconds =
        std::max(static_cast<float>(item.respawn_time_millis), 0.0f) / 1000.0f;
    value += std::clamp(respawn_seconds * 0.05f, 0.0f, 1.8f);
    return value;
}

float compute_waypoint_path_item_value(const std::vector<int>& path)
{
    if (path.size() < 2) {
        return 0.0f;
    }

    float total_item_value = 0.0f;
    std::unordered_set<int> visited;
    visited.reserve(path.size());
    for (size_t index = 1; index < path.size(); ++index) {
        const int waypoint = path[index];
        if (!visited.insert(waypoint).second) {
            continue;
        }
        total_item_value += get_waypoint_item_value(waypoint);
    }
    return total_item_value;
}

float compute_path_enemy_threat_penalty(
    const std::vector<int>& path,
    const rf::Vector3& threat_pos)
{
    if (path.size() < 2) {
        return 0.0f;
    }

    float accumulated = 0.0f;
    int sample_count = 0;
    rf::Vector3 waypoint_pos{};
    constexpr float kThreatRadius = 18.0f;
    for (size_t index = 1; index < path.size(); ++index) {
        const int waypoint = path[index];
        if (waypoint <= 0 || !waypoints_get_pos(waypoint, waypoint_pos)) {
            continue;
        }

        const float dist = std::sqrt(std::max(
            rf::vec_dist_squared(&waypoint_pos, &threat_pos),
            0.0f
        ));
        const float near_factor = std::clamp((kThreatRadius - dist) / kThreatRadius, 0.0f, 1.0f);
        accumulated += near_factor * near_factor;
        ++sample_count;
    }

    if (sample_count <= 0) {
        return 0.0f;
    }
    return accumulated / static_cast<float>(sample_count);
}

float compute_waypoint_path_special_penalty(const std::vector<int>& path)
{
    if (path.size() < 2) {
        return 0.0f;
    }

    const BotPersonality& personality = get_active_bot_personality();
    const float decision_skill = bot_get_decision_skill_factor();
    const float risk_tolerance = std::clamp(personality.decision_risk_tolerance, 0.25f, 2.5f);
    const float crouch_route_avoidance = std::clamp(
        personality.crouch_route_avoidance_bias,
        0.0f,
        3.0f
    );
    const bool is_enemy_goal = g_client_bot_state.active_goal == BotGoalType::eliminate_target;
    const bool is_ctf_goal = bot_goal_is_ctf_objective(g_client_bot_state.active_goal);
    float penalty = 0.0f;

    for (size_t index = 1; index < path.size(); ++index) {
        int type_raw = 0;
        int subtype = 0;
        int movement_subtype = static_cast<int>(WaypointDroppedSubtype::normal);
        if (!waypoints_get_type_subtype(path[index], type_raw, subtype)) {
            continue;
        }
        (void)subtype;
        (void)waypoints_get_movement_subtype(path[index], movement_subtype);
        const auto type = static_cast<WaypointType>(type_raw);
        if (crouch_route_avoidance > 0.0f
            && movement_subtype == static_cast<int>(WaypointDroppedSubtype::crouch_needed)) {
            penalty += (is_enemy_goal ? 0.10f : 0.28f) * crouch_route_avoidance;
        }
        switch (type) {
            case WaypointType::std:
                break;
            case WaypointType::std_new:
                // Prefer mature std routes over exploratory std_new routes when both are viable.
                penalty += 0.65f;
                break;
            case WaypointType::jump_pad:
                penalty += (is_enemy_goal || is_ctf_goal) ? 0.15f : 0.45f;
                if (bot_personality_has_quirk(BotPersonalityQuirk::jump_pad_pathing_affinity)) {
                    penalty -= 0.55f;
                }
                break;
            case WaypointType::tele_entrance:
                penalty += (is_enemy_goal || is_ctf_goal) ? 0.20f : 0.60f;
                break;
            case WaypointType::lift_entrance:
                penalty += 0.40f;
                break;
            case WaypointType::ladder:
                penalty += 0.30f;
                break;
            case WaypointType::crater:
                penalty += is_enemy_goal ? 0.08f : 0.35f;
                if (bot_personality_has_quirk(BotPersonalityQuirk::crater_unlock_affinity)) {
                    penalty -= 0.35f;
                }
                break;
            default:
                break;
        }

        if (waypoints_waypoint_has_zone_type(
                path[index],
                WaypointZoneType::instant_death_zone)) {
            penalty += kRouteInstantDeathZonePenalty;
            continue;
        }

        if (waypoints_waypoint_has_zone_type(
                path[index],
                WaypointZoneType::damage_zone)) {
            penalty += is_enemy_goal
                ? kRouteDamageZoneEnemyGoalPenalty
                : kRouteDamageZonePenalty;
        }
    }

    const float risk_weight = std::lerp(
        1.35f,
        0.45f,
        std::clamp(risk_tolerance * decision_skill * 0.65f, 0.0f, 1.0f)
    );
    return penalty * risk_weight;
}

float compute_waypoint_path_power_position_bonus(const std::vector<int>& path)
{
    if (path.size() < 2) {
        return 0.0f;
    }

    rf::Vector3 start_pos{};
    if (!waypoints_get_pos(path.front(), start_pos)) {
        return 0.0f;
    }

    float elevation_bonus = 0.0f;
    float zone_bonus = 0.0f;
    int sample_count = 0;
    for (size_t index = 1; index < path.size(); ++index) {
        const int waypoint = path[index];
        rf::Vector3 waypoint_pos{};
        if (waypoint <= 0 || !waypoints_get_pos(waypoint, waypoint_pos)) {
            continue;
        }

        const float y_delta = waypoint_pos.y - start_pos.y;
        if (y_delta > 0.0f) {
            elevation_bonus += std::clamp(
                y_delta / kPowerPositionElevationScale,
                0.0f,
                kPowerPositionElevationClamp
            );
        }
        if (waypoints_waypoint_has_zone_type(
                waypoint,
                WaypointZoneType::high_power_zone)) {
            zone_bonus += kPowerPositionZoneBonus;
        }

        ++sample_count;
    }

    if (sample_count <= 0) {
        return 0.0f;
    }

    const float inv_sample_count = 1.0f / static_cast<float>(sample_count);
    return elevation_bonus * inv_sample_count * 2.1f
        + zone_bonus * inv_sample_count * 2.6f;
}

float get_waypoint_corner_hug_penalty(const int waypoint)
{
    static int cached_waypoint_total = 0;
    static std::vector<float> cached_penalties{};

    const int waypoint_total = waypoints_count();
    if (cached_waypoint_total != waypoint_total) {
        cached_waypoint_total = waypoint_total;
        cached_penalties.assign(
            static_cast<size_t>(std::max(waypoint_total, 0)),
            -1.0f
        );
    }

    if (waypoint <= 0 || waypoint >= waypoint_total) {
        return 0.0f;
    }

    float& cached = cached_penalties[static_cast<size_t>(waypoint)];
    if (cached >= 0.0f) {
        return cached;
    }

    rf::Vector3 waypoint_pos{};
    if (!waypoints_get_pos(waypoint, waypoint_pos)) {
        cached = 0.0f;
        return cached;
    }

    constexpr float kProbeDistance = 0.90f;
    constexpr float kInvSqrt2 = 0.70710678f;
    static const std::array<rf::Vector3, 8> kProbeDirs{
        rf::Vector3{1.0f, 0.0f, 0.0f},
        rf::Vector3{kInvSqrt2, 0.0f, kInvSqrt2},
        rf::Vector3{0.0f, 0.0f, 1.0f},
        rf::Vector3{-kInvSqrt2, 0.0f, kInvSqrt2},
        rf::Vector3{-1.0f, 0.0f, 0.0f},
        rf::Vector3{-kInvSqrt2, 0.0f, -kInvSqrt2},
        rf::Vector3{0.0f, 0.0f, -1.0f},
        rf::Vector3{kInvSqrt2, 0.0f, -kInvSqrt2},
    };

    int blocked_count = 0;
    bool blocked_pos_x = false;
    bool blocked_neg_x = false;
    bool blocked_pos_z = false;
    bool blocked_neg_z = false;

    for (int index = 0; index < static_cast<int>(kProbeDirs.size()); ++index) {
        rf::Vector3 p0 = waypoint_pos;
        rf::Vector3 p1 = waypoint_pos + kProbeDirs[index] * kProbeDistance;
        rf::GCollisionOutput collision{};
        if (!rf::collide_linesegment_level_solid(
                p0,
                p1,
                kBotRouteProbeTraceFlags,
                &collision)) {
            continue;
        }

        ++blocked_count;
        switch (index) {
            case 0:
                blocked_pos_x = true;
                break;
            case 2:
                blocked_pos_z = true;
                break;
            case 4:
                blocked_neg_x = true;
                break;
            case 6:
                blocked_neg_z = true;
                break;
            default:
                break;
        }
    }

    const float blocked_ratio =
        static_cast<float>(blocked_count) / static_cast<float>(kProbeDirs.size());
    const bool x_side_blocked = blocked_pos_x || blocked_neg_x;
    const bool z_side_blocked = blocked_pos_z || blocked_neg_z;
    const float corner_boost = (x_side_blocked && z_side_blocked) ? 0.65f : 0.0f;
    cached = std::clamp(blocked_ratio + corner_boost, 0.0f, 2.0f);
    return cached;
}

float get_waypoint_edge_exposure_penalty(const int waypoint)
{
    static int cached_waypoint_total = 0;
    static std::vector<float> cached_penalties{};

    const int waypoint_total = waypoints_count();
    if (cached_waypoint_total != waypoint_total) {
        cached_waypoint_total = waypoint_total;
        cached_penalties.assign(
            static_cast<size_t>(std::max(waypoint_total, 0)),
            -1.0f
        );
    }

    if (waypoint <= 0 || waypoint >= waypoint_total) {
        return 0.0f;
    }

    float& cached = cached_penalties[static_cast<size_t>(waypoint)];
    if (cached >= 0.0f) {
        return cached;
    }

    int type_raw = 0;
    int subtype = 0;
    if (!waypoints_get_type_subtype(waypoint, type_raw, subtype)) {
        cached = 0.0f;
        return cached;
    }
    (void)subtype;
    const WaypointType type = static_cast<WaypointType>(type_raw);
    switch (type) {
        case WaypointType::std:
        case WaypointType::std_new:
        case WaypointType::item:
        case WaypointType::respawn:
        case WaypointType::ctf_flag:
            break;
        default:
            cached = 0.0f;
            return cached;
    }

    rf::Vector3 waypoint_pos{};
    if (!waypoints_get_pos(waypoint, waypoint_pos)) {
        cached = 0.0f;
        return cached;
    }

    constexpr float kProbeRadius = 0.75f;
    constexpr float kCenterProbeStartYOffset = 0.12f;
    constexpr float kProbeDownDistance = 2.2f;
    constexpr float kLedgeDropTolerance = 0.60f;
    constexpr float kTwoPi = 6.283185307f;
    constexpr int kProbeCount = 8;

    rf::Vector3 center_p0 = waypoint_pos;
    rf::Vector3 center_p1 = waypoint_pos;
    center_p0.y += kCenterProbeStartYOffset;
    center_p1.y -= kProbeDownDistance;
    rf::GCollisionOutput center_collision{};
    if (!rf::collide_linesegment_level_solid(
            center_p0,
            center_p1,
            kBotRouteProbeTraceFlags,
            &center_collision)) {
        cached = 1.0f;
        return cached;
    }
    const float center_floor_y = center_collision.hit_point.y;

    int exposed_count = 0;
    for (int probe_index = 0; probe_index < kProbeCount; ++probe_index) {
        const float angle =
            (kTwoPi * static_cast<float>(probe_index)) / static_cast<float>(kProbeCount);
        rf::Vector3 sample_pos{
            waypoint_pos.x + std::cos(angle) * kProbeRadius,
            waypoint_pos.y,
            waypoint_pos.z + std::sin(angle) * kProbeRadius,
        };
        rf::Vector3 p0 = sample_pos;
        rf::Vector3 p1 = sample_pos;
        p0.y += kCenterProbeStartYOffset;
        p1.y -= kProbeDownDistance;
        rf::GCollisionOutput collision{};
        if (!rf::collide_linesegment_level_solid(
                p0,
                p1,
                kBotRouteProbeTraceFlags,
                &collision)) {
            ++exposed_count;
            continue;
        }

        if (collision.hit_point.y < (center_floor_y - kLedgeDropTolerance)) {
            ++exposed_count;
        }
    }

    const float exposed_ratio =
        static_cast<float>(exposed_count) / static_cast<float>(kProbeCount);
    cached = std::clamp(exposed_ratio, 0.0f, 1.0f);
    return cached;
}

float compute_waypoint_path_corner_hug_penalty(const std::vector<int>& path)
{
    if (path.size() < 2) {
        return 0.0f;
    }

    const size_t end_index = path.size() > 2 ? path.size() - 1 : path.size();
    float total_penalty = 0.0f;
    int sample_count = 0;
    for (size_t index = 1; index < end_index; ++index) {
        const int waypoint = path[index];
        if (waypoint <= 0) {
            continue;
        }
        total_penalty += get_waypoint_corner_hug_penalty(waypoint);
        ++sample_count;
    }

    if (sample_count <= 0) {
        return 0.0f;
    }
    return total_penalty / static_cast<float>(sample_count);
}

float compute_waypoint_path_edge_exposure_penalty(const std::vector<int>& path)
{
    if (path.size() < 2) {
        return 0.0f;
    }

    const size_t end_index = path.size() > 2 ? path.size() - 1 : path.size();
    float total_penalty = 0.0f;
    int sample_count = 0;
    for (size_t index = 1; index < end_index; ++index) {
        const int waypoint = path[index];
        if (waypoint <= 0) {
            continue;
        }
        total_penalty += get_waypoint_edge_exposure_penalty(waypoint);
        ++sample_count;
    }

    if (sample_count <= 0) {
        return 0.0f;
    }
    return total_penalty / static_cast<float>(sample_count);
}

int choose_weighted_top_rank(const int candidate_count, const float decisiveness)
{
    if (candidate_count <= 1) {
        return 0;
    }

    const float exponent = std::lerp(1.0f, 3.2f, std::clamp(decisiveness, 0.0f, 1.0f));
    float total_weight = 0.0f;
    for (int rank = 0; rank < candidate_count; ++rank) {
        total_weight += 1.0f / std::pow(static_cast<float>(rank + 1), exponent);
    }

    std::uniform_real_distribution<float> roll_dist(0.0f, total_weight);
    const float roll = roll_dist(g_rng);
    float accumulated = 0.0f;
    for (int rank = 0; rank < candidate_count; ++rank) {
        accumulated += 1.0f / std::pow(static_cast<float>(rank + 1), exponent);
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

int get_current_route_next_waypoint_for_goal(const int goal_waypoint)
{
    if (goal_waypoint <= 0 || g_client_bot_state.waypoint_goal != goal_waypoint) {
        return 0;
    }
    if (g_client_bot_state.waypoint_next_index <= 0
        || g_client_bot_state.waypoint_next_index
            >= static_cast<int>(g_client_bot_state.waypoint_path.size())) {
        return 0;
    }
    return g_client_bot_state.waypoint_path[g_client_bot_state.waypoint_next_index];
}

int get_current_route_next_waypoint_any_goal()
{
    if (g_client_bot_state.waypoint_next_index <= 0
        || g_client_bot_state.waypoint_next_index
            >= static_cast<int>(g_client_bot_state.waypoint_path.size())) {
        return 0;
    }
    return g_client_bot_state.waypoint_path[g_client_bot_state.waypoint_next_index];
}

template<typename RouteCandidateT>
int choose_route_candidate_index(
    const std::vector<RouteCandidateT>& route_candidates,
    const int pool_size,
    const float decisiveness,
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

    int sticky_next_waypoint = get_current_route_next_waypoint_for_goal(goal_waypoint);
    if (sticky_next_waypoint <= 0 && recover_eliminate) {
        sticky_next_waypoint = get_current_route_next_waypoint_any_goal();
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

    return choose_weighted_top_rank(bounded_pool_size, decisiveness);
}

bool path_contains_waypoint(const std::vector<int>& path, const int waypoint)
{
    return waypoint > 0
        && std::find(path.begin(), path.end(), waypoint) != path.end();
}

bool waypoint_type_allows_blacklist(const int waypoint)
{
    int type_raw = 0;
    int subtype = 0;
    if (waypoint <= 0
        || !waypoints_get_type_subtype(waypoint, type_raw, subtype)) {
        return false;
    }
    (void)subtype;
    const WaypointType type = static_cast<WaypointType>(type_raw);
    return type == WaypointType::std || type == WaypointType::std_new;
}
}

bool bot_nav_is_link_traversable_for_route(const int from_waypoint, const int to_waypoint)
{
    if (!waypoints_has_direct_link(from_waypoint, to_waypoint)) {
        return false;
    }
    if (!is_ladder_boundary_transition_valid(from_waypoint, to_waypoint)) {
        return false;
    }

    const bool from_instant_death_zone = waypoints_waypoint_has_zone_type(
        from_waypoint,
        WaypointZoneType::instant_death_zone
    );
    const bool to_instant_death_zone = waypoints_waypoint_has_zone_type(
        to_waypoint,
        WaypointZoneType::instant_death_zone
    );
    if (to_instant_death_zone && !from_instant_death_zone) {
        return false;
    }
    if (from_instant_death_zone && to_instant_death_zone) {
        return false;
    }

    return !is_link_unclimbable_upward_for_route(from_waypoint, to_waypoint);
}

bool bot_nav_path_has_unclimbable_upward_links(const std::vector<int>& path)
{
    if (path.size() < 2) {
        return false;
    }

    for (size_t i = 1; i < path.size(); ++i) {
        if (!bot_nav_is_link_traversable_for_route(path[i - 1], path[i])) {
            return true;
        }
    }

    return false;
}

bool bot_nav_link_midpoint_has_support(const int from_waypoint, const int to_waypoint)
{
    if (from_waypoint <= 0 || to_waypoint <= 0) {
        return true;
    }

    const int waypoint_total = waypoints_count();
    if (g_route_gap_support_cache_waypoint_total != waypoint_total) {
        g_route_gap_support_cache_waypoint_total = waypoint_total;
        g_route_gap_support_cache.clear();
    }
    else if (static_cast<int>(g_route_gap_support_cache.size()) > kRouteGapEdgeCacheMaxEntries) {
        bool keep = false;
        for (auto it = g_route_gap_support_cache.begin();
             it != g_route_gap_support_cache.end(); ) {
            if (keep) { ++it; } else { it = g_route_gap_support_cache.erase(it); }
            keep = !keep;
        }
    }

    const uint64_t edge_key = make_link_key(from_waypoint, to_waypoint);
    if (auto it = g_route_gap_support_cache.find(edge_key); it != g_route_gap_support_cache.end()) {
        return it->second;
    }

    const bool has_support = compute_link_midpoint_support_uncached(from_waypoint, to_waypoint);
    g_route_gap_support_cache.emplace(edge_key, has_support);
    return has_support;
}

float bot_nav_compute_route_score(
    const std::vector<int>& path,
    const float target_distance)
{
    const PathGeometryMetrics geometry = compute_path_geometry_metrics(path);
    if (!std::isfinite(geometry.route_length)) {
        return -std::numeric_limits<float>::infinity();
    }

    const float item_score = compute_waypoint_path_item_value(path);
    float special_penalty = compute_waypoint_path_special_penalty(path);
    const float corner_hug_penalty = compute_waypoint_path_corner_hug_penalty(path);
    const float edge_exposure_penalty = compute_waypoint_path_edge_exposure_penalty(path);
    const float gap_penalty = compute_waypoint_path_gap_penalty(path);
    const BotPersonality& personality = get_active_bot_personality();
    const float retrace_avoidance = std::clamp(personality.retrace_avoidance_bias, 0.0f, 3.0f);
    const int retrace_lookback = std::clamp(
        personality.retrace_lookback_waypoints,
        2,
        static_cast<int>(kWaypointVisitHistoryCapacity)
    );
    float recent_visit_penalty = 0.0f;
    if (path.size() >= 2 && !g_client_bot_state.recent_waypoint_visits.empty()) {
        std::unordered_set<int> path_waypoints;
        path_waypoints.reserve(path.size());
        for (size_t index = 1; index < path.size(); ++index) {
            path_waypoints.insert(path[index]);
        }

        const auto& history = g_client_bot_state.recent_waypoint_visits;
        const size_t history_size = history.size();
        const size_t lookback =
            std::min<size_t>(history_size, static_cast<size_t>(retrace_lookback));
        for (size_t i = 0; i < lookback; ++i) {
            const int recent_waypoint = history[history_size - 1 - i];
            if (recent_waypoint <= 0) {
                continue;
            }
            if (path_waypoints.contains(recent_waypoint)) {
                const float recency =
                    1.0f - (static_cast<float>(i) / std::max(1.0f, static_cast<float>(lookback)));
                const float recency_weight = 0.9f + recency * 1.8f;
                recent_visit_penalty += recency_weight;
            }
        }

        if (history_size >= 2 && path.size() >= 2) {
            const int last_waypoint = history[history_size - 1];
            const int previous_waypoint = history[history_size - 2];
            if (path[0] == last_waypoint && path[1] == previous_waypoint) {
                recent_visit_penalty += 6.0f;
            }
            else if (path[1] == previous_waypoint) {
                recent_visit_penalty += 4.2f;
            }
            if (history_size >= 3 && path.size() >= 3) {
                const int two_back_waypoint = history[history_size - 3];
                if (path[1] == previous_waypoint && path[2] == two_back_waypoint) {
                    recent_visit_penalty += 2.4f;
                }
            }
        }

        if (retrace_avoidance > 0.0f) {
            recent_visit_penalty *= std::lerp(1.0f, 2.25f, std::clamp(retrace_avoidance, 0.0f, 1.0f));
        }
    }

    const float decision_skill = bot_get_decision_skill_factor();
    const float efficiency = std::clamp(personality.decision_efficiency_bias, 0.35f, 2.25f);
    const float smoothing = std::clamp(personality.path_smoothing_bias, 0.25f, 2.5f);
    const float opportunism = std::clamp(personality.opportunism_bias, 0.25f, 2.5f);
    const float roam_intensity = std::clamp(personality.roam_intensity_bias, 0.25f, 2.5f);
    const float roam_intensity_norm = std::clamp((roam_intensity - 0.25f) / 2.25f, 0.0f, 1.0f);
    const float camping_bias = std::clamp(personality.camping_bias, 0.25f, 2.5f);
    float revisit_penalty_weight =
        kRouteRecentVisitPenaltyWeight
        * personality.revisit_avoidance_bias
        * std::lerp(1.25f, 0.55f, std::clamp((camping_bias - 0.25f) / 2.25f, 0.0f, 1.0f))
        * std::lerp(0.85f, 1.25f, decision_skill);
    float item_weight = kRouteItemValueWeight * personality.super_pickup_bias * opportunism;
    float distance_weight =
        kRouteDistanceWeight
        * std::lerp(0.85f, 1.45f, std::clamp(decision_skill * std::sqrt(efficiency), 0.0f, 1.0f));
    float target_distance_weight = kRouteTargetDistanceWeight * std::lerp(0.9f, 1.4f, decision_skill);
    float turn_weight = std::lerp(2.0f, 8.5f, decision_skill) * smoothing;
    float density_weight = std::lerp(1.0f, 5.5f, decision_skill) * smoothing;
    float detour_weight = std::lerp(4.0f, 15.0f, decision_skill) * efficiency;
    float special_weight = std::lerp(2.0f, 7.5f, decision_skill) * std::max(0.4f, 2.0f - personality.decision_risk_tolerance);
    const float risk_tolerance_norm =
        std::clamp((personality.decision_risk_tolerance - 0.25f) / 2.25f, 0.0f, 1.0f);
    float gap_weight = std::lerp(18.0f, 8.0f, risk_tolerance_norm);
    float corner_weight =
        kRouteCornerHugPenaltyWeight
        * std::clamp(personality.corner_centering_bias, 0.0f, 3.0f)
        * std::lerp(0.85f, 1.35f, decision_skill)
        * smoothing;
    float edge_exposure_weight =
        kRouteEdgeExposurePenaltyWeight
        * std::lerp(1.35f, 0.75f, risk_tolerance_norm)
        * std::lerp(0.90f, 1.25f, std::clamp(personality.corner_centering_bias / 3.0f, 0.0f, 1.0f));
    float power_position_weight = 0.0f;

    switch (g_client_bot_state.fsm_state) {
        case BotFsmState::recover_navigation:
            distance_weight *= 1.45f;
            target_distance_weight *= 1.35f;
            item_weight *= 0.45f;
            detour_weight *= 1.8f;
            corner_weight *= 0.7f;
            edge_exposure_weight *= 0.75f;
            gap_weight *= 0.85f;
            break;
        case BotFsmState::retreat:
            distance_weight *= 1.35f;
            target_distance_weight *= 1.45f;
            item_weight *= 0.95f;
            special_weight *= 1.35f;
            corner_weight *= 0.85f;
            edge_exposure_weight *= 0.85f;
            gap_weight *= 0.95f;
            break;
        case BotFsmState::seek_weapon:
            item_weight *= 1.65f;
            distance_weight *= 0.92f;
            special_weight *= 0.85f;
            break;
        case BotFsmState::replenish_health_armor:
            item_weight *= 1.72f;
            distance_weight *= 0.95f;
            special_weight *= 0.82f;
            break;
        case BotFsmState::find_power_position:
            target_distance_weight *= 1.22f;
            turn_weight *= 0.88f;
            detour_weight *= 1.10f;
            power_position_weight = std::lerp(
                4.0f,
                10.0f,
                std::clamp(personality.power_position_bias / 2.5f, 0.0f, 1.0f)
            );
            break;
        case BotFsmState::create_crater:
        case BotFsmState::shatter_glass:
            target_distance_weight *= 1.28f;
            item_weight *= 0.55f;
            detour_weight *= 0.92f;
            break;
        case BotFsmState::ctf_objective:
            target_distance_weight *= 1.36f;
            item_weight *= 0.62f;
            detour_weight *= 0.88f;
            gap_weight *= 1.10f;
            edge_exposure_weight *= 1.25f;
            break;
        case BotFsmState::collect_pickup:
            item_weight *= 1.5f;
            distance_weight *= 0.9f;
            edge_exposure_weight *= 1.20f;
            gap_weight *= 1.20f;
            break;
        case BotFsmState::engage_enemy:
        case BotFsmState::pursue_enemy:
        case BotFsmState::seek_enemy:
            target_distance_weight *= 1.25f;
            special_weight *= 0.8f;
            break;
        case BotFsmState::roam:
            item_weight *= 0.9f;
            break;
        default:
            break;
    }

    if (g_client_bot_state.active_goal == BotGoalType::eliminate_target) {
        target_distance_weight *= 1.3f;
        item_weight *= 0.65f;
    }
    else if (bot_goal_is_item_collection(g_client_bot_state.active_goal)) {
        item_weight *= 1.35f;
        target_distance_weight *= 0.9f;
    }
    else if (g_client_bot_state.active_goal == BotGoalType::roam) {
        // Roaming should favor exploration over shortest-path convergence.
        item_weight *= std::lerp(0.75f, 0.35f, roam_intensity_norm);
        distance_weight *= std::lerp(0.85f, 0.55f, roam_intensity_norm);
        target_distance_weight *= std::lerp(0.70f, 0.35f, roam_intensity_norm);
        detour_weight *= std::lerp(0.60f, 0.20f, roam_intensity_norm);
        revisit_penalty_weight *= std::lerp(1.45f, 2.35f, roam_intensity_norm);
    }
    else if (bot_goal_is_ctf_objective(g_client_bot_state.active_goal)) {
        const bool sneaky_capper = bot_personality_has_quirk(BotPersonalityQuirk::sneaky_capper);
        target_distance_weight *= 1.40f;
        item_weight *= 0.68f;
        if (g_client_bot_state.active_goal == BotGoalType::ctf_return_flag) {
            target_distance_weight *= 1.12f;
            special_weight *= 0.90f;
        }
        if (g_client_bot_state.active_goal == BotGoalType::ctf_capture_flag) {
            target_distance_weight *= 1.08f;
            special_weight *= 0.92f;
        }

        if (!sneaky_capper
            && (g_client_bot_state.active_goal == BotGoalType::ctf_steal_flag
                || g_client_bot_state.active_goal == BotGoalType::ctf_capture_flag)) {
            // Non-sneaky CTF behavior should commit to direct, stable cap routes.
            detour_weight *= 2.25f;
            turn_weight *= 1.35f;
            target_distance_weight *= 1.18f;
            revisit_penalty_weight *= 0.35f;
        }

        if (sneaky_capper
            && (g_client_bot_state.active_goal == BotGoalType::ctf_steal_flag
                || g_client_bot_state.active_goal == BotGoalType::ctf_capture_flag)
            && g_client_bot_state.ctf_threat_handle >= 0) {
            const float threat_penalty = compute_path_enemy_threat_penalty(
                path,
                g_client_bot_state.ctf_threat_pos
            );
            special_penalty += threat_penalty * 2.1f;
        }
    }

    const float power_position_bonus = (power_position_weight > 0.0f)
        ? compute_waypoint_path_power_position_bonus(path)
        : 0.0f;

    return item_score * item_weight
        - geometry.route_length * distance_weight
        - target_distance * target_distance_weight
        - recent_visit_penalty * revisit_penalty_weight
        - geometry.turn_penalty * turn_weight
        - geometry.density_penalty * density_weight
        - geometry.detour_penalty * detour_weight
        - special_penalty * special_weight
        - gap_penalty * gap_weight
        - corner_hug_penalty * corner_weight
        - edge_exposure_penalty * edge_exposure_weight
        + power_position_bonus * power_position_weight;
}

void bot_nav_prune_failed_edge_cooldowns()
{
    auto& cooldowns = g_client_bot_state.failed_edge_cooldowns;
    cooldowns.erase(
        std::remove_if(
            cooldowns.begin(),
            cooldowns.end(),
            [](const FailedEdgeCooldown& cooldown) {
                return cooldown.from_waypoint <= 0
                    || cooldown.to_waypoint <= 0
                    || !cooldown.cooldown.valid()
                    || cooldown.cooldown.elapsed();
            }
        ),
        cooldowns.end()
    );
}

void bot_nav_register_failed_edge_cooldown(
    const int from_waypoint,
    const int to_waypoint,
    const int cooldown_ms)
{
    if (from_waypoint <= 0
        || to_waypoint <= 0
        || from_waypoint == to_waypoint
        || cooldown_ms <= 0) {
        return;
    }

    bot_nav_prune_failed_edge_cooldowns();
    for (FailedEdgeCooldown& cooldown : g_client_bot_state.failed_edge_cooldowns) {
        if (cooldown.from_waypoint == from_waypoint
            && cooldown.to_waypoint == to_waypoint) {
            cooldown.cooldown.set(cooldown_ms);
            return;
        }
    }

    if (g_client_bot_state.failed_edge_cooldowns.size() >= kFailedEdgeCooldownCapacity) {
        g_client_bot_state.failed_edge_cooldowns.erase(
            g_client_bot_state.failed_edge_cooldowns.begin()
        );
    }

    FailedEdgeCooldown cooldown{};
    cooldown.from_waypoint = from_waypoint;
    cooldown.to_waypoint = to_waypoint;
    cooldown.cooldown.set(cooldown_ms);
    g_client_bot_state.failed_edge_cooldowns.push_back(cooldown);
}

void bot_nav_register_failed_edge_cooldown_bidirectional(
    const int waypoint_a,
    const int waypoint_b,
    const int cooldown_ms)
{
    bot_nav_register_failed_edge_cooldown(waypoint_a, waypoint_b, cooldown_ms);
    bot_nav_register_failed_edge_cooldown(waypoint_b, waypoint_a, cooldown_ms);
}

bool bot_nav_is_failed_edge_cooldown_active_no_prune(
    const int from_waypoint,
    const int to_waypoint)
{
    for (const FailedEdgeCooldown& cooldown : g_client_bot_state.failed_edge_cooldowns) {
        if (cooldown.from_waypoint == from_waypoint
            && cooldown.to_waypoint == to_waypoint
            && cooldown.cooldown.valid()
            && !cooldown.cooldown.elapsed()) {
            return true;
        }
    }

    return false;
}

bool bot_nav_path_contains_failed_edge_cooldown(const std::vector<int>& path)
{
    if (path.size() < 2) {
        return false;
    }

    bot_nav_prune_failed_edge_cooldowns();
    for (size_t i = 1; i < path.size(); ++i) {
        if (bot_nav_is_failed_edge_cooldown_active_no_prune(path[i - 1], path[i])) {
            return true;
        }
    }

    return false;
}

void bot_nav_prune_failed_waypoint_blacklist()
{
    auto& cooldowns = g_client_bot_state.failed_waypoint_cooldowns;
    cooldowns.erase(
        std::remove_if(
            cooldowns.begin(),
            cooldowns.end(),
            [](const FailedWaypointCooldown& cooldown) {
                return cooldown.waypoint <= 0
                    || !cooldown.cooldown.valid()
                    || cooldown.cooldown.elapsed()
                    || !waypoint_type_allows_blacklist(cooldown.waypoint);
            }
        ),
        cooldowns.end()
    );
}

int bot_nav_choose_blacklist_waypoint_for_failed_link(
    const int from_waypoint,
    const int to_waypoint)
{
    if (waypoint_type_allows_blacklist(to_waypoint)) {
        return to_waypoint;
    }
    if (waypoint_type_allows_blacklist(from_waypoint)) {
        return from_waypoint;
    }
    return 0;
}

void bot_nav_blacklist_waypoint_temporarily(const int waypoint, const int cooldown_ms)
{
    if (waypoint <= 0
        || cooldown_ms <= 0
        || !waypoint_type_allows_blacklist(waypoint)) {
        return;
    }

    bot_nav_prune_failed_waypoint_blacklist();
    for (FailedWaypointCooldown& cooldown : g_client_bot_state.failed_waypoint_cooldowns) {
        if (cooldown.waypoint == waypoint) {
            cooldown.cooldown.set(cooldown_ms);
            return;
        }
    }

    if (g_client_bot_state.failed_waypoint_cooldowns.size() >= kFailedWaypointBlacklistCapacity) {
        g_client_bot_state.failed_waypoint_cooldowns.erase(
            g_client_bot_state.failed_waypoint_cooldowns.begin()
        );
    }

    FailedWaypointCooldown cooldown{};
    cooldown.waypoint = waypoint;
    cooldown.cooldown.set(cooldown_ms);
    g_client_bot_state.failed_waypoint_cooldowns.push_back(cooldown);
}

bool bot_nav_is_waypoint_blacklisted_no_prune(const int waypoint)
{
    for (const FailedWaypointCooldown& cooldown : g_client_bot_state.failed_waypoint_cooldowns) {
        if (cooldown.waypoint == waypoint
            && cooldown.cooldown.valid()
            && !cooldown.cooldown.elapsed()) {
            return true;
        }
    }

    return false;
}

void bot_nav_append_blacklisted_waypoints_to_avoidset(
    std::vector<int>& avoidset,
    const int start_waypoint,
    const int goal_waypoint)
{
    bot_nav_prune_failed_waypoint_blacklist();
    for (const FailedWaypointCooldown& cooldown : g_client_bot_state.failed_waypoint_cooldowns) {
        const int waypoint = cooldown.waypoint;
        if (waypoint <= 0
            || waypoint == start_waypoint
            || waypoint == goal_waypoint) {
            continue;
        }
        avoidset.push_back(waypoint);
    }
}

void bot_nav_record_waypoint_visit(const int waypoint)
{
    if (waypoint <= 0) {
        return;
    }

    auto& history = g_client_bot_state.recent_waypoint_visits;
    if (!history.empty() && history.back() == waypoint) {
        return;
    }

    history.push_back(waypoint);
}

bool bot_nav_detect_recent_waypoint_ping_pong_loop(
    int& out_waypoint_a,
    int& out_waypoint_b)
{
    const auto& history = g_client_bot_state.recent_waypoint_visits;
    if (history.size() < 4) {
        return false;
    }

    const size_t n = history.size();
    const int w0 = history[n - 4];
    const int w1 = history[n - 3];
    const int w2 = history[n - 2];
    const int w3 = history[n - 1];

    if (w0 > 0 && w1 > 0 && w0 != w1 && w0 == w2 && w1 == w3) {
        out_waypoint_a = w0;
        out_waypoint_b = w1;
        return true;
    }

    return false;
}

bool bot_nav_pick_waypoint_route_to_goal_randomized_after_stuck(
    const int start_waypoint,
    const int goal_waypoint,
    const int avoid_waypoint,
    const int repath_ms)
{
    if (start_waypoint <= 0 || goal_waypoint <= 0 || start_waypoint == goal_waypoint) {
        return false;
    }

    struct RouteCandidate
    {
        float score = 0.0f;
        std::vector<int> path{};
    };

    const int waypoint_total = waypoints_count();
    int detour_budget = std::min(
        kMaxGoalDetourAttempts,
        compute_attempt_budget(
            kWaypointRecoveryDetourAttempts,
            waypoint_total,
            4
        )
    );
    const bool performance_sensitive_goal =
        bot_goal_is_control_point_objective(g_client_bot_state.active_goal)
        || bot_goal_is_item_collection(g_client_bot_state.active_goal)
        || bot_goal_is_ctf_objective(g_client_bot_state.active_goal)
        || g_client_bot_state.active_goal == BotGoalType::activate_bridge
        || g_client_bot_state.active_goal == BotGoalType::create_crater
        || g_client_bot_state.active_goal == BotGoalType::shatter_glass;
    if (performance_sensitive_goal && !g_client_bot_state.recovery_pending_reroute) {
        detour_budget = std::min(detour_budget, 4);
    }
    if (waypoint_total >= 1800) {
        detour_budget = std::min(detour_budget, 4);
    }
    if (waypoint_total >= 2500) {
        detour_budget = std::min(detour_budget, 3);
    }
    std::vector<RouteCandidate> route_candidates;
    route_candidates.reserve(detour_budget + 3);

    std::vector<int> avoidset{};
    if (avoid_waypoint > 0) {
        avoidset.push_back(avoid_waypoint);
    }
    bot_nav_append_blacklisted_waypoints_to_avoidset(avoidset, start_waypoint, goal_waypoint);
    std::sort(avoidset.begin(), avoidset.end());
    const std::size_t strict_avoid_count = avoidset.size();
    const bool has_strict_blacklist_avoidance =
        strict_avoid_count > (avoid_waypoint > 0 ? 1u : 0u);

    rf::Vector3 start_pos{};
    rf::Vector3 goal_pos{};
    const bool has_start_goal_positions =
        waypoints_get_pos(start_waypoint, start_pos) && waypoints_get_pos(goal_waypoint, goal_pos);
    const float corridor_radius = has_start_goal_positions
        ? std::clamp((goal_pos - start_pos).len() * 0.38f, 22.0f, 120.0f)
        : 120.0f;
    const float corridor_radius_sq = corridor_radius * corridor_radius;

    std::vector<int> path_direct;
    if (bot_waypoint_route(start_waypoint, goal_waypoint, avoidset, path_direct)
        && path_direct.size() >= 2) {
        const float route_score = bot_nav_compute_route_score(path_direct);
        if (std::isfinite(route_score)
            && !bot_nav_path_contains_failed_edge_cooldown(path_direct)
            && !bot_nav_path_has_unclimbable_upward_links(path_direct)) {
            if (avoid_waypoint <= 0) {
                // Common-case route solve: keep it cheap and avoid detour expansion.
                commit_route_path(std::move(path_direct), goal_waypoint, repath_ms);
                return true;
            }
            route_candidates.push_back(RouteCandidate{route_score, std::move(path_direct)});
        }
    }

    // If strict blacklist avoidance is forcing expensive detour search for objective-style goals,
    // try a cheaper relaxed direct solve before detour expansion.
    if (route_candidates.empty() && performance_sensitive_goal && has_strict_blacklist_avoidance) {
        const std::vector<int> relaxed_avoidset = avoid_waypoint > 0
            ? std::vector<int>{avoid_waypoint}
            : std::vector<int>{};
        std::vector<int> relaxed_path{};
        const bool relaxed_found =
            bot_waypoint_route(start_waypoint, goal_waypoint, relaxed_avoidset, relaxed_path);
        if (relaxed_found
            && relaxed_path.size() >= 2
            && !bot_nav_path_contains_failed_edge_cooldown(relaxed_path)
            && !bot_nav_path_has_unclimbable_upward_links(relaxed_path)) {
            commit_route_path(std::move(relaxed_path), goal_waypoint, repath_ms);
            return true;
        }
    }

    std::vector<int> detour_candidates;
    detour_candidates.reserve(waypoint_total);
    rf::Vector3 detour_pos{};
    for (int waypoint = 1; waypoint < waypoint_total; ++waypoint) {
        if (waypoint == start_waypoint || waypoint == goal_waypoint || waypoint == avoid_waypoint) {
            continue;
        }
        if (has_start_goal_positions && waypoints_get_pos(waypoint, detour_pos)) {
            const float dist_sq = point_to_segment_dist_sq_xz(detour_pos, start_pos, goal_pos);
            if (dist_sq > corridor_radius_sq) {
                continue;
            }
        }
        detour_candidates.push_back(waypoint);
    }
    std::shuffle(detour_candidates.begin(), detour_candidates.end(), g_rng);

    const int max_detours = std::min<int>(
        static_cast<int>(detour_candidates.size()),
        detour_budget
    );
    std::vector<int> path_a;
    std::vector<int> path_b;
    std::vector<int> combined_path;
    for (int index = 0; index < max_detours; ++index) {
        const int detour_waypoint = detour_candidates[index];
        path_a.clear();
        path_b.clear();
        combined_path.clear();
        if (!bot_waypoint_route(start_waypoint, detour_waypoint, avoidset, path_a)
            || path_a.size() < 2
            || !bot_waypoint_route(detour_waypoint, goal_waypoint, avoidset, path_b)
            || path_b.size() < 2) {
            continue;
        }

        combined_path = path_a;
        combined_path.insert(combined_path.end(), path_b.begin() + 1, path_b.end());
        if (combined_path.size() < 2
            || path_contains_waypoint(combined_path, avoid_waypoint)
            || bot_nav_path_contains_failed_edge_cooldown(combined_path)
            || bot_nav_path_has_unclimbable_upward_links(combined_path)) {
            continue;
        }

        const float route_score = bot_nav_compute_route_score(combined_path);
        if (!std::isfinite(route_score)) {
            continue;
        }
        route_candidates.push_back(
            RouteCandidate{
                route_score,
                std::move(combined_path),
            }
        );
    }

    // Hard fallback to direct shortest route if avoiding the blocked waypoint is impossible.
    if (route_candidates.empty()) {
        const std::vector<int> relaxed_avoidset = avoid_waypoint > 0
            ? std::vector<int>{avoid_waypoint}
            : std::vector<int>{};
        const std::vector<int> no_avoidset{};
        std::vector<int> path;
        const bool relaxed_found = has_strict_blacklist_avoidance
            && bot_waypoint_route(start_waypoint, goal_waypoint, relaxed_avoidset, path);
        const bool fallback_found = relaxed_found
            || bot_waypoint_route(start_waypoint, goal_waypoint, no_avoidset, path);
        if (!fallback_found
            || path.size() < 2
            || bot_nav_path_contains_failed_edge_cooldown(path)
            || bot_nav_path_has_unclimbable_upward_links(path)) {
            return false;
        }

        commit_route_path(std::move(path), goal_waypoint, repath_ms);
        return true;
    }

    std::sort(
        route_candidates.begin(),
        route_candidates.end(),
        [](const RouteCandidate& lhs, const RouteCandidate& rhs) {
            return lhs.score > rhs.score;
        }
    );

    const bool force_direct_ctf_choice =
        bot_goal_is_ctf_objective(g_client_bot_state.active_goal)
        && !bot_personality_has_quirk(BotPersonalityQuirk::sneaky_capper)
        && (g_client_bot_state.active_goal == BotGoalType::ctf_steal_flag
            || g_client_bot_state.active_goal == BotGoalType::ctf_capture_flag);
    const bool force_stable_route_choice =
        (bot_goal_is_item_collection(g_client_bot_state.active_goal)
            || g_client_bot_state.active_goal == BotGoalType::activate_bridge
            || g_client_bot_state.active_goal == BotGoalType::create_crater
            || g_client_bot_state.active_goal == BotGoalType::shatter_glass)
        && !g_client_bot_state.recovery_pending_reroute;
    const bool force_stable_recover_eliminate =
        g_client_bot_state.active_goal == BotGoalType::eliminate_target
        && (g_client_bot_state.fsm_state == BotFsmState::recover_navigation
            || g_client_bot_state.recovery_pending_reroute);
    const int pool_size = (force_direct_ctf_choice
                           || force_stable_route_choice
                           || force_stable_recover_eliminate)
        ? 1
        : std::min<int>(
              static_cast<int>(route_candidates.size()),
              kRouteSelectionPool
          );
    const float decisiveness = (force_direct_ctf_choice
                                || force_stable_route_choice
                                || force_stable_recover_eliminate)
        ? 1.0f
        : std::clamp(
              bot_get_decision_skill_factor()
                  * std::clamp(get_active_bot_personality().decision_efficiency_bias, 0.35f, 2.25f),
              0.0f,
              1.0f
          );
    const int selected_rank = choose_route_candidate_index(
        route_candidates,
        pool_size,
        decisiveness,
        goal_waypoint
    );
    RouteCandidate& selected = route_candidates[selected_rank];

    commit_route_path(std::move(selected.path), goal_waypoint, repath_ms);
    return true;
}

bool bot_nav_pick_waypoint_route_to_goal_long_detour(
    const int start_waypoint,
    const int goal_waypoint,
    const int repath_ms)
{
    if (start_waypoint <= 0 || goal_waypoint <= 0 || start_waypoint == goal_waypoint) {
        return false;
    }

    struct RouteCandidate
    {
        float score = 0.0f;
        std::vector<int> path{};
    };

    const int waypoint_total = waypoints_count();
    int detour_budget = std::min(
        kMaxLongDetourAttempts,
        compute_attempt_budget(
            kLongRouteDetourAttempts,
            waypoint_total,
            6
        )
    );
    const bool performance_sensitive_goal =
        bot_goal_is_control_point_objective(g_client_bot_state.active_goal)
        || bot_goal_is_item_collection(g_client_bot_state.active_goal)
        || bot_goal_is_ctf_objective(g_client_bot_state.active_goal)
        || g_client_bot_state.active_goal == BotGoalType::activate_bridge
        || g_client_bot_state.active_goal == BotGoalType::create_crater
        || g_client_bot_state.active_goal == BotGoalType::shatter_glass;
    if (performance_sensitive_goal) {
        detour_budget = std::min(detour_budget, 4);
    }
    if (waypoint_total >= 1800) {
        detour_budget = std::min(detour_budget, 5);
    }
    if (waypoint_total >= 2500) {
        detour_budget = std::min(detour_budget, 3);
    }
    std::vector<RouteCandidate> route_candidates;
    route_candidates.reserve(detour_budget + 1);

    // Keep direct route as last-resort candidate.
    std::vector<int> avoidset{};
    bot_nav_append_blacklisted_waypoints_to_avoidset(avoidset, start_waypoint, goal_waypoint);
    std::sort(avoidset.begin(), avoidset.end());
    const bool has_blacklisted_avoidance = !avoidset.empty();
    rf::Vector3 start_pos{};
    rf::Vector3 goal_pos{};
    const bool has_start_goal_positions =
        waypoints_get_pos(start_waypoint, start_pos) && waypoints_get_pos(goal_waypoint, goal_pos);
    const float corridor_radius = has_start_goal_positions
        ? std::clamp((goal_pos - start_pos).len() * 0.55f, 28.0f, 180.0f)
        : 180.0f;
    const float corridor_radius_sq = corridor_radius * corridor_radius;

    std::vector<int> direct_path;
    if (bot_waypoint_route(start_waypoint, goal_waypoint, avoidset, direct_path)
        && direct_path.size() >= 2) {
        const float route_len = compute_waypoint_path_length(direct_path);
        const float route_score = bot_nav_compute_route_score(direct_path) + route_len * 0.20f;
        if (std::isfinite(route_score)
            && !bot_nav_path_contains_failed_edge_cooldown(direct_path)
            && !bot_nav_path_has_unclimbable_upward_links(direct_path)) {
            if (performance_sensitive_goal) {
                commit_route_path(std::move(direct_path), goal_waypoint, repath_ms);
                return true;
            }
            route_candidates.push_back(RouteCandidate{route_score, std::move(direct_path)});
        }
    }

    std::vector<int> detour_candidates;
    detour_candidates.reserve(waypoint_total);
    rf::Vector3 detour_pos{};
    for (int waypoint = 1; waypoint < waypoint_total; ++waypoint) {
        if (waypoint == start_waypoint || waypoint == goal_waypoint) {
            continue;
        }
        if (has_start_goal_positions && waypoints_get_pos(waypoint, detour_pos)) {
            const float dist_sq = point_to_segment_dist_sq_xz(detour_pos, start_pos, goal_pos);
            if (dist_sq > corridor_radius_sq) {
                continue;
            }
        }
        detour_candidates.push_back(waypoint);
    }
    std::shuffle(detour_candidates.begin(), detour_candidates.end(), g_rng);

    const int max_detours = std::min<int>(
        static_cast<int>(detour_candidates.size()),
        detour_budget
    );
    std::vector<int> path_a;
    std::vector<int> path_b;
    std::vector<int> combined_path;
    for (int index = 0; index < max_detours; ++index) {
        const int detour_waypoint = detour_candidates[index];
        path_a.clear();
        path_b.clear();
        combined_path.clear();
        if (!bot_waypoint_route(start_waypoint, detour_waypoint, avoidset, path_a)
            || path_a.size() < 2
            || !bot_waypoint_route(detour_waypoint, goal_waypoint, avoidset, path_b)
            || path_b.size() < 2) {
            continue;
        }

        combined_path = path_a;
        combined_path.insert(combined_path.end(), path_b.begin() + 1, path_b.end());
        if (combined_path.size() < 3
            || bot_nav_path_contains_failed_edge_cooldown(combined_path)
            || bot_nav_path_has_unclimbable_upward_links(combined_path)) {
            continue;
        }

        const float route_len = compute_waypoint_path_length(combined_path);
        const float route_score = bot_nav_compute_route_score(combined_path) + route_len * 0.20f;
        if (!std::isfinite(route_score)) {
            continue;
        }
        route_candidates.push_back(RouteCandidate{route_score, std::move(combined_path)});
    }

    if (route_candidates.empty()) {
        if (!has_blacklisted_avoidance) {
            return false;
        }

        const std::vector<int> no_avoidset{};
        std::vector<int> fallback_direct_path;
        if (!bot_waypoint_route(start_waypoint, goal_waypoint, no_avoidset, fallback_direct_path)
            || fallback_direct_path.size() < 2
            || bot_nav_path_contains_failed_edge_cooldown(fallback_direct_path)
            || bot_nav_path_has_unclimbable_upward_links(fallback_direct_path)) {
            return false;
        }

        commit_route_path(std::move(fallback_direct_path), goal_waypoint, repath_ms);
        return true;
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
    const float decisiveness = std::clamp(
        bot_get_decision_skill_factor() * std::clamp(get_active_bot_personality().decision_efficiency_bias, 0.35f, 2.25f),
        0.0f,
        1.0f
    );
    const int selected_rank = choose_route_candidate_index(
        route_candidates,
        pool_size,
        decisiveness,
        goal_waypoint
    );
    RouteCandidate& selected = route_candidates[selected_rank];

    commit_route_path(std::move(selected.path), goal_waypoint, repath_ms);
    return true;
}

bool bot_nav_pick_waypoint_route(const int start_waypoint)
{
    const int waypoint_total = waypoints_count();
    if (waypoint_total <= 1 || start_waypoint <= 0) {
        return false;
    }

    std::vector<int> candidates;
    candidates.reserve(waypoint_total - 1);
    for (int candidate = 1; candidate < waypoint_total; ++candidate) {
        if (candidate != start_waypoint) {
            candidates.push_back(candidate);
        }
    }
    std::shuffle(candidates.begin(), candidates.end(), g_rng);

    const int max_attempts = std::min<int>(
        std::min(
            compute_attempt_budget(kWaypointPickAttempts, waypoint_total, 8),
            kMaxRoamRouteAttempts
        ),
        static_cast<int>(candidates.size())
    );

    struct RouteCandidate
    {
        int goal_waypoint = 0;
        float score = 0.0f;
        std::vector<int> path{};
    };

    std::vector<int> avoidset{};
    bot_nav_append_blacklisted_waypoints_to_avoidset(avoidset, start_waypoint, 0);
    std::sort(avoidset.begin(), avoidset.end());
    const bool has_blacklisted_avoidance = !avoidset.empty();
    const std::vector<int> no_avoidset{};
    std::vector<RouteCandidate> route_candidates;
    route_candidates.reserve(max_attempts);
    auto collect_candidates = [&](const std::vector<int>& active_avoidset,
                                   const bool ignore_failed_edges) {
        std::vector<int> path;
        for (int attempt = 0; attempt < max_attempts; ++attempt) {
            const int candidate = candidates[attempt];
            path.clear();
            if (!bot_waypoint_route(start_waypoint, candidate, active_avoidset, path)) {
                continue;
            }
            if (path.size() < 2
                || (!ignore_failed_edges && bot_nav_path_contains_failed_edge_cooldown(path))
                || bot_nav_path_has_unclimbable_upward_links(path)) {
                continue;
            }

            const float route_score = bot_nav_compute_route_score(path);
            if (!std::isfinite(route_score)) {
                continue;
            }

            float adjusted_route_score = route_score;
            if (g_client_bot_state.active_goal == BotGoalType::roam) {
                const float roam_intensity = std::clamp(
                    get_active_bot_personality().roam_intensity_bias,
                    0.25f,
                    2.5f
                );
                const float roam_intensity_norm =
                    std::clamp((roam_intensity - 0.25f) / 2.25f, 0.0f, 1.0f);
                const float path_len_bonus = std::lerp(0.10f, 0.35f, roam_intensity_norm);
                adjusted_route_score += compute_waypoint_path_length(path) * path_len_bonus;
            }

            route_candidates.push_back(
                RouteCandidate{
                    candidate,
                    adjusted_route_score,
                    path,
                }
            );
        }
    };

    collect_candidates(avoidset, false);
    if (route_candidates.empty() && has_blacklisted_avoidance) {
        collect_candidates(no_avoidset, false);
    }
    // Last resort: ignore failed edge cooldowns entirely. Walking a previously-failed
    // edge is better than standing still as a statue.
    if (route_candidates.empty()) {
        collect_candidates(no_avoidset, true);
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
    const float decisiveness = std::clamp(
        bot_get_decision_skill_factor() * std::clamp(get_active_bot_personality().decision_efficiency_bias, 0.35f, 2.25f),
        0.0f,
        1.0f
    );
    const int selected_rank = choose_route_candidate_index(
        route_candidates,
        pool_size,
        decisiveness,
        0
    );
    RouteCandidate& selected = route_candidates[selected_rank];

    commit_route_path(std::move(selected.path), selected.goal_waypoint, kWaypointRepathMs);
    return true;
}

bool bot_nav_pick_waypoint_route_to_goal(
    const int start_waypoint,
    const int goal_waypoint,
    const int repath_ms)
{
    return bot_nav_pick_waypoint_route_to_goal_randomized_after_stuck(
        start_waypoint,
        goal_waypoint,
        0,
        repath_ms
    );
}
