#pragma once

#include "bot_weapon_profiles.h"
#include "../../rf/entity.h"

float bot_decision_get_entity_health_ratio(const rf::Entity& entity);
float bot_decision_get_entity_armor_ratio(const rf::Entity& entity);
float bot_decision_apply_aggression_to_threshold(float threshold, float aggression_bias);

BotWeaponRangeBand bot_decision_classify_combat_distance_band(float distance);
float bot_decision_compute_weapon_readiness_score_for_type(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target,
    int weapon_type);
float bot_decision_compute_weapon_readiness_score(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target);

float bot_decision_compute_combat_readiness_threshold();
float bot_decision_compute_combat_readiness(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target);
bool bot_decision_is_combat_ready(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target);

float bot_decision_compute_enemy_goal_score(
    const rf::Entity& local_entity,
    const rf::Entity& enemy_target,
    bool enemy_has_los);
