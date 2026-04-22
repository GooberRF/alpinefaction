#include "bot_perception_manager.h"

#include "bot_alerts.h"
#include "bot_decision_eval.h"
#include "bot_internal.h"
#include "bot_math.h"
#include "bot_personality.h"
#include "bot_personality_manager.h"
#include "bot_utils.h"
#include "../gametype.h"
#include "../../rf/collide.h"
#include "../../rf/multi.h"
#include "../../rf/weapon.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
bool has_visibility_to_target_position(
    const rf::Vector3& origin,
    const rf::Vector3& target_pos,
    const rf::Vector3& target_eye_pos,
    const rf::Object* ignore1,
    const rf::Object* ignore2)
{
    if (bot_has_unobstructed_level_los(origin, target_eye_pos, ignore1, ignore2)) {
        return true;
    }

    rf::Vector3 chest_pos = target_pos;
    chest_pos.y = std::lerp(target_pos.y, target_eye_pos.y, 0.6f);
    return bot_has_unobstructed_level_los(origin, chest_pos, ignore1, ignore2);
}

bool has_visibility_to_target_entity(
    const rf::Entity& viewer,
    const rf::Entity& target)
{
    return has_visibility_to_target_position(
        viewer.eye_pos,
        target.pos,
        target.eye_pos,
        &viewer,
        &target
    );
}

// resolve_weapon_type_cached and entity_has_weapon_type are defined in bot_utils.cpp
}

void bot_perception_manager_reset_tracking()
{
    g_client_bot_state.los_check_timer.invalidate();
    g_client_bot_state.los_target_handle = -1;
    g_client_bot_state.los_to_enemy = false;
    g_client_bot_state.preferred_enemy_handle = -1;
    g_client_bot_state.preferred_enemy_lock_timer.invalidate();
}

bool bot_perception_manager_get_enemy_los_cached(
    const rf::Entity& local_entity,
    const rf::Entity& enemy_entity)
{
    const int enemy_handle = enemy_entity.handle;
    if (g_client_bot_state.los_target_handle != enemy_handle) {
        g_client_bot_state.los_target_handle = enemy_handle;
        g_client_bot_state.los_check_timer.invalidate();
    }

    if (!g_client_bot_state.los_check_timer.valid()
        || g_client_bot_state.los_check_timer.elapsed()) {
        g_client_bot_state.los_to_enemy = has_visibility_to_target_entity(
            local_entity,
            enemy_entity
        );
        const float alertness = std::clamp(
            bot_personality_manager_get_active_skill_profile().alertness,
            0.25f,
            2.5f
        );
        const float alertness_norm = std::clamp((alertness - 0.25f) / 2.25f, 0.0f, 1.0f);
        const int visible_interval = static_cast<int>(std::lround(
            std::lerp(300.0f, 110.0f, alertness_norm)
        ));
        const int blocked_interval = static_cast<int>(std::lround(
            std::lerp(130.0f, 55.0f, alertness_norm)
        ));
        g_client_bot_state.los_check_timer.set(
            g_client_bot_state.los_to_enemy
                ? visible_interval
                : blocked_interval
        );
    }

    return g_client_bot_state.los_to_enemy;
}

rf::Entity* bot_perception_manager_select_enemy_target(
    const rf::Player& local_player,
    const rf::Entity& local_entity)
{
    const bool team_mode = multi_is_team_game_type();
    const BotPersonality& personality = bot_personality_manager_get_active_personality();
    const BotSkillProfile& skill_profile = bot_personality_manager_get_active_skill_profile();
    const float alertness = std::clamp(skill_profile.alertness, 0.25f, 2.5f);
    const float alertness_norm = std::clamp((alertness - 0.25f) / 2.25f, 0.0f, 1.0f);
    const float focus_bias = std::clamp(skill_profile.target_focus_bias, 0.25f, 2.5f);
    const float focus_norm = std::clamp((focus_bias - 0.25f) / 2.25f, 0.0f, 1.0f);
    const float eliminate_commitment = std::clamp(
        personality.eliminate_target_commitment_bias,
        0.25f,
        2.5f
    );
    const float eliminate_commitment_norm = std::clamp(
        (eliminate_commitment - 0.25f) / 2.25f,
        0.0f,
        1.0f
    );
    const float retaliation_bias = std::clamp(personality.retaliation_bias, 0.25f, 2.5f);
    const float retaliation_norm = std::clamp((retaliation_bias - 0.25f) / 2.25f, 0.0f, 1.0f);
    const float adjusted_fov_deg = std::clamp(
        skill_profile.fov_degrees * std::lerp(0.85f, 1.20f, alertness_norm),
        50.0f,
        220.0f
    );
    constexpr float kDegreesToRadians = 0.017453292519943295769f;
    const float min_view_dot = std::cos(adjusted_fov_deg * 0.5f * kDegreesToRadians);
    const float close_detect_dist = std::lerp(5.0f, 13.0f, alertness_norm);
    const float close_detect_dist_sq = close_detect_dist * close_detect_dist;
    rf::Vector3 view_forward = forward_from_non_linear_yaw_pitch(
        local_entity.control_data.phb.y,
        local_entity.control_data.eye_phb.x
    );
    view_forward.normalize_safe();

    if (g_client_bot_state.preferred_enemy_lock_timer.valid()
        && g_client_bot_state.preferred_enemy_lock_timer.elapsed()) {
        g_client_bot_state.preferred_enemy_lock_timer.invalidate();
    }
    const bool has_locked_target =
        g_client_bot_state.preferred_enemy_handle >= 0
        && g_client_bot_state.preferred_enemy_lock_timer.valid();
    if (g_client_bot_state.retaliation_timer.valid()
        && g_client_bot_state.retaliation_timer.elapsed()) {
        g_client_bot_state.retaliation_timer.invalidate();
        g_client_bot_state.retaliation_target_handle = -1;
    }
    const bool retaliation_active =
        g_client_bot_state.retaliation_target_handle >= 0
        && g_client_bot_state.retaliation_timer.valid();

    rf::Entity* best_target = nullptr;
    float best_score = std::numeric_limits<float>::max();
    rf::Entity* locked_target_candidate = nullptr;
    float locked_target_score = std::numeric_limits<float>::max();

    for (const rf::Player& candidate : SinglyLinkedList{rf::player_list}) {
        if (&candidate == &local_player) {
            continue;
        }

        if (candidate.is_browser || candidate.is_spectator || candidate.is_spawn_disabled) {
            continue;
        }

        if (team_mode && candidate.team == local_player.team) {
            continue;
        }

        if (rf::player_is_dead(&candidate) || rf::player_is_dying(&candidate)) {
            continue;
        }

        rf::Entity* candidate_entity = rf::entity_from_handle(candidate.entity_handle);
        if (!candidate_entity) {
            continue;
        }

        const float dist_sq = rf::vec_dist_squared(&local_entity.pos, &candidate_entity->pos);
        rf::Vector3 to_candidate = candidate_entity->eye_pos - local_entity.eye_pos;
        if (to_candidate.len_sq() < 0.001f) {
            to_candidate = candidate_entity->pos - local_entity.pos;
        }
        if (to_candidate.len_sq() < 0.001f) {
            continue;
        }
        to_candidate.normalize_safe();

        const float facing_dot = std::clamp(view_forward.dot_prod(to_candidate), -1.0f, 1.0f);
        const bool inside_fov = facing_dot >= min_view_dot;
        const bool in_close_awareness_range = dist_sq <= close_detect_dist_sq;
        float alert_awareness_weight = 0.0f;
        BotAlertType alert_type = BotAlertType::none;
        const bool has_alert_awareness = bot_alerts_get_contact_awareness(
            candidate_entity->handle,
            alert_awareness_weight,
            &alert_type);
        if (!inside_fov && !in_close_awareness_range && !has_alert_awareness) {
            continue;
        }

        float score = dist_sq;
        score += (1.0f - facing_dot) * std::lerp(250.0f, 70.0f, alertness_norm);
        if (!inside_fov) {
            float outside_fov_penalty = std::lerp(120.0f, 40.0f, alertness_norm);
            if (has_alert_awareness) {
                outside_fov_penalty *= std::lerp(
                    1.0f,
                    0.20f,
                    std::clamp(alert_awareness_weight, 0.0f, 1.0f));
            }
            score += outside_fov_penalty;
        }

        if (has_alert_awareness) {
            const float awareness_norm = std::clamp(alert_awareness_weight, 0.0f, 1.0f);
            float awareness_scale = std::lerp(1.0f, 0.68f, awareness_norm);
            switch (alert_type) {
                case BotAlertType::damaged_by_enemy:
                    awareness_scale *= std::lerp(1.0f, 0.52f, retaliation_norm);
                    break;
                case BotAlertType::nearby_weapon_fire:
                    awareness_scale *= std::lerp(1.0f, 0.72f, awareness_norm);
                    break;
                case BotAlertType::nearby_weapon_reload:
                    awareness_scale *= std::lerp(1.0f, 0.76f, awareness_norm);
                    break;
                default:
                    break;
            }
            score *= awareness_scale;
        }

        const float enemy_health_ratio = bot_decision_get_entity_health_ratio(*candidate_entity);
        const float enemy_armor_ratio = bot_decision_get_entity_armor_ratio(*candidate_entity);
        const float enemy_survivability = enemy_health_ratio * 0.70f + enemy_armor_ratio * 0.30f;
        const float easy_frag_bias = std::clamp(personality.easy_frag_bias, 0.25f, 2.5f);
        const float easy_frag_norm = std::clamp((easy_frag_bias - 0.25f) / 2.25f, 0.0f, 1.0f);
        score *= std::lerp(
            1.0f,
            std::clamp(1.10f - (1.0f - enemy_survivability) * 0.55f, 0.55f, 1.10f),
            easy_frag_norm
        );

        if (bot_personality_has_quirk(BotPersonalityQuirk::spawn_hunter)) {
            const bool spawn_protection_active =
                candidate.spawn_protection_timestamp.valid()
                && !candidate.spawn_protection_timestamp.elapsed();
            if (spawn_protection_active) {
                score *= 0.62f;
            }
        }

        const float enemy_dist = std::sqrt(std::max(dist_sq, 0.0f));
        if (bot_personality_has_quirk(BotPersonalityQuirk::shotgun_low_health_finisher)
            && enemy_survivability <= 0.42f
            && enemy_dist <= 18.0f) {
            const int shotgun_type = bot_resolve_weapon_type_cached("Shotgun");
            if (bot_entity_has_weapon_type(local_entity, shotgun_type)) {
                score *= 0.70f;
            }
        }
        if (bot_personality_has_quirk(BotPersonalityQuirk::melee_finisher)
            && enemy_survivability <= 0.30f
            && enemy_dist <= 3.2f) {
            const int riot_stick_type = bot_resolve_weapon_type_cached("Riot Stick");
            if (bot_entity_has_weapon_type(local_entity, riot_stick_type)) {
                score *= 0.66f;
            }
        }

        if (retaliation_active) {
            if (candidate_entity->handle == g_client_bot_state.retaliation_target_handle) {
                score *= std::lerp(1.0f, 0.46f, retaliation_norm);
            }
            else {
                score *= std::lerp(1.0f, 1.12f, retaliation_norm);
            }
        }

        if (has_locked_target) {
            if (candidate_entity->handle == g_client_bot_state.preferred_enemy_handle) {
                score *= std::lerp(0.90f, 0.62f, focus_norm);
            }
            else {
                score *= std::lerp(1.05f, 1.25f, focus_norm);
            }
        }

        if (g_client_bot_state.active_goal == BotGoalType::eliminate_target
            && candidate_entity->handle == g_client_bot_state.goal_target_handle) {
            score *= std::lerp(0.88f, 0.52f, eliminate_commitment_norm);
        }
        else if (g_client_bot_state.active_goal == BotGoalType::eliminate_target
            && g_client_bot_state.goal_target_handle >= 0) {
            score *= std::lerp(1.0f, 1.22f, eliminate_commitment_norm);
        }

        if (score < best_score) {
            best_score = score;
            best_target = candidate_entity;
        }
        if (has_locked_target
            && candidate_entity->handle == g_client_bot_state.preferred_enemy_handle) {
            locked_target_candidate = candidate_entity;
            locked_target_score = score;
        }
    }

    if (has_locked_target
        && locked_target_candidate
        && best_target
        && best_target->handle != g_client_bot_state.preferred_enemy_handle) {
        // If alternatives are similarly appealing, keep the locked target to avoid rapid
        // second-guessing between equivalent choices.
        float lock_switch_margin = std::lerp(60.0f, 220.0f, focus_norm);
        if (g_client_bot_state.active_goal == BotGoalType::eliminate_target) {
            lock_switch_margin *= std::lerp(1.10f, 2.20f, eliminate_commitment_norm);
        }
        if (g_client_bot_state.fsm_state == BotFsmState::recover_navigation
            || g_client_bot_state.recovery_pending_reroute) {
            lock_switch_margin *= 1.75f;
        }
        if (locked_target_score <= best_score + lock_switch_margin) {
            best_target = locked_target_candidate;
            best_score = locked_target_score;
        }
    }

    if (best_target) {
        if (best_target->handle != g_client_bot_state.preferred_enemy_handle
            || !g_client_bot_state.preferred_enemy_lock_timer.valid()) {
            g_client_bot_state.preferred_enemy_handle = best_target->handle;
            float lock_duration = std::lerp(
                350.0f,
                1700.0f,
                std::clamp((alertness_norm + focus_norm) * 0.5f, 0.0f, 1.0f)
            );
            if (g_client_bot_state.active_goal == BotGoalType::eliminate_target) {
                lock_duration *= std::lerp(1.0f, 1.75f, eliminate_commitment_norm);
            }
            if (g_client_bot_state.fsm_state == BotFsmState::recover_navigation
                || g_client_bot_state.recovery_pending_reroute) {
                lock_duration *= std::lerp(1.10f, 1.60f, focus_norm);
            }
            const int lock_ms = static_cast<int>(std::lround(
                std::clamp(lock_duration, 200.0f, 4500.0f)
            ));
            g_client_bot_state.preferred_enemy_lock_timer.set(lock_ms);
        }
    }
    else {
        g_client_bot_state.preferred_enemy_handle = -1;
        g_client_bot_state.preferred_enemy_lock_timer.invalidate();
    }

    return best_target;
}
