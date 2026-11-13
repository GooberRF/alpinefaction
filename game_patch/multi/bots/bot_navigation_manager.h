#pragma once

#include "../../rf/entity.h"

bool bot_navigation_manager_sync_pursuit_target(rf::Entity* enemy_target);
void bot_navigation_manager_update_pursuit_recovery_timer();
void bot_navigation_manager_update_move_target(
    const rf::Entity& local_entity,
    rf::Entity* enemy_target,
    bool enemy_has_los,
    bool pursuing_enemy_goal,
    rf::Vector3& move_target,
    bool& has_move_target);
