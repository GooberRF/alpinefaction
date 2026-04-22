#pragma once

#include "../../rf/entity.h"

bool bot_sync_pursuit_target(rf::Entity* enemy_target);
void bot_update_pursuit_recovery_timer();
void bot_update_move_target(
    const rf::Entity& local_entity,
    rf::Entity* enemy_target,
    bool enemy_has_los,
    bool pursuing_enemy_goal,
    rf::Vector3& move_target,
    bool& has_move_target);
