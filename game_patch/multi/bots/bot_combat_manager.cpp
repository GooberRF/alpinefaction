#include "bot_combat_manager.h"

#include "bot_combat.h"

void bot_combat_manager_prepare_frame(
    rf::Player& local_player,
    rf::Entity& local_entity)
{
    ensure_weapon_ready(local_player, local_entity);
}

void bot_combat_manager_process_frame(
    rf::Player& local_player,
    rf::Entity& local_entity,
    rf::Entity* enemy_target,
    const bool enemy_has_los,
    const float skill_factor,
    const bool has_move_target,
    const rf::Vector3& move_target)
{
    bot_process_combat(
        local_player,
        local_entity,
        enemy_target,
        enemy_has_los,
        skill_factor,
        has_move_target,
        move_target
    );
}
