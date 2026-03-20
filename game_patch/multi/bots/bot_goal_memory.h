#pragma once

#include "bot_internal.h"

void bot_goal_memory_prune_failed_item_goal_cooldowns();
void bot_goal_memory_register_failed_item_goal_cooldown(int item_handle, int cooldown_ms);
bool bot_goal_memory_is_failed_item_goal_cooldown_active_no_prune(int item_handle);

void bot_goal_memory_note_item_goal_selection(int item_handle, BotGoalType goal_type);
float bot_goal_memory_get_recent_item_goal_penalty(int item_handle, BotGoalType goal_type);
float bot_goal_memory_get_secondary_goal_repeat_penalty(BotGoalType goal_type);
