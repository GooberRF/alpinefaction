#pragma once

#include "bot_internal.h"
#include "../../rf/entity.h"
#include "../../rf/player/player.h"

BotFsmState bot_fsm_manager_select_state(
    const rf::Player& local_player,
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target,
    bool pursuing_enemy_goal,
    bool enemy_has_los,
    bool has_move_target);

void bot_fsm_manager_transition_state(
    BotFsmState new_state,
    int transition_timer_ms = 300);
