#include "bot_context.h"

#include "bot_state.h"
#include "server_bots.h"
#include "../multi.h"
#include "../server.h"
#include "../../hud/hud.h"
#include "../../rf/multi.h"
#include "../../rf/player/control_config.h"
#include "../../rf/player/player_fpgun.h"
#include <string>

BotContext* g_active_bot = nullptr;

namespace
{
BotContext g_client_bot{};
}

BotContext& client_bot_context()
{
    return g_client_bot;
}

void bot_context_activate(BotContext& ctx)
{
    ctx.state.server_config_received = true;
    ctx.state.server_deactivated = false;
    ctx.state.server_config_timeout_timer.invalidate();
    ctx.state.connection_watchdog_timer.set(kBotConnectionWatchdogMs);
}

void bot_context_deactivate(BotContext& ctx)
{
    BotActiveScope scope{ctx};
    ctx.state.server_deactivated = true;
    bot_state_clear_goal();
    bot_state_clear_waypoint_route(true, true, true);
    bot_state_reset_fsm(BotFsmState::inactive);
}

bool bot_backend_weapon_busy(rf::Player& pp, rf::Entity& ep)
{
    if (bot_ctx().is_server_bot) {
        return server_bot_is_reloading(pp, ep) || multi_is_selecting_weapon(&pp);
    }
    return rf::player_fpgun_action_anim_is_playing(&pp, rf::WeaponAction::WA_RELOAD)
        || rf::player_fpgun_action_anim_is_playing(&pp, rf::WeaponAction::WA_HOLSTER)
        || rf::player_fpgun_action_anim_is_playing(&pp, rf::WeaponAction::WA_DRAW);
}

void bot_backend_jump(rf::Player& pp)
{
    rf::player_execute_action(&pp, rf::CC_ACTION_JUMP, true);
}

void bot_backend_crouch(rf::Player& pp)
{
    rf::player_execute_action(&pp, rf::CC_ACTION_CROUCH, true);
}

void bot_backend_use(rf::Player& pp)
{
    rf::player_execute_action(&pp, rf::CC_ACTION_USE, true);
}

void bot_backend_reload(rf::Player& pp)
{
    if (bot_ctx().is_server_bot) {
        if (const rf::Entity* const ep = rf::entity_from_handle(pp.entity_handle)) {
            server_bot_reload(pp, ep->ai.current_primary_weapon);
        }
        return;
    }
    rf::player_execute_action(&pp, rf::CC_ACTION_RELOAD, true);
}

void bot_backend_switch_weapon(rf::Player& pp, rf::Entity& ep, const int weapon_type)
{
    if (bot_ctx().is_server_bot) {
        rf::multi_select_weapon_server_side(&pp, &ep, weapon_type);
        return;
    }
    if (rf::is_multi && !rf::is_server) {
        // Match multiplayer direct-weapon selection behavior: request selection from server.
        rf::multi_set_next_weapon(weapon_type);
    }
    else {
        rf::player_make_weapon_current_selection(&pp, weapon_type);
        ep.ai.current_primary_weapon = weapon_type;
    }
}

void bot_backend_request_respawn(rf::Player& pp)
{
    if (bot_ctx().is_server_bot) {
        // Not player_execute_action: its respawn path shares one global spawn timer across all callers.
        rf::multi_spawn_player_server_side(&pp);
        return;
    }
    rf::player_execute_action(&pp, rf::CC_ACTION_SECONDARY_ATTACK, true);
}

bool bot_backend_say(const BotContext& ctx, const std::string_view message, const bool is_team_msg)
{
    if (ctx.is_server_bot) {
        if (!ctx.player) {
            return false;
        }
        server_bot_say(*ctx.player, message, is_team_msg);
        return true;
    }
    std::string owned_message{message};
    rf::multi_chat_say(owned_message.c_str(), is_team_msg);
    return true;
}

bool bot_backend_taunt(const BotContext& ctx, const std::string_view taunt_message)
{
    if (ctx.is_server_bot) {
        if (!ctx.player || taunt_message.empty()) {
            return false;
        }
        server_bot_say(*ctx.player, multi_hud_format_taunt_chat_message(taunt_message), false);
        return true;
    }
    return multi_hud_send_taunt_chat_message(taunt_message);
}
