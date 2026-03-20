#pragma once

#include "bot_internal.h"
#include "../../rf/player/player.h"

void bot_memory_manager_reset();
void bot_memory_manager_update_context(
    const rf::Player& local_player,
    const rf::Entity& local_entity);
void bot_memory_manager_note_failed_goal_target(
    BotGoalType goal,
    int target_handle,
    int cooldown_ms = kFailedItemGoalCooldownMs);
