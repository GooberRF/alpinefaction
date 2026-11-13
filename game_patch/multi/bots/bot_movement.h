#pragma once

#include "../../rf/entity.h"
#include "../../rf/player/player.h"

void bot_process_movement(
    rf::Player& local_player,
    const rf::Entity& local_entity,
    const rf::Vector3& move_target,
    bool pursuing_enemy_goal,
    bool enemy_has_los,
    float skill_factor);
