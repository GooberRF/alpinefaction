#include <algorithm>
#include <regex>
#include <xlog/xlog.h>
#include <winsock2.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <common/version/version.h>
#include <common/utils/list-utils.h>
#include "multi.h"
#include "endgame_votes.h"
#include "multi_private.h"
#include "alpine_packets.h"
#include "server_internal.h"
#include "gametype.h"
#include "../hud/hud.h"
#include "../hud/multi_spectate.h"
#include "../rf/file/file.h"
#include "../rf/level.h"
#include "../os/console.h"
#include "../misc/misc.h"
#include "../misc/alpine_settings.h"
#include "../rf/os/os.h"
#include "../rf/event.h"
#include "../rf/gameseq.h"
#include "../rf/os/timer.h"
#include "../rf/player/camera.h"
#include "../rf/multi.h"
#include "../rf/os/console.h"
#include "../rf/weapon.h"
#include "../rf/entity.h"
#include "../rf/player/player.h"
#include "../rf/localize.h"
#include "../rf/ai.h"
#include "../rf/item.h"
#include "../main/main.h"
#include "../graphics/gr.h"

// Note: this must be called from DLL init function
// Note: we can't use global variable because that would lead to crash when launcher loads this DLL to check dependencies
static rf::CmdLineParam& get_url_cmd_line_param()
{
    static rf::CmdLineParam url_param{"-url", "", true};
    return url_param;
}

static rf::CmdLineParam& get_levelm_cmd_line_param()
{
    static rf::CmdLineParam levelm_param{"-levelm", "", true};
    return levelm_param;
}

void handle_url_param()
{
    if (!get_url_cmd_line_param().found()) {
        return;
    }

    const char* url = get_url_cmd_line_param().get_arg();
    std::regex e{R"(^rf://([\w\.-]+):(\d+)/?(?:\?password=(.*))?$)"};
    std::cmatch cm;
    if (!std::regex_match(url, cm, e)) {
        xlog::warn("Unsupported URL: {}", url);
        return;
    }

    auto host_name = cm[1].str();
    auto port = static_cast<uint16_t>(std::stoi(cm[2].str()));
    auto password = cm[3].str();

    hostent* hp = gethostbyname(host_name.c_str());
    if (!hp) {
        xlog::warn("URL host lookup failed");
        return;
    }

    if (hp->h_addrtype != AF_INET) {
        xlog::warn("Unsupported address type (only IPv4 is supported)");
        return;
    }

    rf::console::print("Connecting to {}:{}...", host_name, port);
    auto host = ntohl(reinterpret_cast<in_addr *>(hp->h_addr_list[0])->S_un.S_addr);

    rf::NetAddr addr{host, port};
    start_join_multi_game_sequence(addr, password);
}

void handle_levelm_param()
{
    // do nothing unless -levelm is specified
    if (!get_levelm_cmd_line_param().found()) {
        return;
    }

    std::string level_filename = get_levelm_cmd_line_param().get_arg();

    //rf::console::print("filename {}", level_filename);
    start_levelm_load_sequence(level_filename);
}

FunHook<void()> multi_limbo_init{
    0x0047C280,
    []() {
        rf::activate_all_events_of_type(rf::EventType::When_Round_Ends, -1, -1, true);

        int limbo_time = 10000;

        if (rf::is_server) {
            server_on_limbo_state_enter();
            multi_player_set_can_endgame_vote(false); // servers can't endgame vote

            if (g_match_info.match_active) {
                af_broadcast_automated_chat_msg("\xA6 Match complete!");
                g_match_info.reset();
            }
            else if (g_match_info.pre_match_active && g_match_info.everyone_ready) {
                limbo_time = 5000; // reduce limbo time to 5 sec on match start (will always be a restart of the current map)
                g_match_info.match_active = g_match_info.everyone_ready;
                g_match_info.everyone_ready = false;
                g_match_info.pre_match_active = false;
            }
            else if (g_match_info.pre_match_active) {
                cancel_match(); // cancel match if map forcefully ends during pre-match phase
            }
        }

        // don't let clients vote if the map has been played for less than 1 min
        else if(rf::level.time >= 60.0f) {
            multi_player_set_can_endgame_vote(true);
        }

        // purge any vote or ready notifications on level end
        if (!rf::is_server && !rf::is_dedicated_server) {
            remove_hud_vote_notification();
            set_local_pre_match_active(false);
        }

        if (!rf::player_list) {
            xlog::trace("Wait between levels shortened because server is empty");
            limbo_time = 100;
        }

        rf::multi_limbo_timer.set(limbo_time);

        if (!rf::local_player)
            return;

        rf::camera_enter_random_fixed_pos();
        rf::camera_enter_fixed(rf::local_player->cam);
        rf::local_screen_flash(rf::local_player, 0xFF, 0xFF, 0xFF, 0x01);

        const auto gt = rf::multi_get_game_type();
        bool we_win = false;

        if (gt == rf::NG_TYPE_DM) {
            // need at least 2 players to possibly win
            if (rf::multi_num_players() >= 2) {
                int my_score = 0;
                int max_score = 0;

                if (rf::local_player->stats)
                    my_score = rf::local_player->stats->score;

                for (auto& p : SinglyLinkedList{rf::player_list}) {
                    if (p.stats && p.stats->score > max_score)
                        max_score = p.stats->score;
                }

                we_win = (my_score >= max_score); // count it as a win if tied for win
            }
        }
        else if (gt != rf::NG_TYPE_RUN) { // no winner in run
            int red = 0, blue = 0;
            switch (gt) {
            case rf::NG_TYPE_CTF: {
                red = rf::multi_ctf_get_red_team_score();
                blue = rf::multi_ctf_get_blue_team_score();
                break;
            }
            case rf::NG_TYPE_TEAMDM: {
                red = rf::multi_tdm_get_red_team_score();
                blue = rf::multi_tdm_get_blue_team_score();
                break;
            }
            case rf::NG_TYPE_DC:
            case rf::NG_TYPE_KOTH: {
                red = multi_koth_get_red_team_score();
                blue = multi_koth_get_blue_team_score();
                break;
            }
            case rf::NG_TYPE_REV: {
                red = static_cast<int>(rev_all_points_permalocked());
                blue = static_cast<int>(!rev_all_points_permalocked());
                break;
            }
            case rf::NG_TYPE_ESC: {
                for (const auto& hill : g_koth_info.hills) {
                    if (hill.ownership == HillOwner::HO_Red) {
                        red++;
                    }
                    else if (hill.ownership == HillOwner::HO_Blue) {
                        blue++;
                    }
                }
                break;
            }
            default:
                break;
            }
            if (rf::local_player->team == 0)
                we_win = (red > blue);
            else if (rf::local_player->team == 1)
                we_win = (blue > red);
        }

        rf::snd_play(we_win ? 80 : 78, 0, 0.0f, 1.0f);
    },
};

CodeInjection multi_start_injection{
    0x0046D5B0,
    []() {
        void debug_multi_init();
        debug_multi_init();
        void reset_restricted_cmds_on_init_multi();
        reset_restricted_cmds_on_init_multi();
        if (g_alpine_game_config.try_disable_textures) {
            evaluate_lightmaps_only();
        }        
    },
};

CodeInjection ctf_flag_return_fix{
    0x0047381D,
    [](auto& regs) {
        auto stack_frame = regs.esp + 0x1C;
        bool red = addr_as_ref<bool>(stack_frame + 4);
        if (red) {
            regs.eip = 0x00473827;
        }
        else {
            regs.eip = 0x00473822;
        }
    },
};

FunHook<void()> multi_ctf_level_init_hook{
    0x00472E30,
    []() {
        multi_ctf_level_init_hook.call_target();
        // Make sure CTF flag does not spin in new level if it was dropped in the previous level
        int info_index = rf::item_lookup_type("flag_red");
        if (info_index >= 0) {
            rf::item_info[info_index].flags &= ~rf::IIF_SPINS_IN_MULTI;
        }
        info_index = rf::item_lookup_type("flag_blue");
        if (info_index >= 0) {
            rf::item_info[info_index].flags &= ~rf::IIF_SPINS_IN_MULTI;
        }
    },
};

rf::Timestamp g_select_weapon_done_timestamp[rf::multi_max_player_id];

bool multi_is_selecting_weapon(rf::Player* pp)
{
    auto& done_timestamp = g_select_weapon_done_timestamp[pp->net_data->player_id];
    return done_timestamp.valid() && !done_timestamp.elapsed();
}

void server_set_player_weapon(rf::Player* pp, rf::Entity* ep, int weapon_type)
{
    rf::player_make_weapon_current_selection(pp, weapon_type);
    ep->ai.current_primary_weapon = weapon_type;
    g_select_weapon_done_timestamp[pp->net_data->player_id].set(300);
}

FunHook<void(rf::Player*, rf::Entity*, int)> multi_select_weapon_server_side_hook{
    0x004858D0,
    [](rf::Player *pp, rf::Entity *ep, int weapon_type) {
        if (weapon_type == -1 || ep->ai.current_primary_weapon == weapon_type) {
            // Nothing to do
            return;
        }
        if (g_alpine_server_config_active_rules.gungame.enabled &&
            !((ep->ai.current_primary_weapon == 1 && weapon_type == 0) || (ep->ai.current_primary_weapon == 0 && weapon_type == 1))) {
            // af_send_automated_chat_msg("Weapon switch denied. In GunGame, you get new weapons by getting frags.", pp);
            return;
        }
        bool has_weapon;
        if (weapon_type == rf::remote_charge_det_weapon_type) {
            has_weapon = rf::ai_has_weapon(&ep->ai, rf::remote_charge_weapon_type);
        }
        else {
            has_weapon = rf::ai_has_weapon(&ep->ai, weapon_type);
        }
        if (!has_weapon) {
            xlog::debug("Player {} attempted to select an unpossesed weapon {}", pp->name, weapon_type);
        }
        else if (multi_is_selecting_weapon(pp)) {
            xlog::debug("Player {} attempted to select weapon {} while selecting weapon {}",
                pp->name, weapon_type, ep->ai.current_primary_weapon);
        }
        else if (rf::entity_is_reloading(ep)) {
            xlog::debug("Player {} attempted to select weapon {} while reloading weapon {}",
                pp->name, weapon_type, ep->ai.current_primary_weapon);
        }
        else {
            rf::player_make_weapon_current_selection(pp, weapon_type);
            ep->ai.current_primary_weapon = weapon_type;
            g_select_weapon_done_timestamp[pp->net_data->player_id].set(300);
        }
    },
};

void multi_reload_weapon_server_side(rf::Player* pp, int weapon_type)
{
    rf::Entity* ep = rf::entity_from_handle(pp->entity_handle);
    if (!ep) {
        // Entity is dead
    }
    else if (ep->ai.current_primary_weapon != weapon_type) {
        xlog::debug("Player {} attempted to reload unselected weapon {}", pp->name, weapon_type);
    }
    else if (multi_is_selecting_weapon(pp)) {
        xlog::debug("Player {} attempted to reload weapon {} while selecting it", pp->name, weapon_type);
    }
    else if (rf::entity_is_reloading(ep)) {
        xlog::debug("Player {} attempted to reload weapon {} while reloading it", pp->name, weapon_type);
    }
    else {
        rf::entity_reload_current_primary(ep, false, false);
    }
}

void multi_ensure_ammo_is_not_empty(rf::Entity* ep)
{
    int weapon_type = ep->ai.current_primary_weapon;
    auto& wi = rf::weapon_types[weapon_type];
    // at least ammo for 100 milliseconds
    int min_ammo = std::max(static_cast<int>(0.1f / wi.fire_wait), 1);
    if (rf::weapon_is_melee(weapon_type)) {
        return;
    }
    if (rf::weapon_uses_clip(weapon_type)) {
        auto& clip_ammo = ep->ai.clip_ammo[weapon_type];
        clip_ammo = std::max(clip_ammo, min_ammo);
    }
    else {
        auto& ammo = ep->ai.ammo[wi.ammo_type];
        ammo = std::max(ammo, min_ammo);
    }
}

void multi_turn_weapon_on(rf::Entity* ep, rf::Player* pp, bool alt_fire)
{
    // Note: pp is always null client-side
    auto weapon_type = ep->ai.current_primary_weapon;
    if (!rf::weapon_is_on_off_weapon(weapon_type, alt_fire)) {
        xlog::debug("Player {} attempted to turn on weapon {} which has no continous fire flag", ep->name, weapon_type);
    }
    else if (rf::is_server && multi_is_selecting_weapon(pp)) {
        xlog::debug("Player {} attempted to turn on weapon {} while selecting it", ep->name, weapon_type);
    }
    else if (rf::is_server && rf::entity_is_reloading(ep)) {
        xlog::debug("Player {} attempted to turn on weapon {} while reloading it", ep->name, weapon_type);
    }
    else {
        if (!rf::is_server) {
            // Make sure client-side ammo is not empty when we know that the player is currently shooting
            // It can be empty if a reload packet was lost or if it got desynced because of network delays
            multi_ensure_ammo_is_not_empty(ep);
        }
        rf::entity_turn_weapon_on(ep->handle, weapon_type, alt_fire);
    }
}

void multi_turn_weapon_off(rf::Entity* ep)
{
    auto current_primary_weapon = ep->ai.current_primary_weapon;
    if (rf::weapon_is_on_off_weapon(current_primary_weapon, false)
        || rf::weapon_is_on_off_weapon(current_primary_weapon, true)) {

        rf::entity_turn_weapon_off(ep->handle, current_primary_weapon);
    }
}

bool weapon_uses_ammo(int weapon_type, bool alt_fire)
{
    if (rf::weapon_is_detonator(weapon_type)) {
         return false;
    }
    if (rf::weapon_is_riot_stick(weapon_type) && alt_fire) {
        return true;
    }
    rf::WeaponInfo* winfo = &rf::weapon_types[weapon_type];
    return !(winfo->flags & rf::WTF_MELEE);
}

bool is_entity_out_of_ammo(rf::Entity *entity, int weapon_type, bool alt_fire)
{
    if (!weapon_uses_ammo(weapon_type, alt_fire)) {
        return false;
    }
    rf::WeaponInfo* winfo = &rf::weapon_types[weapon_type];
    if (winfo->clip_size == 0) {
        auto ammo = entity->ai.ammo[winfo->ammo_type];
        return ammo == 0;
    }
    auto clip_ammo = entity->ai.clip_ammo[weapon_type];
    return clip_ammo == 0;
}
void send_private_message_for_cancelled_shot(rf::Player* player, const std::string& reason)
{
    auto message = std::format("\xA6 Shot canceled: {}", reason);
    af_send_automated_chat_msg(message, player);
}

bool multi_is_player_firing_too_fast(rf::Player* pp, int weapon_type)
{
    // do not consider melee weapons for click limiter
    if (rf::weapon_is_melee(weapon_type)) {
        return false;
    }

    int player_id = pp->net_data->player_id;
    int now = rf::timer_get(1000);

    static std::vector<int> last_weapon_id(rf::multi_max_player_id, 0);
    static std::vector<int> last_weapon_fire(rf::multi_max_player_id, 0);

    int fire_wait_ms = 0;

    if (rf::weapon_is_semi_automatic(weapon_type)) {

        // if semi auto click limit is on
        if (get_af_server_info().has_value() &&
            get_af_server_info()->click_limit) {

            // use override value for pistol/PR in stock game
            // stock game pistol has alt fire wait of 200ms, so can't use normal min logic for it
            if (rf::weapon_get_fire_wait_ms(weapon_type, 0) == 500) {
                fire_wait_ms = get_semi_auto_fire_wait_override();
            }
            // otherwise use the minimum fire wait between both modes in weapons.tbl
            else {
                fire_wait_ms = std::min(rf::weapon_get_fire_wait_ms(weapon_type, 0),  // primary
                    rf::weapon_get_fire_wait_ms(weapon_type, 1)); // alt
            }
        }
        // otherwise, don't enforce a limit for semi autos
        else {
            fire_wait_ms = 0;
        }
    }
    // for automatic weapons, use the minimum fire wait between both modes in weapons.tbl
    else {
        fire_wait_ms = std::min(rf::weapon_get_fire_wait_ms(weapon_type, 0),  // primary
            rf::weapon_get_fire_wait_ms(weapon_type, 1)); // alt
    }

    // reset if weapon changed
    if (last_weapon_id[player_id] != weapon_type) {
        last_weapon_fire[player_id] = 0;
        last_weapon_id[player_id] = weapon_type;
    }

    // check if time since last shot is less than minimum wait time
    int time_since_last_shot = std::max(0, now - last_weapon_fire[player_id]); // ensure time is positive

    // calculate server-enforced cooldown from weapon fire wait, halfping, and 50ms jitter tolerance
    int adjusted_ping = std::max(0, pp->net_data->ping); // ensure ping is positive
    int cooldown_threshold = std::max(0, fire_wait_ms - (adjusted_ping / 2) - 50); // halfping and jitter tolerance
    if (time_since_last_shot < cooldown_threshold) {
        // send notification to player for firing too fast
        //send_private_message_for_cancelled_shot(pp, "firing too fast!");

        return true;
    }

    // we fired
    last_weapon_fire[player_id] = now;
    return false;
}

bool multi_is_weapon_fire_allowed_server_side(rf::Entity *ep, int weapon_type, bool alt_fire)
{
    rf::Player* pp = rf::player_from_entity_handle(ep->handle);
    if (ep->ai.current_primary_weapon != weapon_type) {
        xlog::debug("Player {} attempted to fire unselected weapon {}", pp->name, weapon_type);
    }
    else if (is_entity_out_of_ammo(ep, weapon_type, alt_fire)) {
        xlog::debug("Player {} attempted to fire weapon {} without ammunition", pp->name, weapon_type);
    }
    else if (rf::weapon_is_on_off_weapon(weapon_type, alt_fire)) {
        xlog::debug("Player {} attempted to fire a single bullet from on/off weapon {}", pp->name, weapon_type);
    }
    else if (multi_is_selecting_weapon(pp)) {
        xlog::debug("Player {} attempted to fire weapon {} while selecting it", pp->name, weapon_type);
    }
    // causes shots fired immediately after reloading to be cancelled (especially noticable with shotgun)
    // is because entity_is_reloading looks at anim length and some anims are longer than the actual reload time
    // todo: make new entity_is_reloading function that calculates based on reload start and duration
    //else if (rf::entity_is_reloading(ep)) {
    //    xlog::debug("Player {} attempted to fire weapon {} while reloading it", pp->name, weapon_type);
    //}
    else if (!multi_is_player_firing_too_fast(pp, weapon_type)) {
        return true;
    }
    return false;
}

FunHook<void(rf::Entity*, int, rf::Vector3&, rf::Matrix3&, bool)> multi_process_remote_weapon_fire_hook{
    0x0047D220,
    [](rf::Entity *ep, int weapon_type, rf::Vector3& pos, rf::Matrix3& orient, bool alt_fire) {
        if (rf::is_server) {
            // Do some checks server-side to prevent cheating
            if (!multi_is_weapon_fire_allowed_server_side(ep, weapon_type, alt_fire)) {
                return;
            }
        }
        multi_process_remote_weapon_fire_hook.call_target(ep, weapon_type, pos, orient, alt_fire);

        // Notify spectate system of weapon fire so the fpgun fire animation is triggered.
        // Skip thrown projectile weapons (grenade, C4, flamethrower canister alt-fire) because
        // their animation is driven earlier and at the correct time by entity_play_attack_anim_spectate_hook.
        if (!rf::is_server) {
            bool is_thrown = (weapon_type == rf::grenade_weapon_type)
                || (weapon_type == rf::remote_charge_weapon_type)
                || (rf::weapon_is_flamethrower(weapon_type) && alt_fire);
            if (!is_thrown) {
                multi_spectate_on_obj_update_fire(ep, alt_fire);
            }
        }
    },
};

void multi_init_player(rf::Player* player)
{
    multi_kill_init_player(player);
}

std::string_view multi_game_type_name(const rf::NetGameType game_type) {
    if (game_type == rf::NG_TYPE_DM) {
        return std::string_view{"Deathmatch"};
    } else if (game_type == rf::NG_TYPE_CTF) {
        return std::string_view{"Capture the Flag"};
    } else if (game_type == rf::NG_TYPE_KOTH) {
        return std::string_view{"King of the Hill"};
    } else if (game_type == rf::NG_TYPE_DC) {
        return std::string_view{"Damage Control"};
    } else if (game_type == rf::NG_TYPE_REV) {
        return std::string_view{"Revolt"};
    } else if (game_type == rf::NG_TYPE_RUN) {
        return std::string_view{"Run"};
    } else if (game_type == rf::NG_TYPE_ESC) {
        return std::string_view{"Escalation"};
    } else {
        if (game_type != rf::NG_TYPE_TEAMDM) {
            xlog::warn("{} is an invalid `NetGameType`", static_cast<int>(game_type));
        }
        return std::string_view{"Team Deathmatch"};
    }
}

std::string_view multi_game_type_name_upper(const rf::NetGameType game_type) {
    if (game_type == rf::NG_TYPE_DM) {
        return std::string_view{rf::strings::deathmatch};
    } else if (game_type == rf::NG_TYPE_CTF) {
        return std::string_view{rf::strings::capture_the_flag};
    } else if (game_type == rf::NG_TYPE_KOTH) {
        return std::string_view{"KING OF THE HILL"};
    } else if (game_type == rf::NG_TYPE_DC) {
        return std::string_view{"DAMAGE CONTROL"};
    } else if (game_type == rf::NG_TYPE_REV) {
        return std::string_view{"REVOLT"};
    } else if (game_type == rf::NG_TYPE_RUN) {
        return std::string_view{"RUN"};
    } else if (game_type == rf::NG_TYPE_ESC) {
        return std::string_view{"ESCALATION"};
    } else {
        if (game_type != rf::NG_TYPE_TEAMDM) {
            xlog::warn("{} is an invalid `NetGameType`", static_cast<int>(game_type));
        }
        return std::string_view{rf::strings::team_deathmatch};
    }
}

std::string_view multi_game_type_name_short(const rf::NetGameType game_type) {
    if (game_type == rf::NG_TYPE_DM) {
        return std::string_view{"DM"};
    } else if (game_type == rf::NG_TYPE_CTF) {
        return std::string_view{"CTF"};
    } else if (game_type == rf::NG_TYPE_KOTH) {
        return std::string_view{"KOTH"};
    } else if (game_type == rf::NG_TYPE_DC) {
        return std::string_view{"DC"};
    } else if (game_type == rf::NG_TYPE_REV) {
        return std::string_view{"REV"};
    } else if (game_type == rf::NG_TYPE_RUN) {
        return std::string_view{"RUN"};
    } else if (game_type == rf::NG_TYPE_ESC) {
        return std::string_view{"ESC"};
    } else {
        if (game_type != rf::NG_TYPE_TEAMDM) {
            xlog::warn("{} is an invalid `NetGameType`", static_cast<int>(game_type));
        }
        return std::string_view{"TDM"};
    }
}

std::string_view multi_game_type_prefix(const rf::NetGameType game_type) {
    if (game_type == rf::NG_TYPE_DM) {
        return std::string_view{"dm"};
    } else if (game_type == rf::NG_TYPE_TEAMDM) {
        return std::string_view{"dm"};
    } else if (game_type == rf::NG_TYPE_CTF) {
        return std::string_view{"ctf"};
    } else if (game_type == rf::NG_TYPE_KOTH) {
        return std::string_view{"koth"};
    } else if (game_type == rf::NG_TYPE_DC) {
        return std::string_view{"dc"};
    } else if (game_type == rf::NG_TYPE_REV) {
        return std::string_view{"rev"};
    } else if (game_type == rf::NG_TYPE_RUN) {
        return std::string_view{"run"};
    } else if (game_type == rf::NG_TYPE_ESC) {
        return std::string_view{"esc"};
    } else {
        if (game_type != rf::NG_TYPE_TEAMDM) {
            xlog::warn("{} is an invalid `NetGameType`", static_cast<int>(game_type));
        }
        return std::string_view{"dm"};
    }
}

int multi_num_spawned_players() {
    return std::ranges::count_if(SinglyLinkedList{rf::player_list}, [] (const auto& p) {
        return !rf::player_is_dead(&p) && !rf::player_is_dying(&p);
    });
}

void configure_custom_gametype_listen_server_settings() {
    // reset to defaults
    g_alpine_server_config = AlpineServerConfig{};
    g_alpine_server_config_active_rules = AlpineServerConfigRules{};
    set_upcoming_game_type(rf::netgame.type);

    auto& rules = g_alpine_server_config_active_rules;
    rules.game_type = rf::netgame.type;
    apply_defaults_for_game_type(rules.game_type, rules);
    rules.set_koth_score_limit(3600);
    rules.set_dc_score_limit(3600);
}

void start_level_in_multi(std::string filename) {

    auto [is_valid, valid_filename] = is_level_name_valid(filename);

    if (is_valid) {
        rf::netgame.levels.add(valid_filename.c_str());
        rf::netgame.max_time_seconds = 3600.0f;
        rf::netgame.max_kills = 30;
        rf::netgame.geomod_limit = 64;
        rf::netgame.max_captures = 5;
        rf::netgame.flags = 0; // no broadcast to tracker
        rf::netgame.type = string_istarts_with(filename, "ctf") ? rf::NetGameType::NG_TYPE_CTF
            : string_istarts_with(filename, "koth") ? rf::NetGameType::NG_TYPE_KOTH
            : string_istarts_with(filename, "dc") ? rf::NetGameType::NG_TYPE_DC
            : string_istarts_with(filename, "rev") ? rf::NetGameType::NG_TYPE_REV
            : string_istarts_with(filename, "run") ? rf::NetGameType::NG_TYPE_RUN
            : string_istarts_with(filename, "esc") ? rf::NetGameType::NG_TYPE_ESC
            : rf::NetGameType::NG_TYPE_DM;
        rf::netgame.name = "Alpine Faction Test Server";
        rf::netgame.password = "password";

        configure_custom_gametype_listen_server_settings();

        rf::set_in_mp_flag();
        rf::multi_start(0, 0);
        rf::multi_hud_clear_chat();
        rf::multi_load_next_level();
        rf::multi_init_server();
    }
}

CodeInjection multi_customize_listen_server_settings_patch {
    0x0044E485,
    [](auto& regs) {
        configure_custom_gametype_listen_server_settings();
    },
};

ConsoleCommand2 levelm_cmd{
    "levelm",
    [](std::string filename) {
        if (rf::gameseq_get_state() == rf::GS_MAIN_MENU ||
            rf::gameseq_get_state() == rf::GS_EXTRAS_MENU) {
            start_level_in_multi(filename);
            rf::console::print("Starting local multiplayer game on {}", filename);
        }
        else {
            rf::console::print("You must run this command from the main menu!");
        }
    },
    "Start a local multiplayer game on the specified level",
    "levelm <filename>",
};

DcCommandAlias mapm_cmd{
    "mapm",
    levelm_cmd,
};

ConsoleCommand2 mapver_cmd{
    "dbg_mapver",
    [](std::string filename) {
        // Append .rfl if missing
        if (filename.find(".rfl") == std::string::npos) {
            filename += ".rfl";
        }

        int map_ver = get_level_file_version(filename);
        if (map_ver == -1) {
            rf::console::print("Level {} not found.", filename);
        }
        else {
            std::string version_text;

            if (map_ver == 175) {
                version_text = "Official - PS2 retail";
            }
            else if (map_ver == 180) {
                version_text = "Official - PC retail";
            }
            else if (map_ver == 200) {
                version_text = "Community - RF/PF/DF";
            }
            else if (map_ver > 0 && map_ver < 200) {
                version_text = "Official - Internal";
            }
            else if (map_ver >= 300) {
                version_text = "Community - Alpine";
            }
            else {
                version_text = "Unsupported";
            }

            rf::console::print("RFL version for level {} is {} ({}). You can {}load this map.", filename,
                               map_ver, version_text, map_ver > MAXIMUM_RFL_VERSION ? " NOT" : "");
        }
    },
    "Check the version of a specific level file",
    "dbg_mapver <filename>",
};

void mp_send_handicap_request(bool force) {
    if (force || g_alpine_game_config.desired_handicap > 0) {
        af_send_handicap_request(static_cast<uint8_t>(g_alpine_game_config.desired_handicap));
    }
}

ConsoleCommand2 set_handicap_cmd{
    "mp_handicap",
    [](std::optional<int> new_handicap) {
        if (new_handicap) {
            g_alpine_game_config.set_desired_handicap(new_handicap.value());
            mp_send_handicap_request(true);
        }
        rf::console::print("Your desired damage reduction handicap is {}. It will only be applied in servers that support this feature.", g_alpine_game_config.desired_handicap);
    },
    "Set desired multiplayer damage reduction handicap",
};

CallHook<float(int, float, int, int, int, rf::PCollisionOut*, int, bool)> obj_apply_damage_lava_hook{
    {
        0x004212A1,
        0x004212D4
    },
    [](int obj_handle, float damage, int killer_handle, int a4, int damage_type, rf::PCollisionOut* collide_out, int resp_ent_uid, bool force) {
        // use obj_handle for killer_handle on servers so players kill themselves in lava and acid instead of
        // "killed mysteriously" or a random player getting credit for the kill
        // on clients, use killer_handle as passed (-1) so players visually ignite in lava for clients
        int killer_handle_new = rf::is_dedicated_server || rf::is_server ? obj_handle : killer_handle;
        return obj_apply_damage_lava_hook.call_target(obj_handle, damage, killer_handle_new, a4, damage_type, collide_out, resp_ent_uid, force);
    }
};

CallHook<void(const char* filename)> level_cmd_multi_change_level_hook{
    0x00435108,
    [](const char* filename) {
        if (rf::is_multi)
            set_manually_loaded_level(true); // "level" console command
        level_cmd_multi_change_level_hook.call_target(filename);
    }
};

void multi_do_patch()
{
    multi_limbo_init.install();
    multi_start_injection.install();

    // Fix CTF flag not returning to the base if the other flag was returned when the first one was waiting
    ctf_flag_return_fix.install();

    // Weapon select server-side handling
    multi_select_weapon_server_side_hook.install();

    // Check ammo server-side when handling weapon fire packets
    multi_process_remote_weapon_fire_hook.install();

    // Make sure CTF flag does not spin in new level if it was dropped in the previous level
    multi_ctf_level_init_hook.install();

    // Set custom listen server settings based on gametype
    multi_customize_listen_server_settings_patch.install();

    multi_kill_do_patch();
    faction_files_do_patch();
    level_download_do_patch();
    network_init();
    multi_tdm_apply_patch();

    level_download_init();
    multi_ban_apply_patch();

    // Fix lava damage sometimes being attributed to a player
    obj_apply_damage_lava_hook.install();

    // Flag manually loaded levels from "level" command
    level_cmd_multi_change_level_hook.install();

    // Init cmd line param
    get_url_cmd_line_param();
    get_levelm_cmd_line_param();

    // console commands
    levelm_cmd.register_cmd();
    mapver_cmd.register_cmd();
    mapm_cmd.register_cmd();
    set_handicap_cmd.register_cmd();
}

void multi_after_full_game_init()
{
    populate_gametype_table();
    handle_url_param();
    handle_levelm_param();
}
