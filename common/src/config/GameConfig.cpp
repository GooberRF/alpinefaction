#include <shlwapi.h>
#include <algorithm>
#include <xlog/xlog.h>
#include <common/config/GameConfig.h>
#include <common/config/RegKey.h>
#include <common/version/version.h>

const char rf_key_name[] = R"(SOFTWARE\Volition\Red Faction)";
const char af_subkey_name[] = "Alpine Faction";

const char fallback_executable_path[] = R"(C:\games\RedFaction\rf.exe)";

bool GameConfig::load() try
{
    bool result = visit_vars([](RegKey& reg_key, const char* name, auto& var) {
        auto value_temp = var.value();
        bool read_success = reg_key.read_value(name, &value_temp);
        var = value_temp;
        return read_success;
    }, false);

    if (game_executable_path.value().empty() && !detect_game_path()) {
        game_executable_path = fallback_executable_path;
    }

    if (update_rate == 0) {
        // Update Rate is set to "None" - this would prevent Multi menu from working - fix it
        update_rate = GameConfig::default_update_rate;
        result = false;
    }

    return result;
}
catch (...) {
    std::throw_with_nested(std::runtime_error("failed to load config"));
}

void GameConfig::save() try
{
    visit_vars([](RegKey& reg_key, const char* name, auto& var) {
        if (var.is_dirty()) {
            reg_key.write_value(name, var.value());
            var.set_dirty(false);
        }
        return true;
    }, true);
}
catch (...) {
    std::throw_with_nested(std::runtime_error("failed to save config"));
}

bool GameConfig::detect_game_path()
{
    std::string install_path;

    // Dash Faction
    try
    {
        RegKey reg_key(HKEY_CURRENT_USER, "SOFTWARE\\Volition\\Red Faction\\Dash Faction", KEY_READ);
        if (reg_key.read_value("Executable Path", &install_path))
        {
            game_executable_path = install_path;
            return true;
        }
    }
    catch (...)
    {
        // ignore
    }

    // Standard RF installer
    try
    {
        RegKey reg_key(HKEY_LOCAL_MACHINE, rf_key_name, KEY_READ);
        if (reg_key.read_value("InstallPath", &install_path))
        {
            game_executable_path = install_path + "\\RF.exe";
            return true;
        }
    }
    catch (...)
    {
        // ignore
    }

    // Steam
    try
    {
        BOOL Wow64Process = FALSE;
        IsWow64Process(GetCurrentProcess(), &Wow64Process);
        REGSAM reg_sam = KEY_READ;
        if (Wow64Process) {
            reg_sam |= KEY_WOW64_64KEY;
        }
        RegKey reg_key(HKEY_LOCAL_MACHINE, R"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 20530)", reg_sam);
        if (reg_key.read_value("InstallLocation", &install_path))
        {
            game_executable_path = install_path + "\\RF.exe";
            return true;
        }
    }
    catch (...)
    {
        // ignore
    }

    // GOG
    try
    {
        RegKey reg_key(HKEY_LOCAL_MACHINE, "SOFTWARE\\Nordic Games\\Red Faction", KEY_READ);
        if (reg_key.read_value("INSTALL_DIR", &install_path))
        {
            game_executable_path = install_path + "\\RF.exe";
            return true;
        }
    }
    catch (...)
    {
        // ignore
    }

    char current_dir[MAX_PATH];
    GetCurrentDirectoryA(sizeof(current_dir), current_dir);
    std::string full_path = std::string(current_dir) + "\\RF.exe";
    if (PathFileExistsA(full_path.c_str()))
    {
        game_executable_path = full_path;
        return true;
    }

    return false;
}

template<typename T>
bool GameConfig::visit_vars(T&& visitor, bool is_save)
{
    bool result = true;

    RegKey red_faction_key(HKEY_CURRENT_USER, rf_key_name, is_save ? KEY_WRITE : KEY_READ, is_save);
    result &= red_faction_key.is_open();
    result &= visitor(red_faction_key, "Resolution Width", res_width);
    result &= visitor(red_faction_key, "Resolution Height", res_height);
    result &= visitor(red_faction_key, "Resolution Bit Depth", res_bpp);
    result &= visitor(red_faction_key, "Resolution Backbuffer Format", res_backbuffer_format);
    result &= visitor(red_faction_key, "Selected Video Card", selected_video_card);
    result &= visitor(red_faction_key, "Vsync", vsync);
    result &= visitor(red_faction_key, "Geometry Cache Size", geometry_cache_size);
    result &= visitor(red_faction_key, "UpdateRate", update_rate);
    result &= visitor(red_faction_key, "EAX", eax_sound);
    result &= visitor(red_faction_key, "ForcePort", force_port);

    RegKey alpine_faction_key(red_faction_key, af_subkey_name, is_save ? KEY_WRITE : KEY_READ, is_save);
    result &= alpine_faction_key.is_open();
    result &= visitor(alpine_faction_key, "Window Mode", wnd_mode);
    result &= visitor(alpine_faction_key, "Anisotropic Filtering", anisotropic_filtering);
    result &= visitor(alpine_faction_key, "MSAA", msaa_level);
    result &= visitor(alpine_faction_key, "High Scanner Resolution", high_scanner_res);
    result &= visitor(alpine_faction_key, "True Color Textures", true_color_textures);
    result &= visitor(alpine_faction_key, "Renderer", renderer);
    result &= visitor(alpine_faction_key, "Executable Path", game_executable_path);
    result &= visitor(alpine_faction_key, "Fast Start", fast_start);
    result &= visitor(alpine_faction_key, "Allow Overwriting Game Files", allow_overwrite_game_files);
    result &= visitor(alpine_faction_key, "Version", alpine_faction_version);
    result &= visitor(alpine_faction_key, "Keep Launcher Open", keep_launcher_open);
    result &= visitor(alpine_faction_key, "Language", language);
    result &= visitor(alpine_faction_key, "Reduced Speed In Background", reduced_speed_in_background);
    result &= visitor(alpine_faction_key, "FFLink Token", fflink_token);
    result &= visitor(alpine_faction_key, "FFLink Username", fflink_username);
    result &= visitor(alpine_faction_key, "Already Saw First Launch Window", suppress_first_launch_window);
    result &= visitor(alpine_faction_key, "Already Saw FF Link Prompt", suppress_ff_link_prompt);

    return result;
}

template<>
bool is_valid_enum_value<GameConfig::WndMode>(int value)
{
    return value == GameConfig::FULLSCREEN
        || value == GameConfig::WINDOWED
        || value == GameConfig::STRETCHED;
}

template<>
bool is_valid_enum_value<GameConfig::Renderer>(int value)
{
    return value == static_cast<int>(GameConfig::Renderer::d3d8)
        || value == static_cast<int>(GameConfig::Renderer::d3d9)
        || value == static_cast<int>(GameConfig::Renderer::d3d11);
}
