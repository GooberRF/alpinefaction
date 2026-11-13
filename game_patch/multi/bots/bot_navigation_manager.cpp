#include "bot_navigation_manager.h"

#include "bot_navigation.h"

bool bot_navigation_manager_sync_pursuit_target(rf::Entity* enemy_target)
{
    return bot_sync_pursuit_target(enemy_target);
}

void bot_navigation_manager_update_pursuit_recovery_timer()
{
    bot_update_pursuit_recovery_timer();
}

void bot_navigation_manager_update_move_target(
    const rf::Entity& local_entity,
    rf::Entity* enemy_target,
    const bool enemy_has_los,
    const bool pursuing_enemy_goal,
    rf::Vector3& move_target,
    bool& has_move_target)
{
    bot_update_move_target(
        local_entity,
        enemy_target,
        enemy_has_los,
        pursuing_enemy_goal,
        move_target,
        has_move_target
    );
}
