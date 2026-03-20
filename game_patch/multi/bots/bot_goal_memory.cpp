#include "bot_goal_memory.h"

#include <algorithm>
#include <cmath>

namespace
{
void prune_recent_item_goal_selections()
{
    auto& recent = g_client_bot_state.recent_item_goal_selections;
    recent.erase(
        std::remove_if(
            recent.begin(),
            recent.end(),
            [](const RecentItemGoalSelection& entry) {
                return entry.item_handle < 0
                    || !bot_goal_is_item_collection(entry.goal_type)
                    || !entry.cooldown.valid()
                    || entry.cooldown.elapsed();
            }),
        recent.end());
}

void register_recent_item_goal_selection(const int item_handle, const BotGoalType goal_type)
{
    if (item_handle < 0 || !bot_goal_is_item_collection(goal_type)) {
        return;
    }

    prune_recent_item_goal_selections();
    for (RecentItemGoalSelection& entry : g_client_bot_state.recent_item_goal_selections) {
        if (entry.item_handle == item_handle) {
            entry.goal_type = goal_type;
            entry.cooldown.set(kRecentItemGoalSelectionMs);
            return;
        }
    }

    if (g_client_bot_state.recent_item_goal_selections.size()
        >= kRecentItemGoalSelectionCapacity) {
        g_client_bot_state.recent_item_goal_selections.erase(
            g_client_bot_state.recent_item_goal_selections.begin());
    }

    RecentItemGoalSelection entry{};
    entry.item_handle = item_handle;
    entry.goal_type = goal_type;
    entry.cooldown.set(kRecentItemGoalSelectionMs);
    g_client_bot_state.recent_item_goal_selections.push_back(entry);
}
}

void bot_goal_memory_prune_failed_item_goal_cooldowns()
{
    auto& cooldowns = g_client_bot_state.failed_item_goal_cooldowns;
    cooldowns.erase(
        std::remove_if(
            cooldowns.begin(),
            cooldowns.end(),
            [](const FailedItemGoalCooldown& cooldown) {
                return cooldown.item_handle < 0
                    || !cooldown.cooldown.valid()
                    || cooldown.cooldown.elapsed();
            }),
        cooldowns.end());
}

void bot_goal_memory_register_failed_item_goal_cooldown(
    const int item_handle,
    const int cooldown_ms)
{
    if (item_handle < 0 || cooldown_ms <= 0) {
        return;
    }

    bot_goal_memory_prune_failed_item_goal_cooldowns();
    for (FailedItemGoalCooldown& cooldown : g_client_bot_state.failed_item_goal_cooldowns) {
        if (cooldown.item_handle == item_handle) {
            cooldown.cooldown.set(cooldown_ms);
            return;
        }
    }

    if (g_client_bot_state.failed_item_goal_cooldowns.size() >= kFailedItemGoalCooldownCapacity) {
        g_client_bot_state.failed_item_goal_cooldowns.erase(
            g_client_bot_state.failed_item_goal_cooldowns.begin());
    }

    FailedItemGoalCooldown cooldown{};
    cooldown.item_handle = item_handle;
    cooldown.cooldown.set(cooldown_ms);
    g_client_bot_state.failed_item_goal_cooldowns.push_back(cooldown);
}

bool bot_goal_memory_is_failed_item_goal_cooldown_active_no_prune(const int item_handle)
{
    if (item_handle < 0) {
        return false;
    }

    for (const FailedItemGoalCooldown& cooldown : g_client_bot_state.failed_item_goal_cooldowns) {
        if (cooldown.item_handle == item_handle
            && cooldown.cooldown.valid()
            && !cooldown.cooldown.elapsed()) {
            return true;
        }
    }

    return false;
}

void bot_goal_memory_note_item_goal_selection(
    const int item_handle,
    const BotGoalType goal_type)
{
    if (!bot_goal_is_item_collection(goal_type) || item_handle < 0) {
        return;
    }

    register_recent_item_goal_selection(item_handle, goal_type);
    g_client_bot_state.recent_secondary_goal_type = goal_type;
    g_client_bot_state.recent_secondary_goal_timer.set(kSecondaryGoalTypeRepeatMs);
}

float bot_goal_memory_get_recent_item_goal_penalty(
    const int item_handle,
    const BotGoalType goal_type)
{
    if (item_handle < 0 || !bot_goal_is_item_collection(goal_type)) {
        return 0.0f;
    }

    prune_recent_item_goal_selections();
    float penalty = 0.0f;
    for (const RecentItemGoalSelection& entry : g_client_bot_state.recent_item_goal_selections) {
        if (entry.item_handle != item_handle
            || !entry.cooldown.valid()
            || entry.cooldown.elapsed()) {
            continue;
        }

        const float remaining_norm = std::clamp(
            static_cast<float>(entry.cooldown.time_until())
                / static_cast<float>(kRecentItemGoalSelectionMs),
            0.0f,
            1.0f);
        const float handle_penalty = std::lerp(18.0f, 85.0f, remaining_norm);
        penalty = std::max(penalty, handle_penalty);
        if (entry.goal_type == goal_type) {
            penalty += std::lerp(6.0f, 24.0f, remaining_norm);
        }
    }

    return penalty;
}

float bot_goal_memory_get_secondary_goal_repeat_penalty(const BotGoalType goal_type)
{
    if (!bot_goal_is_item_collection(goal_type)
        || g_client_bot_state.recent_secondary_goal_type != goal_type
        || !g_client_bot_state.recent_secondary_goal_timer.valid()
        || g_client_bot_state.recent_secondary_goal_timer.elapsed()) {
        return 0.0f;
    }

    const float remaining_norm = std::clamp(
        static_cast<float>(g_client_bot_state.recent_secondary_goal_timer.time_until())
            / static_cast<float>(kSecondaryGoalTypeRepeatMs),
        0.0f,
        1.0f);
    return std::lerp(10.0f, 45.0f, remaining_norm);
}
