#include "bot_goal_manager.h"

#include "bot_goals.h"
#include "bot_state.h"

void bot_goal_manager_refresh(
    const rf::Entity& local_entity,
    rf::Entity* enemy_target,
    const bool enemy_has_los)
{
    bot_refresh_goal_state(local_entity, enemy_target, enemy_has_los);
}

void bot_goal_manager_ensure_active(
    const rf::Entity& local_entity,
    rf::Entity* enemy_target,
    const bool enemy_has_los,
    const int fallback_eval_delay_ms)
{
    if (g_client_bot_state.active_goal != BotGoalType::none) {
        return;
    }

    bot_state_set_roam_fallback_goal(fallback_eval_delay_ms);
    // Allow a fresh evaluation even if the per-frame dedup already ran this frame.
    // Recovery paths (watchdogs, invariant checks) clear the goal and need a real
    // reevaluation to pick a new objective.
    g_client_bot_state.goal_refreshed_this_frame = false;
    bot_refresh_goal_state(local_entity, enemy_target, enemy_has_los);
    if (g_client_bot_state.active_goal == BotGoalType::none) {
        bot_state_set_roam_fallback_goal(fallback_eval_delay_ms);
    }
}

void bot_goal_manager_refresh_and_ensure(
    const rf::Entity& local_entity,
    rf::Entity* enemy_target,
    const bool enemy_has_los,
    const int fallback_eval_delay_ms)
{
    bot_goal_manager_refresh(local_entity, enemy_target, enemy_has_los);
    bot_goal_manager_ensure_active(
        local_entity,
        enemy_target,
        enemy_has_los,
        fallback_eval_delay_ms
    );
}

void bot_goal_manager_force_refresh_and_ensure(
    const rf::Entity& local_entity,
    rf::Entity* enemy_target,
    const bool enemy_has_los,
    const int fallback_eval_delay_ms)
{
    // Bypass per-frame dedup so watchdog recovery paths get a real reevaluation
    // even if the initial frame refresh already ran.
    g_client_bot_state.goal_refreshed_this_frame = false;
    bot_goal_manager_refresh(local_entity, enemy_target, enemy_has_los);
    bot_goal_manager_ensure_active(
        local_entity,
        enemy_target,
        enemy_has_los,
        fallback_eval_delay_ms
    );
}
