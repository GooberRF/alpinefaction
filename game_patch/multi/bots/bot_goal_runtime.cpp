#include "bot_goal_runtime.h"

#include "bot_state.h"
#include <limits>

void bot_goal_runtime_clear_bridge_goal_state()
{
    g_client_bot_state.bridge.zone_uid = -1;
    g_client_bot_state.bridge.trigger_uid = -1;
    g_client_bot_state.bridge.trigger_pos = {};
    g_client_bot_state.bridge.activation_radius = 0.0f;
    g_client_bot_state.bridge.requires_use = false;
    g_client_bot_state.bridge.use_press_timer.invalidate();
    g_client_bot_state.bridge.activation_abort_timer.invalidate();
    g_client_bot_state.bridge.activation_best_dist_sq = std::numeric_limits<float>::infinity();
}

void bot_goal_runtime_clear_bridge_post_open_priority_state()
{
    g_client_bot_state.bridge.post_open_zone_uid = -1;
    g_client_bot_state.bridge.post_open_target_waypoint = 0;
    g_client_bot_state.bridge.post_open_priority_timer.invalidate();
}

void bot_goal_runtime_prime_bridge_post_open_priority(
    const int zone_uid,
    const int priority_ms)
{
    if (zone_uid < 0 || priority_ms <= 0) {
        bot_goal_runtime_clear_bridge_post_open_priority_state();
        return;
    }
    g_client_bot_state.bridge.post_open_zone_uid = zone_uid;
    g_client_bot_state.bridge.post_open_target_waypoint = 0;
    g_client_bot_state.bridge.post_open_priority_timer.set(priority_ms);
}

void bot_goal_runtime_clear_non_active_goal_state(const BotGoalType active_goal)
{
    if (active_goal != BotGoalType::activate_bridge) {
        bot_goal_runtime_clear_bridge_goal_state();
    }
    if (active_goal != BotGoalType::create_crater) {
        g_client_bot_state.crater_goal_abort_timer.invalidate();
        g_client_bot_state.crater_goal_timeout_timer.invalidate();
    }
    if (active_goal != BotGoalType::shatter_glass) {
        g_client_bot_state.shatter_goal_abort_timer.invalidate();
        g_client_bot_state.shatter_goal_timeout_timer.invalidate();
    }
    if (active_goal != BotGoalType::control_point_objective) {
        g_client_bot_state.control_point_route_fail_timer.invalidate();
        g_client_bot_state.control_point_patrol_waypoint = 0;
        g_client_bot_state.control_point_patrol_timer.invalidate();
    }
}

bool bot_goal_runtime_abort_bridge_goal()
{
    bot_goal_runtime_clear_bridge_goal_state();
    bot_state_set_roam_fallback_goal(250);
    bot_state_clear_waypoint_route(true, true, false);
    return false;
}

bool bot_goal_runtime_abort_crater_goal()
{
    g_client_bot_state.crater_goal_abort_timer.invalidate();
    g_client_bot_state.crater_goal_timeout_timer.invalidate();
    bot_state_set_roam_fallback_goal(250);
    bot_state_clear_waypoint_route(true, true, false);
    return false;
}

bool bot_goal_runtime_abort_shatter_goal()
{
    g_client_bot_state.shatter_goal_abort_timer.invalidate();
    g_client_bot_state.shatter_goal_timeout_timer.invalidate();
    bot_state_set_roam_fallback_goal(250);
    bot_state_clear_waypoint_route(true, true, false);
    return false;
}

bool bot_goal_runtime_abort_ctf_goal()
{
    bot_state_set_roam_fallback_goal(250);
    g_client_bot_state.ctf_objective_route_fail_timer.invalidate();
    bot_state_clear_waypoint_route(true, true, false);
    return false;
}

bool bot_goal_runtime_abort_control_point_goal()
{
    bot_state_set_roam_fallback_goal(250);
    g_client_bot_state.control_point_route_fail_timer.invalidate();
    g_client_bot_state.control_point_patrol_waypoint = 0;
    g_client_bot_state.control_point_patrol_timer.invalidate();
    bot_state_clear_waypoint_route(true, true, false);
    return false;
}

bool bot_goal_runtime_route_to_waypoint_target_with_recovery(
    const rf::Entity& local_entity,
    const rf::Vector3& waypoint_target_pos,
    const int goal_waypoint,
    const int primary_repath_ms,
    const int recovery_repath_ms)
{
    bool routed = bot_internal_update_waypoint_target_towards(
        local_entity,
        waypoint_target_pos,
        nullptr,
        nullptr,
        primary_repath_ms);
    if (!routed) {
        bot_internal_start_recovery_anchor_reroute(local_entity, goal_waypoint);
        routed = bot_internal_update_waypoint_target_towards(
            local_entity,
            waypoint_target_pos,
            nullptr,
            nullptr,
            recovery_repath_ms);
    }
    return routed;
}
