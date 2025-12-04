#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/AsmWriter.h>
#include <common/config/BuildConfig.h>
#include <common/version/version.h>
#include <common/utils/list-utils.h>
#include <algorithm>
#include "console.h"
#include "../misc/alpine_options.h"
#include "../misc/alpine_settings.h"
#include "../misc/level.h"
#include "../main/main.h"
#include "../rf/player/camera.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/level.h"
#include "../rf/entity.h"
#include "../rf/hud.h"
#include "../rf/character.h"
#include "../misc/misc.h"
#include "../misc/vpackfile.h"

ConsoleCommand2 dot_cmd{
    ".",
    [] (const std::string pattern) {
        for (int i = 0; i < rf::console::num_commands; ++i) {
            const rf::console::Command* const cmd = g_commands_buffer[i];
            if (string_icontains(cmd->name, pattern)
                || (cmd->help && string_icontains(cmd->help, pattern))) {
                if (cmd->help) {
                    rf::console::print("{} - {}", cmd->name, cmd->help);
                } else {
                    rf::console::print("{}", cmd->name);
                }
            }
        }
    },
    "Find a console variable or command",
    ". <query>",
};

ConsoleCommand2 console_history_cmd{
    "dbg_consolehistory",
    []() {
        g_alpine_game_config.save_console_history = !g_alpine_game_config.save_console_history;
        apply_console_history_setting();
        rf::console::print("Console history {}.", g_alpine_game_config.save_console_history ? "enabled" : "disabled");
    },
    "Toggles whether console history persists between game launches",
};

ConsoleCommand2 vli_cmd{
    "vli",
    []() {
        g_alpine_game_config.show_glares = !g_alpine_game_config.show_glares;
        rf::console::print("Volumetric lightining is {}.", g_alpine_game_config.show_glares ? "enabled" : "disabled");
    },
    "Toggles volumetric lightining",
};

ConsoleCommand2 monitor_resolution_scale_cmd{
    "r_monitorscale",
    [](std::optional<int> scale_opt) {
        if (scale_opt) {
            g_alpine_game_config.set_monitor_resolution_scale(scale_opt.value());
        }
        rf::console::print("Monitor resolution scale is {}", g_alpine_game_config.monitor_resolution_scale);
    },
    "Set resolution scale for security monitor screens and mirrors (reload level to apply)",
};

ConsoleCommand2 fast_anim_cmd{
    "r_fastanim",
    []() {
        rf::g_fast_animations = !rf::g_fast_animations;
        rf::console::print("Fast animations are {}.", rf::g_fast_animations ? "enabled" : "disabled");
    },
    "Toggles fast animations for characters (reduce animation smoothness at far distances)",
};

ConsoleCommand2 player_count_cmd{
    "mp_playercount",
    []() {
        if (!rf::is_multi)
            return;

        auto player_list = SinglyLinkedList{rf::player_list};
        auto player_count = std::distance(player_list.begin(), player_list.end());
        rf::console::print("Player count: {}\n", player_count);
    },
    "Get player count",
};

ConsoleCommand2 find_level_cmd{
    "findlevel",
    [](std::string pattern) {
        vpackfile_find_matching_files(StringMatcher().infix(pattern).suffix(".rfl"), [](const char* name) {
            // Print all matching filenames
            rf::console::print("{}\n", name);
        });
    },
    "Find a level by a filename fragment",
    "findlevel <query>",
};

DcCommandAlias find_map_cmd{
    "findmap",
    find_level_cmd,
};

auto& level_cmd = addr_as_ref<rf::console::Command>(0x00637078);
DcCommandAlias map_cmd{
    "map",
    level_cmd,
};

void print_basic_level_info() {
    rf::console::print("Filename: {}", rf::level.filename);
    rf::console::print("Name: {}", rf::level.name);
    rf::console::print("Author: {}", rf::level.author);
    rf::console::print("Date: {}", rf::level.level_date);

    std::string version_text;
    if (rf::level.version == 175) {
        version_text = "Official - PS2 retail";
    }
    else if (rf::level.version == 180) {
        version_text = "Official - PC retail";
    }
    else if (rf::level.version == 200) {
        version_text = "Community - RF/PF/DF";
    }
    else if (rf::level.version > 0 && rf::level.version < 200) {
        version_text = "Official - Internal";
    }
    else if (rf::level.version >= 300) {
        version_text = "Community - Alpine";
    }
    else {
        version_text = "Unsupported";
    }
    rf::console::print("RFL File Version: {} ({})", rf::level.version, version_text);
}

ConsoleCommand2 level_info_cmd{
    "level_info",
    []() {
        if (rf::level.flags & rf::LEVEL_LOADED) {
            print_basic_level_info(); // print basic info before continuing

            rf::console::print("Has Skybox? {}", rf::level.has_skyroom);
            rf::console::print("Hardness: {}", rf::level.default_rock_hardness);
            rf::console::print("Ambient Light: {}, {}, {}",
                rf::level.ambient_light.red, rf::level.ambient_light.green, rf::level.ambient_light.blue);
            rf::console::print("Distance Fog: {}, {}, {}, near clip: {}, far clip: {}",
                rf::level.distance_fog_color.red, rf::level.distance_fog_color.green, rf::level.distance_fog_color.blue,
                rf::level.distance_fog_near_clip, rf::level.distance_fog_far_clip);

            // Alpine level properties
            const auto& level_props = AlpineLevelProperties::instance();
            rf::console::print("Alpine level properties:");
            rf::console::print("- Alpine props chunk version: {}", level_props.chunk_version > 0 ? std::to_string(level_props.chunk_version) : "not found");
            rf::console::print("- Legacy cyclic timers: {}", level_props.legacy_cyclic_timers);
            rf::console::print("- Legacy movers: {}", level_props.legacy_movers);
            rf::console::print("- Starts with headlamp: {}", level_props.starts_with_headlamp);
            rf::console::print("- Override static mesh ambient light scale: {}", level_props.override_static_mesh_ambient_light_modifier);
            rf::console::print("- Mesh static ambient light scale: {}", level_props.static_mesh_ambient_light_modifier);

            // Dash level properties (if found)
            const auto& dash_level_props = DashLevelProps::instance();
            if (dash_level_props.chunk_version > 0) {
                rf::console::print("Dash level properties:");
                rf::console::print("- Dash props chunk version: {}", dash_level_props.chunk_version);
                rf::console::print("- Lightmaps full depth: {}", dash_level_props.lightmaps_full_depth);
            }

            // Lightmap clamping floor
            if (g_alpine_level_info_config.is_option_loaded(rf::level.filename, AlpineLevelInfoID::LightmapClampFloor)) {
                uint32_t clamp_floor = get_level_info_value<uint32_t>(AlpineLevelInfoID::LightmapClampFloor);
                auto [r, g, b, _] = extract_color_components(clamp_floor);
                rf::console::print("Lightmap clamping floor: {}, {}, {}", r, g, b);
            }

            // Lightmap clamping ceiling
            if (g_alpine_level_info_config.is_option_loaded(rf::level.filename, AlpineLevelInfoID::LightmapClampCeiling)) {
                uint32_t clamp_ceiling = get_level_info_value<uint32_t>(AlpineLevelInfoID::LightmapClampCeiling);
                auto [r, g, b, _] = extract_color_components(clamp_ceiling);
                rf::console::print("Lightmap clamping ceiling: {}, {}, {}", r, g, b);
            }

            // Player headlamp color
            if (g_alpine_level_info_config.is_option_loaded(rf::level.filename, AlpineLevelInfoID::PlayerHeadlampColor)) {
                uint32_t headlamp_color = get_level_info_value<uint32_t>(AlpineLevelInfoID::PlayerHeadlampColor);
                auto [r, g, b, _] = extract_color_components(headlamp_color);
                rf::console::print("Player headlamp color: {}, {}, {}", r, g, b);
            }

            // Player headlamp intensity
            if (g_alpine_level_info_config.is_option_loaded(rf::level.filename, AlpineLevelInfoID::PlayerHeadlampIntensity)) {
                float headlamp_intensity = get_level_info_value<float>(AlpineLevelInfoID::PlayerHeadlampIntensity);
                rf::console::print("Player headlamp intensity: {}", headlamp_intensity);
            }

            // Player headlamp range
            if (g_alpine_level_info_config.is_option_loaded(rf::level.filename, AlpineLevelInfoID::PlayerHeadlampRange)) {
                float headlamp_range = get_level_info_value<float>(AlpineLevelInfoID::PlayerHeadlampRange);
                rf::console::print("Player headlamp range: {}", headlamp_range);
            }

            // Player headlamp radius
            if (g_alpine_level_info_config.is_option_loaded(rf::level.filename, AlpineLevelInfoID::PlayerHeadlampRadius)) {
                float headlamp_radius = get_level_info_value<float>(AlpineLevelInfoID::PlayerHeadlampRadius);
                rf::console::print("Player headlamp radius: {}", headlamp_radius);
            }

            // Ideal player count
            if (g_alpine_level_info_config.is_option_loaded(rf::level.filename, AlpineLevelInfoID::IdealPlayerCount)) {
                int player_count = get_level_info_value<int>(AlpineLevelInfoID::IdealPlayerCount);
                rf::console::print("Ideal player count: {}", player_count);
            }

            // Author contact
            if (g_alpine_level_info_config.is_option_loaded(rf::level.filename, AlpineLevelInfoID::AuthorContact)) {
                std::string author_contact = get_level_info_value<std::string>(AlpineLevelInfoID::AuthorContact);
                rf::console::print("Author contact: {}", author_contact);
            }

            // Author website
            if (g_alpine_level_info_config.is_option_loaded(rf::level.filename, AlpineLevelInfoID::AuthorWebsite)) {
                std::string author_website = get_level_info_value<std::string>(AlpineLevelInfoID::AuthorWebsite);
                rf::console::print("Author website: {}", author_website);
            }

            // Description
            if (g_alpine_level_info_config.is_option_loaded(rf::level.filename, AlpineLevelInfoID::Description)) {
                std::string desc = get_level_info_value<std::string>(AlpineLevelInfoID::Description);
                rf::console::print("Level description: {}", desc);
            }

            // Mesh replacements
            const auto& replacements = g_alpine_level_info_config.mesh_replacements;

            if (!replacements.empty()) {
                rf::console::print("Mesh replacements:");

                for (const auto& [original_mesh, replacement_mesh] : replacements) {
                    rf::console::print(" - {} -> {}", original_mesh, replacement_mesh);
                }
            }
        }
        else {
            rf::console::print("No level loaded!");
        }
    },
    "Shows information about the current level",
};

DcCommandAlias map_info_cmd{
    "map_info",
    level_info_cmd,
};

ConsoleCommand2 version_cmd{
    "ver",
    []() {
        rf::console::print("Alpine Faction {} ({}), build date: {} {}", VERSION_STR, VERSION_CODE, __DATE__, __TIME__);
    },
    "Display version info",
};

ConsoleCommand2 dbg_berserk_cmd{
    "dbg_berserk",
    []() {
        if (!rf::is_multi) {
            for (auto& entity : DoublyLinkedList{rf::entity_list}) {
                if (&entity && &entity != rf::local_player_entity) {
                    rf::ai_go_berserk(&entity.ai, 3600.0f, true);
                }
            }
        }
    },
    "Make all entities go berserk",
};

ConsoleCommand2 server_password_cmd{
    "sv_pass",
    [](std::optional<std::string> new_password) {
        if (!rf::is_multi || !rf::is_server) {
            rf::console::print("This command can only be run as a server!");
            return;
        }            

        if (new_password) {
            rf::netgame.password = new_password.value().c_str();
            rf::console::print("Server password set to: {}", rf::netgame.password);
        }
        else {
            rf::netgame.password = "";
            rf::console::print("Server password removed.");
        }
    },
    "Set or remove the server password.",
    "server_password <password>",
};

ConsoleCommand2 server_rcon_password_cmd{
    "sv_rconpass",
    [](std::optional<std::string> new_rcon_password) {
        if (!rf::is_multi || !rf::is_dedicated_server) {
            rf::console::print("This command can only be run on a dedicated server!");
            return;
        }

        if (new_rcon_password) {
            if (new_rcon_password->size() > 16) {
                // game limits client requests to 16 characters
                rf::console::print("Server rcon password cannot exceed 16 characters.");
                return;
            }

            std::strncpy(rf::rcon_password, new_rcon_password->c_str(), sizeof(rf::rcon_password) - 1);
            rf::rcon_password[sizeof(rf::rcon_password) - 1] = '\0'; // null terminator
            rf::console::print("Server rcon password set to: {}", rf::rcon_password);
        }
        else {
            *rf::rcon_password = '\0';
            rf::console::print("Server rcon password removed.");
        }
    },
    "Set or remove the server rcon password.",
    "sv_rconpass <password>",
};

// only allow verify_level if a level is loaded (avoid a crash if command is run in menu)
FunHook<void()> verify_level_cmd_hook{
    0x0045E1F0,
    []() {
        if (rf::level.flags & rf::LEVEL_LOADED) {
            verify_level_cmd_hook.call_target();
        } else {
            rf::console::print("No level loaded!");
        }
    }
};

ConsoleCommand2 autosave_cmd{
    "sp_autosave",
    []() {
        g_alpine_game_config.autosave = !g_alpine_game_config.autosave;
        rf::console::print("Autosave is {}", g_alpine_game_config.autosave ? "enabled" : "disabled");
    },
    "Toggle autosave",
};

ConsoleCommand2 pcollide_cmd{
    "pcollide",
    []() {
        if (rf::is_multi) {
            rf::console::print("That command can't be used in multiplayer.");
            return;
        }
        else {
            rf::local_player->collides_with_world = !rf::local_player->collides_with_world;
            rf::console::print("Player collision with the world is set to {}", rf::local_player->collides_with_world);
        }
    },
    "Toggles player collision with the world",
};

void reset_restricted_cmds_on_init_multi()
{
    // ensure player collides with the world (pcollide = true)
    if (rf::local_player && !rf::local_player->collides_with_world) {        
        rf::console::print("Player collision with the world is set to true.");
        rf::local_player->collides_with_world = true;
    }
}

void restrict_mp_command(FunHook<void()>& hook)
{
    if (rf::is_multi) {
        rf::console::print("That command can't be used in multiplayer.");
        return;
    }
    hook.call_target();
}

FunHook<void()> drop_clutter_hook{0x0040F0A0, []() { restrict_mp_command(drop_clutter_hook); }};
FunHook<void()> drop_entity_hook{0x00418740, []() { restrict_mp_command(drop_entity_hook); }};
FunHook<void()> drop_item_hook{0x00458530, []() { restrict_mp_command(drop_item_hook); }};
//FunHook<void()> pcollide_hook{0x004A0F60, []() { restrict_mp_command(pcollide_hook); }};
FunHook<void()> teleport_hook{0x004A0FC0, []() { restrict_mp_command(teleport_hook); }};
FunHook<void()> level_hardness_hook{0x004663E0, []() { restrict_mp_command(level_hardness_hook); }};

void handle_camera_command(FunHook<void()>& hook)
{
    if (!(rf::level.flags & rf::LEVEL_LOADED)) {
        rf::console::print("No level loaded!");
        return;
    }

    if (rf::is_multi) {
        rf::console::print("That command can't be used in multiplayer.");
        return;
    }

    hook.call_target();

    const rf::CameraMode current_mode = rf::camera_get_mode(*rf::local_player->cam);

    std::string mode_text =
        (current_mode == rf::CAMERA_FIRST_PERSON) ? "first person"
        : (current_mode == rf::CAMERA_THIRD_PERSON) ? "third person"
        : (current_mode == rf::CAMERA_FREELOOK)     ? "free look"
        : "unknown";

    std::string helper_text =
        (current_mode == rf::CAMERA_FIRST_PERSON) ? "" : " Use `camera1` to return to first person.";

    rf::console::print("Camera mode set to {}.{}", mode_text, helper_text);    
}

FunHook<void()> camera1_cmd_hook{0x00431270, []() { handle_camera_command(camera1_cmd_hook); }};
FunHook<void()> camera2_cmd_hook{0x004312D0, []() { handle_camera_command(camera2_cmd_hook); }};
FunHook<void()> camera3_cmd_hook{0x00431330, []() { handle_camera_command(camera3_cmd_hook); }};

FunHook<void()> heehoo_cmd_hook{
    0x00431210,
    []() {
    if (!(rf::level.flags & rf::LEVEL_LOADED)) {
        rf::console::print("No level loaded!");
        return;
    }

    if (rf::is_multi) {
        rf::console::print("That command can't be used in multiplayer.");
        return;
    }

    if (rf::entity_is_flying(rf::local_player_entity)) {
        rf::hud_msg("You feel heavy", 0, 0, 0);
    } else {
        rf::hud_msg("You feel lighter", 0, 0, 0);
    }
    heehoo_cmd_hook.call_target();
    }
};

static void register_builtin_command(const char* name, const char* description, uintptr_t addr)
{
    static std::vector<std::unique_ptr<rf::console::Command>> builtin_commands;
    auto cmd = std::make_unique<rf::console::Command>();
    rf::console::Command::init(cmd.get(), name, description, reinterpret_cast<rf::console::CommandFuncPtr>(addr));
    builtin_commands.push_back(std::move(cmd));
}

void console_commands_apply_patches()
{
    // Allow 'level' command outside of multiplayer game
    AsmWriter(0x00434FEC, 0x00434FF2).nop();

    drop_clutter_hook.install();
    drop_entity_hook.install();
    drop_item_hook.install();
    //pcollide_hook.install();
    teleport_hook.install();
    level_hardness_hook.install();
}

void console_commands_init()
{
    // Register RF builtin commands disabled in PC build

    // Server configuration commands
    register_builtin_command("sv_fraglimit", "Sets kill limit", 0x0046CBC0);
    register_builtin_command("sv_timelimit", "Sets time limit", 0x0046CC10);
    register_builtin_command("sv_geolimit", "Sets geomod limit", 0x0046CC70);
    register_builtin_command("sv_caplimit", "Sets capture limit", 0x0046CCC0);

    // Misc commands
    register_builtin_command("sound", "Toggle sound", 0x00434590);
    register_builtin_command("sp_difficulty", "Set game difficulty", 0x00434EB0);
    // register_builtin_command("ms", "Set mouse sensitivity", 0x0043CE90);
    // register_builtin_command("level_info", "Show level info", 0x0045C210);
    register_builtin_command("dbg_verifylevel", "Verify level", 0x0045E1F0);
    register_builtin_command("ui_playernames", "Toggle player names on HUD", 0x0046CB80);
    register_builtin_command("sv_countclients", "Show number of connected clients", 0x0046CD10);
    register_builtin_command("sv_kickall", "Kick all clients", 0x0047B9E0);
    register_builtin_command("dbg_timedemo", "Start timedemo", 0x004CC1B0);
    register_builtin_command("dbg_frameratetest", "Start frame rate test", 0x004CC360);
    register_builtin_command("dbg_systeminfo", "Show system information", 0x00525A60);
    register_builtin_command("r_trilinearfiltering", "Toggle trilinear filtering", 0x0054F050);
    register_builtin_command("r_detailtextures", "Toggle detail textures", 0x0054F0B0);
    register_builtin_command("cl_togglecrouch", "Toggle crouch mode", 0x00430C50);

    // risky commands, restricted in MP unless debug build
    register_builtin_command("drop_clutter", "Spawn a clutter object by class name", 0x0040F0A0);
    register_builtin_command("drop_entity", "Spawn an entity by class name", 0x00418740);
    register_builtin_command("drop_item", "Spawn an item by class name", 0x00458530);
    // register_builtin_command("pcollide", "Toggle if player collides with the world", 0x004A0F60);
    register_builtin_command("teleport", "Teleport player to specific coordinates (format: X Y Z)", 0x004A0FC0);
    register_builtin_command("sp_levelhardness", "Set default hardness for geomods", 0x004663E0);
    register_builtin_command("save_commands", "Print all console commands to a text file named console_commands.txt", 0x00509920);

#ifdef DEBUG
    register_builtin_command("drop_fixed_cam", "Drop a fixed camera", 0x0040D220);
    register_builtin_command("orbit_cam", "Orbit camera around current target", 0x0040D2A0);
    register_builtin_command("glares_toggle", "toggle glares", 0x00414830);
    register_builtin_command("set_vehicle_bounce", "set the elasticity of vehicle collisions", 0x004184B0);
    register_builtin_command("set_mass", "set the mass of the targeted object", 0x004184F0);
    register_builtin_command("set_skin", "Set the skin of the current player target", 0x00418610);
    register_builtin_command("set_life", "Set the life of the player's current target", 0x00418680);
    register_builtin_command("set_armor", "Set the armor of the player's current target", 0x004186E0);
    register_builtin_command("set_weapon", "set the current weapon for the targeted entity", 0x00418AA0);
    register_builtin_command("jump_height", "set the height the player can jump", 0x00418B80);
    register_builtin_command("fall_factor", nullptr, 0x004282F0);
    register_builtin_command("player_step", nullptr, 0x00433DB0);
    register_builtin_command("mouse_cursor", "Sets the mouse cursor", 0x00435210);
    register_builtin_command("fogme", "Fog everything", 0x004352E0);
    register_builtin_command("mouse_look", nullptr, 0x0043CF30);
    register_builtin_command("set_georegion_hardness", "Set default hardness for geomods", 0x004663E0);
    register_builtin_command("make_myself", nullptr, 0x00475040);
    register_builtin_command("splitscreen", nullptr, 0x00480AA0);
    register_builtin_command("splitscreen_swap", "Swap splitscreen players", 0x00480AB0);
    register_builtin_command("splitscreen_bot_test", "Start a splitscreen game in mk_circuits3.rfl", 0x00480AE0);
    register_builtin_command("max_plankton", "set the max number of plankton bits", 0x00497FC0);
    register_builtin_command("body", "Set player entity type", 0x004A11C0);
    register_builtin_command("invulnerable", "Make self invulnerable", 0x004A12A0);
    register_builtin_command("freelook", "Toggle between freelook and first person", 0x004A1340);
    register_builtin_command("hide_target", "Hide current target, unhide if already hidden", 0x004A13D0);
    register_builtin_command("set_primary", "Set default player primary weapon", 0x004A1430);
    register_builtin_command("set_secondary", "Set default player secondary weapon", 0x004A14B0);
    register_builtin_command("set_view", "Set camera view from entity with specified UID", 0x004A1540);
    register_builtin_command("set_ammo", nullptr, 0x004A15C0);
    register_builtin_command("endgame", "force endgame", 0x004A1640);
    register_builtin_command("irc", "Set the color range of the infrared characters", 0x004AECE0);
    register_builtin_command("irc_close", "Set the close color of the infrared characters", 0x004AEDB0);
    register_builtin_command("irc_far", "Set the far color of the infrared characters", 0x004AEE20);
    // register_builtin_command("pools", nullptr, 0x004B1050);
    register_builtin_command("savegame", "Save the current game", 0x004B3410);
    register_builtin_command("loadgame", "Load a game", 0x004B34C0);
    register_builtin_command("show_obj_times", "Set the number of portal objects to show times for", 0x004D3250);
    // Some commands does not use console_exec variables and were not added e.g. 004D3290
    register_builtin_command("script", "Runs a console script file (.vcs)", 0x0050B7D0);
    register_builtin_command("screen_res", nullptr, 0x0050E400);
    register_builtin_command("wfar", nullptr, 0x005183D0);
    register_builtin_command("play_bik", nullptr, 0x00520A70);
#endif // DEBUG

    // Custom commands
    pcollide_cmd.register_cmd();
    dot_cmd.register_cmd();
    console_history_cmd.register_cmd();
    vli_cmd.register_cmd();
    monitor_resolution_scale_cmd.register_cmd();
    fast_anim_cmd.register_cmd();
    player_count_cmd.register_cmd();
    find_level_cmd.register_cmd();
    find_map_cmd.register_cmd();
    map_cmd.register_cmd();
    level_info_cmd.register_cmd();
    map_info_cmd.register_cmd();
    version_cmd.register_cmd();
    dbg_berserk_cmd.register_cmd();
    server_password_cmd.register_cmd();
    server_rcon_password_cmd.register_cmd();
    verify_level_cmd_hook.install();
    autosave_cmd.register_cmd();

    // Hooks for builtin commands
    camera1_cmd_hook.install();
    camera2_cmd_hook.install();
    camera3_cmd_hook.install();
    heehoo_cmd_hook.install();
}
