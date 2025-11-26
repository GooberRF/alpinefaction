#include "player.h"
#include "../rf/player/player.h"
#include "../rf/player/camera.h"
#include "../rf/entity.h"
#include "../rf/multi.h"
#include "../rf/sound/sound.h"
#include "../rf/bmpman.h"
#include "../rf/weapon.h"
#include "../rf/level.h"
#include "../rf/hud.h"
#include "../rf/input.h"
#include "../rf/collide.h"
#include "../rf/gr/gr_light.h"
#include "../rf/os/os.h"
#include "../rf/os/frametime.h"
#include "../os/console.h"
#include "../main/main.h"
#include "../misc/alpine_options.h"
#include "../misc/alpine_settings.h"
#include "../sound/sound.h"
#include "../input/input.h"
#include "../multi/multi.h"
#include "../multi/gametype.h"
#include "../multi/server_internal.h"
#include "../hud/multi_spectate.h"
#include "../hud/hud.h"
#include "../multi/alpine_packets.h"
#include "../hud/hud_world.h"
#include <common/utils/list-utils.h>
#include <common/config/GameConfig.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>

std::unordered_map<const rf::Player*, PlayerAdditionalData> g_player_additional_data_map;
static rf::PlayerHeadlampSettings g_local_headlamp_settings;

void find_player(const StringMatcher& query, std::function<void(rf::Player*)> consumer)
{
    auto player_list = SinglyLinkedList{rf::player_list};
    for (auto& player : player_list) {
        if (query(player.name))
            consumer(&player);
    }
}

void reset_player_additional_data(const rf::Player* const player)
{
    g_player_additional_data_map.erase(player);
}

PlayerAdditionalData& get_player_additional_data(const rf::Player* const player)
{
    return g_player_additional_data_map[player];
}

// used for compatibility checks
bool is_player_minimum_af_client_version(rf::Player* player, int version_major, int version_minor) {
    if (!player) {
        return false;
    }

    const auto it = g_player_additional_data_map.find(player);
    if (it == g_player_additional_data_map.end()) {
        return false;
    }

    const auto& info = it->second;
    if (info.client_version != ClientVersion::alpine_faction) {
        return false;
    }

    if (info.client_version_major > version_major) {
        return true;
    }
    if (info.client_version_major < version_major) {
        return false;
    }

    return info.client_version_minor >= version_minor;
}

bool is_server_minimum_af_version(int version_major, int version_minor) {
    auto& server_info = get_df_server_info();

    if (!server_info.has_value()) {
        return false;
    }

    return server_info->version_major >= version_major && server_info->version_minor >= version_minor;
}

std::string build_local_spawn_string(bool can_respawn) {
    std::string respawn_timer_notification_text = "";
    if (!can_respawn) {
        respawn_timer_notification_text = "You are not allowed to spawn right now";
    }
    else if (!g_respawn_timer_local.valid() || g_respawn_timer_local.elapsed()) {
        if (g_local_queued_delayed_spawn) {
            respawn_timer_notification_text = "Spawning...";
        }
        else {
            std::string respawn_key_text = get_action_bind_name(rf::ControlConfigAction::CC_ACTION_SECONDARY_ATTACK);
            respawn_timer_notification_text = "Press " + respawn_key_text + " to spawn";
        }
    }
    else {
        int ms_left = g_respawn_timer_local.time_until();

        int tenths = (ms_left > 0) ? ((ms_left + 99) / 100) : 0;

        if (tenths > 0) {
            int whole = tenths / 10;
            int frac = tenths % 10;
            bool singular = (tenths == 10);
            respawn_timer_notification_text =
                std::format("{} in {}.{} second{}", g_local_queued_delayed_spawn ? "Spawning" : "You can spawn", whole, frac, singular ? "" : "s");
        }
        else {
            respawn_timer_notification_text.clear();
        }
    }
    return respawn_timer_notification_text;
}

void set_local_spawn_delay(bool can_respawn, bool force_respawn, int spawn_delay) {
    // only called by Alpine packet handler when ADS server has configured spawn delay
    if (!rf::is_multi || rf::is_dedicated_server) {
        return; // safety, should never happen
    }    

    g_local_queued_delayed_spawn = can_respawn && force_respawn;
    g_respawn_timer_local.set(spawn_delay);
    //xlog::warn("setting delay {}, {}", spawn_delay, g_respawn_timer_local.time_until());
    draw_respawn_timer_notification(can_respawn, force_respawn, spawn_delay);
}

void attempt_spawn_local_player(rf::Player* pp) {
    if (!g_respawn_timer_local.valid() || g_respawn_timer_local.elapsed()) {
        // allow immediate retry after local delay timer resets, only used for click spawning
        // avoids niche case delays where you click with <1 sec remaining and death anim is still playing
        rf::local_spawn_attempt_timer.set(1);

        if (rf::is_server) { // listen server hosts
            rf::multi_spawn_player_server_side(pp);
        }
        else if (pp && pp->net_data) {
            rf::send_respawn_req_packet(pp->settings.multi_character, pp->net_data->player_id);
        }
        else {
            xlog::warn("Local delayed spawn was attempted but failed");
            return;
        }
        g_local_queued_delayed_spawn = false;
        stop_draw_respawn_timer_notification();
        return;
    }
    return;
}

void local_delayed_spawn_do_frame() {
    if (!g_local_queued_delayed_spawn) {
        return; // no delayed spawn queued
    }

    if (!rf::is_multi || rf::is_dedicated_server) {
        return; // safety, should never happen
    }

    attempt_spawn_local_player(rf::local_player);
}

void reset_local_delayed_spawn() {
    stop_draw_respawn_timer_notification();
    g_respawn_timer_local.invalidate();
}

CodeInjection player_execute_action_respawn_req_patch{ // click to spawn
    0x004A678B,
    [](auto& regs) {
        if ((get_df_server_info().has_value() && get_df_server_info()->delayed_spawns) ||
            (rf::is_server && g_alpine_server_config_active_rules.spawn_delay.enabled)) {
            g_local_queued_delayed_spawn = true;
            regs.eip = 0x004A67BB;
        }
    }
};

CodeInjection player_dying_frame_respawn_req_patch{ // force respawn
    0x004A6DCA,
    [](auto& regs) {
        if ((get_df_server_info().has_value() && get_df_server_info()->delayed_spawns) ||
            (rf::is_server && g_alpine_server_config_active_rules.spawn_delay.enabled)) {
            g_local_queued_delayed_spawn = true;
            regs.eip = 0x004A6DF8;
        }
    }
};

FunHook<rf::Player*(bool)> player_create_hook{
    0x004A3310,
    [](bool is_local) {
        rf::Player* player = player_create_hook.call_target(is_local);
        multi_init_player(player);
        return player;
    },
};

FunHook<void(rf::Player*)> player_destroy_hook{
    0x004A35C0,
    [](rf::Player* player) {        
        multi_spectate_on_destroy_player(player);
        reset_player_additional_data(player);
        player_destroy_hook.call_target(player);
        if (rf::is_server) {
            remove_ready_player_silent(player);
            server_vote_on_player_leave(player);
        }
    },
};

FunHook<rf::Entity*(rf::Player*, int, const rf::Vector3*, const rf::Matrix3*, int)> player_create_entity_hook{
    0x004A4130,
    [](rf::Player* pp, int entity_type, const rf::Vector3* pos, const rf::Matrix3* orient, int multi_entity_index) {
        rf::Entity* ep = player_create_entity_hook.call_target(pp, entity_type, pos, orient, multi_entity_index);
        if (ep) {
            multi_spectate_player_create_entity_post(pp, ep);
        }
        if (pp == rf::local_player) {
            // Update sound listener position so respawn sound is not classified as too quiet to play
            rf::Vector3 cam_pos = rf::camera_get_pos(pp->cam);
            rf::Matrix3 cam_orient = rf::camera_get_orient(pp->cam);
            rf::snd_update_sounds(cam_pos, rf::zero_vector, cam_orient);
            reset_local_delayed_spawn();
            multi_hud_on_local_spawn();
        }
        return ep;
    },
};

bool should_swap_weapon_alt_fire(rf::Player* player)
{
    auto* entity = rf::entity_from_handle(player->entity_handle);
    if (!entity) {
        return false;
    }

    // Check if local entity is attached to a parent (vehicle or torret)
    auto* parent = rf::entity_from_handle(entity->host_handle);
    if (parent) {
        return false;
    }

    if (g_alpine_game_config.swap_ar_controls && entity->ai.current_primary_weapon == rf::assault_rifle_weapon_type)
        return true;

    if (g_alpine_game_config.swap_gn_controls && entity->ai.current_primary_weapon == rf::grenade_weapon_type)
        return true;

    if (g_alpine_game_config.swap_sg_controls && entity->ai.current_primary_weapon == rf::shotgun_weapon_type)
        return true;

    return false;
}

bool is_player_weapon_on(rf::Player* player, bool alt_fire) {
    if (!player) {
        player = rf::local_player;
    }
    auto* entity = rf::entity_from_handle(player->entity_handle);
    bool is_continous_fire = rf::entity_weapon_is_on(entity->handle, entity->ai.current_primary_weapon);
    bool is_alt_fire_flag_set = (entity->ai.ai_flags & rf::AIF_ALT_FIRE) != 0;
    if (should_swap_weapon_alt_fire(player)) {
        is_alt_fire_flag_set = !is_alt_fire_flag_set;
    }
    return is_continous_fire && is_alt_fire_flag_set == alt_fire;
}

FunHook<void(rf::Player*, bool, bool)> player_fire_primary_weapon_hook{
    0x004A4E80,
    [](rf::Player* player, bool alt_fire, bool was_pressed) {
        if (should_swap_weapon_alt_fire(player)) {
            alt_fire = !alt_fire;
        }
        player_fire_primary_weapon_hook.call_target(player, alt_fire, was_pressed);
    },
};

CodeInjection stop_continous_primary_fire_patch{
    0x00430EC5,
    [](auto& regs) {
        rf::Entity* entity = regs.esi;
        if (is_player_weapon_on(entity->local_player, false)) {
            regs.eip = 0x00430EDF;
        }
        else {
            regs.eip = 0x00430EF2;
        }
    }
};

CodeInjection stop_continous_alternate_fire_patch{
    0x00430F09,
    [](auto& regs) {
        rf::Entity* entity = regs.esi;
        if (is_player_weapon_on(entity->local_player, true)) {
            regs.eip = 0x00430F23;
        }
        else {
            regs.eip = 0x00430F36;
        }
    }
};

ConsoleCommand2 swap_assault_rifle_controls_cmd{
    "cl_swaparcontrols",
    []() {
        g_alpine_game_config.swap_ar_controls = !g_alpine_game_config.swap_ar_controls;
        rf::console::print("Swap assault rifle controls: {}", g_alpine_game_config.swap_ar_controls ? "enabled" : "disabled");
    },
    "Swap Assault Rifle controls",
};

ConsoleCommand2 swap_grenade_controls_cmd{
    "cl_swapgrenadecontrols",
    []() {
        g_alpine_game_config.swap_gn_controls = !g_alpine_game_config.swap_gn_controls;
        rf::console::print("Swap grenade controls: {}", g_alpine_game_config.swap_gn_controls ? "enabled" : "disabled"); 
    },
    "Swap grenade controls",
};

ConsoleCommand2 swap_shotgun_controls_cmd{
    "cl_swapsgcontrols",
    []() {
        g_alpine_game_config.swap_sg_controls = !g_alpine_game_config.swap_sg_controls;
        rf::console::print("Swap shotgun controls: {}", g_alpine_game_config.swap_sg_controls ? "enabled" : "disabled"); 
    },
    "Swap shotgun controls",
};

ConsoleCommand2 play_join_beep_cmd{
    "mp_notifyonjoin",
    []() {
        g_alpine_game_config.player_join_beep = !g_alpine_game_config.player_join_beep;
        rf::console::print("Out of focus player join notifications are {}",
            g_alpine_game_config.player_join_beep ? "enabled" : "disabled");
    },
    "Toggle notification beeps being played when a player joins the server you are in when your game doesn't have focus",
};

FunHook<void(rf::Player*, int)> player_make_weapon_current_selection_hook{
    0x004A4980,
    [](rf::Player* player, int weapon_type) {
        player_make_weapon_current_selection_hook.call_target(player, weapon_type);
        rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
        if (entity && rf::is_multi) {
            // Reset impact delay timers when switching weapon (except in SP because of speedrunners)
            entity->ai.create_weapon_delay_timestamps[0].invalidate();
            entity->ai.create_weapon_delay_timestamps[1].invalidate();
            // Reset burst counters and timers
            entity->ai.primary_burst_fire_remaining = 0;
            entity->ai.secondary_burst_fire_remaining = 0;
            entity->ai.primary_burst_fire_next_timestamp.invalidate();
            entity->ai.secondary_burst_fire_next_timestamp.invalidate();
        }
    },
};

void __fastcall player_execute_action_timestamp_set_new(rf::Timestamp* fire_wait_timer, int, int value)
{
    if (!fire_wait_timer->valid() || fire_wait_timer->time_until() < value) {
        fire_wait_timer->set(value);
    }
}

CallHook<void __fastcall(rf::Timestamp*, int, int)> player_execute_action_timestamp_set_fire_wait_patch{
    {0x004A62C2u, 0x004A6325u},
    &player_execute_action_timestamp_set_new,
};

FunHook<void(rf::Player*, rf::ControlConfigAction, bool)> player_execute_action_hook{
    0x004A6210,
    [](rf::Player* player, rf::ControlConfigAction action, bool was_pressed) {
        if (!multi_spectate_execute_action(action, was_pressed)) {
            player_execute_action_hook.call_target(player, action, was_pressed);
        }
    },
};

FunHook<bool(rf::Player*)> player_is_local_hook{
    0x004A68D0,
    [](rf::Player* player) {
        if (multi_spectate_is_spectating()) {
            return false;
        }
        return player_is_local_hook.call_target(player);
    }
};

bool player_is_dead_and_not_spectating(rf::Player* player)
{
    if (multi_spectate_is_spectating() || !g_alpine_game_config.death_bars) {
        return false;
    }
    return rf::player_is_dead(player);
}

CallHook player_is_dead_red_bars_hook{0x00432A52, player_is_dead_and_not_spectating};
CallHook player_is_dead_scoreboard_hook{0x00437BEE, player_is_dead_and_not_spectating};
CallHook player_is_dead_scoreboard2_hook{0x00437C25, player_is_dead_and_not_spectating};

static bool player_is_dying_and_not_spectating(rf::Player* player)
{
    if (multi_spectate_is_spectating() || !g_alpine_game_config.death_bars) {
        return false;
    }
    return rf::player_is_dying(player);
}

CallHook player_is_dying_red_bars_hook{0x00432A5F, player_is_dying_and_not_spectating};
CallHook player_is_dying_scoreboard_hook{0x00437C01, player_is_dying_and_not_spectating};
CallHook player_is_dying_scoreboard2_hook{0x00437C36, player_is_dying_and_not_spectating};

FunHook<void()> players_do_frame_hook{
    0x004A26D0,
    []() {
        players_do_frame_hook.call_target();
        if (multi_spectate_is_spectating()) {
            rf::hud_do_frame(multi_spectate_get_target_player());
        }
        else {
            local_delayed_spawn_do_frame(); // try to spawn if a delayed spawn is queued
        }
    },
};

FunHook<void()> player_do_damage_screen_flash_hook{
    0x004A7520,
    []() {
        if (g_alpine_game_config.damage_screen_flash) {
            player_do_damage_screen_flash_hook.call_target();
        }
    },
};

ConsoleCommand2 damage_screen_flash_cmd{
    "cl_damageflash",
    []() {
        g_alpine_game_config.damage_screen_flash = !g_alpine_game_config.damage_screen_flash;
        rf::console::print("Damage screen flash effect is {}", g_alpine_game_config.damage_screen_flash ? "enabled" : "disabled");
    },
    "Toggle damage screen flash effect",
};

void play_local_sound_3d(uint16_t sound_id, rf::Vector3 pos, int group, float volume) {
    rf::snd_play_3d(sound_id, pos, volume, rf::Vector3{}, group);
}

void play_local_sound_2d(uint16_t sound_id, int group, float volume) {
    rf::snd_play(sound_id, group, 0.0f, volume);
}

void play_local_hit_sound(bool died) {
    if (!g_alpine_game_config.play_hit_sounds) {
        return; // turned off
    }

    play_local_sound_2d(get_custom_sound_id(died ? 3 : 2), 0, 1.0f);
}

ConsoleCommand2 tauntsound_cmd{
    "mp_taunts",
    []() {
        g_alpine_game_config.play_taunt_sounds = !g_alpine_game_config.play_taunt_sounds;
        rf::console::print("Voice lines for multiplayer taunts are {}", g_alpine_game_config.play_taunt_sounds ? "enabled" : "disabled");
    },
    "Toggle whether to play voice lines for taunts used by players in multiplayer",
    "mp_taunts",
};

ConsoleCommand2 localhitsound_cmd{
    "cl_hitsounds",
    []() {
        g_alpine_game_config.play_hit_sounds = !g_alpine_game_config.play_hit_sounds;
        rf::console::print("Playing of hit sounds is {}", g_alpine_game_config.play_hit_sounds ? "enabled" : "disabled");
    },
    "Toggle whether to play a sound when you hit players in multiplayer (if enabled by an Alpine Faction server)",
    "cl_hitsounds",
};

ConsoleCommand2 location_ping_display_cmd{
    "cl_locationpings",
    []() {
        g_alpine_game_config.show_location_pings = !g_alpine_game_config.show_location_pings;
        rf::console::print("Location pinging is {}", g_alpine_game_config.show_location_pings ? "enabled" : "disabled");
    },
    "Toggle whether location pinging is enabled (only available in team gametypes and in RUN)",
    "cl_locationpings",
};

ConsoleCommand2 set_autoswitch_fire_wait_cmd{
    "cl_autoswitchfirewait",
    [](std::optional<int> new_fire_wait) {
        if (new_fire_wait) {
            g_alpine_game_config.set_suppress_autoswitch_fire_wait(new_fire_wait.value());
        }
        rf::console::print("Your suppress autoswitch fire wait is {}.", g_alpine_game_config.suppress_autoswitch_fire_wait);
    },
    "Set a minimum delay after firing a weapon before autoswitch can trigger",
};

void ping_looked_at_location() {
    if (!rf::is_multi) {
        return;
    }

    if (!g_alpine_game_config.show_location_pings) {
        rf::String msg{"Failed to ping location because the setting is turned off"};
        rf::String prefix;
        rf::multi_chat_print(msg, rf::ChatMsgColor::white_white, prefix);
        return;
    }

    if (!get_df_server_info().has_value() || !get_df_server_info()->location_pinging) {
        rf::String msg{"This server does not allow you to ping locations"};
        rf::String prefix;
        rf::multi_chat_print(msg, rf::ChatMsgColor::white_white, prefix);
        return;
    }

    if (!multi_is_team_game_type() && !gt_is_run()) {
        rf::String msg{"Location pinging is only available in team gametypes"};
        rf::String prefix;
        rf::multi_chat_print(msg, rf::ChatMsgColor::white_white, prefix);
        return;
    }

    // Get the point the player is looking at
    rf::Player* player = rf::local_player;
    if (!player || !player->cam) {
        return; // check player and camera are valid
    }

    // Raycast from the player camera
    rf::Vector3 p0 = rf::camera_get_pos(player->cam);
    rf::Matrix3 orient = rf::camera_get_orient(player->cam);
    rf::Vector3 p1 = p0 + orient.fvec * 10000.0f;

    // Perform raycast
    rf::LevelCollisionOut col_info;
    col_info.face = nullptr;
    col_info.obj_handle = -1;
    rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
    bool hit = rf::collide_linesegment_level_for_multi(p0, p1, entity, nullptr, &col_info, 0.1f, false, 1.0f);

    if (!hit) {
        return; // If no hit, do not proceed with the ping
    }

    // Only action ping if there's a valid hit
    af_send_ping_location_req_packet(&col_info.hit_point);                    // Send to server
    add_location_ping_world_hud_sprite(col_info.hit_point, player->name, -1); // Render locally
}

ConsoleCommand2 death_bars_cmd{
    "mp_deathbars",
    []() {
        g_alpine_game_config.death_bars = !g_alpine_game_config.death_bars;
        rf::console::print("Death bars are {}", g_alpine_game_config.death_bars ? "enabled" : "disabled");
    },
    "Toggle red bars at the top and bottom of screen when dead",
};

CallHook<void(rf::VMesh*, rf::Vector3*, rf::Matrix3*, void*)> player_cockpit_vmesh_render_hook{
    0x004A7907,
    [](rf::VMesh *vmesh, rf::Vector3 *pos, rf::Matrix3 *orient, void *params) {
        rf::Matrix3 new_orient = *orient;

        if (string_equals_ignore_case(rf::vmesh_get_name(vmesh), "driller01.vfx")) {
            float m = static_cast<float>(rf::gr::screen_width()) / static_cast<float>(rf::gr::screen_height()) / (4.0f / 3.0f);
            new_orient.rvec *= m;
        }

        player_cockpit_vmesh_render_hook.call_target(vmesh, pos, &new_orient, params);
    }
};

CodeInjection sr_load_player_weapon_anims_injection{
    0x004B4F9E,
    [](auto& regs) {
        rf::Entity *ep = regs.ebp;
        static auto& entity_update_weapon_animations = addr_as_ref<void(void*, int)>(0x0042AB20);
        entity_update_weapon_animations(ep, ep->ai.current_primary_weapon);
    },
};

float map_range(float value, float old_min, float old_max, float new_min, float new_max)
{
    value = std::max(old_min, std::min(value, old_max));
    return ((value - old_min) / (old_max - old_min)) * (new_max - new_min) + new_min;
}

CodeInjection player_move_flashlight_light_patch {
    0x004A6B0D,
    [](auto& regs) {
        rf::Vector3* pDest2 = static_cast<rf::Vector3*>(regs.ecx);
        rf::Entity* ep = rf::local_player_entity;
        rf::Vector3 eye_pos = ep->eye_pos;

        float dist = eye_pos.distance_to(*pDest2);
        float adjusted_range = rf::g_player_flashlight_range * sqrt(dist);
        regs.eax = adjusted_range;

        float mapped_dist = map_range(dist, 0.0f, g_local_headlamp_settings.max_range, 1.0f, 0.05f);
        rf::g_player_flashlight_intensity = g_local_headlamp_settings.intensity * mapped_dist;
    },
};

void player_multi_level_post_init() {
    g_local_queued_delayed_spawn = false;
    stop_draw_respawn_timer_notification();
}

// called on game start and during each level post init
void update_player_flashlight() {

    //color
    if (g_alpine_level_info_config.is_option_loaded(rf::level.filename, AlpineLevelInfoID::PlayerHeadlampColor)) {
        auto headlamp_color =
            get_level_info_value<uint32_t>(AlpineLevelInfoID::PlayerHeadlampColor);
        float _a;

        std::tie(g_local_headlamp_settings.r,
            g_local_headlamp_settings.g,
            g_local_headlamp_settings.b,
            _a) = // alpha is discarded
            extract_normalized_color_components(headlamp_color);
    }
    else if (g_alpine_options_config.is_option_loaded(AlpineOptionID::PlayerHeadlampColor)) {
        auto headlamp_color = get_option_value<uint32_t>(AlpineOptionID::PlayerHeadlampColor);
        float _a;

        std::tie(g_local_headlamp_settings.r,
            g_local_headlamp_settings.g,
            g_local_headlamp_settings.b,
            _a) = // alpha is discarded
            extract_normalized_color_components(headlamp_color);
    }    
    else {
        g_local_headlamp_settings.r = 1.0f;
        g_local_headlamp_settings.g = 0.872f;
        g_local_headlamp_settings.b = 0.75f;
    }

    //intensity
    if (g_alpine_level_info_config.is_option_loaded(rf::level.filename, AlpineLevelInfoID::PlayerHeadlampIntensity)) {
        g_local_headlamp_settings.intensity = std::clamp(
            get_level_info_value<float>(AlpineLevelInfoID::PlayerHeadlampIntensity), 0.0f, 0.99f);
    }
    else if (g_alpine_options_config.is_option_loaded(AlpineOptionID::PlayerHeadlampIntensity)) {
        g_local_headlamp_settings.intensity =
            std::clamp(get_option_value<float>(AlpineOptionID::PlayerHeadlampIntensity), 0.0f, 0.99f);
    }    
    else {
        g_local_headlamp_settings.intensity = 0.6f;
    }

    //range
    if (g_alpine_level_info_config.is_option_loaded(rf::level.filename, AlpineLevelInfoID::PlayerHeadlampRange)) {
        g_local_headlamp_settings.max_range =
            get_level_info_value<float>(AlpineLevelInfoID::PlayerHeadlampRange);
    }
    else if (g_alpine_options_config.is_option_loaded(AlpineOptionID::PlayerHeadlampRange)) {
        g_local_headlamp_settings.max_range = get_option_value<float>(AlpineOptionID::PlayerHeadlampRange);
    }    
    else {
        g_local_headlamp_settings.max_range = 20.0f;
    }

    //radius
    if (g_alpine_level_info_config.is_option_loaded(rf::level.filename, AlpineLevelInfoID::PlayerHeadlampRadius)) {
        g_local_headlamp_settings.base_radius =
            get_level_info_value<float>(AlpineLevelInfoID::PlayerHeadlampRadius);
    }
    else if (g_alpine_options_config.is_option_loaded(AlpineOptionID::PlayerHeadlampRadius)) {
        g_local_headlamp_settings.base_radius = get_option_value<float>(AlpineOptionID::PlayerHeadlampRadius);
    }
    else {
        g_local_headlamp_settings.base_radius = 3.25f;
    }

    // Write the values
    AsmWriter{0x004A6AF9}.push(std::bit_cast<int32_t>(g_local_headlamp_settings.b)); // Blue (stock 1.0f)
    AsmWriter{0x004A6AFE}.push(std::bit_cast<int32_t>(g_local_headlamp_settings.g)); // Green (stock 1.0f)
    AsmWriter{0x004A6B03}.push(std::bit_cast<int32_t>(g_local_headlamp_settings.r)); // Red (stock 1.0f)

    // range defined in player_move_flashlight_light_patch
    //*reinterpret_cast<float*>(0x005A0100) = g_local_headlamp_settings.max_range;     // max range (stock 12.0)
    *reinterpret_cast<float*>(0x005A0108) = g_local_headlamp_settings.base_radius;   // base radius (stock 3.0)
    AsmWriter{0x004A6AF3}.push(2);                                                 // attenuation algo (stock 0)
}

void player_do_patch()
{
    // Support player headlamp
    player_move_flashlight_light_patch.install();
    update_player_flashlight();

    // general hooks
    player_create_hook.install();
    player_destroy_hook.install();

    // Support spawn delays in ADS servers
    player_execute_action_respawn_req_patch.install();
    player_dying_frame_respawn_req_patch.install();

    // Allow swapping Assault Rifle primary and alternate fire controls
    player_fire_primary_weapon_hook.install();
    stop_continous_primary_fire_patch.install();
    stop_continous_alternate_fire_patch.install();

    // Reset impact delay timers when switching weapon to avoid delayed fire after switching
    player_make_weapon_current_selection_hook.install();

    // Fix setting fire wait timer when closing weapon switch menu
    // Note: this timer makes sense for weapons that require holding (not clicking) the control to fire (e.g. shotgun)
    player_execute_action_timestamp_set_fire_wait_patch.install();

    // spectate mode support
    player_execute_action_hook.install();
    player_create_entity_hook.install();
    player_is_local_hook.install();

    player_is_dying_red_bars_hook.install();
    player_is_dying_scoreboard_hook.install();
    player_is_dying_scoreboard2_hook.install();

    player_is_dead_red_bars_hook.install();
    player_is_dead_scoreboard_hook.install();
    player_is_dead_scoreboard2_hook.install();

    // Increase damage for kill command in Single Player
    write_mem<float>(0x004A4DF5 + 1, 100000.0f);

    // Allow undefined mp_character in player_create_entity
    // Fixes Go_Undercover event not changing player 3rd person character
    AsmWriter(0x004A414F, 0x004A4153).nop();

    // Fix hud msg never disappearing in spectate mode
    players_do_frame_hook.install();

    // Fix jeep cockpit not rendering for any jeeps entered after the first
    AsmWriter(0x004A77C3).jmp(0x004A77FB);

    // Make sure scanner bitmap is a render target in player_allocate
    write_mem<u8>(0x004A34BF + 1, rf::bm::FORMAT_RENDER_TARGET);

    // Support disabling of damage screen flash effect
    player_do_damage_screen_flash_hook.install();

    // Stretch driller cockpit when using a wide-screen
    player_cockpit_vmesh_render_hook.install();

    // Load correct third person weapon animations when restoring the game from a save file
    // Fixes mirror reflections and view from third person camera
    sr_load_player_weapon_anims_injection.install();

    // Change default 'Use' key to E
    write_mem<u8>(0x0043D0A3 + 1, rf::KEY_E);

    // Commands
    damage_screen_flash_cmd.register_cmd();
    death_bars_cmd.register_cmd();
    swap_assault_rifle_controls_cmd.register_cmd();
    swap_grenade_controls_cmd.register_cmd();
    swap_shotgun_controls_cmd.register_cmd();
    play_join_beep_cmd.register_cmd();
    localhitsound_cmd.register_cmd();
    tauntsound_cmd.register_cmd();
    set_autoswitch_fire_wait_cmd.register_cmd();
    location_ping_display_cmd.register_cmd();
}
