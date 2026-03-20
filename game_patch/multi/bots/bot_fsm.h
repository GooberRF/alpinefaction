#pragma once

#include "bot_internal.h"

bool bot_fsm_state_should_have_move_target(
    BotFsmState fsm_state,
    bool pursuing_enemy_goal,
    bool enemy_has_los);

bool bot_fsm_is_contextual_item_state(BotFsmState fsm_state);

void bot_fsm_update_state(BotFsmState new_state, int transition_timer_ms = 300);

const char* bot_fsm_state_to_string(BotFsmState state);
const char* bot_goal_type_to_string(BotGoalType goal);
