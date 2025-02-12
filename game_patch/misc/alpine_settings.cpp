#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <common/utils/os-utils.h>
#include "alpine_settings.h"
#include <common/version/version.h>
#include "../os/console.h"
#include "../rf/ui.h"
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

static bool g_loaded_alpine_settings_file = false;
static bool g_loaded_players_cfg_file = false;
static bool g_restart_on_close = false;
std::vector<std::string> orphaned_lines;
int loaded_afs_version = -1;

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
    if (rf::mod_param.found()) {
        std::string mod_name = rf::mod_param.get_arg();
        return "alpine_settings_" + mod_name + ".ini";
    }
    return "alpine_settings.ini";
}

bool alpine_player_settings_load(rf::Player* player)
{
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
    if (settings.count("MultiplayerCharacter")) {
        player->settings.multi_character = std::stoi(settings["MultiplayerCharacter"]);
        processed_keys.insert("MultiplayerCharacter");
    }
    if (settings.count("GoreLevel")) {
        rf::game_set_gore_level(std::stoi(settings["GoreLevel"]));
        processed_keys.insert("GoreLevel");
    }
    if (settings.count("DifficultyLevel")) {
        rf::game_set_skill_level(static_cast<rf::GameDifficultyLevel>(std::stoi(settings["DifficultyLevel"])));
        processed_keys.insert("DifficultyLevel");
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

    // Load input settings
    if (settings.count("MouseSensitivity")) {
        player->settings.controls.mouse_sensitivity = std::stof(settings["MouseSensitivity"]);
        processed_keys.insert("MouseSensitivity");
    }
    if (settings.count("MouseYInvert")) {
        player->settings.controls.axes[1].invert = std::stoi(settings["MouseYInvert"]);
        processed_keys.insert("MouseYInvert");
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



    rf::console::printf("Successfully loaded settings from %s", filename);
    g_loaded_alpine_settings_file = true;
    return true;
}

void alpine_control_config_serialize(std::ofstream& file, const rf::ControlConfig& cc)
{
    file << "\n[InputSettings]\n";
    file << "MouseSensitivity=" << cc.mouse_sensitivity << "\n";
    file << "MouseYInvert=" << cc.axes[1].invert << "\n";

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
    file << "; Unless you really know what you are doing, editing this file manually is NOT recommended.\n";
    file << "; Making edits to this file while the game is running is NOT recommended.\n\n";

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
    file << "MultiplayerCharacter=" << player->settings.multi_character << "\n";
    file << "GoreLevel=" << rf::game_get_gore_level() << "\n";
    file << "DifficultyLevel=" << static_cast<int>(rf::game_get_skill_level()) << "\n";
    file << "ShowFPGun=" << player->settings.render_fpgun << "\n";
    file << "AutoswitchWeapons=" << player->settings.autoswitch_weapons << "\n";
    file << "NeverAutoswitchExplosives=" << player->settings.dont_autoswitch_to_explosives << "\n";
    file << "ToggleCrouch=" << player->settings.toggle_crouch << "\n";

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
    
    alpine_control_config_serialize(file, player->settings.controls);

    file.close();
    xlog::info("Saved settings to {}", filename);
}

void close_and_restart_game() {
    g_restart_on_close = true;
    rf::ui::mainmenu_quit_game_confirmed();
}

CallHook<void(rf::Player*)> player_settings_load_hook{
    0x004B2726,
    [](rf::Player* player) {
        if (!alpine_player_settings_load(player)) {
            xlog::warn("Alpine Faction settings file not found. Attempting to import legacy RF settings file.");
            player_settings_load_hook.call_target(player); // load players.cfg

            // Display restart popup due to players.cfg import
            // players.cfg from legacy client version will import fine on first load, apart from Alpine controls
            // Restart cleanly loads game without baggage from players.cfg, and adds Alpine controls without issue
            if (g_loaded_players_cfg_file) {
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
    }
};

FunHook<void(rf::Player*)> player_settings_save_hook{
    0x004A8F50,
    [](rf::Player* player) {
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

ConsoleCommand2 load_settings_cmd{
    "dbg_loadsettings",
    []() {
        alpine_player_settings_load(rf::local_player);
        rf::console::print("Loading settings file...");
    },
    "Force the game to read and apply settings from config file",
};

ConsoleCommand2 save_settings_cmd{
    "dbg_savesettings",
    []() {
        alpine_player_settings_save(rf::local_player);
        rf::console::print("Saving settings file...");
    },
    "Force a write of current settings to config file",
};

void alpine_settings_apply_patch()
{
    // Handle loading and saving settings ini file
    player_settings_load_hook.install();
    player_settings_save_hook.install();
    player_settings_save_quit_hook.install();
    player_settings_load_players_cfg_patch.install();

    // Register commands
    load_settings_cmd.register_cmd();
    save_settings_cmd.register_cmd();
}
