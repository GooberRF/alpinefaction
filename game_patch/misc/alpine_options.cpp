#include "alpine_options.h"
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/ShortTypes.h>
#include <patch_common/AsmWriter.h>
#include <regex>
#include "player.h"
#include "alpine_settings.h"
#include "../misc/vpackfile.h"
#include "../os/console.h"
#include "../rf/file/file.h"
#include "../rf/gr/gr.h"
#include "../rf/player/camera.h"
#include "../rf/sound/sound.h"
#include "../rf/geometry.h"
#include "../rf/level.h"
#include "../rf/entity.h"
#include "../rf/multi.h"
#include "../rf/misc.h"
#include "../rf/os/os.h"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <windows.h>
#include <shellapi.h>
#include <xlog/xlog.h>

AlpineOptionsConfig g_alpine_options_config;
AlpineLevelInfoConfig g_alpine_level_info_config;

static std::unordered_set<std::string> g_p2t_fix_levels; // list of levels to apply power2tex fix
static std::unordered_set<std::string> g_known_run_levels; // list of run mode levels manually added
static std::string af_default_player_entity = "miner1";
static std::string af_suit_player_entity = "parker_suit";
static std::string af_sci_player_entity = "parker_sci";

bool is_p2t_fix_level(const std::string& filename)
{
    return g_p2t_fix_levels.contains(string_to_lower(filename));
}

bool is_known_run_level(const std::string& filename)
{
    std::string level_name = filename;
    
    if (!string_iends_with(level_name, ".rfl")) {
        level_name += ".rfl";
    }

    return g_known_run_levels.contains(string_to_lower(level_name));
}

// trim leading and trailing whitespace
std::string trim(const std::string& str, bool remove_quotes = false)
{
    auto start = str.find_first_not_of(" \t\n\r");
    auto end = str.find_last_not_of(" \t\n\r");
    if (start == std::string::npos) {
        return "";
    }        

    std::string trimmed = str.substr(start, end - start + 1);

    // extract quoted value
    if (remove_quotes && trimmed.size() >= 2 && trimmed.front() == '"' && trimmed.back() == '"') {
        trimmed = trimmed.substr(1, trimmed.size() - 2);
    }

    return trimmed;
}

// ===== Parsers for AlpineOptions =====

// strings can be provided in quotation marks or not
std::optional<OptionValue> parse_string(const std::string& value)
{
    return trim(value, true);
}

// colors can be provided in quotation marks or not, and can be hex formatted or RF-style
std::optional<OptionValue> parse_color(const std::string& value)
{
    std::string trimmed_value = trim(value, true);

    try {
        // Check if it's a valid hex string (6 or 8 characters)
        if (!trimmed_value.empty() && trimmed_value.length() <= 8 &&
            std::all_of(trimmed_value.begin(), trimmed_value.end(), ::isxdigit)) {
            uint32_t color = std::stoul(trimmed_value, nullptr, 16); // Parse hex

            // If it's a 24-bit color (6 characters), add full alpha (0xFF)
            if (trimmed_value.length() == 6)
                color = (color << 8) | 0xFF;

            return color;
        }

        // Check for RGB formats using regex
        std::regex rgb_pattern(R"([\{\<]?\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*[\>\}]?)");
        std::smatch matches;

        if (std::regex_match(trimmed_value, matches, rgb_pattern)) {
            if (matches.size() == 4) { // First match is the full string, next 3 are R, G, B
                int r = std::stoi(matches[1].str());
                int g = std::stoi(matches[2].str());
                int b = std::stoi(matches[3].str());

                // Ensure values are within the valid range
                if (r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255) {
                    uint32_t color = (r << 24) | (g << 16) | (b << 8) | 0xFF; // Add full alpha
                    return color;
                }
            }
        }
    }
    catch (...) {
        return std::nullopt; // Return null if parsing fails
    }

    return std::nullopt;
}

// floats, ints, and bools do not use quotation marks
std::optional<OptionValue> parse_float(const std::string& value) { 
    try { return std::stof(value); } catch (...) { return std::nullopt; } 
}
std::optional<OptionValue> parse_int(const std::string& value) { 
    try { return std::stoi(value); } catch (...) { return std::nullopt; } 
}
std::optional<OptionValue> parse_bool(const std::string& value) { 
    return value == "1" || value == "true" || value == "True"; 
}

// master list of options: mapped to option ID, associated tbl, and parser
const std::unordered_map<std::string, OptionMetadata> option_metadata = {
    {"$Default Geomod Smoke Emitter", {AlpineOptionID::GeomodEmitter_Default, "af_game.tbl", parse_string}},
    {"$Driller Geomod Smoke Emitter", {AlpineOptionID::GeomodEmitter_Driller, "af_game.tbl", parse_string}},
    {"$Ice Geomod Texture", {AlpineOptionID::GeomodTexture_Ice, "af_game.tbl", parse_string}},
    {"$First Level Filename", {AlpineOptionID::FirstLevelFilename, "af_game.tbl", parse_string}},
    {"$Training Level Filename", {AlpineOptionID::TrainingLevelFilename, "af_game.tbl", parse_string}},
    {"$Disable Multiplayer Button", {AlpineOptionID::DisableMultiplayerButton, "af_ui.tbl", parse_bool}},
    {"$Disable Singleplayer Buttons", {AlpineOptionID::DisableSingleplayerButtons, "af_ui.tbl", parse_bool}},
    {"$Use Base Game Players Config", {AlpineOptionID::UseStockPlayersConfig, "af_game.tbl", parse_bool}},
    {"$Assault Rifle Ammo Counter Color", {AlpineOptionID::AssaultRifleAmmoColor, "af_client.tbl", parse_color}},
    {"$Precision Rifle Scope Color", {AlpineOptionID::PrecisionRifleScopeColor, "af_client.tbl", parse_color}},
    {"$Sniper Rifle Scope Color", {AlpineOptionID::SniperRifleScopeColor, "af_client.tbl", parse_color}},
    {"$Rail Driver Fire Glow Color", {AlpineOptionID::RailDriverFireGlowColor, "af_client.tbl", parse_color}},
    {"$Rail Driver Fire Flash Color", {AlpineOptionID::RailDriverFireFlashColor, "af_client.tbl", parse_color}},
    {"$Summoner Trailer Button Action", {AlpineOptionID::SumTrailerButtonAction, "af_ui.tbl", parse_int}},
    {"+Summoner Trailer Button URL", {AlpineOptionID::SumTrailerButtonURL, "af_ui.tbl", parse_string}},
    {"+Summoner Trailer Button Bink Filename", {AlpineOptionID::SumTrailerButtonBikFile, "af_ui.tbl", parse_string}},
    {"+Summoner Trailer Button Level Filename", {AlpineOptionID::SumTrailerButtonLevelFile, "af_ui.tbl", parse_string}},
    {"$Player Entity Type", {AlpineOptionID::PlayerEntityType, "af_game.tbl", parse_string, true}},
    {"$Player Undercover Suit Entity Type", {AlpineOptionID::PlayerSuitEntityType, "af_game.tbl", parse_string}},
    {"$Player Undercover Scientist Entity Type", {AlpineOptionID::PlayerScientistEntityType, "af_game.tbl", parse_string}},
    {"$Fall Damage Land Multiplier", {AlpineOptionID::FallDamageLandMultiplier, "af_game.tbl", parse_float, true}},
    {"$Fall Damage Slam Multiplier", {AlpineOptionID::FallDamageSlamMultiplier, "af_game.tbl", parse_float, true}},
    {"$Multiplayer Walk Speed", {AlpineOptionID::MultiplayerWalkSpeed, "af_game.tbl", parse_float, true}},
    {"$Multiplayer Crouch Walk Speed", {AlpineOptionID::MultiplayerCrouchWalkSpeed, "af_game.tbl", parse_float, true}},
    {"$Walkable Slope Threshold", {AlpineOptionID::WalkableSlopeThreshold, "af_game.tbl", parse_float}},
    {"$Player Headlamp Color", {AlpineOptionID::PlayerHeadlampColor, "af_game.tbl", parse_color}},
    {"$Player Headlamp Intensity", {AlpineOptionID::PlayerHeadlampIntensity, "af_game.tbl", parse_float}},
    {"$Player Headlamp Range", {AlpineOptionID::PlayerHeadlampRange, "af_game.tbl", parse_float}},
    {"$Player Headlamp Radius", {AlpineOptionID::PlayerHeadlampRadius, "af_game.tbl", parse_float}},
    {"$Rail Driver Scanner Color", {AlpineOptionID::RailDriverScannerColor, "af_client.tbl", parse_color}},
    {"$Multi Timer X Offset", {AlpineOptionID::MultiTimerXOffset, "af_client.tbl", parse_int}},
    {"$Multi Timer Y Offset", {AlpineOptionID::MultiTimerYOffset, "af_client.tbl", parse_int}},
    {"$Multi Timer Color", {AlpineOptionID::MultiTimerColor, "af_client.tbl", parse_color}},
    {"$Default Third Person", {AlpineOptionID::DefaultThirdPerson, "af_game.tbl", parse_bool}},
};

// ===== Parsers for Alpine level info =====

std::string trim_level(const std::string& str)
{
    auto start = str.find_first_not_of(" \t\n\r");
    auto end = str.find_last_not_of(" \t\n\r");
    if (start == std::string::npos)
        return "";
    return str.substr(start, end - start + 1);
}

// Parsing functions for level settings
std::optional<LevelInfoValue> parse_float_level(const std::string& value)
{
    try { return std::stof(value); } catch (...) { return std::nullopt; } 
}
std::optional<LevelInfoValue> parse_int_level(const std::string& value)
{
    try { return std::stoi(value); } catch (...) { return std::nullopt; } 
}
std::optional<LevelInfoValue> parse_bool_level(const std::string& value)
{
    return value == "1" || value == "true" || value == "True";
}
std::optional<LevelInfoValue> parse_string_level(const std::string& value)
{
    return trim(value, true);
}

std::optional<LevelInfoValue> parse_color_level(const std::string& value)
{
    std::string trimmed_value = trim(value, true);

    try {
        // Check if it's a valid hex string (6 or 8 characters)
        if (!trimmed_value.empty() && trimmed_value.length() <= 8 &&
            std::all_of(trimmed_value.begin(), trimmed_value.end(), ::isxdigit)) {
            uint32_t color = std::stoul(trimmed_value, nullptr, 16); // Parse hex

            // If it's a 24-bit color (6 characters), add full alpha (0xFF)
            if (trimmed_value.length() == 6)
                color = (color << 8) | 0xFF;

            return color;
        }

        // Check for RGB formats using regex
        std::regex rgb_pattern(R"([\{\<]?\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*[\>\}]?)");
        std::smatch matches;

        if (std::regex_match(trimmed_value, matches, rgb_pattern)) {
            if (matches.size() == 4) { // First match is the full string, next 3 are R, G, B
                int r = std::stoi(matches[1].str());
                int g = std::stoi(matches[2].str());
                int b = std::stoi(matches[3].str());

                // Ensure values are within the valid range
                if (r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255) {
                    uint32_t color = (r << 24) | (g << 16) | (b << 8) | 0xFF; // Add full alpha
                    return color;
                }
            }
        }
    }
    catch (...) {
        return std::nullopt; // Return null if parsing fails
    }

    return std::nullopt;
}

std::optional<std::pair<std::string, std::string>> parse_mesh_replacement(const std::string& value)
{
    std::string trimmed_value = trim(value, true);
    std::regex mesh_replacement_pattern("\\{\\s*\"([^\"]+)\"\\s*,\\s*\"([^\"]+)\"\\s*\\}");
    std::smatch matches;

    if (std::regex_match(trimmed_value, matches, mesh_replacement_pattern)) {
        if (matches.size() == 3) { // Full match and 2 filename captures
            return std::make_pair(trim(matches[1].str(), true), trim(matches[2].str(), true));
        }
    }
    return std::nullopt;
}

// Metadata map for level options
const std::unordered_map<std::string, LevelInfoMetadata> level_info_metadata = {
    {"$Lightmap Clamp Floor", {AlpineLevelInfoID::LightmapClampFloor, parse_color_level}},
    {"$Lightmap Clamp Ceiling", {AlpineLevelInfoID::LightmapClampCeiling, parse_color_level}},
    {"$Ideal Player Count", {AlpineLevelInfoID::IdealPlayerCount, parse_int_level}},
    {"$Author Contact", {AlpineLevelInfoID::AuthorContact, parse_string_level}},
    {"$Author Website", {AlpineLevelInfoID::AuthorWebsite, parse_string_level}},
    {"$Description", {AlpineLevelInfoID::Description, parse_string_level}},
    {"$Player Headlamp Color", {AlpineLevelInfoID::PlayerHeadlampColor, parse_color_level}},
    {"$Player Headlamp Intensity", {AlpineLevelInfoID::PlayerHeadlampIntensity, parse_float_level}},
    {"$Player Headlamp Range", {AlpineLevelInfoID::PlayerHeadlampRange, parse_float_level}},
    {"$Player Headlamp Radius", {AlpineLevelInfoID::PlayerHeadlampRadius, parse_float_level}},
    {"$Chat Menu 1", {AlpineLevelInfoID::ChatMap1, parse_string_level}},
    {"$Chat Menu 2", {AlpineLevelInfoID::ChatMap2, parse_string_level}},
    {"$Chat Menu 3", {AlpineLevelInfoID::ChatMap3, parse_string_level}},
    {"$Chat Menu 4", {AlpineLevelInfoID::ChatMap4, parse_string_level}},
    {"$Chat Menu 5", {AlpineLevelInfoID::ChatMap5, parse_string_level}},
    {"$Chat Menu 6", {AlpineLevelInfoID::ChatMap6, parse_string_level}},
    {"$Chat Menu 7", {AlpineLevelInfoID::ChatMap7, parse_string_level}},
    {"$Chat Menu 8", {AlpineLevelInfoID::ChatMap8, parse_string_level}},
    {"$Chat Menu 9", {AlpineLevelInfoID::ChatMap9, parse_string_level}},
    {"$Crater Texture PPM", {AlpineLevelInfoID::CraterTexturePPM, parse_float_level}}
};

// Load level info from filename_info.tbl
void load_level_info_config(const std::string& level_filename)
{
    if (g_alpine_level_info_config.current_level == level_filename) {
        xlog::debug("Reusing previously loaded mapname_info.tbl settings for {}", level_filename);
        return; // already loaded, reuse
    }

    g_alpine_level_info_config.reset_for_level(level_filename);

    std::string base_filename = level_filename.substr(0, level_filename.size() - 4);
    std::string info_filename = base_filename + "_info.tbl";
    auto level_info_file = std::make_unique<rf::File>();

    if (level_info_file->open(info_filename.c_str()) != 0) {
        xlog::debug("Could not open {}", info_filename);
        return;
    }

    xlog::debug("Successfully opened {}", info_filename);

    // Read entire file content into a single string buffer
    std::string file_content;
    std::string buffer(2048, '\0');
    int bytes_read;

    while ((bytes_read = level_info_file->read(&buffer[0], buffer.size() - 1)) > 0) {
        buffer.resize(bytes_read);
        file_content += buffer;
        buffer.resize(2048, '\0');
    }

    level_info_file->close();

    // Process file content line-by-line
    std::istringstream file_stream(file_content);
    std::string line;
    bool found_start = false;

    // Search for #Start marker
    while (std::getline(file_stream, line)) {
        line = trim(line, false);
        if (line == "#Start") {
            found_start = true;
            break;
        }
    }

    if (!found_start) {
        xlog::warn("No #Start marker found in {}", info_filename);
        return;
    }

    // Process options until #End marker is found
    bool found_end = false;
    while (std::getline(file_stream, line)) {
        line = trim(line, false);

        if (line == "#End") {
            found_end = true;
            break;
        }
        if (line.empty() || line.starts_with("//")) {
            continue;
        }

        auto delimiter_pos = line.find(':');
        if (delimiter_pos == std::string::npos || line[0] != '$') {
            continue;
        }

        std::string option_name = trim(line.substr(0, delimiter_pos), false);
        std::string option_value = trim(line.substr(delimiter_pos + 1), false);

        // handle mesh replacements
        if (option_name == "$Mesh Replacement") {
            auto parsed_value = parse_mesh_replacement(option_value);
            if (parsed_value) {
                g_alpine_level_info_config.mesh_replacements[string_to_lower(parsed_value->first)] = parsed_value->second;

                xlog::debug("Mesh Replacement Added: {} -> {} in {}", parsed_value->first, parsed_value->second, level_filename);
            }
            else {
                xlog::warn("Invalid mesh replacement format in {}", info_filename);
            }

            continue;
        }

        // handle other level info options
        auto meta_it = level_info_metadata.find(option_name);
        if (meta_it != level_info_metadata.end()) {
            const auto& metadata = meta_it->second;
            auto parsed_value = metadata.parse_function(option_value);
            if (parsed_value) {
                g_alpine_level_info_config.level_options[metadata.id] = *parsed_value;
                xlog::debug("Parsed and applied {} for {}: {}", option_name, level_filename, option_value);
            }
        }
        else {
            xlog::warn("Unknown option {} in {}", option_name, info_filename);
        }
    }

    if (!found_end) {
        xlog::warn("No #End marker found in {}", info_filename);
    }
}

void open_url(const std::string& url)
{
    try {
        if (url.empty()) {
            xlog::error("URL is empty");
            return;
        }
        xlog::info("Opening URL: {}", url);
        HINSTANCE result = ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOW);
        if (reinterpret_cast<int>(result) <= 32) {
            xlog::error("Failed to open URL. Error code: {}", reinterpret_cast<int>(result));
        }
    }
    catch (const std::exception& ex) {
        xlog::error("Exception occurred while trying to open URL: {}", ex.what());
    }
}

CallHook<void(int, int, int, int)> fpgun_ar_ammo_digit_color_hook{
    0x004ABC03,
    [](int red, int green, int blue, int alpha) {
        if (g_alpine_options_config.is_option_loaded(AlpineOptionID::AssaultRifleAmmoColor)) {
            auto ar_ammo_color = get_option_value<uint32_t>(AlpineOptionID::AssaultRifleAmmoColor);
            std::tie(red, green, blue, alpha) = extract_color_components(ar_ammo_color);
        }
        else if (g_alpine_game_config.ar_ammo_digit_color_override) {
            std::tie(red, green, blue, alpha) =
                extract_color_components(*g_alpine_game_config.ar_ammo_digit_color_override);
        }
        fpgun_ar_ammo_digit_color_hook.call_target(red, green, blue, alpha);
    }
};

CallHook<void(int, int, int, int)> precision_rifle_scope_color_hook{
    0x004AC850, [](int red, int green, int blue, int alpha) {
        if (g_alpine_options_config.is_option_loaded(AlpineOptionID::PrecisionRifleScopeColor)) {
            auto pr_scope_color = get_option_value<uint32_t>(AlpineOptionID::PrecisionRifleScopeColor);
            std::tie(red, green, blue, alpha) = extract_color_components(pr_scope_color);
        }
        else if (g_alpine_game_config.precision_scope_color_override) {
            std::tie(red, green, blue, alpha) =
                extract_color_components(*g_alpine_game_config.precision_scope_color_override);
        }
        precision_rifle_scope_color_hook.call_target(red, green, blue, alpha);
    }
};

CallHook<void(int, int, int, int)> sniper_rifle_scope_color_hook{
    0x004AC458, [](int red, int green, int blue, int alpha) {
        if (g_alpine_options_config.is_option_loaded(AlpineOptionID::SniperRifleScopeColor)) {
            auto sr_scope_color = get_option_value<uint32_t>(AlpineOptionID::SniperRifleScopeColor);
            std::tie(red, green, blue, alpha) = extract_color_components(sr_scope_color);
        }
        else if (g_alpine_game_config.sniper_scope_color_override) {
            std::tie(red, green, blue, alpha) =
                extract_color_components(*g_alpine_game_config.sniper_scope_color_override);
        }
        sniper_rifle_scope_color_hook.call_target(red, green, blue, alpha);
    }
};

CallHook<void(int, int, int, int)> rail_gun_fire_glow_hook{
    0x004AC00E, [](int red, int green, int blue, int alpha) {
        auto rail_glow_color = get_option_value<uint32_t>(AlpineOptionID::RailDriverFireGlowColor);
        std::tie(red, green, blue, alpha) = extract_color_components(rail_glow_color);
        rail_gun_fire_glow_hook.call_target(red, green, blue, alpha);
    }
};

CallHook<void(int, int, int, int)> rail_gun_fire_flash_hook{
    0x004AC04A, [](int red, int green, int blue, int alpha) {
        auto rail_flash_color = get_option_value<uint32_t>(AlpineOptionID::RailDriverFireFlashColor);
        std::tie(red, green, blue, std::ignore) = extract_color_components(rail_flash_color);
        rail_gun_fire_flash_hook.call_target(red, green, blue, alpha);
    }
};

CallHook<void(int, int, int, int)> rail_driver_scanner_color_hook{
    0x004323AA, [](int red, int green, int blue, int alpha) {
        if (g_alpine_options_config.is_option_loaded(AlpineOptionID::RailDriverScannerColor)) {
            auto rail_scope_color = get_option_value<uint32_t>(AlpineOptionID::RailDriverScannerColor);
            std::tie(red, green, blue, alpha) = extract_color_components(rail_scope_color);
        }
        else if (g_alpine_game_config.rail_scope_color_override) {
            std::tie(red, green, blue, alpha) =
                extract_color_components(*g_alpine_game_config.rail_scope_color_override);
        }
        rail_driver_scanner_color_hook.call_target(red, green, blue, alpha);
    }
};

// Override default geomod smoke emitter
CallHook<int(const char*)> default_geomod_emitter_get_index_hook{
    0x00437150, [](const char* emitter_name) -> int {
        auto modded_emitter_name = get_option_value<std::string>(AlpineOptionID::GeomodEmitter_Default);
        return default_geomod_emitter_get_index_hook.call_target(modded_emitter_name.c_str());
    }
};

// Override driller geomod smoke emitter
CallHook<int(const char*)> driller_geomod_emitter_get_index_hook{
    0x0043715F, [](const char* emitter_name) -> int {
        auto modded_emitter_name = get_option_value<std::string>(AlpineOptionID::GeomodEmitter_Driller);
        return driller_geomod_emitter_get_index_hook.call_target(modded_emitter_name.c_str());
    }
};

// Replace ice geomod region texture
CallHook<int(const char*, int, bool)> ice_geo_crater_bm_load_hook {
    {
        0x004673B5, // chunks
        0x00466BEF, // crater
    },
    [](const char* filename, int path_id, bool generate_mipmaps) -> int {
        auto modded_filename = get_option_value<std::string>(AlpineOptionID::GeomodTexture_Ice);
        return ice_geo_crater_bm_load_hook.call_target(modded_filename.c_str(), path_id, generate_mipmaps);
    }
};

// fall damage when impacting
CallHook<void(rf::Entity*, float)> physics_calc_fall_damage_slam_hook{
    0x0049DE39,
    [](rf::Entity* entity, float rel_vel) {
        float damage_multiplier = get_option_or_default<float>(AlpineOptionID::FallDamageSlamMultiplier, 1.0f);
        float adjusted_rel_vel = rel_vel * damage_multiplier;

        xlog::warn("New slam damage value is {}", adjusted_rel_vel);

        physics_calc_fall_damage_slam_hook.call_target(entity, adjusted_rel_vel);
    }
};

// fall damage when landing
CallHook<void(rf::Entity*, float)> physics_calc_fall_damage_land_hook{
    0x004A0C28,
    [](rf::Entity* entity, float rel_vel) {
        float damage_multiplier = get_option_or_default<float>(AlpineOptionID::FallDamageLandMultiplier, 1.0f);
        float adjusted_rel_vel = rel_vel * damage_multiplier;

        xlog::warn("New land damage value is {}", adjusted_rel_vel);

        physics_calc_fall_damage_land_hook.call_target(entity, adjusted_rel_vel);
    }
};

// Override first level filename for new game menu
CallHook<void(const char*)> first_load_level_hook{
    0x00443B15, [](const char* level_name) {
        auto new_level_name = get_option_value<std::string>(AlpineOptionID::FirstLevelFilename);
        first_load_level_hook.call_target(new_level_name.c_str());
    }
};

// Override training level filename for new game menu
CallHook<void(const char*)> training_load_level_hook{
    0x00443A85, [](const char* level_name) {
        auto new_level_name = get_option_value<std::string>(AlpineOptionID::TrainingLevelFilename);
        training_load_level_hook.call_target(new_level_name.c_str());
    }
};

// Implement demo_extras_summoner_trailer_click using FunHook
FunHook<void(int, int)> extras_summoner_trailer_click_hook{
    0x0043EC80, [](int x, int y) {
        xlog::debug("Summoner trailer button clicked");
        int action = get_option_value<int>(AlpineOptionID::SumTrailerButtonAction);

        switch (action) {
            case 1: { // Open URL
            if (g_alpine_options_config.is_option_loaded(AlpineOptionID::SumTrailerButtonURL)) {
                    auto url = get_option_value<std::string>(AlpineOptionID::SumTrailerButtonURL);
                    open_url(url);
                }
                break;
            }
            case 3: // Remove button
            case 2: // Disable button
                break;
            case 4: { // Load level
                auto level_filename = get_option_value<std::string>(AlpineOptionID::SumTrailerButtonLevelFile);
                rf::level_set_level_to_load(level_filename.c_str(), "");
                rf::game_new_game();
                break;
            }
            default: { // Play Bink video
                auto trailer_path = get_option_value<std::string>(AlpineOptionID::SumTrailerButtonBikFile);
                xlog::debug("Playing BIK file: {}", trailer_path);
                rf::snd_pause(true);
                rf::bink_play(trailer_path.c_str());
                rf::snd_pause(false);
                break;
            }
        }
    }
};

void handle_summoner_trailer_button()
{
    int action = get_option_value<int>(AlpineOptionID::SumTrailerButtonAction);
    if (action != -1) {
        if (action == 3) {
            // Action 3: Remove the button
            AsmWriter(0x0043EE14).nop(5);
        }
        else {
            // Otherwise, install the hook
            extras_summoner_trailer_click_hook.install();
        }
    }
}

// default player entity hook (miner1)
CallHook<int(const char*)> player_entity_lookup_type_hook{
    {
        0x004A33D4, // player_allocate
        0x004706A7, // multi_respawn_bot
        0x0046D693, // multi_start
        0x00480865, // multi_spawn_player_server_side
        0x004B0044  // player_undercover_init
    },
    [](const char* entity_name) {
        return player_entity_lookup_type_hook.call_target(af_default_player_entity.c_str());
    }
};

// player entity hook (parker_sci)
CallHook<int(const char*)> player_entity_sci_lookup_type_hook{
    0x004B0062, [](const char* entity_name) {
        return player_entity_sci_lookup_type_hook.call_target(af_sci_player_entity.c_str());
    }
};

// player entity hook (parker_suit)
CallHook<int(const char*)> player_entity_suit_lookup_type_hook{
    0x004B0053, [](const char* entity_name) {
        return player_entity_suit_lookup_type_hook.call_target(af_suit_player_entity.c_str());
    }
};

// Only install hooks for options that have been loaded and specified
void apply_af_options_patches()
{
    xlog::debug("Applying Alpine Faction options patches");

    // ===========================
    // af_client.tbl
    // ===========================
    fpgun_ar_ammo_digit_color_hook.install();

    precision_rifle_scope_color_hook.install();

    sniper_rifle_scope_color_hook.install();

    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::RailDriverFireGlowColor)) {
        rail_gun_fire_glow_hook.install();
    }

    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::RailDriverFireFlashColor)) {
        rail_gun_fire_flash_hook.install();
    }

    rail_driver_scanner_color_hook.install();

    // ===========================
    // af_game.tbl
    // ===========================
    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::UseStockPlayersConfig) &&
        std::get<bool>(g_alpine_options_config.options[AlpineOptionID::UseStockPlayersConfig])) {
        AsmWriter(0x004A8F99).jmp(0x004A9010);
        AsmWriter(0x004A8DCC).jmp(0x004A8E53);
    }

    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::GeomodEmitter_Default)) {
        default_geomod_emitter_get_index_hook.install();
    }

    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::GeomodEmitter_Driller)) {
        driller_geomod_emitter_get_index_hook.install();
    }

    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::GeomodTexture_Ice)) {
        ice_geo_crater_bm_load_hook.install();
    }

    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::FirstLevelFilename)) {
        first_load_level_hook.install();
    }

    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::TrainingLevelFilename)) {
        training_load_level_hook.install();
    }

    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::PlayerEntityType)) {
        af_default_player_entity = get_option_value<std::string>(AlpineOptionID::PlayerEntityType);
        player_entity_lookup_type_hook.install();
    }

    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::PlayerScientistEntityType)) {
        af_sci_player_entity = get_option_value<std::string>(AlpineOptionID::PlayerScientistEntityType);
        player_entity_sci_lookup_type_hook.install();
    }

    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::PlayerSuitEntityType)) {
        af_suit_player_entity = get_option_value<std::string>(AlpineOptionID::PlayerSuitEntityType);
        player_entity_suit_lookup_type_hook.install();
    }

    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::FallDamageSlamMultiplier)) {
        physics_calc_fall_damage_slam_hook.install();
    }

    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::FallDamageLandMultiplier)) {
        physics_calc_fall_damage_land_hook.install();
    }

    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::MultiplayerWalkSpeed)) {
        float multiplayer_walk_speed = get_option_or_default<float>(AlpineOptionID::MultiplayerWalkSpeed, 9.0f);
        rf::multiplayer_walk_speed = multiplayer_walk_speed;
    }

    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::MultiplayerCrouchWalkSpeed)) {
        float multiplayer_crouch_walk_speed = get_option_or_default<float>(AlpineOptionID::MultiplayerCrouchWalkSpeed, 7.0f);
        rf::multiplayer_crouch_walk_speed = multiplayer_crouch_walk_speed;
    }

    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::WalkableSlopeThreshold)) {
        static float walkable_slope_threshold = get_option_or_default<float>(AlpineOptionID::WalkableSlopeThreshold, 0.5f);
        AsmWriter(0x004A0A82).fcomp<float>(AsmRegMem(reinterpret_cast<uintptr_t>(&walkable_slope_threshold)));
    }

    // ===========================
    // af_ui.tbl
    // ===========================
    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::DisableMultiplayerButton) &&
        std::get<bool>(g_alpine_options_config.options[AlpineOptionID::DisableMultiplayerButton])) {
        AsmWriter(0x0044391F).nop(5); // Disable multiplayer button
    }

    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::DisableSingleplayerButtons) &&
        std::get<bool>(g_alpine_options_config.options[AlpineOptionID::DisableSingleplayerButtons])) {
        AsmWriter(0x00443906).nop(5); // Disable save button
        AsmWriter(0x004438ED).nop(5); // Disable load button
        AsmWriter(0x004438D4).nop(5); // Disable new game button
    }

    if (g_alpine_options_config.is_option_loaded(AlpineOptionID::SumTrailerButtonAction)) {
        handle_summoner_trailer_button();
    }
}

void load_single_af_options_file(const std::string& file_name)
{
    auto af_options_file = std::make_unique<rf::File>();
    if (af_options_file->open(file_name.c_str()) != 0) {
        //xlog::warn("Could not open {}", file_name);
        return;
    }
    xlog::debug("Successfully opened {}", file_name);

    // Read entire file content into a single string buffer
    std::string file_content;
    std::string buffer(2048, '\0');
    int bytes_read;

    while ((bytes_read = af_options_file->read(&buffer[0], buffer.size() - 1)) > 0) {
        buffer.resize(bytes_read); // Adjust buffer to actual bytes read
        file_content += buffer;
        buffer.resize(2048, '\0'); // Reset buffer size for next read
    }

    af_options_file->close();

    // Process file content line-by-line
    std::istringstream file_stream(file_content);
    std::string line;
    bool found_start = false;

    // Search for #Start marker
    while (std::getline(file_stream, line)) {
        line = trim(line, false);
        if (line == "#Start") {
            found_start = true;
            break;
        }
    }

    if (!found_start) {
        xlog::warn("No #Start marker found in {}", file_name);
        return;
    }

    // Process options until #End marker is found
    bool found_end = false;
    while (std::getline(file_stream, line)) {
        line = trim(line, false);

        if (line == "#End") {
            found_end = true;
            break;
        }
        if (line.empty() || line.starts_with("//")) {
            continue;
        }

        auto delimiter_pos = line.find(':');
        if (delimiter_pos == std::string::npos || (line[0] != '$' && line[0] != '+')) {
            continue;
        }

        std::string option_name = trim(line.substr(0, delimiter_pos), false);
        std::string option_value = trim(line.substr(delimiter_pos + 1), false);

        // Handle af_level_quirks.tbl
        if (file_name == "af_level_quirks.tbl" &&
            (option_name == "$P2T Fix" || option_name == "$Known Run Maps")) {
            std::regex filename_pattern("\\\"([^\"]+)\\\"");
            auto begin = std::sregex_iterator(option_value.begin(), option_value.end(), filename_pattern);
            auto end = std::sregex_iterator();

            for (std::sregex_iterator i = begin; i != end; ++i) {
                std::string filename = (*i)[1].str();
                if (option_name == "$P2T Fix") {
                    g_p2t_fix_levels.insert(string_to_lower(filename));
                    //xlog::warn("P2T Fix level added: {}", filename);
                }
                else if (option_name == "$Known Run Maps") {
                    g_known_run_levels.insert(string_to_lower(filename));
                    //xlog::warn("Run mode level added: {}", filename);
                }
            }

            continue;
        }

        auto meta_it = option_metadata.find(option_name);

        // Allow any af_client*.tbl file for options designated to af_client.tbl
        bool is_af_client_variant = (meta_it->second.filename == "af_client.tbl" &&
                                     file_name.rfind("af_client", 0) == 0 && file_name.ends_with(".tbl"));

        if (meta_it != option_metadata.end() &&
            (meta_it->second.filename == file_name || is_af_client_variant) &&
            (!rf::is_dedicated_server || meta_it->second.apply_on_server)) {
            
            const auto& metadata = meta_it->second;
            auto parsed_value = metadata.parse_function(option_value);
            if (parsed_value) {
                g_alpine_options_config.options[metadata.id] = *parsed_value;
                g_alpine_options_config.options_loaded[static_cast<std::size_t>(metadata.id)] = true;
                xlog::debug("Parsed and applied option {}: {}", option_name, option_value);
                xlog::debug("Option ID {} marked as loaded", static_cast<std::size_t>(metadata.id));
            }
        }
        else if (meta_it != option_metadata.end()) {
            if (meta_it->second.filename != file_name && !is_af_client_variant) {
                xlog::warn("Option {} in {} skipped (wrong alpine tbl file)", option_name, file_name);
            }
            else if (rf::is_dedicated_server && !meta_it->second.apply_on_server) {
                xlog::debug("Option {} in {} skipped (not needed on dedicated server)", option_name, file_name);
            }
            else {
                xlog::debug("Option {} in {} skipped (reason unknown)", option_name, file_name);
            }
        }
    }

    if (!found_end) {
        xlog::warn("No #End marker found in {}", file_name);
    }
}

void load_af_options_config()
{
    xlog::info("Loading Alpine Faction Options configuration");

    // load af_client_*.tbl
    vpackfile_find_matching_files(
        StringMatcher().prefix("af_client_").suffix(".tbl"),
        [](const char* filename) {
            //xlog::info("Loading configuration file: {}", filename);
            load_single_af_options_file(filename);
        });

    // if a TC mod is loaded, handle the other options files
    if (rf::mod_param.found()) {
        // Collect unique file names from option_metadata
        std::unordered_set<std::string> config_files;
        for (const auto& [name, metadata] : option_metadata) {
            config_files.insert(metadata.filename);
        }

        // Now load the rest of the af_options files
        for (const auto& file_name : config_files) {
            load_single_af_options_file(file_name);
        }
    }

    load_single_af_options_file("af_level_quirks.tbl");

    xlog::debug("Loaded options:");
    for (std::size_t i = 0; i < af_option_count; ++i) {
        if (g_alpine_options_config.options_loaded[i]) {
            xlog::debug("Option {} is loaded", i);
        }
        else {
            xlog::debug("Option {} is NOT loaded", i);
        }
    }

    // Apply all specified patches based on the parsed configuration
    apply_af_options_patches();

    xlog::info("Alpine Faction Options configuration loaded successfully");
}
