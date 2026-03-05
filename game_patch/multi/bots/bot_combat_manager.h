#pragma once

#include "../../rf/entity.h"
#include "../../rf/player/player.h"

void bot_combat_manager_prepare_frame(
    rf::Player& local_player,
    rf::Entity& local_entity);

void bot_combat_manager_process_frame(
    rf::Player& local_player,
    rf::Entity& local_entity,
    rf::Entity* enemy_target,
    bool enemy_has_los,
    float skill_factor,
    bool has_move_target,
    const rf::Vector3& move_target);
