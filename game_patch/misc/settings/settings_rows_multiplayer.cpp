#include <algorithm>
#include <stdexcept>
#include "../../main/main.h"
#include "../../multi/multi.h"
#include "../../multi/server_internal.h"
#include "../../multi/sprays.h"
#include "../../os/os.h"
#include "../../rf/multi.h"
#include "settings_rows.h"

namespace
{

void commit_player_name(const std::string& value)
{
    if (!rf::local_player) {
        return;
    }
    const std::string name = setting_sanitize_player_name(value);
    if (name == rf::local_player->name.c_str()) {
        return;
    }
    // The engine would open a modal popup for an empty or over-long name, so reject it here.
    if (name.empty() || name.size() > player_name_max_len) {
        throw std::invalid_argument{"invalid player name"};
    }
    if (!rf::player_change_name(rf::String{name.c_str()})) {
        throw std::invalid_argument{"player name rejected"};
    }
    if (rf::is_multi) {
        rf::send_name_change_packet();
    }
}

void signal_server_cfg_changed()
{
    g_alpine_server_config.printed_cfg.clear();
    g_alpine_server_config.signal_cfg_changed = true;
}

void apply_server_netfps()
{
    if (rf::is_server) {
        signal_server_cfg_changed();
    }
}

void apply_server_max_fps()
{
    apply_maximum_fps();
    if (rf::is_dedicated_server) {
        signal_server_cfg_changed();
    }
}

const SettingRow rows[] = {
    {
        .id = "player_name",
        .key = "PlayerName",
        .label = "Player name",
        .tooltip = "Your multiplayer name.",
        .category = Cat::Multiplayer,
        .subcategory = "Identity",
        .widget = Wid::Text,
        .access = AccFnString{
            [](rf::Player* p) {
                auto* pl = ctx_player(p);
                return pl ? std::string{pl->name.c_str()} : std::string{};
            },
            [](rf::Player*, const std::string& v) { commit_player_name(v); }},
        .flags = SF_MP_ONLY | SF_BESPOKE_INI | SF_NEEDS_LOCAL_PLAYER,
        .console_cmd = "name",
    },
    {
        .id = "multi_character",
        .key = "MultiplayerCharacter",
        .label = "Character model",
        .tooltip = "Character model used in multiplayer.",
        .category = Cat::Multiplayer,
        .subcategory = "Identity",
        .widget = Wid::Enum,
        .access = AccFnInt{
            [](rf::Player* p) { auto* pl = ctx_player(p); return pl ? pl->settings.multi_character : 0; },
            [](rf::Player* p, int v) {
                // pc_multi.tbl decides how many characters exist; before it is parsed only the
                // engine's array bounds the index.
                const int count = rf::num_multi_characters > 0 ? rf::num_multi_characters : rf::MAX_MP_CHARACTERS;
                if (auto* pl = ctx_player(p)) {
                    pl->settings.multi_character = std::clamp(v, 0, count - 1);
                }
            },
            0, rf::MAX_MP_CHARACTERS - 1},
        .flags = SF_MP_ONLY | SF_NEEDS_LOCAL_PLAYER,
        .console_cmd = "mp_character",
        .default_str = "0",
    },
    {
        .id = "desired_handicap",
        .key = "DesiredHandicap",
        .label = "Handicap",
        .tooltip = "Damage-reduction handicap requested from the server.",
        .category = Cat::Multiplayer,
        .subcategory = "Gameplay",
        .widget = Wid::Slider,
        .access = AccInt{&AGS::desired_handicap, &AGS::set_desired_handicap,
            AGS::min_desired_handicap, AGS::max_desired_handicap},
        .flags = SF_MP_ONLY | SF_ADVANCED | SF_COMMIT_ON_RELEASE,
        // Outside multiplayer this sends nothing; a non-zero value is sent on the next join.
        .apply = [] { mp_send_handicap_request(false); },
        .console_cmd = "mp_handicap",
        .ui_min = 0.0f,
        .ui_max = 99.0f,
        .ui_step = 1.0f,
        .ui_decimals = 0,
    },
    {
        .id = "selected_spray_index",
        .key = "SpraySelection",
        .label = "Spray",
        .tooltip = "Which spray you place.",
        .category = Cat::Multiplayer,
        .subcategory = "Identity",
        .widget = Wid::Enum,
        .access = AccInt{&AGS::selected_spray_index, &AGS::set_selected_spray_index},
        .flags = SF_MP_ONLY,
        .console_cmd = "spray",
    },
    {
        .id = "spray_display",
        .key = "SprayDisplay",
        .label = "Show sprays",
        .tooltip = "Show other players' sprays.",
        .category = Cat::Multiplayer,
        .subcategory = "Sprays",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::spray_display},
        .flags = SF_MP_ONLY,
        .apply = sprays_apply_display_toggle,
        .console_cmd = "cl_sprays",
    },
    {
        .id = "server_netfps",
        .key = "ServerNetFPS",
        .label = "Server net FPS",
        .tooltip = "Object updates per second sent when hosting.",
        .category = Cat::Multiplayer,
        .subcategory = "Hosting",
        .widget = Wid::Stepper,
        .access = AccUint{&AGS::server_netfps, &AGS::set_server_netfps, AGS::min_server_netfps, AGS::max_server_netfps},
        .flags = SF_MP_ONLY | SF_HOST_ONLY | SF_ADVANCED,
        .apply = apply_server_netfps,
        .console_cmd = "sv_netfps",
        .ui_suffix = " Hz",
    },
    {
        .id = "server_max_fps",
        .key = "ServerMaxFPS",
        .label = "Server max FPS",
        .tooltip = "Frame cap used by the dedicated server.",
        .category = Cat::Multiplayer,
        .subcategory = "Hosting",
        .widget = Wid::Stepper,
        .access = AccUint{&AGS::server_max_fps, &AGS::set_server_max_fps, AGS::min_fps_limit, AGS::max_fps_limit},
        .flags = SF_DEDICATED_ONLY | SF_HIDDEN | SF_APPLY_ON_LOAD,
        .apply = apply_server_max_fps,
        .console_cmd = "maxfps",
    },
    {
        .id = "fflink_status",
        .label = "FactionFiles account",
        .tooltip = "Linked FactionFiles account, for achievements and map ratings. Link or unlink it in the launcher.",
        .category = Cat::Multiplayer,
        .subcategory = "FactionFiles",
        .widget = Wid::Label,
        .access = AccRegString{[] {
            const std::string& name = g_game_config.fflink_username.value();
            return name.empty() ? std::string{"Not linked"} : name;
        }},
        .flags = SF_READ_ONLY,
        .persistence = Pers::Registry,
        .console_cmd = "fflink_status",
    },
    {
        .id = "multiplayer_tracker",
        .key = "MultiplayerTracker",
        .label = "Master server",
        .tooltip = "Master server used for the server list (restart required).",
        .category = Cat::Multiplayer,
        .subcategory = "Network",
        .widget = Wid::Text,
        .access = AccString{&AGS::multiplayer_tracker, &AGS::set_multiplayer_tracker},
        .flags = SF_HIDDEN | SF_RESTART_REQUIRED | SF_NOT_WHILE_IN_GAME,
    },
    {
        .id = "dbg_bot",
        .key = "DbgBot",
        .label = "Bot debug logging",
        .tooltip = "Debug: verbose bot logging.",
        .category = Cat::Multiplayer,
        .subcategory = "Debug",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::dbg_bot},
        .flags = SF_HIDDEN,
        .console_cmd = "dbg_bot",
    },
    {
        .id = "waypoints_edit_default_enabled",
        .key = "WaypointsEditDefaultEnabled",
        .label = "Waypoint editing on start",
        .tooltip = "Start with bot waypoint edit mode enabled.",
        .category = Cat::Multiplayer,
        .subcategory = "Debug",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::waypoints_edit_default_enabled},
        .flags = SF_HIDDEN | SF_RESTART_REQUIRED,
    },
};

} // namespace

std::span<const SettingRow> settings_rows_multiplayer()
{
    return {rows, std::size(rows)};
}
