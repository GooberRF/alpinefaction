#include "bot_chat_manager.h"

#include "bot_context.h"
#include "bot_personality.h"
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

bool bot_chat_should_process(const BotContext& ctx)
{
    if (ctx.is_server_bot) {
        return rf::is_server;
    }
    return client_bot_launch_enabled()
        && rf::is_multi
        && !rf::is_dedicated_server;
}

bool bot_chat_can_send_now(const BotContext& ctx)
{
    return !ctx.chat.chat_cooldown_timer.valid()
        || ctx.chat.chat_cooldown_timer.elapsed();
}

bool bot_chat_send_message(
    BotContext& ctx,
    const BotChatType type,
    const std::string_view message,
    const bool is_team_msg,
    const bool bypass_cooldown = false)
{
    if (!bot_chat_should_process(ctx)
        || message.empty()
        || (!bypass_cooldown && !bot_chat_can_send_now(ctx))) {
        return false;
    }

    if (!bot_backend_say(ctx, message, is_team_msg)) {
        return false;
    }

    ctx.chat.last_chat_type = type;
    const int cooldown_ms =
        (type == BotChatType::status)
            ? kBotStatusRetryCooldownMs
            : kBotChatGlobalCooldownMs;
    ctx.chat.chat_cooldown_timer.set(cooldown_ms);
    return true;
}

void bot_chat_sync_sit_out_state(BotContext& ctx)
{
    const bool missing_awp = waypoints_missing_awp_from_level_init()
        && !waypoints_awp_download_pending()
        && !waypoints_awp_load_retry_pending();
    ctx.chat.sit_out_for_missing_waypoints = missing_awp;
    if (!missing_awp) {
        ctx.chat.missing_waypoint_status_sent = false;
    }
}

std::string_view pick_random_taunt()
{
    return multi_hud_get_random_taunt_message();
}

bool bot_chat_send_taunt_message(BotContext& ctx, const std::string_view taunt_message)
{
    if (!bot_chat_should_process(ctx)
        || taunt_message.empty()
        || !bot_chat_can_send_now(ctx)) {
        return false;
    }

    if (!bot_backend_taunt(ctx, taunt_message)) {
        return false;
    }

    ctx.chat.last_chat_type = BotChatType::taunt;
    ctx.chat.chat_cooldown_timer.set(kBotChatGlobalCooldownMs);
    return true;
}

bool bot_chat_has_human_audience()
{
    for (const rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (player.is_bot || player.is_non_participant()) {
            continue;
        }
        return true;
    }
    return false;
}

void bot_chat_try_enqueue_kill_taunt(BotContext& ctx)
{
    bot_chat_sync_sit_out_state(ctx);
    if (ctx.chat.sit_out_for_missing_waypoints) {
        return;
    }
    if (!bot_chat_has_human_audience()) {
        return;
    }

    const float taunt_chance = std::clamp(
        bot_personality_get(ctx).taunt_on_kill_chance,
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

    ctx.chat.pending_taunts = std::min(
        kMaxPendingTaunts,
        ctx.chat.pending_taunts + 1
    );
}

void bot_chat_update_local_kill_confirmation(BotContext& ctx, const rf::Player& local_player)
{
    const auto* stats = local_player.stats
        ? static_cast<const PlayerStatsNew*>(local_player.stats)
        : nullptr;
    const int current_kills = stats
        ? std::max(0, static_cast<int>(stats->num_kills))
        : -1;
    if (current_kills < 0) {
        ctx.chat.observed_local_kill_count = -1;
        return;
    }

    if (ctx.chat.observed_local_kill_count < 0) {
        ctx.chat.observed_local_kill_count = current_kills;
        return;
    }

    if (current_kills < ctx.chat.observed_local_kill_count) {
        // Round reset / reconnect / score reset.
        ctx.chat.observed_local_kill_count = current_kills;
        return;
    }

    if (current_kills == ctx.chat.observed_local_kill_count) {
        return;
    }

    const int delta_kills = current_kills - ctx.chat.observed_local_kill_count;
    ctx.chat.observed_local_kill_count = current_kills;
    for (int i = 0; i < delta_kills; ++i) {
        bot_chat_try_enqueue_kill_taunt(ctx);
    }
}

void bot_chat_maybe_send_join_hello(BotContext& ctx, const rf::Player& local_player)
{
    if (ctx.chat.join_hello_evaluated) {
        return;
    }
    ctx.chat.join_hello_evaluated = true;

    if (!local_player.is_bot || local_player.is_spawn_disabled) {
        return;
    }

    const float hello_chance = std::clamp(
        bot_personality_get(ctx).hello_on_join_chance,
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

    bot_chat_send_message(ctx, BotChatType::misc, kRadioHelloMessage, false, true);
}

void bot_chat_update_pending_responses(BotContext& ctx)
{
    if (!ctx.chat.pending_red_faction_response) {
        return;
    }
    if (ctx.chat.red_faction_response_delay_timer.valid()
        && !ctx.chat.red_faction_response_delay_timer.elapsed()) {
        return;
    }

    if (bot_chat_send_message(
            ctx,
            BotChatType::response,
            kRadioRedFactionMessage,
            false,
            true)) {
        ctx.chat.pending_red_faction_response = false;
        ctx.chat.red_faction_response_delay_timer.invalidate();
        ctx.chat.red_faction_response_cooldown_timer.set(kBotRedFactionResponseCooldownMs);
    }
}
}

void bot_chat_manager_reset(BotContext& ctx)
{
    ctx.chat = {};
}

void bot_chat_manager_on_limbo_enter(BotContext& ctx, const rf::Player& local_player)
{
    if (!bot_chat_should_process(ctx)) {
        return;
    }
    if (!local_player.is_bot || local_player.is_spawn_disabled) {
        return;
    }

    const float gg_chance = std::clamp(
        bot_personality_get(ctx).gg_on_map_end_chance,
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
        ctx,
        BotChatType::misc,
        kMapEndGoodGameMessage,
        false,
        true);
}

void bot_chat_manager_on_remote_chat_message(
    BotContext& ctx,
    const rf::Player& sender,
    const std::string_view message)
{
    if (!bot_chat_should_process(ctx) || message.empty()) {
        return;
    }

    const rf::Player* const local_player = ctx.player;
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

    if (ctx.chat.red_faction_response_cooldown_timer.valid()
        && !ctx.chat.red_faction_response_cooldown_timer.elapsed()) {
        return;
    }

    if (ctx.chat.pending_red_faction_response) {
        return;
    }

    const float response_chance = std::clamp(
        bot_personality_get(ctx).red_faction_response_chance,
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

    ctx.chat.pending_red_faction_response = true;
    ctx.chat.red_faction_response_delay_timer.set(kBotRedFactionResponseDelayMs);
}

void bot_chat_manager_update_frame(BotContext& ctx, const rf::Player& local_player)
{
    if (!bot_chat_should_process(ctx)) {
        bot_chat_manager_reset(ctx);
        return;
    }

    bot_chat_maybe_send_join_hello(ctx, local_player);
    bot_chat_update_pending_responses(ctx);

    bot_chat_sync_sit_out_state(ctx);
    bot_chat_update_local_kill_confirmation(ctx, local_player);

    if (ctx.chat.sit_out_for_missing_waypoints
        && !ctx.chat.missing_waypoint_status_sent) {
        if (bot_chat_send_message(
                ctx,
                BotChatType::status,
                kMissingWaypointStatusMessage,
                false)) {
            ctx.chat.missing_waypoint_status_sent = true;
        }
    }

    if (ctx.chat.pending_taunts <= 0
        || ctx.chat.sit_out_for_missing_waypoints) {
        return;
    }
    if (!bot_chat_has_human_audience()) {
        ctx.chat.pending_taunts = 0;
        return;
    }

    if (ctx.chat.taunt_cooldown_timer.valid()
        && !ctx.chat.taunt_cooldown_timer.elapsed()) {
        return;
    }

    const std::string_view taunt = pick_random_taunt();
    if (taunt.empty()) {
        ctx.chat.pending_taunts = 0;
        return;
    }

    if (bot_chat_send_taunt_message(ctx, taunt)) {
        ctx.chat.taunt_cooldown_timer.set(kBotTauntCooldownMs);
        ctx.chat.pending_taunts =
            std::max(0, ctx.chat.pending_taunts - 1);
    }
}
