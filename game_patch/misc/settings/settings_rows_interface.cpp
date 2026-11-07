#include "../../main/main.h"
#include "settings_rows.h"

namespace
{

const char* const labels_seasonal[] = {"Off", "Auto (December)", "Always"};
const char* const labels_server_cfg_layout[] = {"Right / boxes",  "Right / lines", "Right / compact",
                                                "Left / boxes",   "Left / lines",  "Left / compact"};

const SettingRow rows[] = {
    {
        .id = "seasonal_effect",
        .key = "SeasonalEffect",
        .label = "Menu snow",
        .tooltip = "Snow on the main menu (Auto = 11-31 December).",
        .category = Cat::Interface,
        .subcategory = "Menus",
        .widget = Wid::Enum,
        .access = AccInt{&AGS::seasonal_effect},
        .flags = SF_HIDDEN,
        ENUM_LABELS(labels_seasonal),
    },
    {
        .id = "remote_server_cfg_display_mode",
        .key = "RemoteServerCfgDisplayMode",
        .label = "Server config layout",
        .tooltip = "Layout of the remote server-config popup.",
        .category = Cat::Interface,
        .subcategory = "Menus",
        .widget = Wid::Enum,
        .access = AccFnInt{
            [](rf::Player*) { return static_cast<int>(g_alpine_game_config.remote_server_cfg_display_mode); },
            [](rf::Player*, int v) {
                g_alpine_game_config.remote_server_cfg_display_mode = static_cast<RemoteServerCfgPopup::DisplayMode>(v);
            }},
        .flags = SF_MP_ONLY | SF_HIDDEN,
        ENUM_LABELS(labels_server_cfg_layout),
        .default_str = "0",
    },
    {
        .id = "show_advanced_settings",
        .key = "ShowAdvancedSettings",
        .label = "Show advanced settings",
        .tooltip = "List advanced settings in every category; search finds them either way.",
        .category = Cat::Interface,
        .subcategory = "Menus",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::show_advanced_settings},
        // Driven by the checkbox under the category buttons rather than a row.
        .flags = SF_HIDDEN,
    },
    {
        .id = "player_join_flash",
        .key = "PlayerJoinFlash",
        .label = "Taskbar flash on join",
        .tooltip = "Flash the taskbar when a player joins while you are tabbed out.",
        .category = Cat::Interface,
        .subcategory = "Notifications",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::player_join_flash},
        .flags = SF_MP_ONLY,
        .console_cmd = "mp_join_flash",
    },
    {
        .id = "discord_rich_presence",
        .key = "DiscordRichPresence",
        .label = "Discord rich presence",
        .tooltip = "Show what you are playing, including the server, map and player count in multiplayer, as "
                   "your Discord activity.",
        .category = Cat::Interface,
        .subcategory = "Discord",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::discord_rich_presence},
        .console_cmd = "cl_discord_presence",
    },
    {
        .id = "discord_join_button",
        .key = "DiscordJoinButton",
        .label = "Discord join button",
        .tooltip = "Offer a join link to public, password-free servers in your Discord activity.",
        .category = Cat::Interface,
        .subcategory = "Discord",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::discord_join_button},
        .console_cmd = "cl_discord_join_button",
    },
    {
        .id = "autodl_blur_background",
        .key = "AutodlBlurBackground",
        .label = "Blur download screen",
        .tooltip = "Blur the background of the map-download screen.",
        .category = Cat::Interface,
        .subcategory = "Downloads",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::autodl_blur_background},
        .flags = SF_MP_ONLY,
        .console_cmd = "autodl_blur_background",
    },
    {
        .id = "autodl_download_awps",
        .key = "AutodlDownloadAwps",
        .label = "Download bot waypoints",
        .tooltip = "Also download bot waypoint files with maps.",
        .category = Cat::Interface,
        .subcategory = "Downloads",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::autodl_download_awps},
        .flags = SF_MP_ONLY,
        .console_cmd = "autodl_download_awps",
    },
    {
        .id = "save_console_history",
        .key = "SaveConsoleHistory",
        .label = "Save console history",
        .tooltip = "Keep console command history between sessions.",
        .category = Cat::Interface,
        .subcategory = "Console",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::save_console_history},
        .flags = SF_APPLY_ON_LOAD,
        .apply = apply_console_history_setting,
        .console_cmd = "dbg_consolehistory",
    },
    {
        .id = "console_color",
        .key = "ConsoleColor",
        .label = "Console color",
        .tooltip = "Background color of the console.",
        .category = Cat::Interface,
        .subcategory = "Console",
        .widget = Wid::Color,
        .access = AccColor{&AGS::console_color},
        .flags = SF_APPLY_ON_LOAD,
        .apply = apply_console_color_setting,
        .console_cmd = "ui_color_console",
    },
};

} // namespace

std::span<const SettingRow> settings_rows_interface()
{
    return {rows, std::size(rows)};
}
