#include "bot_memory_manager.h"

#include "bot_alerts.h"
#include "bot_goal_memory.h"

void bot_memory_manager_reset()
{
    bot_alerts_reset();
}

void bot_memory_manager_update_context(
    const rf::Player& local_player,
    const rf::Entity& local_entity)
{
    bot_alerts_update_context(local_player, local_entity);
}

void bot_memory_manager_note_failed_goal_target(
    const BotGoalType goal,
    const int target_handle,
    const int cooldown_ms)
{
    if (!bot_goal_is_item_collection(goal) || target_handle < 0) {
        return;
    }
    bot_goal_memory_register_failed_item_goal_cooldown(target_handle, cooldown_ms);
}
