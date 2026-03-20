#pragma once

#include "../../rf/entity.h"
#include "../../rf/player/player.h"

void clear_enemy_aim_error_state();
void clear_semi_auto_click_state();
void ensure_weapon_ready(rf::Player& player, rf::Entity& entity);
bool bot_has_usable_crater_weapon(rf::Player& player, rf::Entity& entity);
bool bot_has_usable_shatter_weapon(rf::Player& player, rf::Entity& entity);
void bot_process_combat(
    rf::Player& local_player,
    rf::Entity& local_entity,
    rf::Entity* enemy_target,
    bool enemy_has_los,
    float skill_factor,
    bool has_move_target,
    const rf::Vector3& move_target);
