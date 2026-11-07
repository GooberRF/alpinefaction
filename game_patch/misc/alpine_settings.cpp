#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <common/utils/os-utils.h>
#include "alpine_settings.h"
#include "alpine_options.h"
#include "saved_votes.h"
#include "settings_registry.h"
#include <common/version/version.h>
#include "../input/input.h"
#include "../input/bind_context.h"
#include "../os/console.h"
#include "../os/os.h"
#include "../rf/ui.h"
#include "../rf/character.h"
#include "../rf/os/console.h"
#include "../rf/player/player.h"
#include "../rf/sound/sound.h"
#include "../graphics/d3d11/gr_d3d11_entity_shadow.h"
#include "../rf/gr/gr.h"
#include "../rf/input.h"
#include "../rf/multi.h"
#include "../sound/sound.h"
#include "../input/gyro.h"
#include "../input/gamepad.h"
#include "../multi/multi.h"
#include <shlwapi.h>
#include <windows.h>
#include <shellapi.h>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <xlog/xlog.h>
#include "../graphics/gr.h"

bool g_loaded_alpine_settings_file = false;
bool g_loaded_alpine_core_config_file = false;
static bool g_loaded_players_cfg_file = false;
static bool g_restart_on_close = false;
std::vector<std::string> orphaned_lines;
std::vector<std::string> orphaned_lines_core_config;
int loaded_afs_version = -1;
int loaded_afcc_version = -1;
std::optional<std::string> afs_cmd_line_filename;

AlpineGameSettings g_alpine_game_config;

// Alpine actions are keyed by name; stock actions keep the legacy positional form. Context
// overrides use their own prefix: builds that predate contexts read every "Bind:" line as a
// global binding, but carry an unrecognised key forward untouched.
static std::string bind_key_prefix(BindContextId ctx)
{
    return std::string{ctx == BindContextId::Global ? "Bind:" : "BindCtx:"} + bind_context_ini_name(ctx) + ":";
}

// '~' is the inherit sentinel; it is only meaningful in a context override line.
static std::string bind_field_to_ini(int16_t value)
{
    return value == bind_inherit ? std::string{"~"} : std::to_string(value);
}

static constexpr int bind_mouse_btn_id_count = 5;
static_assert(CTRL_EXTRA_MOUSE_SCAN_BASE + CTRL_EXTRA_MOUSE_SCAN_COUNT <= rf::KEY_MASK + 1);

static int16_t bind_field_from_ini(const std::string& text, bool allow_inherit, bool mouse)
{
    if (text == "~") {
        return allow_inherit ? bind_inherit : static_cast<int16_t>(-1);
    }
    if (text.empty()) {
        return -1;
    }
    // The engine indexes its key and mouse tables with these unchecked; mouse ids 3 and 4 are the wheel.
    const int value = std::stoi(text);
    if (value < -1 || value >= (mouse ? bind_mouse_btn_id_count : rf::KEY_MASK + 1)) {
        xlog::warn("[Settings] Bind field '{}' is out of range, treating as unbound", text);
        return -1;
    }
    return static_cast<int16_t>(value);
}

// Controller input field of a Bind line. An absent field in a context line says nothing, so it
// inherits; '~' does too. Anything that is not a controller code is treated as unbound.
static int16_t pad_field_from_ini(const std::string& text, bool allow_inherit)
{
    if (text == "~" || text.empty()) {
        return allow_inherit ? bind_inherit : static_cast<int16_t>(-1);
    }
    const int value = std::stoi(text);
    if (value != -1 && (value < CTRL_GAMEPAD_SCAN_BASE || value > CTRL_GAMEPAD_RIGHT_TRIGGER)) {
        xlog::warn("[Settings] Controller bind field '{}' is out of range, treating as unbound", text);
        return -1;
    }
    return static_cast<int16_t>(value);
}

namespace
{

// RAII so a throw out of the rest of the load cannot leave the context runtime suspended.
struct BindContextSuspendGuard
{
    BindContextSuspendGuard() { bind_context_suspend(); }
    ~BindContextSuspendGuard() { bind_context_resume(); }
};

} // namespace

bool is_d3d11() {
    return g_game_config.renderer == GameConfig::Renderer::d3d11
        && !is_headless_mode();
}

std::optional<uint32_t> parse_hex_color_string(const std::string& value)
{
    auto value_view = std::string_view{value};

    constexpr auto is_separator = [](unsigned char c) {
        return c == ',' || std::isspace(c) != 0;
    };

    const bool has_decimal_separator = std::ranges::any_of(value_view, is_separator);

    if (has_decimal_separator) {
        std::array<uint32_t, 4> rgba{};
        std::size_t component_count = 0;
        std::size_t position = 0;

        while (position < value_view.size()) {
            while (position < value_view.size() && is_separator(static_cast<unsigned char>(value_view[position]))) {
                ++position;
            }

            if (position == value_view.size()) {
                break;
            }

            const auto start = position;
            while (position < value_view.size() && !is_separator(static_cast<unsigned char>(value_view[position]))) {
                ++position;
            }

            const auto component_view = value_view.substr(start, position - start);
            int parsed_value = 0;
            const auto result = std::from_chars(component_view.data(), component_view.data() + component_view.size(), parsed_value, 10);

            if (result.ec != std::errc{} || parsed_value < 0 || parsed_value > 255) {
                return std::nullopt;
            }

            if (component_count >= rgba.size()) {
                return std::nullopt;
            }

            rgba[component_count++] = static_cast<uint32_t>(parsed_value);
        }

        if (component_count != 3 && component_count != 4) {
            return std::nullopt;
        }

        if (component_count == 3) {
            rgba[3] = 0xFF;
        }

        return (rgba[0] << 24) | (rgba[1] << 16) | (rgba[2] << 8) | rgba[3];
    }

    std::string hex_buffer;
    hex_buffer.reserve(value_view.size());
    std::ranges::copy_if(value_view, std::back_inserter(hex_buffer), [](unsigned char c) { return std::isspace(c) == 0; });

    auto hex_view = std::string_view{hex_buffer};
    if (hex_view.starts_with("0x") || hex_view.starts_with("0X")) {
        hex_view.remove_prefix(2);
    }

    if (hex_view.size() != 6 && hex_view.size() != 8) {
        return std::nullopt;
    }

    if (!std::ranges::all_of(hex_view, [](unsigned char c) { return std::isxdigit(c) != 0; })) {
        return std::nullopt;
    }

    // Set color, assuming fully opaque if alpha value is not specified
    uint32_t color = 0;
    const auto result = std::from_chars(hex_view.data(), hex_view.data() + hex_view.size(), color, 16);
    if (result.ec != std::errc{}) {
        return std::nullopt;
    }

    if (hex_view.size() == 6) {
        color = (color << 8) | 0xFF;
    }

    return color;
}

std::string format_hex_color_string(uint32_t color)
{
    const auto alpha = static_cast<uint8_t>(color & 0xFF);
    const bool include_alpha = (alpha != 0xFF);
    const uint32_t encoded_value = include_alpha ? color : (color >> 8);

    if (include_alpha) {
        return std::format("{:08X}", encoded_value);
    }
    else {
        return std::format("{:06X}", encoded_value);
    }
}

// parse colors to 0.0-1.0 floats
std::tuple<float, float, float, float> extract_normalized_color_components(uint32_t color)
{
    return std::make_tuple(
        ((color >> 24) & 0xFF) / 255.0f, // Red   (0-1)
        ((color >> 16) & 0xFF) / 255.0f, // Green (0-1)
        ((color >> 8)  & 0xFF) / 255.0f, // Blue  (0-1)
        (color & 0xFF) / 255.0f          // Alpha (0-1)
    );
}

static rf::CmdLineParam& get_afs_cmd_line_param()
{
    static rf::CmdLineParam afs_param{"-afs", "", true};
    return afs_param;
}

void handle_afs_param()
{
    // do nothing unless -afs is specified
    if (!get_afs_cmd_line_param().found()) {
        return;
    }    

    std::string afs_filename = get_afs_cmd_line_param().get_arg();

    rf::console::print("afs filename forced {}", afs_filename);
    afs_cmd_line_filename = afs_filename;
}

void resolve_scan_code_conflicts(rf::ControlConfig& config)
{
    std::unordered_set<int> used_keys; // Track already assigned scan codes

    for (int i = 0; i < config.num_bindings; ++i) {
        auto& binding = config.bindings[i];

        for (int j = 0; j < 2; ++j) { // Check both scan_codes[0] and scan_codes[1]
            int key = binding.scan_codes[j];

            if (key != -1) {
                // If this key is already assigned earlier, unbind it
                if (used_keys.find(key) != used_keys.end()) {
                    xlog::warn("Scan code conflict detected: Key {} already used, unbinding action {}", key, i);
                    binding.scan_codes[j] = -1;
                }
                else {
                    used_keys.insert(key);
                }
            }
        }
    }
}

void resolve_mouse_button_conflicts(rf::ControlConfig& config)
{
    std::unordered_set<int> used_buttons; // Track already assigned mouse buttons

    for (int i = 0; i < config.num_bindings; ++i) {
        auto& binding = config.bindings[i];
        int button = binding.mouse_btn_id;

        if (button != -1) {
            // If this button is already assigned earlier, unbind it
            if (used_buttons.find(button) != used_buttons.end()) {
                xlog::warn("Mouse button conflict detected: Button {} already used, unbinding action {}", button, i);
                binding.mouse_btn_id = -1;
            }
            else {
                used_buttons.insert(button);
            }
        }
    }
}

std::string alpine_get_settings_filename()
{
    // -afs switch
    if (afs_cmd_line_filename.has_value()) {
        return afs_cmd_line_filename.value();
    }

    const bool use_stock_players_config =
        g_alpine_options_config.is_option_loaded(AlpineOptionID::UseStockPlayersConfig)
        && std::get<bool>(g_alpine_options_config.options[AlpineOptionID::UseStockPlayersConfig]);
    const bool is_tc_mod = rf::mod_param.found() && !use_stock_players_config;

    // tc mod
    if (is_tc_mod) {
        std::string mod_name = rf::mod_param.get_arg();
        return "alpine_settings_" + mod_name + ".ini";
    }

    // normal
    return "alpine_settings.ini";
}

// Registry rows carrying their own alpine_settings.ini key. Keys stay case sensitive.
static const SettingRow* find_afs_row(const std::string& key)
{
    for (const auto& row : settings_registry_all()) {
        if (row.persistence != SettingPersistence::Afs || (row.flags & SF_BESPOKE_INI)) {
            continue;
        }
        if (row.key && key == row.key) {
            return &row;
        }
    }
    return nullptr;
}

static int find_binding_by_name(const rf::ControlConfig& cc, const std::string& name)
{
    for (int i = 0; i < cc.num_bindings; ++i) {
        if (cc.bindings[i].name == name.c_str()) {
            return i;
        }
    }
    return -1;
}

bool alpine_player_settings_load(rf::Player* player)
{
    // Nothing may be applied over the live table while it is being rewritten, and a reload
    // starts from an empty override store.
    BindContextSuspendGuard bind_context_guard;
    bind_context_reset_all();

    handle_afs_param();
    std::string filename = alpine_get_settings_filename();
    std::ifstream file(filename);

    if (!file.is_open()) {
        xlog::info("Failed to read {}", filename);
        return false;
    }

    std::unordered_map<std::string, std::string> settings;
    std::unordered_set<std::string> processed_keys;
    std::string line;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == ';' || line[0] == '#' || line[0] == '[') continue;

        std::istringstream key_value(line);
        std::string key, value;

        if (std::getline(key_value, key, '=') && std::getline(key_value, value)) {
            settings[key] = value;
        }
    }

    file.close();

    // Store loaded settings file version
    if (settings.count("AFSFileVersion")) {
        loaded_afs_version = std::stoi(settings["AFSFileVersion"]);
        xlog::info("Loaded Alpine Faction settings file with version {}", loaded_afs_version);
        processed_keys.insert("AFSFileVersion");
    }

    // Discard deprecated settings
    // WorldHUDOverdraw (deprecated in AFS v9), parse into world_hud_flag_overdraw
    if (settings.count("WorldHUDOverdraw") && loaded_afs_version < 9) {
        g_alpine_game_config.world_hud_flag_overdraw = std::stoi(settings["WorldHUDOverdraw"]);
        processed_keys.insert("WorldHUDOverdraw");
        xlog::info("Successfully parsed legacy setting 'WorldHUDOverdraw' from loaded pre-v9 AFS file.");
    }

    if (settings.count("PlayerName")) {
        const std::string& raw_name = settings["PlayerName"];
        const std::string player_name = setting_sanitize_player_name(raw_name);
        if (player_name.empty()) {
            xlog::warn("[Settings] PlayerName in {} is empty; keeping the current name.", filename);
        }
        else {
            if (trim(raw_name).size() > player_name.size()) {
                xlog::warn("[Settings] PlayerName in {} is too long and has been truncated.", filename);
            }
            player->name = player_name.c_str();
        }
        processed_keys.insert("PlayerName");
    }

    // Generic scalar path. A key the registry does not know falls through to the orphan
    // collector below; a key it knows but cannot parse keeps its compiled default while the
    // rest of the file still loads.
    std::unordered_set<const SettingRow*> loaded_rows;
    for (const auto& [key, value] : settings) {
        if (processed_keys.count(key) || string_starts_with(key, "Bind:") || string_starts_with(key, "BindCtx:")) {
            continue;
        }
        const SettingRow* row = find_afs_row(key);
        if (!row) {
            continue;
        }
        processed_keys.insert(key);
        try {
            setting_load_ini_value(*row, value, player);
            loaded_rows.insert(row);
        }
        catch (const std::exception& e) {
            xlog::error("[Settings] Invalid value for {} in {}: '{}' ({}). Keeping the default.", key, filename, value,
                        e.what());
        }
    }

    if (!settings.count("GoreLevel")) {
        rf::game_set_gore_level(1); // if gore level not in ini file, match stock gameplay
    }

    // Load weapon autoswitch priority. Parsed into a local first so a bad token leaves the
    // existing prefs untouched instead of half written, and leaves the key to the orphan collector.
    if (settings.count("WeaponAutoswitchPriority")) {
        const std::string& weapon_value = settings["WeaponAutoswitchPriority"];
        try {
            std::istringstream weapon_stream(weapon_value);
            std::string weapon_id;
            std::array<int, 32> prefs{};
            int index = 0;

            while (std::getline(weapon_stream, weapon_id, ',') && index < 32) {
                prefs[index++] = std::stoi(weapon_id);
            }

            while (index < 32) {
                prefs[index++] = -1;
            }

            std::ranges::copy(prefs, player->weapon_prefs);
            processed_keys.insert("WeaponAutoswitchPriority");
        }
        catch (const std::exception& e) {
            xlog::warn("[Settings] Malformed WeaponAutoswitchPriority entry {} ({}), skipping", weapon_value, e.what());
        }
    }

    // Per-button gamepad bindings
    for (int b = 0; b < gamepad_get_button_count(); ++b) {
        std::string key = "GamepadBtn_" + std::to_string(b);
        if (settings.count(key)) {
            gamepad_set_button_binding(b, std::stoi(settings[key]));
            processed_keys.insert(key);
        }
    }
    if (settings.count("GamepadLTAction")) {
        gamepad_set_trigger_action(0, std::stoi(settings["GamepadLTAction"]));
        processed_keys.insert("GamepadLTAction");
    }
    if (settings.count("GamepadRTAction")) {
        gamepad_set_trigger_action(1, std::stoi(settings["GamepadRTAction"]));
        processed_keys.insert("GamepadRTAction");
    }

    // Load binds. Three accepted forms, each with optional trailing {ControllerButton},{ControllerAlt}
    // fields:
    //   Bind:{Name}={ID},{ScanCode0},{ScanCode1},{MouseButtonID}                 (legacy, keyed by ID)
    //   Bind:global:{AlpineActionName}={ID},{ScanCode0},{ScanCode1},{MouseButtonID}
    //   BindCtx:{Context}:{Name}={ID},{ScanCode0},{ScanCode1},{MouseButtonID}    (context override)
    // The named forms resolve the row by its registered English action name. Stock rows stay
    // positional because their names are localized, so a context line for a stock action falls
    // back to its ID; any other unknown name is left to the orphan collector, since the ID may
    // now belong to a different action.
    for (const auto& [key, value] : settings) {
        const bool ctx_key = string_starts_with(key, "BindCtx:");
        if (!ctx_key && !string_starts_with(key, "Bind:")) {
            continue;
        }
        // One malformed token must not abort the rest of the load. The key stays out of
        // processed_keys so the orphan collector carries the line forward verbatim.
        try {
            BindContextId bind_ctx = BindContextId::Global;
            bool named = false;
            std::string action_name = key.substr(5);
            for (int c = 0; c < static_cast<int>(BindContextId::_count); ++c) {
                const std::string prefix = bind_key_prefix(static_cast<BindContextId>(c));
                if (string_starts_with(key, prefix)) {
                    bind_ctx = static_cast<BindContextId>(c);
                    named = true;
                    action_name = key.substr(prefix.size());
                    break;
                }
            }
            // A context key this build does not know, or a "Bind:{something}:" key, must not be
            // read as the legacy ID-keyed form; leave it to the orphan collector.
            if (!named && (ctx_key || action_name.find(':') != std::string::npos)) {
                xlog::warn("[Settings] Unknown bind context in '{}', preserving line", key);
                continue;
            }
            std::istringstream bind_values(value);
            std::string id_str, scan1, scan2, mouse_btn;

            if (std::getline(bind_values, id_str, ',') &&
                std::getline(bind_values, scan1, ',') &&
                std::getline(bind_values, scan2, ',') &&
                std::getline(bind_values, mouse_btn, ',')) {

                int bind_id = std::stoi(id_str);
                if (named) {
                    const int by_name = find_binding_by_name(player->settings.controls, action_name);
                    const int alpine_start = alpine_control_start_index();
                    const bool stock_ctx_line =
                        bind_ctx != BindContextId::Global && (alpine_start < 0 || bind_id < alpine_start);
                    if (by_name >= 0) {
                        bind_id = by_name;
                    }
                    else if (stock_ctx_line) {
                        xlog::warn("[Settings] Unknown action name '{}' in config file, falling back to bind ID {}",
                                   action_name, bind_id);
                    }
                    else {
                        xlog::warn("[Settings] Unknown action name '{}' in config file, preserving line", action_name);
                        continue;
                    }
                }
                if (bind_id >= 0 && bind_id < player->settings.controls.num_bindings) {
                    const bool ctx_line = bind_ctx != BindContextId::Global;
                    const int16_t s0 = bind_field_from_ini(scan1, ctx_line, false);
                    const int16_t s1 = bind_field_from_ini(scan2, ctx_line, false);
                    const int16_t mb = bind_field_from_ini(mouse_btn, ctx_line, true);
                    // Optional 5th and 6th fields: the BUTTON and ALT controller inputs.
                    std::string pad_str1, pad_str2;
                    std::getline(bind_values, pad_str1, ',');
                    std::getline(bind_values, pad_str2, ',');
                    if (bind_ctx == BindContextId::Global) {
                        // Note action_name is not loaded for stock rows because the game uses
                        // localized strings for those; they are tracked by bind ID instead
                        player->settings.controls.bindings[bind_id].scan_codes[0] = s0;
                        player->settings.controls.bindings[bind_id].scan_codes[1] = s1;
                        player->settings.controls.bindings[bind_id].mouse_btn_id = mb;
                        // A line without them leaves the controller defaults in place.
                        if (!pad_str1.empty()) {
                            gamepad_bind_action(bind_id, 0, pad_field_from_ini(pad_str1, false));
                        }
                        if (!pad_str2.empty()) {
                            gamepad_bind_action(bind_id, 1, pad_field_from_ini(pad_str2, false));
                        }
                    }
                    else {
                        bind_context_set_override(
                            bind_ctx, bind_id,
                            BindOverride{{s0, s1}, mb,
                                         {pad_field_from_ini(pad_str1, true), pad_field_from_ini(pad_str2, true)}});
                    }

                    xlog::info("[Settings] Loaded Bind:{}: {} = {}, {}, {}, {}", bind_context_ini_name(bind_ctx),
                               action_name, bind_id, scan1, scan2, mouse_btn);
                    processed_keys.insert(key);
                }
                else {
                    xlog::warn("Invalid Bind ID {} for action {} found in config file!", bind_id, action_name);
                }
            }
            else {
                xlog::warn("Malformed Bind entry: {} found in config file!", value);
            }
        }
        catch (const std::exception& e) {
            xlog::warn("[Settings] Malformed Bind entry {}={} ({}), skipping", key, value, e.what());
            continue;
        }
    }

    // Load saved votes. Keys are reindexed from the store on every save, so the
    // numeric suffix is only a load order; a key whose suffix is not a number was
    // not written by this build and is left to the generic orphan mechanism below.
    {
        std::vector<std::pair<unsigned long, const std::string*>> saved_vote_keys;
        for (const auto& [key, value] : settings) {
            if (!string_starts_with(key, "SavedVote")) {
                continue;
            }
            const std::string suffix = key.substr(9); // strlen("SavedVote")
            if (suffix.empty() || suffix.find_first_not_of("0123456789") != std::string::npos
                || suffix.size() > 9) {
                continue;
            }
            saved_vote_keys.emplace_back(std::stoul(suffix), &key);
        }
        std::ranges::sort(saved_vote_keys, {}, [](const auto& entry) { return entry.first; });

        // The settings file can be reloaded; without this the store (and its
        // preserved-line list) would double.
        saved_votes_clear();
        for (const auto& [index, key] : saved_vote_keys) {
            const std::string& value = settings[*key];
            // Claimed either way: saved_votes owns the preservation of anything it
            // cannot use, so the key must never reach the orphan collector.
            processed_keys.insert(*key);

            SavedVote vote;
            if (saved_votes_is_full()) {
                // Stop parsing entirely once the store is full: the remaining lines
                // are carried forward verbatim instead of being dropped.
                xlog::warn("Saved vote store full; preserving unread entry {}", *key);
                saved_votes_add_unparsed(value);
            }
            else if (saved_vote_parse(value, vote)) {
                saved_votes_load_add(std::move(vote));
            }
            else {
                xlog::warn("Preserving unreadable saved vote {}={}", *key, value);
                saved_votes_add_unparsed(value);
            }
        }
    }

    // Apply hooks run once every value is in place, in table order, so a hook that reads another
    // row sees its loaded value.
    for (const auto& row : settings_registry_all()) {
        if ((row.flags & SF_APPLY_ON_LOAD) && loaded_rows.count(&row)) {
            setting_run_apply(row);
        }
    }

    // Iterate through newly loaded bindings and resolve conflicts
    // Earlier bind takes priority when conflicts occur
    resolve_scan_code_conflicts(player->settings.controls);
    resolve_mouse_button_conflicts(player->settings.controls);

    // Context overrides are loaded before the Global table has settled, so dedupe them now.
    bind_context_dedupe_against_global();

    // Waypoint edit mode is session-only. Apply startup default from settings (never in a -bot/-debugbot client).
    g_alpine_game_config.waypoints_edit_mode =
        g_alpine_game_config.waypoints_edit_default_enabled
        && !(rf::is_multi && !rf::is_server)
        && !client_bot_launch_enabled();

    // Store orphaned settings
    for (const auto& [key, value] : settings) {
        if (processed_keys.find(key) == processed_keys.end() && !string_starts_with(key, "AFS")) {
            xlog::info("Saving unrecognized setting as orphaned: {}={}", key, value);
            orphaned_lines.push_back(key + "=" + value);
        }
    }

    // apply loaded graphics options
    rf::player_settings_apply_graphics_options(player);

    rf::console::printf("Successfully loaded settings from %s", filename.c_str());
    g_loaded_alpine_settings_file = true;
    return true;
}

// alpine_settings.ini section a category's rows are written under. The loader ignores sections,
// so they only make the file readable.
static const char* afs_section_name(SettingCategory category)
{
    switch (category) {
        case SettingCategory::Graphics: return "GraphicsSettings";
        case SettingCategory::Hud: return "HUDSettings";
        case SettingCategory::Interface: return "InterfaceSettings";
        case SettingCategory::Audio: return "AudioSettings";
        case SettingCategory::Input: return "InputSettings";
        case SettingCategory::Gameplay: return "GameplaySettings";
        case SettingCategory::Multiplayer: return "MultiplayerSettings";
        default: return nullptr;
    }
}

static void serialize_weapon_prefs(std::ostream& file, rf::Player* player)
{
    file << "WeaponAutoswitchPriority=";
    bool first = true;
    for (int i = 0; i < 32; ++i) {
        int weapon_id = player->weapon_prefs[i];
        if (weapon_id > -1 && weapon_id < 255) { // Only save valid weapons
            if (!first) {
                file << ",";
            }
            file << weapon_id;
            first = false;
        }
    }
    file << "\n";
}

// Every alpine_settings.ini row, written straight from the table: one section per category, rows
// in table order. Bespoke rows write their own lines; bindings and saved votes have sections of
// their own.
static void serialize_setting_rows(std::ostream& file, rf::Player* player)
{
    for (int c = 0; c < static_cast<int>(SettingCategory::_count); ++c) {
        const auto category = static_cast<SettingCategory>(c);
        const char* section = afs_section_name(category);
        if (!section) {
            continue;
        }
        file << "\n[" << section << "]\n";
        for (const SettingRow* row : settings_in_category(category)) {
            if (row->persistence != SettingPersistence::Afs) {
                continue;
            }
            const std::string_view id{row->id};
            if (id == "player_name") {
                file << "PlayerName=" << player->name << "\n";
                continue;
            }
            if (id == "weapon_prefs") {
                serialize_weapon_prefs(file, player);
                continue;
            }
            if ((row->flags & SF_BESPOKE_INI) || !row->key) {
                continue;
            }
            std::string value;
            if (setting_format_ini_value(*row, player, value)) {
                file << row->key << "=" << value << "\n";
            }
        }
    }
}

void alpine_control_config_serialize(std::ostream& file, rf::Player* player)
{
    const rf::ControlConfig& cc = player->settings.controls;

    file << "\n[ActionBinds]\n";
    file << "; Format is Bind:{Name}={ID},{ScanCode0},{ScanCode1},{MouseButtonID},{ControllerButton},{ControllerAlt}\n";
    file << "; Alpine actions are written as Bind:global:{Name} and are matched by name on load.\n";
    file << "; BindCtx:vehicles:, BindCtx:spectator: and BindCtx:demos: lines override a binding\n";
    file << "; while in a vehicle, spectating or during demo playback; -1 means unbound\n";
    file << "; in that context and ~ means the field inherits its global binding. An action with\n";
    file << "; no override there keeps its global binding.\n";

    // Stock rows keep the positional form because their names are localized; Alpine rows are
    // keyed by their registered English name.
    const int alpine_start = alpine_control_start_index();
    for (int i = 0; i < cc.num_bindings; ++i) {
        // Never the live item: a context applied right now has its overrides written into it.
        const BindOverride global = bind_context_global_binding(i);

        xlog::info("[Settings] Saving Bind: {} = {}, {}, {}, {}, {}, {}",
                   cc.bindings[i].name,
                   i,
                   global.scan_codes[0],
                   global.scan_codes[1],
                   global.mouse_btn_id,
                   global.pad_codes[0],
                   global.pad_codes[1]);

        file << "Bind:";
        if (alpine_start >= 0 && i >= alpine_start) {
            file << "global:";
        }
        file << cc.bindings[i].name << "="
             << i << ","
             << global.scan_codes[0] << ","
             << global.scan_codes[1] << ","
             << global.mouse_btn_id << ","
             << global.pad_codes[0] << ","
             << global.pad_codes[1] << "\n";
    }

    for (int c = 1; c < static_cast<int>(BindContextId::_count); ++c) {
        const auto ctx = static_cast<BindContextId>(c);
        for (int i = 0; i < cc.num_bindings; ++i) {
            const BindOverride* value = bind_context_override(ctx, i);
            if (!value) {
                continue;
            }
            file << bind_key_prefix(ctx) << cc.bindings[i].name << "="
                 << i << ","
                 << bind_field_to_ini(value->scan_codes[0]) << ","
                 << bind_field_to_ini(value->scan_codes[1]) << ","
                 << bind_field_to_ini(value->mouse_btn_id) << ","
                 << bind_field_to_ini(value->pad_codes[0]) << ","
                 << bind_field_to_ini(value->pad_codes[1]) << "\n";
        }
    }
}

static void alpine_settings_serialize_header(std::ostream& file)
{
    std::time_t current_time = std::time(nullptr);
    std::tm* now_tm = std::localtime(&current_time);

    file << "; Alpine Faction Settings File";
    if (rf::mod_param.found()) {
        std::string mod_name = rf::mod_param.get_arg();
        file << " for mod " << mod_name;
    }
    file << "\n\n; This file is automatically generated by Alpine Faction.\n";
    file << "; Unless you really know what you are doing, manually editing it is NOT recommended.\n";
    file << "; Any edits made while the game is running will be discarded.\n\n";

    file << "\n[Metadata]\n";
    file << "; DO NOT edit this section.\n";
    file << "AFSTimestamp=" << std::put_time(now_tm, "%Y-%m-%d %H:%M:%S") << "\n";
    file << "AFSClientVersion=" << PRODUCT_NAME_VERSION << " (" << VERSION_CODE << ")\n";
    file << "AFSFileVersion=" << AFS_VERSION << "\n";

    // Write saved orphaned settings
    if (!orphaned_lines.empty()) {
        file << "\n[OrphanedSettings]\n";
        file << "; Items in this section were unrecognized by your Alpine Faction client.\n";
        file << "; They could be malformed or may require a newer version of Alpine Faction.\n";

        for (const std::string& orphaned_setting : orphaned_lines) {
            file << orphaned_setting << "\n";
        }
    }
}

static void alpine_settings_serialize_saved_votes(std::ostream& file)
{
    // Saved votes. Reindexed from the store every save, so a deletion leaves no gap.
    const std::vector<SavedVote>& saved_votes = saved_votes_get();
    const std::vector<std::string>& unreadable_saved_votes = saved_votes_unparsed();
    if (!saved_votes.empty() || !unreadable_saved_votes.empty()) {
        file << "\n[SavedVotes]\n";
        file << "; Format is SavedVote{N}=2|{Name}|{Type}|{Level}|{GameType}|{TeamSize}|{ExtendMinutes}|{Mutators}|{MutatorsExplicit}\n";

        for (size_t i = 0; i < saved_votes.size(); ++i) {
            file << "SavedVote" << i << "=" << saved_vote_encode(saved_votes[i]) << "\n";
        }
        if (!unreadable_saved_votes.empty()) {
            // Keyed after the valid entries so the reindexing above can never
            // collide with (and so overwrite) one of these.
            file << "; The entries below could not be read by this client and are carried forward as-is.\n";
            for (size_t i = 0; i < unreadable_saved_votes.size(); ++i) {
                file << "SavedVote" << saved_votes.size() + i << "=" << unreadable_saved_votes[i] << "\n";
            }
        }
    }
}

static void alpine_player_settings_serialize(std::ostream& file, rf::Player* player)
{
    alpine_settings_serialize_header(file);
    serialize_setting_rows(file, player);
    alpine_settings_serialize_saved_votes(file);

    alpine_control_config_serialize(file, player);
}

static void alpine_player_settings_save(rf::Player* player)
{
    if (client_bot_launch_enabled() || is_awpgen_active()) {
        return;
    }

    std::string filename = alpine_get_settings_filename();
    std::ofstream file(filename);

    if (!file.is_open()) {
        xlog::info("[Settings] Failed to write {}", filename);
        return;
    }

    alpine_player_settings_serialize(file, player);

    file.close();
    xlog::info("Saved settings to {}", filename);
}

void alpine_core_config_save()
{
    if (rf::local_player) {
        alpine_player_settings_save(rf::local_player);
    }
}

static void close_and_restart_game() {
    g_restart_on_close = true;
    rf::ui::mainmenu_quit_game_confirmed();
}

void ignore_ff_link_prompt()
{
    rf::console::print("Ignoring FF link prompt...");
}

void open_ff_link_info_and_close_game()
{
    open_url("https://alpinefaction.com/link");
    rf::ui::mainmenu_quit_game_confirmed();
}

// defaults if alpine_settings.ini isn't loaded
void set_alpine_config_defaults() {
    rf::game_set_gore_level(2);
    rf::g_fast_animations = false;
    g_alpine_game_config.save_console_history = true; // must be set here because evaluated before config loaded
    build_time_left_string_format();
    set_play_sound_events_volume_scale();
    apply_entity_sim_distance();
}

static void set_headless_defaults(rf::Player* player, const char* player_name, unsigned max_fps)
{
    // Apply general defaults first, then override with headless-specific values
    set_alpine_config_defaults();
    g_alpine_game_config.rendering_enabled = false;
    g_alpine_game_config.sound_enabled = false;
    rf::sound_enabled = false;
    set_sound_enabled(false);
    g_alpine_game_config.swap_ar_controls = false;
    g_alpine_game_config.swap_gn_controls = false;
    g_alpine_game_config.swap_sg_controls = false;
    g_alpine_game_config.direct_input = false;
    g_alpine_game_config.save_console_history = false;
    g_alpine_game_config.set_max_fps(max_fps);
    g_alpine_game_config.dbg_bot = false;
    g_loaded_alpine_settings_file = true;

    if (player) {
        std::strncpy(player->settings.name, player_name, sizeof(player->settings.name) - 1);
        player->settings.name[sizeof(player->settings.name) - 1] = '\0';
        player->name = player_name;
    }
}

CallHook<void(rf::Player*)> player_settings_load_hook{
    0x004B2726,
    [](rf::Player* player) {
        // Headless modes skip all settings I/O
        if (is_headless_mode()) {
            if (is_awpgen_active()) {
                set_headless_defaults(player, "af_awpgen", AlpineGameSettings::max_fps_limit);
            }
            else {
                set_headless_defaults(player, "af_bot", 30);
            }
            return;
        }

        bool ff_link_prompt = true;
        bool settings_file_found = false;
        try {
            settings_file_found = alpine_player_settings_load(player);
        }
        catch (const std::exception& e) {
            // A hand-edited or truncated value throws out of std::stoi/std::stof and would
            // otherwise take down startup for good. The file exists, so keep compiled defaults
            // for whatever came after the bad line instead of importing players.cfg over it.
            xlog::error("Malformed value in Alpine Faction settings file: {}", e.what());
            settings_file_found = true;
        }
        if (!settings_file_found) {
            xlog::warn("Alpine Faction settings file not found. Attempting to import legacy RF settings file.");
            player_settings_load_hook.call_target(player); // load players.cfg

            set_alpine_config_defaults();
            gamepad_reset_to_defaults();

            // Display restart popup due to players.cfg import
            // players.cfg from legacy client version will import fine on first load, apart from Alpine controls
            // Restart cleanly loads game without baggage from players.cfg, and adds Alpine controls without issue
            if (g_loaded_players_cfg_file) {
                ff_link_prompt = false; // do not display both ff link popup and config migration popup

                const char* choices[1] = {"RESTART GAME"};
                void (*callbacks[1])() = {close_and_restart_game};
                int keys[1] = {1};
                
                rf::ui::popup_custom(
                    "Legacy Red Faction Settings Imported",
                    "Alpine Faction must restart to finish applying imported settings.\n\nIf you have any questions, visit alpinefaction.com/help",
                    1, choices, callbacks, 1, keys);
            }
            else {
                xlog::warn("Legacy RF settings file not found. Applying default settings.");
            }
        }

        // Re-apply gyro calibration system
        gyro_update_calibration_mode();

        // HACKFIX.  We need to load our settings earlier.
        if (g_alpine_game_config.sample_count != 1) {
           gr_flush_frame_buffers();
        }

        // display popup recommending ff link (skip for bots)
        if (ff_link_prompt && !g_game_config.suppress_ff_link_prompt && !client_bot_launch_enabled()) {
            g_game_config.suppress_ff_link_prompt = true; // only display popup once
            g_game_config.save();

            // only display popup if unlinked
            if (g_game_config.fflink_token.value().empty()) {
                const char* choices[2] = {"IGNORE", "LEARN MORE"};
                void (*callbacks[2])() = {ignore_ff_link_prompt, open_ff_link_info_and_close_game};
                int keys[2] = {1, 2};

                rf::ui::popup_custom(
                    "IMPORTANT: Not yet linked to FactionFiles!",
                    "FactionFiles account linking enables achievements and other features.\nTo learn more, click to visit alpinefaction.com/link",
                    2, choices, callbacks, 1, keys);
            }
        }
    }
};

FunHook<void(rf::Player*)> player_settings_save_hook{
    0x004A8F50,
    [](rf::Player* player) {
        if (client_bot_launch_enabled() || is_awpgen_active()) {
            return;
        }
        g_alpine_system_config.save();
        alpine_player_settings_save(player);
    }
};

CallHook<void(rf::Player*)> player_settings_save_quit_hook{
    0x004B2D77,
    [](rf::Player* player) {
        player_settings_save_quit_hook.call_target(player);

        if (client_bot_launch_enabled() || is_awpgen_active()) {
            return;
        }

        if (g_restart_on_close) {
            xlog::info("Restarting Alpine Faction to finish applying imported settings.");
            std::string af_install_dir = get_module_dir(g_hmodule);
            std::string af_launcher_filename = "AlpineFactionLauncher.exe";
            std::string af_launcher_arguments = " -game";

            if (rf::mod_param.found()) {
                std::string mod_name = rf::mod_param.get_arg();
                af_launcher_arguments = " -game -mod " + mod_name;
            }

            std::string af_launcher_path = af_install_dir + af_launcher_filename;
            xlog::warn("executing {}{}", af_launcher_path, af_launcher_arguments);
            if (PathFileExistsA(af_launcher_path.c_str())) {
                ShellExecuteA(nullptr, "open", af_launcher_path.c_str(), af_launcher_arguments.c_str(), nullptr, SW_SHOWNORMAL);
            }
        }
    }
};

CodeInjection player_settings_load_players_cfg_patch{
    0x004A8E6F,
    []() {
        xlog::warn("Successfully imported legacy RF settings file. Client must restart to finish applying imported settings.");
        g_loaded_players_cfg_file = true;
    }
};

// apply tracker from player settings config file
CallHook<int(const char*, const char*, char*)> os_config_read_string_tracker_hook{
    0x00482B97,
    [](const char* key_name, const char* value_name, char* str_out) {
        constexpr size_t max_len = AlpineGameSettings::max_tracker_hostname_length + 1; // allow for null terminator
        std::strncpy(str_out, g_alpine_game_config.multiplayer_tracker.c_str(), max_len - 1);
        str_out[max_len - 1] = '\0';
        return 0; // success
    }
};

// called before rf_init initializes console
void initialize_alpine_core_config() {
    if (!g_alpine_system_config.load()) {
        xlog::warn("Alpine Faction core config file not found. Using default configuration.");
    }
}

// apply vsync from system config file at game start
CallHook<int(const char*, const char*, unsigned*, unsigned)> os_config_read_uint_vsync_hook{
    0x00545B0F,
    [](const char* key_name, const char* value_name, unsigned* data, unsigned def_val) {
        *data = static_cast<unsigned>(g_alpine_system_config.vsync);
        return 0;
    }
};

ConsoleCommand2 shadow_corpses_cmd{
    "r_shadowcorpses",
    []() {
        setting_console_set("shadow_corpses", "toggle");
        rf::console::print("Corpse shadows: {}", g_alpine_game_config.shadow_corpses ? "enabled" : "disabled");
    },
    "Toggle corpse shadow rendering",
};

ConsoleCommand2 shadow_items_cmd{
    "r_shadowitems",
    []() {
        setting_console_set("shadow_items", "toggle");
        rf::console::print("Item shadows: {}", g_alpine_game_config.shadow_items ? "enabled" : "disabled");
    },
    "Toggle item shadow rendering",
};

ConsoleCommand2 dbg_shadows_cmd{
    "dbg_shadows",
    []() {
        using ESR = gr::d3d11::EntityShadowRenderer;
        if (rf::is_multi && !rf::is_server) {
            rf::console::print("This command is only available in single-player or as host");
            return;
        }
        ESR::debug_enabled = !ESR::debug_enabled;
        rf::console::print("Shadow debug overlay: {}", ESR::debug_enabled ? "enabled" : "disabled");
    },
    "Toggle shadow map debug overlay (SP/host only)",
};

ConsoleCommand2 shadow_distance_cmd{
    "r_shadowdistance",
    [](std::optional<int> value_opt) {
        if (value_opt) {
            setting_console_set("shadow_distance", std::to_string(value_opt.value()));
        }
        using ESR = gr::d3d11::EntityShadowRenderer;
        int d = g_alpine_game_config.shadow_distance;
        rf::console::print("Shadow distance: {} ({}) [0-5]",
            d, ESR::preset_names[d]);
    },
    "Set shadow distance preset (0-5)",
    "r_shadowdistance <0-5>",
};

ConsoleCommand2 shadow_quality_cmd{
    "r_shadowquality",
    [](std::optional<int> value_opt) {
        if (value_opt) {
            setting_console_set("shadow_quality", std::to_string(value_opt.value()));
        }
        using ESR = gr::d3d11::EntityShadowRenderer;
        int q = g_alpine_game_config.shadow_quality;
        const auto& preset = ESR::shadow_quality_presets[q];
        if (preset.resolution == 0) {
            rf::console::print("Shadow quality: {} ({}) - blob shadows [0-5]",
                q, ESR::preset_names[q]);
        }
        else {
            rf::console::print("Shadow quality: {} ({}) - {}x{}, {} PCF taps [0-5]",
                q, ESR::preset_names[q], preset.resolution, preset.resolution, preset.pcf_taps);
        }
    },
    "Set shadow quality preset (0=blob, 1-5=shadow maps)",
    "r_shadowquality <0-5>",
};

ConsoleCommand2 shadow_frame_lag_cmd{
    "r_shadowupdateinterval",
    [](std::optional<int> value_opt) {
        if (value_opt) {
            setting_console_set("shadow_frame_lag", std::to_string(value_opt.value()));
        }
        rf::console::print("Shadow update interval: {} (shadow map refreshes every {} frame{})",
            g_alpine_game_config.shadow_frame_lag,
            g_alpine_game_config.shadow_frame_lag,
            g_alpine_game_config.shadow_frame_lag == 1 ? "" : "s");
    },
    "Set shadow map update interval in frames (1=every frame, 2-30=reuse cached shadow map)",
    "r_shadowupdateinterval <1-30>",
};

ConsoleCommand2 load_settings_cmd{
    "dbg_loadsettings",
    []() {
        try {
            alpine_player_settings_load(rf::local_player);
        }
        catch (const std::exception& e) {
            xlog::error("Malformed value in Alpine Faction settings file: {}", e.what());
        }
        rf::console::print("Loading settings file...");
    },
    "Force the game to read and apply player settings from config file",
};

ConsoleCommand2 save_settings_cmd{
    "dbg_savesettings",
    []() {
        g_alpine_system_config.save();
        alpine_player_settings_save(rf::local_player);
        rf::console::print("Saving settings files...");
    },
    "Force a write of current player settings and system config files",
};

void alpine_settings_apply_patch()
{
    // Handle loading and saving settings ini file
    player_settings_load_hook.install();
    player_settings_save_hook.install();
    player_settings_save_quit_hook.install();
    player_settings_load_players_cfg_patch.install();

    // Handle instances where alpine settings are used instead of registry keys
    os_config_read_string_tracker_hook.install();
    os_config_read_uint_vsync_hook.install();

    // Register commands
    load_settings_cmd.register_cmd();
    save_settings_cmd.register_cmd();
    shadow_corpses_cmd.register_cmd();
    shadow_items_cmd.register_cmd();
    shadow_distance_cmd.register_cmd();
    shadow_quality_cmd.register_cmd();
    shadow_frame_lag_cmd.register_cmd();
    dbg_shadows_cmd.register_cmd();

    // Init cmd line
    get_afs_cmd_line_param();
}
