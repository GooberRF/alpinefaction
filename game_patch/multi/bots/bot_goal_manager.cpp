#include "bot_goal_manager.h"

#include "bot_context.h"
#include "bot_goals.h"
#include "bot_state.h"

namespace
{
// A full goal evaluation is expensive: past the regular one, a bot gets at most one more per frame.
void allow_forced_goal_refresh()
{
    if (!bot_state().forced_goal_refresh_this_frame) {
        bot_state().forced_goal_refresh_this_frame = true;
        bot_state().goal_refreshed_this_frame = false;
    }
}
}

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
    if (bot_state().active_goal != BotGoalType::none) {
        return;
    }

    bot_state_set_roam_fallback_goal(fallback_eval_delay_ms);
    // Recovery paths clear the goal and need a real reevaluation; only the first forced call per frame gets one.
    allow_forced_goal_refresh();
    bot_refresh_goal_state(local_entity, enemy_target, enemy_has_los);
    if (bot_state().active_goal == BotGoalType::none) {
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
    allow_forced_goal_refresh();
    bot_goal_manager_refresh(local_entity, enemy_target, enemy_has_los);
    bot_goal_manager_ensure_active(
        local_entity,
        enemy_target,
        enemy_has_los,
        fallback_eval_delay_ms
    );
}
