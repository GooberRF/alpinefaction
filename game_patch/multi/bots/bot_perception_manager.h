#pragma once

#include "../../rf/entity.h"
#include "../../rf/player/player.h"

void bot_perception_manager_reset_tracking();
void bot_perception_manager_reset_spawn_grace();
// Runs every frame the bot has a player, alive or not, so spawns are seen while it waits to respawn.
void bot_perception_manager_update_spawn_grace(const rf::Player& local_player);

rf::Entity* bot_perception_manager_select_enemy_target(
    const rf::Player& local_player,
    const rf::Entity& local_entity);

bool bot_perception_manager_get_enemy_los_cached(
    const rf::Entity& local_entity,
    const rf::Entity& enemy_entity);
