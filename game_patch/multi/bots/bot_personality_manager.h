#pragma once

#include "bot_personality.h"
#include "../../rf/entity.h"
#include "../../rf/player/player.h"

const BotPersonality& bot_personality_manager_get_active_personality();
const BotSkillProfile& bot_personality_manager_get_active_skill_profile();

bool bot_personality_manager_is_deathmatch_mode();
bool bot_personality_manager_is_ctf_mode();
bool bot_personality_manager_is_control_point_mode();

float bot_personality_manager_compute_combat_readiness_threshold();
float bot_personality_manager_compute_combat_readiness(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target);
bool bot_personality_manager_is_combat_ready(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target);

bool bot_personality_manager_should_seek_weapon_state(
    const rf::Player& local_player,
    const rf::Entity& local_entity);
bool bot_personality_manager_should_replenish_state(const rf::Entity& local_entity);
bool bot_personality_manager_should_retreat_state(
    const rf::Entity& local_entity,
    bool enemy_has_los);
