#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <common/utils/os-utils.h>
#include "alpine_settings.h"
#include "alpine_options.h"
#include <common/version/version.h>
#include "../os/console.h"
#include "../os/os.h"
#include "../rf/ui.h"
#include "../rf/character.h"
#include "../rf/os/console.h"
#include "../rf/player/player.h"
#include "../rf/sound/sound.h"
#include "../rf/gr/gr.h"
#include <shlwapi.h>
#include <windows.h>
#include <shellapi.h>
#include <array>
#include <fstream>
#include <iostream>
#include <string>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <xlog/xlog.h>

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

// parse colors to 0-255 ints
std::tuple<int, int, int, int> extract_color_components(uint32_t color)
{
    return std::make_tuple(
        (color >> 24) & 0xFF, // Red
        (color >> 16) & 0xFF, // Green
        (color >> 8) & 0xFF,  // Blue
        color & 0xFF          // Alpha
    );
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

    // tc mod
    if (rf::mod_param.found() &&
        !(g_alpine_options_config.is_option_loaded(AlpineOptionID::UseStockPlayersConfig) &&
        std::get<bool>(g_alpine_options_config.options[AlpineOptionID::UseStockPlayersConfig]))) {
        std::string mod_name = rf::mod_param.get_arg();
        return "alpine_settings_" + mod_name + ".ini";
    }

    // normal
    return "alpine_settings.ini";
}

bool alpine_player_settings_load(rf::Player* player)
{
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

    // Load player settings
    if (settings.count("PlayerName")) {
        std::string player_name = settings["PlayerName"];

        if (player_name.length() > 31) {
            xlog::warn("PlayerName in {} is too long and has been truncated.", filename);
            player_name = player_name.substr(0, 31);
        }

        player->name = player_name.c_str();
        processed_keys.insert("PlayerName");
    }
    if (settings.count("GoreLevel")) {
        rf::game_set_gore_level(std::stoi(settings["GoreLevel"]));
        processed_keys.insert("GoreLevel");
    }
    else {
        rf::game_set_gore_level(2); // if gore level not in ini file, default to 2
    }

    if (settings.count("ShowFPGun")) {
        player->settings.render_fpgun = std::stoi(settings["ShowFPGun"]);
        processed_keys.insert("ShowFPGun");
    }
    if (settings.count("AutoswitchWeapons")) {
        player->settings.autoswitch_weapons = std::stoi(settings["AutoswitchWeapons"]);
        processed_keys.insert("AutoswitchWeapons");
    }
    if (settings.count("NeverAutoswitchExplosives")) {
        player->settings.dont_autoswitch_to_explosives = std::stoi(settings["NeverAutoswitchExplosives"]);
        processed_keys.insert("NeverAutoswitchExplosives");
    }
    if (settings.count("ToggleCrouch")) {
        player->settings.toggle_crouch = std::stoi(settings["ToggleCrouch"]);
        processed_keys.insert("ToggleCrouch");
    }
    if (settings.count("DamageScreenFlash")) {
        g_alpine_game_config.damage_screen_flash = std::stoi(settings["DamageScreenFlash"]);
        processed_keys.insert("DamageScreenFlash");
    }
    if (settings.count("SpectateMinimalUI")) {
        g_alpine_game_config.spectate_mode_minimal_ui = std::stoi(settings["SpectateMinimalUI"]);
        processed_keys.insert("SpectateMinimalUI");
    }
    if (settings.count("ShowFPS")) {
        g_alpine_game_config.fps_counter = std::stoi(settings["ShowFPS"]);
        processed_keys.insert("ShowFPS");
    }
    if (settings.count("SaveConsoleHistory")) {
        g_alpine_game_config.save_console_history = std::stoi(settings["SaveConsoleHistory"]);
        apply_console_history_setting();
        processed_keys.insert("SaveConsoleHistory");
    }
    if (settings.count("AlpineBranding")) {
        g_alpine_game_config.af_branding = std::stoi(settings["AlpineBranding"]);
        processed_keys.insert("AlpineBranding");
    }
    if (settings.count("SeasonalEffect")) {
        g_alpine_game_config.seasonal_effect = std::stoi(settings["SeasonalEffect"]);
        processed_keys.insert("SeasonalEffect");
    }
    if (settings.count("RealArmorValues")) {
        g_alpine_game_config.real_armor_values = std::stoi(settings["RealArmorValues"]);
        processed_keys.insert("RealArmorValues");
    }
    if (settings.count("QuickExit")) {
        g_alpine_game_config.quick_exit = std::stoi(settings["QuickExit"]);
        processed_keys.insert("QuickExit");
    }
    if (settings.count("ColorblindMode")) {
        int mode = std::stoi(settings["ColorblindMode"]);
        g_alpine_game_config.colorblind_mode = std::clamp(mode, 0, 3);
        processed_keys.insert("ColorblindMode");
    }
    if (settings.count("AutoswitchFireWait")) {
        g_alpine_game_config.suppress_autoswitch_fire_wait = std::stoi(settings["AutoswitchFireWait"]);
        processed_keys.insert("AutoswitchFireWait");
    }
    if (settings.count("AlwaysAutoswitchEmpty")) {
        g_alpine_game_config.always_autoswitch_empty = std::stoi(settings["AlwaysAutoswitchEmpty"]);
        processed_keys.insert("AlwaysAutoswitchEmpty");
    }

    // Load weapon autoswitch priority
    if (settings.count("WeaponAutoswitchPriority")) {
        std::istringstream weapon_stream(settings["WeaponAutoswitchPriority"]);
        std::string weapon_id;
        int index = 0;

        while (std::getline(weapon_stream, weapon_id, ',') && index < 32) {
            player->weapon_prefs[index++] = std::stoi(weapon_id);
        }

        while (index < 32) {
            player->weapon_prefs[index++] = -1;
        }

        processed_keys.insert("WeaponAutoswitchPriority");
    }

    // Load audio settings
    if (settings.count("EffectsVolume")) {
        rf::snd_set_group_volume(0, std::stof(settings["EffectsVolume"]));
        processed_keys.insert("EffectsVolume");
    }
    if (settings.count("MusicVolume")) {
        rf::snd_set_group_volume(1, std::stof(settings["MusicVolume"]));
        processed_keys.insert("MusicVolume");
    }
    if (settings.count("MessagesVolume")) {
        rf::snd_set_group_volume(2, std::stof(settings["MessagesVolume"]));
        processed_keys.insert("MessagesVolume");
    }
    if (settings.count("LevelSoundVolume")) {
        g_alpine_game_config.set_level_sound_volume(std::stof(settings["LevelSoundVolume"]));
        set_play_sound_events_volume_scale();
        processed_keys.insert("LevelSoundVolume");
    }
    if (settings.count("EntityPainSounds")) {
        g_alpine_game_config.entity_pain_sounds = std::stoi(settings["EntityPainSounds"]);
        processed_keys.insert("EntityPainSounds");
    }

    // Load video settings
    if (settings.count("Gamma")) {
        rf::gr::set_gamma(std::stof(settings["Gamma"]));
        processed_keys.insert("Gamma");
    }
    if (settings.count("ShowShadows")) {
        player->settings.shadows_enabled = std::stoi(settings["ShowShadows"]);
        processed_keys.insert("ShowShadows");
    }
    if (settings.count("ShowDecals")) {
        player->settings.decals_enabled = std::stoi(settings["ShowDecals"]);
        processed_keys.insert("ShowDecals");
    }
    if (settings.count("ShowDynamicLights")) {
        player->settings.dynamic_lightining_enabled = std::stoi(settings["ShowDynamicLights"]);
        processed_keys.insert("ShowDynamicLights");
    }
    if (settings.count("BilinearFiltering")) {
        player->settings.bilinear_filtering = std::stoi(settings["BilinearFiltering"]);
        processed_keys.insert("BilinearFiltering");
    }
    if (settings.count("DetailLevel")) {
        player->settings.detail_level = std::stoi(settings["DetailLevel"]);
        processed_keys.insert("DetailLevel");
    }
    if (settings.count("CharacterDetailLevel")) {
        player->settings.character_detail_level = std::stoi(settings["CharacterDetailLevel"]);
        processed_keys.insert("CharacterDetailLevel");
    }
    if (settings.count("TextureDetailLevel")) {
        player->settings.textures_resolution_level = std::stoi(settings["TextureDetailLevel"]);
        processed_keys.insert("TextureDetailLevel");
    }
    if (settings.count("HorizontalFOV")) {
        g_alpine_game_config.set_horz_fov(std::stof(settings["HorizontalFOV"]));
        processed_keys.insert("HorizontalFOV");
    }
    if (settings.count("FPGunFOVScale")) {
        g_alpine_game_config.set_fpgun_fov_scale(std::stof(settings["FPGunFOVScale"]));
        processed_keys.insert("FPGunFOVScale");
    }
    if (settings.count("DisableWeaponShake")) {
        g_alpine_game_config.try_disable_weapon_shake = std::stoi(settings["DisableWeaponShake"]);
        processed_keys.insert("DisableWeaponShake");
    }
    if (settings.count("FullbrightCharacters")) {
        g_alpine_game_config.try_fullbright_characters = std::stoi(settings["FullbrightCharacters"]);
        processed_keys.insert("FullbrightCharacters");
    }
    if (settings.count("DisableTextures")) {
        g_alpine_game_config.try_disable_textures = std::stoi(settings["DisableTextures"]);
        processed_keys.insert("DisableTextures");
    }
    if (settings.count("DisableMuzzleFlashLights")) {
        g_alpine_game_config.try_disable_muzzle_flash_lights = std::stoi(settings["DisableMuzzleFlashLights"]);
        processed_keys.insert("DisableMuzzleFlashLights");
    }
    if (settings.count("ShowGlares")) {
        g_alpine_game_config.show_glares = std::stoi(settings["ShowGlares"]);
        processed_keys.insert("ShowGlares");
    }
    if (settings.count("MeshStaticLighting")) {
        g_alpine_game_config.mesh_static_lighting = std::stoi(settings["MeshStaticLighting"]);
        recalc_mesh_static_lighting();
        processed_keys.insert("MeshStaticLighting");
    }
    if (settings.count("Picmip")) {
        g_alpine_game_config.set_picmip(std::stoi(settings["Picmip"]));
        gr_update_texture_filtering();
        processed_keys.insert("Picmip");
    }
    if (settings.count("NearestTextureFiltering")) {
        g_alpine_game_config.nearest_texture_filtering = std::stoi(settings["NearestTextureFiltering"]);
        gr_update_texture_filtering();
        processed_keys.insert("NearestTextureFiltering");
    }
    if (settings.count("FastAnimations")) {
        rf::g_fast_animations = std::stoi(settings["FastAnimations"]);
        processed_keys.insert("FastAnimations");
    }
    if (settings.count("MonitorResolutionScale")) {
        g_alpine_game_config.set_monitor_resolution_scale(std::stoi(settings["MonitorResolutionScale"]));
        processed_keys.insert("MonitorResolutionScale");
    }
    if (settings.count("MaxFPS")) {
        g_alpine_game_config.set_max_fps(std::stoi(settings["MaxFPS"]));
        apply_maximum_fps();
        processed_keys.insert("MaxFPS");
    }
    if (settings.count("LODDistanceScale")) {
        g_alpine_game_config.set_lod_dist_scale(std::stof(settings["LODDistanceScale"]));
        processed_keys.insert("LODDistanceScale");
    }
    if (settings.count("SimulationDistance")) {
        g_alpine_game_config.set_entity_sim_distance(std::stof(settings["SimulationDistance"]));
        apply_entity_sim_distance();
        processed_keys.insert("SimulationDistance");
    }
    if (settings.count("FullRangeLighting")) {
        g_alpine_game_config.full_range_lighting = std::stoi(settings["FullRangeLighting"]);
        processed_keys.insert("FullRangeLighting");
    }
    if (settings.count("AlwaysClampOfficialLightmaps")) {
        g_alpine_game_config.always_clamp_official_lightmaps = std::stoi(settings["AlwaysClampOfficialLightmaps"]);
        processed_keys.insert("AlwaysClampOfficialLightmaps");
    }

    // Load UI settings
    if (settings.count("BigHUD")) {
        g_alpine_game_config.big_hud = std::stoi(settings["BigHUD"]);
        set_big_hud(g_alpine_game_config.big_hud);
        processed_keys.insert("BigHUD");
    }
    if (settings.count("ReticleScale")) {
        g_alpine_game_config.set_reticle_scale(std::stof(settings["ReticleScale"]));
        processed_keys.insert("ReticleScale");
    }
    if (settings.count("SniperScopeColor")) {
        auto color_override = parse_hex_color_string(settings["SniperScopeColor"]);
        if (color_override) {
            g_alpine_game_config.sniper_scope_color_override = color_override;
        }
        else {
            xlog::warn("Invalid sniper scope color override: {}", settings["SniperScopeColor"]);
        }
        processed_keys.insert("SniperScopeColor");
    }
    if (settings.count("PrecisionScopeColor")) {
        auto color_override = parse_hex_color_string(settings["PrecisionScopeColor"]);
        if (color_override) {
            g_alpine_game_config.precision_scope_color_override = color_override;
        }
        else {
            xlog::warn("Invalid precision scope color override: {}", settings["PrecisionScopeColor"]);
        }
        processed_keys.insert("PrecisionScopeColor");
    }
    if (settings.count("RailScopeColor")) {
        auto color_override = parse_hex_color_string(settings["RailScopeColor"]);
        if (color_override) {
            g_alpine_game_config.rail_scope_color_override = color_override;
        }
        else {
            xlog::warn("Invalid rail scope color override: {}", settings["RailScopeColor"]);
        }
        processed_keys.insert("RailScopeColor");
    }
    if (settings.count("ArAmmoColor")) {
        auto color_override = parse_hex_color_string(settings["ArAmmoColor"]);
        if (color_override) {
            g_alpine_game_config.ar_ammo_digit_color_override = color_override;
        }
        else {
            xlog::warn("Invalid AR ammo digit color override: {}", settings["ArAmmoColor"]);
        }
        processed_keys.insert("ArAmmoColor");
    }
    if (settings.count("DamageNotifyColor")) {
        auto color_override = parse_hex_color_string(settings["DamageNotifyColor"]);
        if (color_override) {
            g_alpine_game_config.damage_notify_color_override = color_override;
        }
        else {
            xlog::warn("Invalid damage notification color override: {}", settings["DamageNotifyColor"]);
        }
        processed_keys.insert("DamageNotifyColor");
    }
    if (settings.count("LocationPingColor")) {
        auto color_override = parse_hex_color_string(settings["LocationPingColor"]);
        if (color_override) {
            g_alpine_game_config.location_ping_color_override = color_override;
        }
        else {
            xlog::warn("Invalid location ping color override: {}", settings["LocationPingColor"]);
        }
        processed_keys.insert("LocationPingColor");
    }
    if (settings.count("MultiTimerColor")) {
        auto color_override = parse_hex_color_string(settings["MultiTimerColor"]);
        if (color_override) {
            g_alpine_game_config.multi_timer_color_override = color_override;
        }
        else {
            xlog::warn("Invalid multiplayer timer color override: {}", settings["MultiTimerColor"]);
        }
        processed_keys.insert("MultiTimerColor");
    }

    // Load singleplayer settings
    if (settings.count("DifficultyLevel")) {
        rf::game_set_skill_level(static_cast<rf::GameDifficultyLevel>(std::stoi(settings["DifficultyLevel"])));
        processed_keys.insert("DifficultyLevel");
    }
    if (settings.count("UnlimitedSemiAuto")) {
        g_alpine_game_config.unlimited_semi_auto = std::stoi(settings["UnlimitedSemiAuto"]);
        processed_keys.insert("UnlimitedSemiAuto");
    }
    if (settings.count("GaussianSpread")) {
        g_alpine_game_config.gaussian_spread = std::stoi(settings["GaussianSpread"]);
        processed_keys.insert("GaussianSpread");
    }
    if (settings.count("DisableAllCameraShake")) {
        g_alpine_game_config.screen_shake_force_off = std::stoi(settings["DisableAllCameraShake"]);
        processed_keys.insert("DisableAllCameraShake");
    }
    if (settings.count("Autosave")) {
        g_alpine_game_config.autosave = std::stoi(settings["Autosave"]);
        processed_keys.insert("Autosave");
    }
    if (settings.count("StaticBombCode")) {
        g_alpine_game_config.static_bomb_code = std::stoi(settings["StaticBombCode"]);
        processed_keys.insert("StaticBombCode");
    }
    if (settings.count("ExposureDamage")) {
        g_alpine_game_config.apply_exposure_damage = std::stoi(settings["ExposureDamage"]);
        processed_keys.insert("ExposureDamage");
    }

    // Load multiplayer settings
    if (settings.count("MultiplayerCharacter")) {
        player->settings.multi_character = std::stoi(settings["MultiplayerCharacter"]);
        processed_keys.insert("MultiplayerCharacter");
    }
    if (settings.count("WorldHUDObjIcons")) {
        g_alpine_game_config.world_hud_ctf_icons = std::stoi(settings["WorldHUDObjIcons"]);
        processed_keys.insert("WorldHUDObjIcons");
    }
    if (settings.count("WorldHUDOverdraw")) {
        g_alpine_game_config.world_hud_overdraw = std::stoi(settings["WorldHUDOverdraw"]);
        processed_keys.insert("WorldHUDOverdraw");
    }
    if (settings.count("WorldHUDBigText")) {
        g_alpine_game_config.world_hud_big_text = std::stoi(settings["WorldHUDBigText"]);
        processed_keys.insert("WorldHUDBigText");
    }
    if (settings.count("WorldHUDDamageNumbers")) {
        g_alpine_game_config.world_hud_damage_numbers = std::stoi(settings["WorldHUDDamageNumbers"]);
        processed_keys.insert("WorldHUDDamageNumbers");
    }
    if (settings.count("WorldHUDSpectateLabels")) {
        g_alpine_game_config.world_hud_spectate_player_labels = std::stoi(settings["WorldHUDSpectateLabels"]);
        processed_keys.insert("WorldHUDSpectateLabels");
    }
    if (settings.count("WorldHUDTeamLabels")) {
        g_alpine_game_config.world_hud_team_player_labels = std::stoi(settings["WorldHUDTeamLabels"]);
        processed_keys.insert("WorldHUDTeamLabels");
    }
    if (settings.count("ShowLocationPings")) {
        g_alpine_game_config.show_location_pings = std::stoi(settings["ShowLocationPings"]);
        processed_keys.insert("ShowLocationPings");
    }
    if (settings.count("PlayHitsounds")) {
        g_alpine_game_config.play_hit_sounds = std::stoi(settings["PlayHitsounds"]);
        processed_keys.insert("PlayHitsounds");
    }
    if (settings.count("PlayTaunts")) {
        g_alpine_game_config.play_taunt_sounds = std::stoi(settings["PlayTaunts"]);
        processed_keys.insert("PlayTaunts");
    }
    if (settings.count("ShowRunTimer")) {
        g_alpine_game_config.show_run_timer = std::stoi(settings["ShowRunTimer"]);
        processed_keys.insert("ShowRunTimer");
    }
    if (settings.count("VisualRicochet")) {
        g_alpine_game_config.multi_ricochet = std::stoi(settings["VisualRicochet"]);
        processed_keys.insert("VisualRicochet");
    }
    if (settings.count("DeathBars")) {
        g_alpine_game_config.death_bars = std::stoi(settings["DeathBars"]);
        processed_keys.insert("DeathBars");
    }
    if (settings.count("ShowEnemyBullets")) {
        g_alpine_game_config.show_enemy_bullets = std::stoi(settings["ShowEnemyBullets"]);
        apply_show_enemy_bullets();
        processed_keys.insert("ShowEnemyBullets");
    }
    if (settings.count("ShowPing")) {
        g_alpine_game_config.ping_display = std::stoi(settings["ShowPing"]);
        processed_keys.insert("ShowPing");
    }
    if (settings.count("ShowPlayerNames")) {
        g_alpine_game_config.display_target_player_names = std::stoi(settings["ShowPlayerNames"]);
        processed_keys.insert("ShowPlayerNames");
    }
    if (settings.count("VerboseTimer")) {
        g_alpine_game_config.verbose_time_left_display = std::stoi(settings["VerboseTimer"]);
        build_time_left_string_format();
        processed_keys.insert("VerboseTimer");
    }
    if (settings.count("ScoreboardAnimations")) {
        g_alpine_game_config.scoreboard_anim = std::stoi(settings["ScoreboardAnimations"]);
        processed_keys.insert("ScoreboardAnimations");
    }
    if (settings.count("MultiplayerTracker")) {
        g_alpine_game_config.set_multiplayer_tracker(settings["MultiplayerTracker"]);
        processed_keys.insert("MultiplayerTracker");
    }
    if (settings.count("ServerMaxFPS")) {
        g_alpine_game_config.set_server_max_fps(std::stoi(settings["ServerMaxFPS"]));
        apply_maximum_fps();
        processed_keys.insert("ServerMaxFPS");
    }
    if (settings.count("ServerNetFPS")) {
        g_alpine_game_config.set_server_netfps(std::stoi(settings["ServerNetFPS"]));
        processed_keys.insert("ServerNetFPS");
    }
    if (settings.count("DisableMultiCharacterLOD")) {
        g_alpine_game_config.multi_no_character_lod = std::stoi(settings["DisableMultiCharacterLOD"]);
        processed_keys.insert("DisableMultiCharacterLOD");
    }
    if (settings.count("PlayerJoinBeep")) {
        g_alpine_game_config.player_join_beep = std::stoi(settings["PlayerJoinBeep"]);
        processed_keys.insert("PlayerJoinBeep");
    }
    if (settings.count("WorldHUDAltDamageIndicators")) {
        g_alpine_game_config.world_hud_alt_damage_indicators = std::stoi(settings["WorldHUDAltDamageIndicators"]);
        processed_keys.insert("WorldHUDAltDamageIndicators");
    }
    if (settings.count("DesiredHandicap")) {
        g_alpine_game_config.set_desired_handicap(std::stoi(settings["DesiredHandicap"]));
        processed_keys.insert("DesiredHandicap");
    }
    if (settings.count("CPOutlineHeightScale")) {
        g_alpine_game_config.set_control_point_outline_height_scale(std::stof(settings["CPOutlineHeightScale"]));
        processed_keys.insert("CPOutlineHeightScale");
    }
    if (settings.count("CPOutlineSegments")) {
        g_alpine_game_config.set_control_point_outline_segments(std::stoi(settings["CPOutlineSegments"]));
        processed_keys.insert("CPOutlineSegments");
    }
    if (settings.count("CPColumnSegments")) {
        g_alpine_game_config.set_control_point_column_segments(std::stoi(settings["CPColumnSegments"]));
        processed_keys.insert("CPColumnSegments");
    }
    if (settings.count("CPColumnHeightScale")) {
        g_alpine_game_config.set_control_point_column_height_scale(std::stof(settings["CPColumnHeightScale"]));
        processed_keys.insert("CPColumnHeightScale");
    }
    if (settings.count("AlwaysShowSpectators")) {
        g_alpine_game_config.always_show_spectators = std::stoi(settings["AlwaysShowSpectators"]);
        processed_keys.insert("AlwaysShowSpectators");
    }
    if (settings.count("SimpleServerChatMsgs")) {
        g_alpine_game_config.simple_server_chat_msgs = std::stoi(settings["SimpleServerChatMsgs"]);
        processed_keys.insert("SimpleServerChatMsgs");
    }
    if (settings.count("RemoteServerCfgDisplayMode")) {
        g_alpine_game_config.remote_server_cfg_display_mode =
            static_cast<RemoteServerCfgPopup::DisplayMode>(
                std::stoi(settings["RemoteServerCfgDisplayMode"]) % RemoteServerCfgPopup::_DISPLAY_MODE_COUNT
            );
        processed_keys.insert("RemoteServerCfgDisplayMode");
    }
    if (settings.count("BotSharedSecret")) {
        g_alpine_game_config.bot_shared_secret = std::stoul(settings["BotSharedSecret"]);
        processed_keys.insert("BotSharedSecret");
    }

    // Load input settings
    if (settings.count("MouseSensitivity")) {
        player->settings.controls.mouse_sensitivity = std::stof(settings["MouseSensitivity"]);
        processed_keys.insert("MouseSensitivity");
    }
    if (settings.count("MouseYInvert")) {
        player->settings.controls.axes[1].invert = std::stoi(settings["MouseYInvert"]);
        processed_keys.insert("MouseYInvert");
    }
    if (settings.count("DirectInput")) {
        g_alpine_game_config.direct_input = std::stoi(settings["DirectInput"]);
        processed_keys.insert("DirectInput");
    }
    if (settings.count("MouseLinearPitch")) {
        g_alpine_game_config.mouse_linear_pitch = std::stoi(settings["MouseLinearPitch"]);
        processed_keys.insert("MouseLinearPitch");
    }
    if (settings.count("SwapARBinds")) {
        g_alpine_game_config.swap_ar_controls = std::stoi(settings["SwapARBinds"]);
        processed_keys.insert("SwapARBinds");
    }
    if (settings.count("SwapGNBinds")) {
        g_alpine_game_config.swap_gn_controls = std::stoi(settings["SwapGNBinds"]);
        processed_keys.insert("SwapGNBinds");
    }
    if (settings.count("SwapSGBinds")) {
        g_alpine_game_config.swap_sg_controls = std::stoi(settings["SwapSGBinds"]);
        processed_keys.insert("SwapSGBinds");
    }
    if (settings.count("SkipCutsceneBindAlias")) {
        g_alpine_game_config.skip_cutscene_bind_alias = std::stoi(settings["SkipCutsceneBindAlias"]);
        processed_keys.insert("SkipCutsceneBindAlias");
    }
    if (settings.count("SuppressAutoswitchBindAlias")) {
        g_alpine_game_config.suppress_autoswitch_alias = std::stoi(settings["SuppressAutoswitchBindAlias"]);
        processed_keys.insert("SuppressAutoswitchBindAlias");
    }
    if (settings.count("StaticScopeSensitivity")) {
        g_alpine_game_config.scope_static_sensitivity = std::stoi(settings["StaticScopeSensitivity"]);
        processed_keys.insert("StaticScopeSensitivity");
    }
    if (settings.count("ScopeSensitivityModifier")) {
        g_alpine_game_config.set_scope_sens_mod(std::stof(settings["ScopeSensitivityModifier"]));
        update_scope_sensitivity();
        processed_keys.insert("ScopeSensitivityModifier");
    }
    if (settings.count("ScannerSensitivityModifier")) {
        g_alpine_game_config.set_scanner_sens_mod(std::stof(settings["ScannerSensitivityModifier"]));
        update_scanner_sensitivity();
        processed_keys.insert("ScannerSensitivityModifier");
    }

    // Load binds
    for (const auto& [key, value] : settings) {
        if (key.rfind("Bind:", 0) == 0) {
            std::string action_name = key.substr(5);
            std::istringstream bind_values(value);
            std::string id_str, scan1, scan2, mouse_btn;

            if (std::getline(bind_values, id_str, ',') &&
                std::getline(bind_values, scan1, ',') &&
                std::getline(bind_values, scan2, ',') &&
                std::getline(bind_values, mouse_btn, ',')) {

                int bind_id = std::stoi(id_str);
                if (bind_id >= 0 && bind_id < player->settings.controls.num_bindings) {
                    // Note action_name is not loaded because the game uses localized strings for this
                    // Binds are tracked by bind ID instead
                    //player->settings.controls.bindings[bind_id].name = action_name.c_str();
                    player->settings.controls.bindings[bind_id].scan_codes[0] = scan1.empty() ? -1 : std::stoi(scan1);
                    player->settings.controls.bindings[bind_id].scan_codes[1] = scan2.empty() ? -1 : std::stoi(scan2);
                    player->settings.controls.bindings[bind_id].mouse_btn_id = mouse_btn.empty() ? -1 : std::stoi(mouse_btn);

                    xlog::info("Loaded Bind: {} = {}, {}, {}, {}", action_name, bind_id, scan1, scan2, mouse_btn);
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
    }

    // Iterate through newly loaded bindings and resolve conflicts
    // Earlier bind takes priority when conflicts occur
    resolve_scan_code_conflicts(player->settings.controls);
    resolve_mouse_button_conflicts(player->settings.controls);

    // Store orphaned settings
    for (const auto& [key, value] : settings) {
        if (processed_keys.find(key) == processed_keys.end() && !string_starts_with(key, "AFS")) {
            xlog::warn("Saving unrecognized setting as orphaned: {}={}", key, value);
            orphaned_lines.push_back(key + "=" + value);
        }
    }

    // apply loaded graphics options
    rf::player_settings_apply_graphics_options(player);

    rf::console::printf("Successfully loaded settings from %s", filename);
    g_loaded_alpine_settings_file = true;
    return true;
}

void alpine_control_config_serialize(std::ofstream& file, const rf::ControlConfig& cc)
{
    file << "\n[InputSettings]\n";
    file << "MouseSensitivity=" << cc.mouse_sensitivity << "\n";
    file << "MouseYInvert=" << cc.axes[1].invert << "\n";
    file << "DirectInput=" << g_alpine_game_config.direct_input << "\n";
    file << "MouseLinearPitch=" << g_alpine_game_config.mouse_linear_pitch << "\n";
    file << "SwapARBinds=" << g_alpine_game_config.swap_ar_controls << "\n";
    file << "SwapGNBinds=" << g_alpine_game_config.swap_gn_controls << "\n";
    file << "SwapSGBinds=" << g_alpine_game_config.swap_sg_controls << "\n";
    file << "SkipCutsceneBindAlias=" << g_alpine_game_config.skip_cutscene_bind_alias << "\n";
    file << "SuppressAutoswitchBindAlias=" << g_alpine_game_config.suppress_autoswitch_alias << "\n";
    file << "StaticScopeSensitivity=" << g_alpine_game_config.scope_static_sensitivity << "\n";
    file << "ScopeSensitivityModifier=" << g_alpine_game_config.scope_sensitivity_modifier << "\n";
    file << "ScannerSensitivityModifier=" << g_alpine_game_config.scanner_sensitivity_modifier << "\n";
    file << "QuickExit=" << g_alpine_game_config.quick_exit << "\n";

    file << "\n[ActionBinds]\n";
    file << "; Format is Bind:{Name}={ID},{ScanCode0},{ScanCode1},{MouseButtonID}\n";

    // Key bind format: Bind:ActionName=ID,PrimaryScanCode,SecondaryScanCode,MouseButtonID
    // Note ActionName is not used when loading, ID is. ActionName is included for readability.
    for (int i = 0; i < cc.num_bindings; ++i) {
        xlog::info("Saving Bind: {} = {}, {}, {}, {}", 
                   cc.bindings[i].name, 
                   i, 
                   cc.bindings[i].scan_codes[0], 
                   cc.bindings[i].scan_codes[1], 
                   cc.bindings[i].mouse_btn_id);

        file << "Bind:" << cc.bindings[i].name << "=" 
             << i << "," 
             << cc.bindings[i].scan_codes[0] << "," 
             << cc.bindings[i].scan_codes[1] << ","
             << cc.bindings[i].mouse_btn_id << "\n";
    }
}

void alpine_player_settings_save(rf::Player* player)
{
    std::string filename = alpine_get_settings_filename();
    std::ofstream file(filename);

    if (!file.is_open()) {
        xlog::info("Failed to write {}", filename);
        return;
    }

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

    // Player
    file << "\n[PlayerSettings]\n";
    file << "PlayerName=" << player->name << "\n";
    file << "GoreLevel=" << rf::game_get_gore_level() << "\n";
    file << "ShowFPGun=" << player->settings.render_fpgun << "\n";
    file << "AutoswitchWeapons=" << player->settings.autoswitch_weapons << "\n";
    file << "NeverAutoswitchExplosives=" << player->settings.dont_autoswitch_to_explosives << "\n";
    file << "ToggleCrouch=" << player->settings.toggle_crouch << "\n";
    file << "DamageScreenFlash=" << g_alpine_game_config.damage_screen_flash << "\n";
    file << "SpectateMinimalUI=" << g_alpine_game_config.spectate_mode_minimal_ui << "\n";
    file << "ShowFPS=" << g_alpine_game_config.fps_counter << "\n";
    file << "SaveConsoleHistory=" << g_alpine_game_config.save_console_history << "\n";
    file << "AlpineBranding=" << g_alpine_game_config.af_branding << "\n";
    file << "SeasonalEffect=" << g_alpine_game_config.seasonal_effect << "\n";
    file << "RealArmorValues=" << g_alpine_game_config.real_armor_values << "\n";
    file << "ColorblindMode=" << g_alpine_game_config.colorblind_mode << "\n";
    file << "AutoswitchFireWait=" << g_alpine_game_config.suppress_autoswitch_fire_wait << "\n";
    file << "AlwaysAutoswitchEmpty=" << g_alpine_game_config.always_autoswitch_empty << "\n";

    // Autoswitch priority
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

    // Audio
    file << "\n[AudioSettings]\n";
    file << "EffectsVolume=" << rf::snd_get_group_volume(0) << "\n";
    file << "MusicVolume=" << rf::snd_get_group_volume(1) << "\n";
    file << "MessagesVolume=" << rf::snd_get_group_volume(2) << "\n";
    file << "LevelSoundVolume=" << g_alpine_game_config.level_sound_volume << "\n";
    file << "EntityPainSounds=" << g_alpine_game_config.entity_pain_sounds << "\n";

    // Video
    file << "\n[VideoSettings]\n";
    file << "Gamma=" << rf::gr::gamma << "\n";
    file << "ShowShadows=" << player->settings.shadows_enabled << "\n";
    file << "ShowDecals=" << player->settings.decals_enabled << "\n";
    file << "ShowDynamicLights=" << player->settings.dynamic_lightining_enabled << "\n";
    file << "BilinearFiltering=" << player->settings.bilinear_filtering << "\n";
    file << "DetailLevel=" << player->settings.detail_level << "\n";
    file << "CharacterDetailLevel=" << player->settings.character_detail_level << "\n";
    file << "TextureDetailLevel=" << player->settings.textures_resolution_level << "\n";
    file << "HorizontalFOV=" << g_alpine_game_config.horz_fov << "\n";
    file << "FPGunFOVScale=" << g_alpine_game_config.fpgun_fov_scale << "\n";
    file << "DisableWeaponShake=" << g_alpine_game_config.try_disable_weapon_shake << "\n";
    file << "FullbrightCharacters=" << g_alpine_game_config.try_fullbright_characters << "\n";
    file << "DisableTextures=" << g_alpine_game_config.try_disable_textures << "\n";
    file << "DisableMuzzleFlashLights=" << g_alpine_game_config.try_disable_muzzle_flash_lights << "\n";
    file << "ShowGlares=" << g_alpine_game_config.show_glares << "\n";
    file << "MeshStaticLighting=" << g_alpine_game_config.mesh_static_lighting << "\n";
    file << "Picmip=" << g_alpine_game_config.picmip << "\n";
    file << "NearestTextureFiltering=" << g_alpine_game_config.nearest_texture_filtering << "\n";
    file << "FastAnimations=" << rf::g_fast_animations << "\n";
    file << "MonitorResolutionScale=" << g_alpine_game_config.monitor_resolution_scale << "\n";
    file << "MaxFPS=" << g_alpine_game_config.max_fps << "\n";
    file << "LODDistanceScale=" << g_alpine_game_config.lod_dist_scale << "\n";
    file << "SimulationDistance=" << g_alpine_game_config.entity_sim_distance << "\n";
    file << "FullRangeLighting=" << g_alpine_game_config.full_range_lighting << "\n";
    file << "AlwaysClampOfficialLightmaps=" << g_alpine_game_config.always_clamp_official_lightmaps << "\n";

    // UI
    file << "\n[UISettings]\n";
    file << "BigHUD=" << g_alpine_game_config.big_hud << "\n";
    file << "ReticleScale=" << g_alpine_game_config.reticle_scale << "\n";
    if (g_alpine_game_config.sniper_scope_color_override) {
        file << "SniperScopeColor=" << format_hex_color_string(*g_alpine_game_config.sniper_scope_color_override) << "\n";
    }
    if (g_alpine_game_config.precision_scope_color_override) {
        file << "PrecisionScopeColor=" << format_hex_color_string(*g_alpine_game_config.precision_scope_color_override) << "\n";
    }
    if (g_alpine_game_config.rail_scope_color_override) {
        file << "RailScopeColor=" << format_hex_color_string(*g_alpine_game_config.rail_scope_color_override) << "\n";
    }
    if (g_alpine_game_config.ar_ammo_digit_color_override) {
        file << "ArAmmoColor=" << format_hex_color_string(*g_alpine_game_config.ar_ammo_digit_color_override) << "\n";
    }
    if (g_alpine_game_config.damage_notify_color_override) {
        file << "DamageNotifyColor=" << format_hex_color_string(*g_alpine_game_config.damage_notify_color_override) << "\n";
    }
    if (g_alpine_game_config.location_ping_color_override) {
        file << "LocationPingColor=" << format_hex_color_string(*g_alpine_game_config.location_ping_color_override) << "\n";
    }
    if (g_alpine_game_config.multi_timer_color_override) {
        file << "MultiTimerColor=" << format_hex_color_string(*g_alpine_game_config.multi_timer_color_override) << "\n";
    }

    // Singleplayer
    file << "\n[SingleplayerSettings]\n";
    file << "DifficultyLevel=" << static_cast<int>(rf::game_get_skill_level()) << "\n";
    file << "UnlimitedSemiAuto=" << g_alpine_game_config.unlimited_semi_auto << "\n";
    file << "GaussianSpread=" << g_alpine_game_config.gaussian_spread << "\n";
    file << "DisableAllCameraShake=" << g_alpine_game_config.screen_shake_force_off << "\n";
    file << "Autosave=" << g_alpine_game_config.autosave << "\n";
    file << "StaticBombCode=" << g_alpine_game_config.static_bomb_code << "\n";
    file << "ExposureDamage=" << g_alpine_game_config.apply_exposure_damage << "\n";

    // Multiplayer
    file << "\n[MultiplayerSettings]\n";
    file << "MultiplayerCharacter=" << player->settings.multi_character << "\n";
    file << "WorldHUDObjIcons=" << g_alpine_game_config.world_hud_ctf_icons << "\n";
    file << "WorldHUDOverdraw=" << g_alpine_game_config.world_hud_overdraw << "\n";
    file << "WorldHUDBigText=" << g_alpine_game_config.world_hud_big_text << "\n";
    file << "WorldHUDDamageNumbers=" << g_alpine_game_config.world_hud_damage_numbers << "\n";
    file << "WorldHUDSpectateLabels=" << g_alpine_game_config.world_hud_spectate_player_labels << "\n";
    file << "WorldHUDTeamLabels=" << g_alpine_game_config.world_hud_team_player_labels << "\n";
    file << "ShowLocationPings=" << g_alpine_game_config.show_location_pings << "\n";
    file << "PlayHitsounds=" << g_alpine_game_config.play_hit_sounds << "\n";
    file << "PlayTaunts=" << g_alpine_game_config.play_taunt_sounds << "\n";
    file << "ShowRunTimer=" << g_alpine_game_config.show_run_timer << "\n";
    file << "VisualRicochet=" << g_alpine_game_config.multi_ricochet << "\n";
    file << "DeathBars=" << g_alpine_game_config.death_bars << "\n";
    file << "ShowEnemyBullets=" << g_alpine_game_config.show_enemy_bullets << "\n";
    file << "ShowPing=" << g_alpine_game_config.ping_display << "\n";
    file << "ShowPlayerNames=" << g_alpine_game_config.display_target_player_names << "\n";
    file << "VerboseTimer=" << g_alpine_game_config.verbose_time_left_display << "\n";
    file << "ScoreboardAnimations=" << g_alpine_game_config.scoreboard_anim << "\n";
    file << "MultiplayerTracker=" << g_alpine_game_config.multiplayer_tracker << "\n";
    file << "ServerMaxFPS=" << g_alpine_game_config.server_max_fps << "\n";
    file << "ServerNetFPS=" << g_alpine_game_config.server_netfps << "\n";
    file << "DisableMultiCharacterLOD=" << g_alpine_game_config.multi_no_character_lod << "\n";
    file << "PlayerJoinBeep=" << g_alpine_game_config.player_join_beep << "\n";
    file << "WorldHUDAltDamageIndicators=" << g_alpine_game_config.world_hud_alt_damage_indicators << "\n";
    file << "DesiredHandicap=" << g_alpine_game_config.desired_handicap << "\n";
    file << "CPOutlineHeightScale=" << g_alpine_game_config.control_point_outline_height_scale << "\n";
    file << "CPOutlineSegments=" << g_alpine_game_config.control_point_outline_segments << "\n";
    file << "CPColumnSegments=" << g_alpine_game_config.control_point_column_segments << "\n";
    file << "CPColumnHeightScale=" << g_alpine_game_config.control_point_column_height_scale << "\n";
    file << "AlwaysShowSpectators=" << g_alpine_game_config.always_show_spectators << "\n";
    file << "RemoteServerCfgDisplayMode=" << static_cast<int>(g_alpine_game_config.remote_server_cfg_display_mode) << "\n";
    file << "SimpleServerChatMsgs=" << g_alpine_game_config.simple_server_chat_msgs << "\n";
    file << "BotSharedSecret=" << g_alpine_game_config.bot_shared_secret << "\n";

    alpine_control_config_serialize(file, player->settings.controls);

    file.close();
    xlog::info("Saved settings to {}", filename);
}

void close_and_restart_game() {
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

CallHook<void(rf::Player*)> player_settings_load_hook{
    0x004B2726,
    [](rf::Player* player) {
        bool ff_link_prompt = true;
        if (!alpine_player_settings_load(player)) {
            xlog::warn("Alpine Faction settings file not found. Attempting to import legacy RF settings file.");
            player_settings_load_hook.call_target(player); // load players.cfg

            set_alpine_config_defaults();

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

        // display popup recommending ff link
        if (ff_link_prompt && !g_game_config.suppress_ff_link_prompt) {
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
        g_alpine_system_config.save();
        alpine_player_settings_save(player);
    }
};

CallHook<void(rf::Player*)> player_settings_save_quit_hook{
    0x004B2D77,
    [](rf::Player* player) {
        player_settings_save_quit_hook.call_target(player);

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

ConsoleCommand2 load_settings_cmd{
    "dbg_loadsettings",
    []() {
        alpine_player_settings_load(rf::local_player);
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

    // Init cmd line
    get_afs_cmd_line_param();
}
