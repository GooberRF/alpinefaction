#include "bot_goal_runtime.h"

#include "bot_context.h"
#include "bot_state.h"
#include <limits>

void bot_goal_runtime_clear_bridge_goal_state()
{
    bot_state().bridge.zone_uid = -1;
    bot_state().bridge.trigger_uid = -1;
    bot_state().bridge.trigger_pos = {};
    bot_state().bridge.activation_radius = 0.0f;
    bot_state().bridge.requires_use = false;
    bot_state().bridge.use_press_timer.invalidate();
    bot_state().bridge.activation_abort_timer.invalidate();
    bot_state().bridge.activation_best_dist_sq = std::numeric_limits<float>::infinity();
}

void bot_goal_runtime_clear_bridge_post_open_priority_state()
{
    bot_state().bridge.post_open_zone_uid = -1;
    bot_state().bridge.post_open_target_waypoint = 0;
    bot_state().bridge.post_open_priority_timer.invalidate();
}

void bot_goal_runtime_prime_bridge_post_open_priority(
    const int zone_uid,
    const int priority_ms)
{
    if (zone_uid < 0 || priority_ms <= 0) {
        bot_goal_runtime_clear_bridge_post_open_priority_state();
        return;
    }
    bot_state().bridge.post_open_zone_uid = zone_uid;
    bot_state().bridge.post_open_target_waypoint = 0;
    bot_state().bridge.post_open_priority_timer.set(priority_ms);
}

void bot_goal_runtime_clear_non_active_goal_state(const BotGoalType active_goal)
{
    if (active_goal != BotGoalType::activate_bridge) {
        bot_goal_runtime_clear_bridge_goal_state();
    }
    if (active_goal != BotGoalType::create_crater) {
        bot_state().crater_goal_abort_timer.invalidate();
        bot_state().crater_standoff_abort_timer.invalidate();
        bot_state().crater_goal_timeout_timer.invalidate();
    }
    if (active_goal != BotGoalType::shatter_glass) {
        bot_state().shatter_goal_abort_timer.invalidate();
        bot_state().shatter_goal_timeout_timer.invalidate();
    }
    if (active_goal != BotGoalType::control_point_objective) {
        bot_state().control_point_route_fail_timer.invalidate();
        bot_state().control_point_patrol_waypoint = 0;
        bot_state().control_point_patrol_timer.invalidate();
    }
    if (!bot_goal_is_salvage_positioning(active_goal)) {
        bot_state().salvage_hold_pos = {};
        bot_state().salvage_hold_orbit_timer.invalidate();
        bot_state().salvage_hold_orbit_left = false;
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
    bot_state().crater_goal_abort_timer.invalidate();
    bot_state().crater_standoff_abort_timer.invalidate();
    bot_state().crater_goal_timeout_timer.invalidate();
    bot_state_set_roam_fallback_goal(250);
    bot_state_clear_waypoint_route(true, true, false);
    return false;
}

bool bot_goal_runtime_abort_shatter_goal()
{
    bot_state().shatter_goal_abort_timer.invalidate();
    bot_state().shatter_goal_timeout_timer.invalidate();
    bot_state_set_roam_fallback_goal(250);
    bot_state_clear_waypoint_route(true, true, false);
    return false;
}

bool bot_goal_runtime_abort_ctf_goal()
{
    bot_state_set_roam_fallback_goal(250);
    bot_state().ctf_objective_route_fail_timer.invalidate();
    bot_state_clear_waypoint_route(true, true, false);
    return false;
}

bool bot_goal_runtime_abort_bagman_goal()
{
    bot_state_set_roam_fallback_goal(250);
    // Reuse the CTF objective fail timer slot.
    bot_state().ctf_objective_route_fail_timer.invalidate();
    bot_state_clear_waypoint_route(true, true, false);
    return false;
}

bool bot_goal_runtime_abort_salvage_goal()
{
    bot_state_set_roam_fallback_goal(250);
    // Reuse the CTF objective fail timer slot.
    bot_state().ctf_objective_route_fail_timer.invalidate();
    bot_state_clear_waypoint_route(true, true, false);
    return false;
}

bool bot_goal_runtime_abort_control_point_goal()
{
    bot_state_set_roam_fallback_goal(250);
    bot_state().control_point_route_fail_timer.invalidate();
    bot_state().control_point_patrol_waypoint = 0;
    bot_state().control_point_patrol_timer.invalidate();
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
