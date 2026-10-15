#include "server_bots.h"
#include "bot_chat_manager.h"
#include "bot_context.h"
#include "../gametype.h"
#include "../multi.h"
#include "../network.h"
#include "../server.h"
#include "../server_internal.h"
#include "../demo/demo.h"
#include "../vehicles/vehicle_sync.h"
#include "../../fflink/afstats_events.h"
#include "../../misc/alpine_settings.h"
#include "../../misc/misc.h"
#include "../../misc/waypoints.h"
#include "../../os/os.h"
#include "../../rf/entity.h"
#include "../../rf/gameseq.h"
#include "../../rf/level.h"
#include "../../rf/localize.h"
#include "../../rf/multi.h"
#include "../../rf/os/console.h"
#include "../../rf/os/frametime.h"
#include "../../rf/os/timestamp.h"
#include "../../rf/player/player.h"
#include "../../rf/sound/sound.h"
#include "../../rf/weapon.h"
#include "../../sound/sound.h"
#include <common/rfproto.h>
#include <common/utils/list-utils.h>
#include <common/version/version.h>
#include <patch_common/CallHook.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

struct ServerBot
{
    BotContext ctx;
    int roster_id = 0;
    float pending_dt = 0.0f;
};

namespace
{
constexpr int k_server_bot_create_interval_ms = 500;
// Bots that have not thought yet once this frame's thinks took this long think next frame, first in line.
constexpr int64_t k_server_bot_think_budget_us = 3000;

struct ServerBotRosterEntry
{
    int profile_index = -1;
    int team = -1;
    int id = 0;
};

std::vector<std::unique_ptr<ServerBot>> g_server_bots;
size_t g_server_bot_think_cursor = 0;
std::vector<uint8_t> g_server_bot_removals;
std::vector<ServerBotRosterEntry> g_server_bot_roster;
int g_next_server_bot_roster_id = 1;
rf::Timestamp g_server_bot_create_timer;

const ServerBotConfig& server_bot_config_for(const rf::Player* player, const ServerBotConfig& fallback)
{
    const auto& configs = g_alpine_server_config.bot_configs;
    const int slot = g_bot_profile_slots.get_slot(player);
    return (slot >= 0 && slot < static_cast<int>(configs.size())) ? configs[slot] : fallback;
}

void server_bot_apply_config(BotContext& ctx, const ServerBotConfig& config)
{
    bot_personality_set_presets(ctx, config.personality_preset.c_str(), config.skill_preset.c_str());
    for (const BotConfigOverride& ovr : config.personality_overrides) {
        bot_personality_apply_field_override(ctx, ovr.field_id, ovr.value);
    }
    for (const BotConfigOverride& ovr : config.skill_overrides) {
        bot_skill_apply_field_override(ctx, ovr.field_id, ovr.value);
    }
}

void server_bot_apply_fire(const BotCombatFiringState& firing, rf::Player& pp, rf::Entity& ep, const bool thought)
{
    const int weapon_type = ep.ai.current_primary_weapon;
    if (weapon_type < 0 || weapon_type >= rf::num_weapon_types) {
        return;
    }
    const bool down = firing.synthetic_primary_fire_down || firing.synthetic_secondary_fire_down;
    const bool alt_fire = !firing.synthetic_primary_fire_down && firing.synthetic_secondary_fire_down;
    // A press counts on its think's frame only; the frames until the next think hold the trigger.
    const bool just_pressed = thought
        && (alt_fire ? firing.synthetic_secondary_fire_just_pressed : firing.synthetic_primary_fire_just_pressed);
    const bool weapon_is_on = rf::entity_weapon_is_on(ep.handle, weapon_type);

    if (rf::weapon_is_on_off_weapon(weapon_type, alt_fire)) {
        if (down && !weapon_is_on && !rf::entity_is_reloading(&ep) && !multi_is_selecting_weapon(&pp)) {
            multi_turn_weapon_on(&ep, &pp, alt_fire);
        }
        else if (!down && weapon_is_on) {
            multi_turn_weapon_off(&ep);
        }
        return;
    }
    if (weapon_is_on) {
        multi_turn_weapon_off(&ep);
    }

    const bool trigger = rf::weapon_is_semi_automatic(weapon_type) ? just_pressed : down;
    if (!trigger || (ep.ai.next_fire_primary.valid() && !ep.ai.next_fire_primary.elapsed())) {
        return;
    }
    rf::Vector3 pos{};
    rf::Matrix3 orient{};
    rf::entity_get_weapon_fire_pos_orient(&ep, weapon_type, &pos, &orient, ep.ai.next_fire_primary_slot, true);
    rf::multi_process_remote_weapon_fire(&ep, weapon_type, pos, orient, alt_fire);
    // Thrown weapons are created inline by the remote-fire handler, which never re-arms the fire wait.
    if (!ep.ai.next_fire_primary.valid() || ep.ai.next_fire_primary.elapsed()) {
        ep.ai.next_fire_primary.set(rf::weapon_get_fire_wait_ms(weapon_type, alt_fire));
    }
}

int server_bot_keyframe_interval_ms()
{
    return 1000 / static_cast<int>(g_alpine_game_config.server_netfps);
}

// The obj_update row of a non-local entity is its newest keyframe, and a row whose tick the recipient already
// has is dropped.
void server_bot_refresh_keyframe(rf::Entity& ep, const int min_age_ms)
{
    const rf::ObjInterp* const interp = ep.obj_interp;
    if (!interp) {
        return;
    }
    const auto tick = static_cast<uint16_t>(timer::get_i64(1000));
    if (interp->num_frames() > 0
        && static_cast<uint16_t>(tick - interp->newest_frame_time()) < min_age_ms) {
        return;
    }
    obj_interp_push_keyframe(&ep, ep.pos, ep.control_data.phb, ep.control_data.eye_phb, ep.p_data.vel,
                             ep.ai.ci.move, tick);
}

// The server obj_update loop's row packing (0x0047E630). A recipient's rows are about one send interval apart,
// so pushing at half-interval age gives its rows fresh keyframes at no more than two pushes per interval.
CallHook<int(rf::Player*, rf::Entity*, void*)> server_bot_pack_obj_update_hook{
    0x0047E70B,
    [](rf::Player* recipient, rf::Entity* ep, void* data) {
        if (recipient->net_data->reliable_socket != static_cast<rf::uint>(-1) && server_bot_from_entity(ep)) {
            server_bot_refresh_keyframe(*ep, server_bot_keyframe_interval_ms() / 2);
        }
        return server_bot_pack_obj_update_hook.call_target(recipient, ep, data);
    },
};

// multi_chat_say's listen-server branch (0x00444150) shows the host's own line; it never reaches the chat_line
// receive handler, so this is where server bots hear the host.
CallHook<void(rf::Player*, const char*, int)> listen_host_chat_add_msg_hook{
    0x00444285,
    [](rf::Player* player, const char* msg, int is_team_msg) {
        listen_host_chat_add_msg_hook.call_target(player, msg, is_team_msg);
        if (player) {
            // The engine relays only team byte 1 as team chat.
            server_bots_on_chat_message(*player, msg, is_team_msg == 1);
        }
    },
};

void server_bot_apply_outputs(ServerBot& bot, const bool thought)
{
    BotContext& ctx = bot.ctx;
    rf::Player* const pp = ctx.player;
    rf::Entity* const ep = pp ? rf::entity_from_handle(pp->entity_handle) : nullptr;
    if (!ep) {
        return;
    }

    const BotState& state = ctx.state;
    ep->ai.ci.rot = {};
    ep->ai.ci.mouse_dh = 0.0f;
    ep->ai.ci.mouse_dp = 0.0f;
    ep->ai.ci.move = state.movement_override_active
        ? rf::Vector3{state.move_input_x, state.move_input_y, state.move_input_z}
        : rf::Vector3{};

    if (!rf::entity_is_dying(ep)) {
        server_bot_apply_fire(state.firing, *pp, *ep, thought);
    }
    // Keeps the ring non-empty (obj_update skips an entity without keyframes) and recent without recipients.
    server_bot_refresh_keyframe(*ep, server_bot_keyframe_interval_ms());
}

void server_bot_stop_idle_entity(const ServerBot& bot)
{
    rf::Player* const pp = bot.ctx.player;
    if (rf::Entity* const ep = pp ? rf::entity_from_handle(pp->entity_handle) : nullptr) {
        ep->ai.ci.move = {};
        multi_turn_weapon_off(ep);
    }
}

bool server_bot_removal_queued(const ServerBot& bot)
{
    const uint8_t player_id = bot.ctx.player->net_data->player_id;
    return std::find(g_server_bot_removals.begin(), g_server_bot_removals.end(), player_id)
        != g_server_bot_removals.end();
}

// Queued, not immediate: the receive loop that runs console/rcon commands caches player_list->next.
void server_bot_remove(const ServerBot& bot)
{
    if (!server_bot_removal_queued(bot)) {
        g_server_bot_removals.push_back(bot.ctx.player->net_data->player_id);
    }
}

void server_bots_process_removals()
{
    std::vector<uint8_t> batch;
    batch.swap(g_server_bot_removals);
    for (const uint8_t player_id : batch) {
        rf::Player* const player = rf::multi_find_player_by_id(player_id);
        if (!server_bot_from_player(player)) {
            continue;
        }
        if (rf::multi_get_game_type() == rf::NG_TYPE_CTF) {
            rf::multi_ctf_drop_flag(player);
        }
        rf::multi_powerup_remove_all_for_player(player);
        afstats::note_leave_reason(player, afstats::LeaveReason::quit);
        rf::send_left_game_packet(player_id, RF_LR_NORMAL);
        rf::multi_player_left_game(player);
    }
}

ServerBot* server_bot_for_roster_entry(const int roster_id)
{
    const auto it = std::find_if(g_server_bots.begin(), g_server_bots.end(), [roster_id](const auto& bot) {
        return bot->roster_id == roster_id;
    });
    return it != g_server_bots.end() ? it->get() : nullptr;
}

bool server_bot_roster_has_entry(const int roster_id)
{
    return std::any_of(g_server_bot_roster.begin(), g_server_bot_roster.end(), [roster_id](const auto& entry) {
        return entry.id == roster_id;
    });
}

void server_bot_roster_add(const int profile_index, const int team)
{
    g_server_bot_roster.push_back({profile_index, team, g_next_server_bot_roster_id++});
}

// Waypoints load at level init only while server bots are configured. A host editing waypoints keeps its
// in-memory graph, which the bots then use.
void server_bot_roster_on_changed(const bool was_configured)
{
    if (!was_configured && server_bots_configured() && rf::is_server
        && (rf::level.flags & rf::LEVEL_LOADED)
        && !(g_alpine_game_config.waypoints_edit_mode && waypoints_count() > 1)) {
        waypoints_level_init_with_awp_download();
    }
}

int server_bot_score(const rf::Player& player)
{
    return player.stats ? player.stats->score : 0;
}

ServerBot* pick_server_bot_to_evict()
{
    ServerBot* best = nullptr;
    for (const auto& bot : g_server_bots) {
        if (server_bot_removal_queued(*bot)) {
            continue;
        }
        if (!best) {
            best = bot.get();
            continue;
        }
        const rf::Player& candidate = *bot->ctx.player;
        const rf::Player& current = *best->ctx.player;
        if (candidate.is_spawn_disabled != current.is_spawn_disabled) {
            if (candidate.is_spawn_disabled) {
                best = bot.get();
            }
        }
        else if (server_bot_score(candidate) < server_bot_score(current)) {
            best = bot.get();
        }
    }
    return best;
}

void server_bot_create(const ServerBotRosterEntry& entry)
{
    const int bot_team = (entry.team == rf::TEAM_RED || entry.team == rf::TEAM_BLUE)
        ? entry.team
        : pick_team_for_new_server_bot();
    rf::Player* const pp = rf::player_allocate(false);
    if (!pp) {
        return;
    }

    const auto& configs = g_alpine_server_config.bot_configs;
    const int num_configs = static_cast<int>(configs.size());
    if (entry.profile_index >= 0 && entry.profile_index < num_configs) {
        g_bot_profile_slots.assign_explicit(pp, entry.profile_index);
    }
    else {
        g_bot_profile_slots.assign_slot(pp, num_configs);
    }
    const ServerBotConfig default_config{};
    const ServerBotConfig& config = server_bot_config_for(pp, default_config);

    pp->is_bot = true;
    pp->bot_skill = 100;
    pp->is_spawn_disabled = false;
    pp->is_browser = false;
    pp->is_human_player = false;
    pp->version_info = ClientVersionInfoProfile{
        .software = ClientSoftware::AlpineFaction,
        .major = VERSION_MAJOR,
        .minor = VERSION_MINOR,
        .patch = VERSION_PATCH,
        .type = VERSION_TYPE,
        .max_rfl_ver = MAXIMUM_RFL_VERSION,
        .is_d3d11 = true,
    };

    std::string name;
    int32_t character = 0;
    resolve_bot_identity(config, pp, name, character);
    pp->name = name.c_str();
    pp->settings.multi_character = character;
    pp->settings.toggle_crouch = true;
    pp->team = static_cast<rf::ubyte>(bot_team);

    rf::PlayerNetData* const net_data = pp->net_data;
    net_data->player_id = rf::multi_alloc_player_id();
    // Port 1 is the demo recorder's address; a unique fake address keeps addr-keyed lookups apart.
    net_data->addr = rf::NetAddr{{0x7F000001}, static_cast<uint16_t>(2 + net_data->player_id)};
    net_data->flags = rf::NPF_CLIENT_IS_LOADED | (bot_team == rf::TEAM_BLUE ? rf::NPF_BLUE_TEAM : 0);
    net_data->state = rf::NETPLAYER_STATE_IN_GAME;
    net_data->join_time_ms = static_cast<int>(timer::get_i64(1000));
    net_data->max_update_rate = 1000000;
    rf::update_player_rate(pp);

    auto bot = std::make_unique<ServerBot>();
    bot->ctx.player = pp;
    bot->ctx.is_server_bot = true;
    bot->roster_id = entry.id;
    server_bot_apply_config(bot->ctx, config);
    g_server_bots.push_back(std::move(bot));

    rf::send_new_player_packet(pp);
    update_player_active_status(pp);
    if (g_dedicated_launched_from_ads) {
        print_player_info(pp, true);
    }
    else {
        rf::console::print("{}{} ({})", pp->name, rf::strings::has_joined, net_data->addr.ip_addr);
    }
    afstats::on_player_join(pp);

    rf::console::print("  Server bot {} uses profile '{}' (slot {}, skill '{}', character={})\n", pp->name,
        config.personality_preset, g_bot_profile_slots.get_slot(pp), config.skill_preset, character);
}

// Runs after the removal queue is drained: player ids are reused, so no bot is created while one with a
// queued id is still in the player list.
void server_bots_reconcile(const bool in_gameplay)
{
    for (const auto& bot : g_server_bots) {
        if (!server_bot_roster_has_entry(bot->roster_id)) {
            server_bot_remove(*bot);
        }
    }

    // server_full_num_players_hook admits a human into a full server on the strength of the server bots. The bot
    // leaves once the joiner's reliable socket attaches; a join request that never connects times out instead.
    const int num_players = multi_num_players_occupying_slots();
    const int num_connected = num_players - multi_num_players_waiting_for_reliable_socket();
    int excess = num_connected - rf::netgame.max_players - static_cast<int>(g_server_bot_removals.size());
    for (; excess > 0; --excess) {
        const ServerBot* const bot = pick_server_bot_to_evict();
        if (!bot) {
            break;
        }
        server_bot_remove(*bot);
    }

    if (!in_gameplay || num_players >= rf::netgame.max_players
        || (g_server_bot_create_timer.valid() && !g_server_bot_create_timer.elapsed())) {
        return;
    }
    const auto entry = std::find_if(g_server_bot_roster.begin(), g_server_bot_roster.end(), [](const auto& e) {
        return !server_bot_for_roster_entry(e.id);
    });
    if (entry != g_server_bot_roster.end()) {
        g_server_bot_create_timer.set(k_server_bot_create_interval_ms);
        server_bot_create(*entry);
    }
}

std::string_view server_bot_team_name(const int team)
{
    if (team == rf::TEAM_RED) {
        return "red";
    }
    return team == rf::TEAM_BLUE ? "blue" : "auto";
}

std::string server_bot_profile_label(const int profile_index)
{
    return profile_index >= 0 ? std::to_string(profile_index) : std::string{"auto"};
}

std::string_view server_bot_state_name(rf::Player& player)
{
    if (player.is_spawn_disabled) {
        return "spawn disabled";
    }
    if (rf::player_is_dead(&player) || rf::player_is_dying(&player)) {
        return "dead";
    }
    return "alive";
}
}

bool server_bots_configured()
{
    return !g_server_bot_roster.empty();
}

void server_bots_reset_roster(const int count)
{
    const bool was_configured = server_bots_configured();
    const auto size = static_cast<size_t>(std::clamp(count, 0, k_max_server_bots));
    if (g_server_bot_roster.size() > size) {
        g_server_bot_roster.resize(size);
    }
    while (g_server_bot_roster.size() < size) {
        server_bot_roster_add(-1, -1);
    }
    server_bot_roster_on_changed(was_configured);
}

bool server_bots_add_to_roster(const int profile_index, const int team)
{
    if (static_cast<int>(g_server_bot_roster.size()) >= k_max_server_bots) {
        return false;
    }
    const bool was_configured = server_bots_configured();
    server_bot_roster_add(profile_index, team);
    server_bot_roster_on_changed(was_configured);
    return true;
}

void server_bots_remove_from_roster(const rf::Player* const player)
{
    if (const ServerBot* const bot = server_bot_from_player(player)) {
        const int roster_id = bot->roster_id;
        std::erase_if(g_server_bot_roster, [roster_id](const auto& entry) { return entry.id == roster_id; });
        server_bot_remove(*bot);
    }
}

void server_bots_clear_roster()
{
    g_server_bot_roster.clear();
    for (const auto& bot : g_server_bots) {
        server_bot_remove(*bot);
    }
}

std::string server_bots_build_list_output()
{
    std::string output = std::format("Server bots: {} in roster, {} in game\n", g_server_bot_roster.size(),
        g_server_bots.size());
    const bool team_mode = multi_is_team_game_type();
    for (const ServerBotRosterEntry& entry : g_server_bot_roster) {
        const ServerBot* const bot = server_bot_for_roster_entry(entry.id);
        if (!bot) {
            std::format_to(std::back_inserter(output), "- (waiting for a free slot) | Profile: {} | Team: {}\n",
                server_bot_profile_label(entry.profile_index), server_bot_team_name(entry.team));
            continue;
        }
        rf::Player& player = *bot->ctx.player;
        std::format_to(std::back_inserter(output), "- {} | Profile: {} | Team: {} | State: {} | Score: {} | Ping: 0\n",
            player.name, server_bot_profile_label(g_bot_profile_slots.get_slot(&player)),
            team_mode ? server_bot_team_name(player.team) : std::string_view{"-"}, server_bot_state_name(player),
            server_bot_score(player));
    }
    return output;
}

ServerBot* server_bot_from_player(const rf::Player* player)
{
    if (!player) {
        return nullptr;
    }
    const auto it = std::find_if(g_server_bots.begin(), g_server_bots.end(), [player](const auto& bot) {
        return bot->ctx.player == player;
    });
    return it != g_server_bots.end() ? it->get() : nullptr;
}

ServerBot* server_bot_from_entity(const rf::Entity* entity)
{
    if (!entity || g_server_bots.empty()) {
        return nullptr;
    }
    return server_bot_from_player(rf::player_from_entity_handle(entity->handle));
}

int server_bots_num_evictable()
{
    return static_cast<int>(std::count_if(g_server_bots.begin(), g_server_bots.end(), [](const auto& bot) {
        return !server_bot_removal_queued(*bot);
    }));
}

void server_bot_say(rf::Player& sender, const std::string_view message, const bool is_team_msg)
{
    const std::string msg{message};
    // As the stock chat_line handler shows a received line: team lines only to the sender's team.
    if (!is_team_msg || !rf::local_player || rf::local_player->team == sender.team) {
        rf::multi_chat_add_msg(&sender, msg.c_str(), is_team_msg);
        if (!rf::is_dedicated_server) {
            rf::snd_play(stock_sound_id::end_voice, 0, 0.0f, 1.0f);
        }
    }

    // The stock chat_line relay, sent as the bot.
    const auto packet = build_chat_line_packet(sender.net_data->player_id, is_team_msg, msg);
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (&player != rf::local_player && &player != &sender && (!is_team_msg || player.team == sender.team)) {
            rf::multi_io_send(&player, packet.data(), static_cast<int>(packet.size()));
        }
    }
    if (is_team_msg && demo_record_active()) {
        // The recorder is teamless, so the team-filtered relay never reaches it.
        demo_record_capture_team_scoped(packet.data(), packet.size(), sender.team);
    }
    server_bots_on_chat_message(sender, msg, is_team_msg);
}

void server_bots_on_chat_message(const rf::Player& sender, const std::string_view message, const bool is_team_msg)
{
    for (const auto& bot : g_server_bots) {
        if (!is_team_msg || bot->ctx.player->team == sender.team) {
            bot_chat_manager_on_remote_chat_message(bot->ctx, sender, message);
        }
    }
}

void server_bots_on_limbo_enter()
{
    for (const auto& bot : g_server_bots) {
        bot_chat_manager_on_limbo_enter(bot->ctx, *bot->ctx.player);
    }
}

void server_bots_on_player_destroyed(const rf::Player* const player)
{
    const auto it = std::find_if(g_server_bots.begin(), g_server_bots.end(), [player](const auto& bot) {
        return bot->ctx.player == player;
    });
    if (it == g_server_bots.end()) {
        return;
    }
    if (static_cast<size_t>(it - g_server_bots.begin()) < g_server_bot_think_cursor) {
        --g_server_bot_think_cursor;
    }
    g_server_bots.erase(it);
}

void server_bots_on_multi_stop()
{
    // A dedicated server seeds its roster from the config; a listen host's next game starts without bots.
    g_server_bot_roster.clear();
    g_server_bot_removals.clear();
    g_server_bot_think_cursor = 0;
    g_server_bot_create_timer.invalidate();
}

void server_bots_do_frame()
{
    if (!rf::is_server
        || (g_server_bot_roster.empty() && g_server_bots.empty() && g_server_bot_removals.empty())) {
        return;
    }
    server_bots_process_removals();
    // A listen host's menus are pushed over gameplay and the MP world keeps simulating under them (0x00436160);
    // limbo and level loads replace the whole state stack.
    const bool in_gameplay = (rf::level.flags & rf::LEVEL_LOADED) && gameseq_is_stacked(rf::GS_GAMEPLAY);
    server_bots_reconcile(in_gameplay);

    const size_t num_bots = g_server_bots.size();
    int64_t think_us = 0;
    size_t next_cursor = g_server_bot_think_cursor;
    for (size_t k = 0; k < num_bots; ++k) {
        const size_t i = (g_server_bot_think_cursor + k) % num_bots;
        ServerBot& bot = *g_server_bots[i];
        if (!in_gameplay) {
            server_bot_stop_idle_entity(bot);
            bot_context_reset_idle(bot.ctx);
            bot.pending_dt = 0.0f;
            continue;
        }
        bot.pending_dt += rf::frametime;
        const bool thinks = think_us < k_server_bot_think_budget_us;
        if (thinks) {
            bot.ctx.frame_dt = bot.pending_dt;
            bot.pending_dt = 0.0f;
            const int64_t think_start_us = timer::get_i64(1000000);
            bot_run_frame(bot.ctx);
            think_us += timer::get_i64(1000000) - think_start_us;
            if (think_us >= k_server_bot_think_budget_us) {
                next_cursor = (i + 1) % num_bots;
            }
        }
        server_bot_apply_outputs(bot, thinks);
    }
    g_server_bot_think_cursor = next_cursor;
}

void server_bots_apply_patches()
{
    server_bot_pack_obj_update_hook.install();
    listen_host_chat_add_msg_hook.install();
}
