#include "bot_decision_eval.h"

#include "bot_internal.h"
#include "bot_utils.h"
#include "../../rf/player/player.h"
#include "../../rf/weapon.h"
#include <algorithm>
#include <cmath>

namespace
{
bool weapon_has_readiness_ammo(const rf::Entity& entity, const int weapon_type)
{
    if (weapon_type < 0
        || weapon_type >= rf::num_weapon_types
        || !entity.ai.has_weapon[weapon_type]
        || rf::weapon_is_detonator(weapon_type)) {
        return false;
    }
    if (rf::weapon_is_melee(weapon_type)) {
        return true;
    }
    if (entity.ai.clip_ammo[weapon_type] > 0) {
        return true;
    }
    if (rf::local_player) {
        return rf::player_get_weapon_total_ammo(rf::local_player, weapon_type) > 0;
    }
    return false;
}

// resolve_weapon_type_cached and entity_has_weapon_type are defined in bot_utils.cpp
}

float bot_decision_get_entity_health_ratio(const rf::Entity& entity)
{
    return std::clamp(entity.life / kBotNominalMaxHealth, 0.0f, 1.0f);
}

float bot_decision_get_entity_armor_ratio(const rf::Entity& entity)
{
    return std::clamp(entity.armor / kBotNominalMaxArmor, 0.0f, 1.0f);
}

float bot_decision_apply_aggression_to_threshold(
    const float threshold,
    const float aggression_bias)
{
    const float adjusted = threshold / std::sqrt(std::max(aggression_bias, 0.1f));
    return std::clamp(adjusted, 0.05f, 0.95f);
}

BotWeaponRangeBand bot_decision_classify_combat_distance_band(const float distance)
{
    if (distance <= 2.5f) {
        return BotWeaponRangeBand::melee;
    }
    if (distance <= 10.0f) {
        return BotWeaponRangeBand::close;
    }
    if (distance <= 28.0f) {
        return BotWeaponRangeBand::medium;
    }
    if (distance <= 55.0f) {
        return BotWeaponRangeBand::long_range;
    }
    return BotWeaponRangeBand::very_long;
}

float bot_decision_compute_weapon_readiness_score_for_type(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target,
    const int weapon_type)
{
    if (!weapon_has_readiness_ammo(local_entity, weapon_type)) {
        return 0.0f;
    }

    const BotPersonality& personality = get_active_bot_personality();
    float desired_distance = std::max(
        1.0f,
        0.5f * (personality.preferred_engagement_near + personality.preferred_engagement_far));
    if (enemy_target) {
        desired_distance = std::sqrt(std::max(
            rf::vec_dist_squared(&local_entity.pos, &enemy_target->pos),
            0.0f));
    }

    const BotWeaponRangeBand desired_band =
        bot_decision_classify_combat_distance_band(desired_distance);
    float score = 0.25f;
    if (const BotWeaponProfile* profile = bot_weapon_profile_for_weapon_type(weapon_type)) {
        const float tier_norm = std::clamp(
            static_cast<float>(profile->value_tier) / 5.0f,
            0.0f,
            1.0f);
        float range_fit = bot_weapon_profile_supports_range(*profile, desired_band)
            ? 1.0f
            : 0.18f;
        if ((desired_band == BotWeaponRangeBand::long_range
                || desired_band == BotWeaponRangeBand::very_long)
            && bot_weapon_profile_has_special(
                *profile,
                BotWeaponSpecialProperty::use_scope_for_long_range)) {
            range_fit = std::min(range_fit + 0.16f, 1.0f);
        }
        score += tier_norm * 0.48f + range_fit * 0.34f;
    }
    else {
        const rf::WeaponInfo& weapon_info = rf::weapon_types[weapon_type];
        const float weapon_range = std::max(
            weapon_info.ai_max_range_multi,
            weapon_info.ai_max_range);
        const float range_fit = weapon_range > 0.0f
            ? std::clamp(weapon_range / std::max(desired_distance, 1.0f), 0.18f, 1.0f)
            : 0.5f;
        score += range_fit * 0.40f;
    }

    const float preference_norm = std::clamp(
        (bot_get_weapon_preference_weight(weapon_type) - 0.25f) / 2.25f,
        0.0f,
        1.0f);
    const float weapon_skill_norm = std::clamp(
        (bot_get_weapon_skill_weight(weapon_type) - 0.25f) / 2.25f,
        0.0f,
        1.0f);
    score += preference_norm * 0.10f;
    score += weapon_skill_norm * 0.08f;

    if (rf::weapon_is_melee(weapon_type) && desired_distance > 3.5f) {
        score *= 0.30f;
    }

    return std::clamp(score, 0.0f, 1.0f);
}

float bot_decision_compute_weapon_readiness_score(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target)
{
    const int enemy_handle = enemy_target ? enemy_target->handle : -1;
    if (g_client_bot_state.weapon_readiness_cached
        && g_client_bot_state.weapon_readiness_cache_enemy_handle == enemy_handle) {
        return g_client_bot_state.weapon_readiness_cache_score;
    }

    float best_score = 0.0f;
    for (int weapon_type = 0; weapon_type < rf::num_weapon_types; ++weapon_type) {
        const float score = bot_decision_compute_weapon_readiness_score_for_type(
            local_entity,
            enemy_target,
            weapon_type);
        best_score = std::max(best_score, score);
    }
    const float result = std::clamp(best_score, 0.0f, 1.0f);
    g_client_bot_state.weapon_readiness_cached = true;
    g_client_bot_state.weapon_readiness_cache_score = result;
    g_client_bot_state.weapon_readiness_cache_enemy_handle = enemy_handle;
    return result;
}

float bot_decision_compute_combat_readiness_threshold()
{
    const BotPersonality& personality = get_active_bot_personality();
    const float base_threshold = std::clamp(personality.combat_readiness_threshold, 0.20f, 0.95f);
    const float risk_tolerance = std::clamp(personality.decision_risk_tolerance, 0.25f, 2.5f);
    const float risk_norm = std::clamp((risk_tolerance - 0.25f) / 2.25f, 0.0f, 1.0f);
    const float raw_aggression = std::clamp(personality.raw_aggression_bias, 0.25f, 2.5f);
    const float raw_aggression_norm = std::clamp((raw_aggression - 0.25f) / 2.25f, 0.0f, 1.0f);
    float threshold = base_threshold;
    threshold *= std::lerp(1.12f, 0.88f, risk_norm);
    threshold *= std::lerp(1.08f, 0.90f, raw_aggression_norm);
    return std::clamp(threshold, 0.18f, 0.95f);
}

float bot_decision_compute_combat_readiness(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target)
{
    const BotPersonality& personality = get_active_bot_personality();
    const float health_ratio = bot_decision_get_entity_health_ratio(local_entity);
    const float armor_ratio = bot_decision_get_entity_armor_ratio(local_entity);
    const float survivability = std::clamp(health_ratio * 0.72f + armor_ratio * 0.28f, 0.0f, 1.0f);
    const float retreat_health_floor = std::clamp(personality.retreat_health_threshold, 0.05f, 0.90f);
    const float retreat_armor_floor = std::clamp(personality.retreat_armor_threshold, 0.05f, 0.90f);
    const float health_stability = std::clamp(
        (health_ratio - retreat_health_floor) / std::max(1.0f - retreat_health_floor, 0.05f),
        0.0f,
        1.0f);
    const float armor_stability = std::clamp(
        (armor_ratio - retreat_armor_floor) / std::max(1.0f - retreat_armor_floor, 0.05f),
        0.0f,
        1.0f);
    const float survivability_score = std::clamp(
        survivability * 0.55f + health_stability * 0.30f + armor_stability * 0.15f,
        0.0f,
        1.0f);
    const float weapon_score = bot_decision_compute_weapon_readiness_score(local_entity, enemy_target);
    float readiness = survivability_score * 0.56f + weapon_score * 0.44f;

    const float risk_tolerance = std::clamp(personality.decision_risk_tolerance, 0.25f, 2.5f);
    const float risk_norm = std::clamp((risk_tolerance - 0.25f) / 2.25f, 0.0f, 1.0f);
    const float raw_aggression = std::clamp(personality.raw_aggression_bias, 0.25f, 2.5f);
    const float raw_aggression_norm = std::clamp((raw_aggression - 0.25f) / 2.25f, 0.0f, 1.0f);
    const float decision_skill = bot_get_decision_skill_factor();
    readiness += std::lerp(-0.08f, 0.12f, risk_norm);
    readiness += std::lerp(-0.05f, 0.10f, raw_aggression_norm);
    readiness *= std::lerp(0.94f, 1.08f, decision_skill);

    return std::clamp(readiness, 0.0f, 1.0f);
}

bool bot_decision_is_combat_ready(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target)
{
    const float readiness = bot_decision_compute_combat_readiness(local_entity, enemy_target);
    const float threshold = bot_decision_compute_combat_readiness_threshold();

    // Hysteresis: once not combat-ready, require readiness to exceed threshold by a margin
    // before re-entering combat. This prevents oscillation at the boundary.
    constexpr float kCombatReadinessHysteresisMargin = 0.06f;
    const bool was_combat_ready =
        g_client_bot_state.fsm_state == BotFsmState::engage_enemy
        || g_client_bot_state.fsm_state == BotFsmState::pursue_enemy
        || g_client_bot_state.fsm_state == BotFsmState::find_power_position;
    if (was_combat_ready) {
        return readiness >= threshold;
    }
    return readiness >= (threshold + kCombatReadinessHysteresisMargin);
}

float bot_decision_compute_enemy_goal_score(
    const rf::Entity& local_entity,
    const rf::Entity& enemy_target,
    const bool enemy_has_los)
{
    const BotPersonality& personality = get_active_bot_personality();
    const float decision_skill = bot_get_decision_skill_factor();
    const float enemy_dist = std::sqrt(
        std::max(rf::vec_dist_squared(&local_entity.pos, &enemy_target.pos), 0.0f));
    float score = kGoalEnemyBaseScore - enemy_dist * kGoalEnemyDistancePenalty;
    if (enemy_has_los) {
        score += kGoalEnemyVisibleBonus;
    }

    const float aggression = std::clamp(personality.decision_aggression_bias, 0.25f, 2.5f);
    const float aggression_norm = std::clamp((aggression - 0.25f) / 2.25f, 0.0f, 1.0f);
    const float health_ratio = bot_decision_get_entity_health_ratio(local_entity);
    const float armor_ratio = bot_decision_get_entity_armor_ratio(local_entity);
    const float retreat_health = bot_decision_apply_aggression_to_threshold(
        personality.retreat_health_threshold,
        aggression);
    const float retreat_armor = bot_decision_apply_aggression_to_threshold(
        personality.retreat_armor_threshold,
        aggression);
    const float survivability_signal = std::clamp(
        (health_ratio - retreat_health) * 0.72f + (armor_ratio - retreat_armor) * 0.28f,
        -1.0f,
        1.0f);
    score += survivability_signal * std::lerp(20.0f, 65.0f, decision_skill);
    score += std::lerp(-14.0f, 18.0f, aggression_norm);

    const float raw_aggression = std::clamp(personality.raw_aggression_bias, 0.25f, 2.5f);
    score += std::lerp(-20.0f, 42.0f, std::clamp((raw_aggression - 0.25f) / 2.25f, 0.0f, 1.0f));

    const float easy_frag_bias = std::clamp(personality.easy_frag_bias, 0.25f, 2.5f);
    const float enemy_health_ratio = bot_decision_get_entity_health_ratio(enemy_target);
    const float enemy_armor_ratio = bot_decision_get_entity_armor_ratio(enemy_target);
    const float enemy_survivability = enemy_health_ratio * 0.70f + enemy_armor_ratio * 0.30f;
    const float easy_frag_signal = std::clamp(1.0f - enemy_survivability, 0.0f, 1.0f);
    score += easy_frag_signal
        * std::lerp(0.0f, 55.0f, std::clamp((easy_frag_bias - 0.25f) / 2.25f, 0.0f, 1.0f));

    if (!enemy_has_los
        && bot_personality_has_quirk(BotPersonalityQuirk::railgun_no_los_hunter)) {
        const int railgun_type = bot_resolve_weapon_type_cached("rail_gun");
        if (bot_entity_has_weapon_type(local_entity, railgun_type)) {
            score += 24.0f;
        }
    }

    if (personality.attack_style == BotAttackStyle::aggressive) {
        score += 16.0f;
    }
    else if (personality.attack_style == BotAttackStyle::evasive) {
        score -= 10.0f;
    }

    return score;
}
