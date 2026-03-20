#include "bot_alerts.h"

#include "bot_math.h"
#include "../gametype.h"
#include <common/utils/list-utils.h>
#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
constexpr int kAlertFireRecentMs = 450;
constexpr int kAlertReloadRecentMs = 700;
constexpr int kAlertDamageAwarenessMs = 3200;
constexpr int kAlertMediumAwarenessMs = 1800;
constexpr int kAlertRetaliationDamageMs = 2600;
constexpr int kAlertRetaliationMediumMs = 1400;
constexpr int kAlertRetaliationMediumRefreshMs = 1400;
constexpr float kAlertDamageEpsilon = 0.05f;
constexpr float kAlertNearbyFireRange = 20.0f;
constexpr float kAlertNearbyReloadRange = 10.0f;
constexpr float kAlertDamageSourceRange = 55.0f;
constexpr float kAlertDamageSourceNearRange = 12.0f;
constexpr float kAwarenessWeightDamage = 1.0f;
constexpr float kAwarenessWeightNearbyFire = 0.62f;
constexpr float kAwarenessWeightNearbyReload = 0.56f;
constexpr std::size_t kAlertSourceSampleCapacity = 96;
constexpr std::size_t kAlertContactCapacity = 48;

bool alert_type_has_priority(const BotAlertType lhs, const BotAlertType rhs)
{
    const auto priority = [](const BotAlertType type) {
        switch (type) {
            case BotAlertType::damaged_by_enemy:
                return 3;
            case BotAlertType::nearby_weapon_fire:
                return 2;
            case BotAlertType::nearby_weapon_reload:
                return 1;
            default:
                return 0;
        }
    };

    return priority(lhs) >= priority(rhs);
}

bool is_enemy_candidate_player(
    const rf::Player& local_player,
    const rf::Player& candidate,
    const bool team_mode)
{
    if (&candidate == &local_player) {
        return false;
    }
    if (candidate.is_browser || candidate.is_spectator || candidate.is_spawn_disabled) {
        return false;
    }
    if (team_mode && candidate.team == local_player.team) {
        return false;
    }
    if (rf::player_is_dead(&candidate) || rf::player_is_dying(&candidate)) {
        return false;
    }
    return true;
}

void prune_alert_contacts()
{
    auto& contacts = g_client_bot_state.alert_contacts;
    contacts.erase(
        std::remove_if(
            contacts.begin(),
            contacts.end(),
            [](const BotAlertContact& contact) {
                if (contact.entity_handle < 0
                    || contact.awareness_weight <= 0.0f
                    || !contact.awareness_timer.valid()
                    || contact.awareness_timer.elapsed()) {
                    return true;
                }

                rf::Entity* entity = rf::entity_from_handle(contact.entity_handle);
                return !entity || rf::entity_is_dying(entity);
            }),
        contacts.end());
}

void prune_alert_source_samples()
{
    auto& samples = g_client_bot_state.alert_source_samples;
    samples.erase(
        std::remove_if(
            samples.begin(),
            samples.end(),
            [](const BotAlertSourceSample& sample) {
                if (sample.entity_handle < 0) {
                    return true;
                }
                rf::Entity* entity = rf::entity_from_handle(sample.entity_handle);
                return !entity || rf::entity_is_dying(entity);
            }),
        samples.end());
}

BotAlertSourceSample* find_alert_source_sample(const int entity_handle)
{
    for (BotAlertSourceSample& sample : g_client_bot_state.alert_source_samples) {
        if (sample.entity_handle == entity_handle) {
            return &sample;
        }
    }
    return nullptr;
}

BotAlertSourceSample& add_alert_source_sample(const rf::Entity& entity)
{
    auto& samples = g_client_bot_state.alert_source_samples;
    if (samples.size() >= kAlertSourceSampleCapacity) {
        samples.erase(samples.begin());
    }

    BotAlertSourceSample sample{};
    sample.entity_handle = entity.handle;
    sample.last_fired_timestamp_value = entity.last_fired_timestamp.value;
    sample.last_reload_done_timestamp_value = entity.reload_done_timestamp.value;
    sample.was_reloading = rf::entity_is_reloading(const_cast<rf::Entity*>(&entity));
    samples.push_back(sample);
    return samples.back();
}

void register_alert_contact(
    const rf::Entity& entity,
    const BotAlertType type,
    const float awareness_weight,
    const int duration_ms)
{
    if (entity.handle < 0 || awareness_weight <= 0.0f || duration_ms <= 0) {
        return;
    }

    prune_alert_contacts();
    auto& contacts = g_client_bot_state.alert_contacts;
    for (BotAlertContact& contact : contacts) {
        if (contact.entity_handle != entity.handle) {
            continue;
        }

        contact.last_known_pos = entity.pos;
        contact.awareness_weight = std::max(contact.awareness_weight, awareness_weight);
        if (alert_type_has_priority(type, contact.type)) {
            contact.type = type;
        }
        if (contact.awareness_timer.valid() && !contact.awareness_timer.elapsed()) {
            const int extended_ms = std::max(contact.awareness_timer.time_until(), duration_ms);
            contact.awareness_timer.set(extended_ms);
        }
        else {
            contact.awareness_timer.set(duration_ms);
        }
        return;
    }

    if (contacts.size() >= kAlertContactCapacity) {
        contacts.erase(contacts.begin());
    }

    BotAlertContact contact{};
    contact.entity_handle = entity.handle;
    contact.last_known_pos = entity.pos;
    contact.awareness_weight = awareness_weight;
    contact.type = type;
    contact.awareness_timer.set(duration_ms);
    contacts.push_back(contact);
}

float compute_medium_alert_score(
    const float dist_sq,
    const float max_range_sq,
    const int event_age_ms,
    const int max_age_ms)
{
    const float dist_norm = std::clamp(
        1.0f - std::sqrt(std::max(dist_sq, 0.0f)) / std::sqrt(std::max(max_range_sq, 0.0001f)),
        0.0f,
        1.0f);
    const float recency_norm = std::clamp(
        1.0f - static_cast<float>(std::max(event_age_ms, 0))
            / static_cast<float>(std::max(max_age_ms, 1)),
        0.0f,
        1.0f);
    return dist_norm * 1.4f + recency_norm * 1.8f;
}

float compute_damage_source_score(
    const rf::Entity& candidate_entity,
    const rf::Entity& local_entity,
    const float dist_sq)
{
    if (!candidate_entity.last_fired_timestamp.valid()) {
        return -std::numeric_limits<float>::infinity();
    }

    const int fire_age_ms = candidate_entity.last_fired_timestamp.time_since();
    if (fire_age_ms > kAlertFireRecentMs) {
        return -std::numeric_limits<float>::infinity();
    }

    const float max_dist_sq = kAlertDamageSourceRange * kAlertDamageSourceRange;
    if (dist_sq > max_dist_sq) {
        return -std::numeric_limits<float>::infinity();
    }

    rf::Vector3 shot_forward = forward_from_non_linear_yaw_pitch(
        candidate_entity.control_data.phb.y,
        candidate_entity.control_data.eye_phb.x);
    shot_forward.normalize_safe();

    rf::Vector3 to_local = local_entity.eye_pos - candidate_entity.eye_pos;
    if (to_local.len_sq() < 0.0001f) {
        to_local = local_entity.pos - candidate_entity.pos;
    }
    if (to_local.len_sq() < 0.0001f) {
        return -std::numeric_limits<float>::infinity();
    }
    to_local.normalize_safe();

    const float facing_dot = std::clamp(shot_forward.dot_prod(to_local), -1.0f, 1.0f);
    const float dist = std::sqrt(std::max(dist_sq, 0.0f));
    if (facing_dot < -0.25f && dist > kAlertDamageSourceNearRange) {
        return -std::numeric_limits<float>::infinity();
    }

    const float facing_norm = std::clamp((facing_dot + 1.0f) * 0.5f, 0.0f, 1.0f);
    const float recency_norm = std::clamp(
        1.0f - static_cast<float>(fire_age_ms) / static_cast<float>(kAlertFireRecentMs),
        0.0f,
        1.0f);
    const float dist_norm = std::clamp(1.0f - dist / kAlertDamageSourceRange, 0.0f, 1.0f);
    return recency_norm * 2.7f + dist_norm * 1.9f + facing_norm * 1.3f;
}
}

void bot_alerts_reset()
{
    g_client_bot_state.alert_source_samples.clear();
    g_client_bot_state.alert_contacts.clear();
}

void bot_alerts_update_context(const rf::Player& local_player, const rf::Entity& local_entity)
{
    if (g_client_bot_state.retaliation_timer.valid()
        && g_client_bot_state.retaliation_timer.elapsed()) {
        g_client_bot_state.retaliation_timer.invalidate();
        g_client_bot_state.retaliation_target_handle = -1;
    }

    const float current_health = std::max(local_entity.life, 0.0f);
    const float current_armor = std::max(local_entity.armor, 0.0f);
    if (g_client_bot_state.last_recorded_health < 0.0f
        || g_client_bot_state.last_recorded_armor < 0.0f) {
        g_client_bot_state.last_recorded_health = current_health;
        g_client_bot_state.last_recorded_armor = current_armor;
    }

    const bool took_damage =
        current_health + kAlertDamageEpsilon < g_client_bot_state.last_recorded_health
        || current_armor + kAlertDamageEpsilon < g_client_bot_state.last_recorded_armor;

    prune_alert_source_samples();
    prune_alert_contacts();

    const bool team_mode = multi_is_team_game_type();
    const float fire_range_sq = kAlertNearbyFireRange * kAlertNearbyFireRange;
    const float reload_range_sq = kAlertNearbyReloadRange * kAlertNearbyReloadRange;
    int best_medium_handle = -1;
    float best_medium_score = -std::numeric_limits<float>::infinity();
    int best_damage_source_handle = -1;
    float best_damage_source_score = -std::numeric_limits<float>::infinity();

    for (const rf::Player& candidate : SinglyLinkedList{rf::player_list}) {
        if (!is_enemy_candidate_player(local_player, candidate, team_mode)) {
            continue;
        }

        rf::Entity* candidate_entity = rf::entity_from_handle(candidate.entity_handle);
        if (!candidate_entity || candidate_entity == &local_entity || rf::entity_is_dying(candidate_entity)) {
            continue;
        }

        const float dist_sq = rf::vec_dist_squared(&candidate_entity->pos, &local_entity.pos);
        BotAlertSourceSample* sample = find_alert_source_sample(candidate_entity->handle);
        const bool is_new_sample = sample == nullptr;
        if (!sample) {
            sample = &add_alert_source_sample(*candidate_entity);
        }

        bool fire_event = false;
        bool reload_event = false;
        const bool reloading_now = rf::entity_is_reloading(candidate_entity);
        if (!is_new_sample) {
            if (candidate_entity->last_fired_timestamp.valid()
                && candidate_entity->last_fired_timestamp.value != sample->last_fired_timestamp_value
                && candidate_entity->last_fired_timestamp.time_since() <= kAlertFireRecentMs) {
                fire_event = true;
            }

            const bool reload_started = reloading_now && !sample->was_reloading;
            const bool reload_done_changed =
                candidate_entity->reload_done_timestamp.valid()
                && candidate_entity->reload_done_timestamp.value
                    != sample->last_reload_done_timestamp_value
                && candidate_entity->reload_done_timestamp.time_since() <= kAlertReloadRecentMs;
            reload_event = reload_started || reload_done_changed;
        }

        sample->last_fired_timestamp_value = candidate_entity->last_fired_timestamp.value;
        sample->last_reload_done_timestamp_value = candidate_entity->reload_done_timestamp.value;
        sample->was_reloading = reloading_now;

        if (fire_event && dist_sq <= fire_range_sq) {
            register_alert_contact(
                *candidate_entity,
                BotAlertType::nearby_weapon_fire,
                kAwarenessWeightNearbyFire,
                kAlertMediumAwarenessMs);
            const int event_age_ms = candidate_entity->last_fired_timestamp.valid()
                ? candidate_entity->last_fired_timestamp.time_since()
                : 0;
            const float score = compute_medium_alert_score(
                dist_sq,
                fire_range_sq,
                event_age_ms,
                kAlertFireRecentMs);
            if (score > best_medium_score) {
                best_medium_score = score;
                best_medium_handle = candidate_entity->handle;
            }
        }

        if (reload_event && dist_sq <= reload_range_sq) {
            register_alert_contact(
                *candidate_entity,
                BotAlertType::nearby_weapon_reload,
                kAwarenessWeightNearbyReload,
                kAlertMediumAwarenessMs);
            const int event_age_ms =
                candidate_entity->reload_done_timestamp.valid()
                    ? candidate_entity->reload_done_timestamp.time_since()
                    : 0;
            const float score = compute_medium_alert_score(
                dist_sq,
                reload_range_sq,
                event_age_ms,
                kAlertReloadRecentMs);
            if (score > best_medium_score) {
                best_medium_score = score;
                best_medium_handle = candidate_entity->handle;
            }
        }

        if (took_damage) {
            const float score = compute_damage_source_score(
                *candidate_entity,
                local_entity,
                dist_sq);
            if (score > best_damage_source_score) {
                best_damage_source_score = score;
                best_damage_source_handle = candidate_entity->handle;
            }
        }
    }

    if (took_damage && best_damage_source_handle >= 0) {
        if (rf::Entity* damage_source_entity =
                rf::entity_from_handle(best_damage_source_handle)) {
            register_alert_contact(
                *damage_source_entity,
                BotAlertType::damaged_by_enemy,
                kAwarenessWeightDamage,
                kAlertDamageAwarenessMs);
        }

        g_client_bot_state.retaliation_target_handle = best_damage_source_handle;
        g_client_bot_state.retaliation_timer.set(kAlertRetaliationDamageMs);
    }
    else if (best_medium_handle >= 0) {
        if (!g_client_bot_state.retaliation_timer.valid()
            || g_client_bot_state.retaliation_timer.elapsed()
            || g_client_bot_state.retaliation_timer.time_until()
                < kAlertRetaliationMediumRefreshMs) {
            g_client_bot_state.retaliation_target_handle = best_medium_handle;
            g_client_bot_state.retaliation_timer.set(kAlertRetaliationMediumMs);
        }
    }

    if (g_client_bot_state.retaliation_target_handle >= 0) {
        rf::Entity* retaliation_entity =
            rf::entity_from_handle(g_client_bot_state.retaliation_target_handle);
        if (!retaliation_entity || rf::entity_is_dying(retaliation_entity)) {
            g_client_bot_state.retaliation_target_handle = -1;
            g_client_bot_state.retaliation_timer.invalidate();
        }
    }

    prune_alert_contacts();
    g_client_bot_state.last_recorded_health = current_health;
    g_client_bot_state.last_recorded_armor = current_armor;
}

bool bot_alerts_get_contact_awareness(
    const int entity_handle,
    float& out_awareness_weight,
    BotAlertType* out_alert_type)
{
    out_awareness_weight = 0.0f;
    if (out_alert_type) {
        *out_alert_type = BotAlertType::none;
    }
    if (entity_handle < 0) {
        return false;
    }

    for (const BotAlertContact& contact : g_client_bot_state.alert_contacts) {
        if (contact.entity_handle != entity_handle
            || !contact.awareness_timer.valid()
            || contact.awareness_timer.elapsed()) {
            continue;
        }

        out_awareness_weight = std::clamp(contact.awareness_weight, 0.0f, 1.0f);
        if (out_alert_type) {
            *out_alert_type = contact.type;
        }
        return out_awareness_weight > 0.0f;
    }

    return false;
}
