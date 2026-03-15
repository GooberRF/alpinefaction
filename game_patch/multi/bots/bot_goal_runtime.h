#pragma once

#include "bot_internal.h"

void bot_goal_runtime_clear_bridge_goal_state();
void bot_goal_runtime_clear_bridge_post_open_priority_state();
void bot_goal_runtime_prime_bridge_post_open_priority(int zone_uid, int priority_ms);
void bot_goal_runtime_clear_non_active_goal_state(BotGoalType active_goal);

bool bot_goal_runtime_abort_bridge_goal();
bool bot_goal_runtime_abort_crater_goal();
bool bot_goal_runtime_abort_shatter_goal();
bool bot_goal_runtime_abort_ctf_goal();
bool bot_goal_runtime_abort_control_point_goal();

bool bot_goal_runtime_route_to_waypoint_target_with_recovery(
    const rf::Entity& local_entity,
    const rf::Vector3& waypoint_target_pos,
    int goal_waypoint,
    int primary_repath_ms,
    int recovery_repath_ms);
