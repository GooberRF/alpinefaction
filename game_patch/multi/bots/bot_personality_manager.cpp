#include "bot_personality_manager.h"

#include "bot_internal.h"
#include "bot_decision_eval.h"
#include "bot_weapon_profiles.h"
#include "../gametype.h"
#include "../../rf/multi.h"
#include "../../rf/weapon.h"
#include <algorithm>
#include <cmath>

namespace
{
float get_survivability_maintenance_factor()
{
    const BotSkillProfile& skill_profile = get_active_bot_skill_profile();
    const float skill = bot_get_decision_skill_factor();
    const float maintenance_bias = std::clamp(
        skill_profile.survivability_maintenance_bias,
        0.25f,
        2.5f
    );
    return std::clamp(
        maintenance_bias * std::lerp(0.80f, 1.35f, skill),
        0.35f,
        2.75f
    );
}

bool has_usable_ranged_weapon(const rf::Player& player, const rf::Entity& entity)
{
    for (int weapon_type = 0; weapon_type < rf::num_weapon_types; ++weapon_type) {
        if (!entity.ai.has_weapon[weapon_type]) {
            continue;
        }
        if (rf::weapon_is_detonator(weapon_type) || rf::weapon_is_melee(weapon_type)) {
            continue;
        }
        if (rf::player_get_weapon_total_ammo(const_cast<rf::Player*>(&player), weapon_type) > 0) {
            return true;
        }
    }
    return false;
}

float best_ranged_weapon_value(const rf::Entity& entity)
{
    float best_value = 0.0f;
    for (int weapon_type = 0; weapon_type < rf::num_weapon_types; ++weapon_type) {
        if (!entity.ai.has_weapon[weapon_type]
            || rf::weapon_is_detonator(weapon_type)
            || rf::weapon_is_melee(weapon_type)) {
            continue;
        }

        float score = 1.0f;
        if (const BotWeaponProfile* profile = bot_weapon_profile_for_weapon_type(weapon_type)) {
            score += static_cast<float>(profile->value_tier) * 0.65f;
        }
        score *= bot_get_weapon_preference_weight(weapon_type);
        best_value = std::max(best_value, score);
    }
    return best_value;
}
}

const BotPersonality& bot_personality_manager_get_active_personality()
{
    return get_active_bot_personality();
}

const BotSkillProfile& bot_personality_manager_get_active_skill_profile()
{
    return get_active_bot_skill_profile();
}

bool bot_personality_manager_is_deathmatch_mode()
{
    return rf::is_multi && rf::multi_get_game_type() == rf::NG_TYPE_DM;
}

bool bot_personality_manager_is_ctf_mode()
{
    return rf::is_multi && rf::multi_get_game_type() == rf::NG_TYPE_CTF;
}

bool bot_personality_manager_is_control_point_mode()
{
    return rf::is_multi && multi_is_game_type_with_hills();
}

float bot_personality_manager_compute_combat_readiness_threshold()
{
    return bot_decision_compute_combat_readiness_threshold();
}

float bot_personality_manager_compute_combat_readiness(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target)
{
    return bot_decision_compute_combat_readiness(local_entity, enemy_target);
}

bool bot_personality_manager_is_combat_ready(
    const rf::Entity& local_entity,
    const rf::Entity* enemy_target)
{
    return bot_decision_is_combat_ready(local_entity, enemy_target);
}

bool bot_personality_manager_should_seek_weapon_state(
    const rf::Player& local_player,
    const rf::Entity& local_entity)
{
    if (!has_usable_ranged_weapon(local_player, local_entity)) {
        return true;
    }

    const float weighted_weapon_value = best_ranged_weapon_value(local_entity);
    const float seek_bias = std::clamp(
        get_active_bot_personality().seek_weapon_bias,
        0.25f,
        2.5f
    );
    return weighted_weapon_value < std::lerp(
        0.85f,
        0.45f,
        std::clamp(seek_bias * 0.5f, 0.0f, 1.0f)
    );
}

bool bot_personality_manager_should_replenish_state(const rf::Entity& local_entity)
{
    const BotPersonality& personality = get_active_bot_personality();
    const float health_ratio = bot_decision_get_entity_health_ratio(local_entity);
    const float armor_ratio = bot_decision_get_entity_armor_ratio(local_entity);
    const float aggression = std::clamp(personality.decision_aggression_bias, 0.25f, 2.5f);
    const float health_threshold = bot_decision_apply_aggression_to_threshold(
        personality.replenish_health_threshold,
        aggression
    );
    const float armor_threshold = bot_decision_apply_aggression_to_threshold(
        personality.replenish_armor_threshold,
        aggression
    );
    const float maintenance_factor = get_survivability_maintenance_factor();
    const float adjusted_health_threshold = std::clamp(
        health_threshold * std::lerp(0.85f, 1.30f, std::clamp(maintenance_factor * 0.5f, 0.0f, 1.0f)),
        0.05f,
        0.98f
    );
    const float adjusted_armor_threshold = std::clamp(
        armor_threshold * std::lerp(0.80f, 1.45f, std::clamp(maintenance_factor * 0.5f, 0.0f, 1.0f)),
        0.05f,
        0.98f
    );
    const float maintenance_norm = std::clamp((maintenance_factor - 0.35f) / 2.40f, 0.0f, 1.0f);
    const float armor_topup_threshold = std::lerp(0.40f, 0.72f, maintenance_norm);
    const bool wants_armor_topup = health_ratio > 0.70f && armor_ratio < armor_topup_threshold;
    return health_ratio < adjusted_health_threshold
        || armor_ratio < adjusted_armor_threshold
        || wants_armor_topup;
}

bool bot_personality_manager_should_retreat_state(
    const rf::Entity& local_entity,
    const bool enemy_has_los)
{
    if (!enemy_has_los) {
        return false;
    }

    const BotPersonality& personality = get_active_bot_personality();
    const float health_ratio = bot_decision_get_entity_health_ratio(local_entity);
    const float armor_ratio = bot_decision_get_entity_armor_ratio(local_entity);
    const float aggression = std::clamp(personality.decision_aggression_bias, 0.25f, 2.5f);
    const float health_threshold = bot_decision_apply_aggression_to_threshold(
        personality.retreat_health_threshold,
        aggression
    );
    const float armor_threshold = bot_decision_apply_aggression_to_threshold(
        personality.retreat_armor_threshold,
        aggression
    );

    const float weighted_survivability = health_ratio * 0.7f + armor_ratio * 0.3f;
    float weighted_threshold = health_threshold * 0.7f + armor_threshold * 0.3f;
    const float raw_aggression = std::clamp(personality.raw_aggression_bias, 0.25f, 2.5f);
    weighted_threshold *= std::lerp(
        1.10f,
        0.65f,
        std::clamp((raw_aggression - 0.25f) / 2.25f, 0.0f, 1.0f)
    );
    const float maintenance_factor = get_survivability_maintenance_factor();
    weighted_threshold *= std::lerp(
        0.92f,
        1.25f,
        std::clamp((maintenance_factor - 0.35f) / 2.40f, 0.0f, 1.0f)
    );
    weighted_threshold = std::clamp(weighted_threshold, 0.05f, 0.98f);

    // Hysteresis: once retreating, require survivability to exceed threshold by a margin
    // before exiting retreat. This prevents oscillation at the boundary.
    constexpr float kRetreatHysteresisMargin = 0.06f;
    const bool currently_retreating = g_client_bot_state.fsm_state == BotFsmState::retreat;
    if (currently_retreating) {
        return weighted_survivability < (weighted_threshold + kRetreatHysteresisMargin);
    }
    return weighted_survivability < weighted_threshold;
}
