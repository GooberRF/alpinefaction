#pragma once

#include "bot_internal.h"

void bot_state_set_goal(
    BotGoalType goal,
    int target_handle,
    int target_identifier,
    int target_waypoint,
    const rf::Vector3& target_pos);

void bot_state_clear_goal();
void bot_state_clear_goal_and_eval();
void bot_state_set_roam_fallback_goal(int eval_delay_ms);

void bot_state_clear_recovery_reroute();
void bot_state_clear_waypoint_route(
    bool invalidate_repath_timer,
    bool clear_recovery_state,
    bool clear_recent_waypoint_visits);

void bot_state_clear_contextual_goal_tracking();
void bot_state_clear_item_goal_contact_tracking();

void bot_state_transition_fsm(BotFsmState new_state, int transition_timer_ms);
void bot_state_reset_fsm(BotFsmState new_state);
