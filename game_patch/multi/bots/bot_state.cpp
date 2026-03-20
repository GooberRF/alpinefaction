#include "bot_state.h"

void bot_state_set_goal(
    const BotGoalType goal,
    const int target_handle,
    const int target_identifier,
    const int target_waypoint,
    const rf::Vector3& target_pos)
{
    g_client_bot_state.active_goal = goal;
    g_client_bot_state.goal_target_handle = target_handle;
    g_client_bot_state.goal_target_identifier = target_identifier;
    g_client_bot_state.goal_target_waypoint = target_waypoint;
    g_client_bot_state.goal_target_pos = target_pos;
}

void bot_state_clear_goal()
{
    bot_state_set_goal(BotGoalType::none, -1, -1, 0, {});
}

void bot_state_clear_goal_and_eval()
{
    bot_state_clear_goal();
    g_client_bot_state.goal_eval_timer.invalidate();
    g_client_bot_state.goal_switch_lock_timer.invalidate();
}

void bot_state_set_roam_fallback_goal(const int eval_delay_ms)
{
    bot_state_set_goal(BotGoalType::roam, -1, -1, 0, {});
    if (eval_delay_ms > 0) {
        g_client_bot_state.goal_eval_timer.set(eval_delay_ms);
    }
    else {
        g_client_bot_state.goal_eval_timer.invalidate();
    }
    g_client_bot_state.goal_switch_lock_timer.set(kRoamFallbackGoalLockMs);
}

void bot_state_clear_recovery_reroute()
{
    g_client_bot_state.recovery_pending_reroute = false;
    g_client_bot_state.recovery_anchor_waypoint = 0;
    g_client_bot_state.recovery_avoid_waypoint = 0;
}

void bot_state_clear_waypoint_route(
    const bool invalidate_repath_timer,
    const bool clear_recovery_state,
    const bool clear_recent_waypoint_visits)
{
    if (invalidate_repath_timer) {
        g_client_bot_state.repath_timer.invalidate();
    }
    if (clear_recovery_state) {
        bot_state_clear_recovery_reroute();
    }
    bot_internal_clear_waypoint_route();
    if (clear_recent_waypoint_visits) {
        g_client_bot_state.recent_waypoint_visits.clear();
    }
}

void bot_state_clear_contextual_goal_tracking()
{
    g_client_bot_state.contextual_goal_eval_timer.invalidate();
    g_client_bot_state.contextual_goal_item_handle = -1;
    g_client_bot_state.contextual_goal_waypoint = 0;
    g_client_bot_state.contextual_goal_pos = {};
}

void bot_state_clear_item_goal_contact_tracking()
{
    g_client_bot_state.item_goal_contact_timer.invalidate();
    g_client_bot_state.item_goal_contact_handle = -1;
}

static bool is_critical_fsm_state(const BotFsmState state)
{
    return state == BotFsmState::retreat
        || state == BotFsmState::recover_navigation;
}

void bot_state_transition_fsm(const BotFsmState new_state, const int transition_timer_ms)
{
    if (g_client_bot_state.fsm_state == new_state) {
        return;
    }

    // Minimum hold time: prevent rapid oscillation between non-critical states.
    // Critical states (retreat, recover_navigation) always bypass the cooldown.
    constexpr int kFsmTransitionCooldownMs = 200;
    if (!is_critical_fsm_state(new_state)
        && g_client_bot_state.fsm_transition_cooldown.valid()
        && !g_client_bot_state.fsm_transition_cooldown.elapsed()) {
        return;
    }

    g_client_bot_state.fsm_state = new_state;
    g_client_bot_state.fsm_transition_cooldown.set(kFsmTransitionCooldownMs);
    if (transition_timer_ms > 0) {
        g_client_bot_state.fsm_state_timer.set(transition_timer_ms);
    }
    else {
        g_client_bot_state.fsm_state_timer.invalidate();
    }
}

void bot_state_reset_fsm(const BotFsmState new_state)
{
    g_client_bot_state.fsm_state = new_state;
    g_client_bot_state.fsm_state_timer.invalidate();
    g_client_bot_state.fsm_transition_cooldown.invalidate();
}
