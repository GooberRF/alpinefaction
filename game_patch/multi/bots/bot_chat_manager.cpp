#include "bot_chat_manager.h"

#include "bot_personality_manager.h"
#include "../multi.h"
#include "../../hud/hud.h"
#include "../../main/main.h"
#include "../../misc/alpine_settings.h"
#include "../../misc/waypoints.h"
#include "../../rf/multi.h"
#include "../../rf/player/player.h"
#include <common/utils/string-utils.h>
#include <algorithm>
#include <random>
#include <string>
#include <string_view>

namespace
{
constexpr std::string_view kMissingWaypointStatusMessage =
    "I don't have waypoints for this map, so I'll sit this one out.";
constexpr std::string_view kMapEndGoodGameMessage = "gg";
constexpr std::string_view kRadioHelloMessage = "\xA8 Hello";
constexpr std::string_view kRadioRedFactionMessage = "\xA8 RED FACTION!";
constexpr int kBotChatGlobalCooldownMs = 2400;
constexpr int kBotTauntCooldownMs = 10000;
constexpr int kBotStatusRetryCooldownMs = 1200;
constexpr int kBotRedFactionResponseDelayMs = 1000;
constexpr int kBotRedFactionResponseCooldownMs = 30000;
constexpr int kMaxPendingTaunts = 2;

struct BotChatRuntimeState
{
    bool sit_out_for_missing_waypoints = false;
    bool missing_waypoint_status_sent = false;
    bool join_hello_evaluated = false;
    bool pending_red_faction_response = false;
    int pending_taunts = 0;
    int observed_local_kill_count = -1;
    BotChatType last_chat_type = BotChatType::status;
    rf::Timestamp chat_cooldown_timer{};
    rf::Timestamp taunt_cooldown_timer{};
    rf::Timestamp red_faction_response_delay_timer{};
    rf::Timestamp red_faction_response_cooldown_timer{};
};

BotChatRuntimeState g_bot_chat_state{};

bool bot_chat_can_send_now()
{
    return !g_bot_chat_state.chat_cooldown_timer.valid()
        || g_bot_chat_state.chat_cooldown_timer.elapsed();
}

bool bot_chat_send_message(
    const BotChatType type,
    const std::string_view message,
    const bool is_team_msg,
    const bool bypass_cooldown = false)
{
    if (!rf::is_multi
        || rf::is_dedicated_server
        || message.empty()
        || (!bypass_cooldown && !bot_chat_can_send_now())) {
        return false;
    }

    std::string owned_message{message};
    rf::multi_chat_say(owned_message.c_str(), is_team_msg);

    g_bot_chat_state.last_chat_type = type;
    const int cooldown_ms =
        (type == BotChatType::status)
            ? kBotStatusRetryCooldownMs
            : kBotChatGlobalCooldownMs;
    g_bot_chat_state.chat_cooldown_timer.set(cooldown_ms);
    return true;
}

void bot_chat_sync_sit_out_state()
{
    const bool missing_awp = waypoints_missing_awp_from_level_init();
    g_bot_chat_state.sit_out_for_missing_waypoints = missing_awp;
    if (!missing_awp) {
        g_bot_chat_state.missing_waypoint_status_sent = false;
    }
}

std::string_view pick_random_taunt()
{
    return multi_hud_get_random_taunt_message();
}

bool bot_chat_send_taunt_message(const std::string_view taunt_message)
{
    if (!rf::is_multi
        || rf::is_dedicated_server
        || taunt_message.empty()
        || !bot_chat_can_send_now()) {
        return false;
    }

    if (!multi_hud_send_taunt_chat_message(taunt_message)) {
        return false;
    }

    g_bot_chat_state.last_chat_type = BotChatType::taunt;
    g_bot_chat_state.chat_cooldown_timer.set(kBotChatGlobalCooldownMs);
    return true;
}

bool bot_chat_has_human_audience()
{
    for (const rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (player.is_bot || player.is_browser) {
            continue;
        }
        return true;
    }
    return false;
}

void bot_chat_try_enqueue_kill_taunt()
{
    bot_chat_sync_sit_out_state();
    if (g_bot_chat_state.sit_out_for_missing_waypoints) {
        return;
    }
    if (!bot_chat_has_human_audience()) {
        return;
    }

    const float taunt_chance = std::clamp(
        bot_personality_manager_get_active_personality().taunt_on_kill_chance,
        0.0f,
        1.0f
    );
    if (taunt_chance <= 0.0f) {
        return;
    }

    std::uniform_real_distribution<float> chance_dist(0.0f, 1.0f);
    if (chance_dist(g_rng) > taunt_chance) {
        return;
    }

    g_bot_chat_state.pending_taunts = std::min(
        kMaxPendingTaunts,
        g_bot_chat_state.pending_taunts + 1
    );
}

void bot_chat_update_local_kill_confirmation(const rf::Player& local_player)
{
    const auto* stats = local_player.stats
        ? static_cast<const PlayerStatsNew*>(local_player.stats)
        : nullptr;
    const int current_kills = stats
        ? std::max(0, static_cast<int>(stats->num_kills))
        : -1;
    if (current_kills < 0) {
        g_bot_chat_state.observed_local_kill_count = -1;
        return;
    }

    if (g_bot_chat_state.observed_local_kill_count < 0) {
        g_bot_chat_state.observed_local_kill_count = current_kills;
        return;
    }

    if (current_kills < g_bot_chat_state.observed_local_kill_count) {
        // Round reset / reconnect / score reset.
        g_bot_chat_state.observed_local_kill_count = current_kills;
        return;
    }

    if (current_kills == g_bot_chat_state.observed_local_kill_count) {
        return;
    }

    const int delta_kills = current_kills - g_bot_chat_state.observed_local_kill_count;
    g_bot_chat_state.observed_local_kill_count = current_kills;
    for (int i = 0; i < delta_kills; ++i) {
        bot_chat_try_enqueue_kill_taunt();
    }
}

bool bot_chat_should_process_for_local_bot()
{
    return client_bot_launch_enabled()
        && rf::is_multi
        && !rf::is_dedicated_server;
}

void bot_chat_maybe_send_join_hello(const rf::Player& local_player)
{
    if (g_bot_chat_state.join_hello_evaluated) {
        return;
    }
    g_bot_chat_state.join_hello_evaluated = true;

    if (!local_player.is_bot || local_player.is_spawn_disabled) {
        return;
    }

    const float hello_chance = std::clamp(
        bot_personality_manager_get_active_personality().hello_on_join_chance,
        0.0f,
        1.0f
    );
    if (hello_chance <= 0.0f) {
        return;
    }

    std::uniform_real_distribution<float> chance_dist(0.0f, 1.0f);
    if (chance_dist(g_rng) > hello_chance) {
        return;
    }

    bot_chat_send_message(BotChatType::misc, kRadioHelloMessage, false, true);
}

void bot_chat_update_pending_responses()
{
    if (!g_bot_chat_state.pending_red_faction_response) {
        return;
    }
    if (g_bot_chat_state.red_faction_response_delay_timer.valid()
        && !g_bot_chat_state.red_faction_response_delay_timer.elapsed()) {
        return;
    }

    if (bot_chat_send_message(
            BotChatType::response,
            kRadioRedFactionMessage,
            false,
            true)) {
        g_bot_chat_state.pending_red_faction_response = false;
        g_bot_chat_state.red_faction_response_delay_timer.invalidate();
        g_bot_chat_state.red_faction_response_cooldown_timer.set(kBotRedFactionResponseCooldownMs);
    }
}
}

void bot_chat_manager_reset()
{
    g_bot_chat_state = {};
}

void bot_chat_manager_on_limbo_enter(const rf::Player& local_player)
{
    if (!bot_chat_should_process_for_local_bot()) {
        return;
    }
    if (!local_player.is_bot || local_player.is_spawn_disabled) {
        return;
    }

    const float gg_chance = std::clamp(
        bot_personality_manager_get_active_personality().gg_on_map_end_chance,
        0.0f,
        1.0f
    );
    if (gg_chance <= 0.0f) {
        return;
    }

    std::uniform_real_distribution<float> chance_dist(0.0f, 1.0f);
    if (chance_dist(g_rng) > gg_chance) {
        return;
    }

    bot_chat_send_message(
        BotChatType::misc,
        kMapEndGoodGameMessage,
        false,
        true);
}

void bot_chat_manager_on_remote_chat_message(
    const rf::Player& sender,
    const std::string_view message)
{
    if (!bot_chat_should_process_for_local_bot() || message.empty()) {
        return;
    }

    const rf::Player* const local_player = rf::local_player;
    if (!local_player) {
        return;
    }

    if (&sender == local_player) {
        return;
    }
    if (local_player->net_data && sender.net_data
        && sender.net_data->player_id == local_player->net_data->player_id) {
        return;
    }

    if (!string_iequals(message, kRadioRedFactionMessage)) {
        return;
    }

    if (g_bot_chat_state.red_faction_response_cooldown_timer.valid()
        && !g_bot_chat_state.red_faction_response_cooldown_timer.elapsed()) {
        return;
    }

    if (g_bot_chat_state.pending_red_faction_response) {
        return;
    }

    const float response_chance = std::clamp(
        bot_personality_manager_get_active_personality().red_faction_response_chance,
        0.0f,
        1.0f
    );
    if (response_chance <= 0.0f) {
        return;
    }

    std::uniform_real_distribution<float> chance_dist(0.0f, 1.0f);
    if (chance_dist(g_rng) > response_chance) {
        return;
    }

    g_bot_chat_state.pending_red_faction_response = true;
    g_bot_chat_state.red_faction_response_delay_timer.set(kBotRedFactionResponseDelayMs);
}

void bot_chat_manager_update_frame(const rf::Player& local_player)
{
    if (!bot_chat_should_process_for_local_bot()) {
        bot_chat_manager_reset();
        return;
    }

    bot_chat_maybe_send_join_hello(local_player);
    bot_chat_update_pending_responses();

    bot_chat_sync_sit_out_state();
    bot_chat_update_local_kill_confirmation(local_player);

    if (g_bot_chat_state.sit_out_for_missing_waypoints
        && !g_bot_chat_state.missing_waypoint_status_sent) {
        if (bot_chat_send_message(
                BotChatType::status,
                kMissingWaypointStatusMessage,
                false)) {
            g_bot_chat_state.missing_waypoint_status_sent = true;
        }
    }

    if (g_bot_chat_state.pending_taunts <= 0
        || g_bot_chat_state.sit_out_for_missing_waypoints) {
        return;
    }
    if (!bot_chat_has_human_audience()) {
        g_bot_chat_state.pending_taunts = 0;
        return;
    }

    if (g_bot_chat_state.taunt_cooldown_timer.valid()
        && !g_bot_chat_state.taunt_cooldown_timer.elapsed()) {
        return;
    }

    const std::string_view taunt = pick_random_taunt();
    if (taunt.empty()) {
        g_bot_chat_state.pending_taunts = 0;
        return;
    }

    if (bot_chat_send_taunt_message(taunt)) {
        g_bot_chat_state.taunt_cooldown_timer.set(kBotTauntCooldownMs);
        g_bot_chat_state.pending_taunts =
            std::max(0, g_bot_chat_state.pending_taunts - 1);
    }
}
