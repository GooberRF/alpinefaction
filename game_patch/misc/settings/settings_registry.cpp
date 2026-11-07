// Registry assembly, persistence and lookup helpers for the setting table.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <xlog/xlog.h>
#include <common/utils/string-utils.h>
#include "settings_rows.h"
#include "../alpine_settings.h"
#include "../../hud/hud.h"
#include "../../multi/multi.h"
#include "../../rf/level.h"
#include "../../rf/os/console.h"
#include "../../rf/multi.h"
#include "../../rf/player/player.h"

namespace
{

// Defaults for AlpineGameSettings / AlpineCoreConfig backed rows.
// The struct defaults, corrected to what a fresh install starts with (set_alpine_config_defaults):
// save_console_history is read before the config loads, so the struct has to keep it false.
const AlpineGameSettings g_ags_defaults = [] {
    AlpineGameSettings defaults;
    defaults.save_console_history = true;
    return defaults;
}();
const AlpineCoreConfig g_asc_defaults{};

std::string format_float(float value)
{
    return std::format("{}", value);
}

// std::stof accepts "nan" and "inf", which every clamp passes straight through.
float parse_finite_float(const std::string& value)
{
    const float v = std::stof(value);
    if (!std::isfinite(v)) {
        throw std::invalid_argument{"not a finite number"};
    }
    return v;
}

// An enum row only stores a value one of its labels stands for; anything else snaps to the nearest one.
int nearest_enum_value(const SettingRow& row, int value)
{
    if (!row.enum_labels || row.enum_count <= 0) {
        return value;
    }
    int best = setting_enum_value(row, 0);
    for (int i = 1; i < row.enum_count; ++i) {
        const int candidate = setting_enum_value(row, i);
        if (std::llabs(static_cast<long long>(candidate) - value) < std::llabs(static_cast<long long>(best) - value)) {
            best = candidate;
        }
    }
    return best;
}

int clamp_int(int v, int lo, int hi)
{
    return hi < lo ? v : std::clamp(v, lo, hi);
}

unsigned clamp_uint(unsigned v, unsigned lo, unsigned hi)
{
    return hi < lo ? v : std::clamp(v, lo, hi);
}

float clamp_float(float v, float lo, float hi)
{
    return hi < lo ? v : std::clamp(v, lo, hi);
}

bool server_permission_granted(ServerPermission permission)
{
    if (permission == ServerPermission::None) {
        return true;
    }
    if (!rf::is_multi || rf::is_server) {
        return true;
    }
    const auto& info = get_af_server_info();
    if (!info) {
        return false;
    }
    switch (permission) {
        case ServerPermission::FullbrightMeshes: return info->allow_fb_mesh;
        case ServerPermission::LightmapsOnly: return info->allow_lmap;
        case ServerPermission::NoScreenShake: return info->allow_no_ss;
        case ServerPermission::NoMuzzleFlash: return info->allow_no_mf;
        case ServerPermission::Footsteps: return info->allow_footsteps;
        case ServerPermission::Outlines: return info->allow_outlines;
        case ServerPermission::OutlinesXray: return info->allow_outlines_xray;
        default: return true;
    }
}

} // namespace

std::span<const SettingRow> settings_registry_all()
{
    static const std::vector<SettingRow> rows = [] {
        std::vector<SettingRow> out;
        // The vehicle and spectator tables come after the categories they join, so their rows
        // close those categories.
        for (auto fn : {settings_rows_graphics, settings_rows_hud, settings_rows_interface, settings_rows_audio,
                        settings_rows_bindings, settings_rows_input, settings_rows_gameplay, settings_rows_vehicles,
                        settings_rows_multiplayer, settings_rows_spectator}) {
            const auto part = fn();
            out.insert(out.end(), part.begin(), part.end());
        }
        return out;
    }();
    return rows;
}

std::vector<const SettingRow*> settings_in_category(SettingCategory category)
{
    std::vector<const SettingRow*> out;
    for (const auto& row : settings_registry_all()) {
        if (row.category == category) {
            out.push_back(&row);
        }
    }
    return out;
}

const SettingRow* settings_find(std::string_view key_or_id)
{
    for (const auto& row : settings_registry_all()) {
        if (string_iequals(row.id, key_or_id) || (row.key && string_iequals(row.key, key_or_id))) {
            return &row;
        }
    }
    return nullptr;
}

const char* setting_category_name(SettingCategory category)
{
    switch (category) {
        case SettingCategory::Graphics: return "Graphics";
        case SettingCategory::Hud: return "HUD";
        case SettingCategory::Interface: return "Interface";
        case SettingCategory::Audio: return "Audio";
        case SettingCategory::Bindings: return "Bindings";
        case SettingCategory::Input: return "Input";
        case SettingCategory::Gameplay: return "Gameplay";
        case SettingCategory::Multiplayer: return "Multiplayer";
        default: return "?";
    }
}

SettingValueType setting_value_type(const SettingRow& row)
{
    return std::visit(
        [](const auto& acc) {
            using T = std::decay_t<decltype(acc)>;
            if constexpr (std::is_same_v<T, AccBool> || std::is_same_v<T, AccCoreBool>
                          || std::is_same_v<T, AccFnBool>) {
                return SettingValueType::Bool;
            }
            else if constexpr (std::is_same_v<T, AccInt> || std::is_same_v<T, AccFnInt>) {
                return SettingValueType::Int;
            }
            else if constexpr (std::is_same_v<T, AccUint>) {
                return SettingValueType::Unsigned;
            }
            else if constexpr (std::is_same_v<T, AccFloat> || std::is_same_v<T, AccFnFloat>) {
                return SettingValueType::Float;
            }
            else if constexpr (std::is_same_v<T, AccColor>) {
                return SettingValueType::Color;
            }
            else if constexpr (std::is_same_v<T, AccOptColor>) {
                return SettingValueType::OptionalColor;
            }
            else if constexpr (std::is_same_v<T, AccOptFloat>) {
                return SettingValueType::OptionalFloat;
            }
            else if constexpr (std::is_same_v<T, AccString> || std::is_same_v<T, AccFnString>
                               || std::is_same_v<T, AccRegString>) {
                return SettingValueType::String;
            }
            else {
                return SettingValueType::None;
            }
        },
        row.access);
}

bool setting_ui_range(const SettingRow& row, float& lo, float& hi, float& step)
{
    if (row.ui_max >= row.ui_min) {
        lo = row.ui_min;
        hi = row.ui_max;
        step = row.ui_step;
        return true;
    }

    bool ok = false;
    std::visit(
        [&](const auto& acc) {
            using T = std::decay_t<decltype(acc)>;
            if constexpr (std::is_same_v<T, AccInt> || std::is_same_v<T, AccFnInt> || std::is_same_v<T, AccUint>) {
                if (acc.hi >= acc.lo) {
                    lo = static_cast<float>(acc.lo);
                    hi = static_cast<float>(acc.hi);
                    step = row.ui_step > 0.0f ? row.ui_step : 1.0f;
                    ok = true;
                }
            }
            else if constexpr (std::is_same_v<T, AccFloat> || std::is_same_v<T, AccFnFloat>) {
                if (acc.hi >= acc.lo) {
                    lo = acc.lo;
                    hi = acc.hi;
                    step = row.ui_step;
                    ok = true;
                }
            }
        },
        row.access);
    return ok;
}

int setting_enum_value(const SettingRow& row, const int index)
{
    return row.enum_values ? row.enum_values[index] : index;
}

std::vector<int> setting_enum_offered(const SettingRow& row)
{
    std::vector<int> out;
    out.reserve(row.enum_count);
    for (int i = 0; i < row.enum_count; ++i) {
        if (!row.enum_offered || row.enum_offered(setting_enum_value(row, i))) {
            out.push_back(i);
        }
    }
    if (out.empty()) {
        for (int i = 0; i < row.enum_count; ++i) {
            out.push_back(i);
        }
    }
    return out;
}

const char* setting_disabled_reason(const SettingRow& row)
{
    if (row.flags & SF_READ_ONLY) {
        return "read only";
    }
    if ((row.flags & SF_D3D11_ONLY) && !is_d3d11()) {
        return "requires the Direct3D 11 renderer";
    }
    if ((row.flags & SF_LEGACY_RENDERER_ONLY) && is_d3d11()) {
        return "legacy renderers only";
    }
    // SF_MP_ONLY / SF_SP_ONLY / SF_HOST_ONLY deliberately do NOT gate editing: their badges say
    // where the setting applies, and the value is stored and persisted from anywhere.
    if ((row.flags & SF_DEDICATED_ONLY) && !rf::is_dedicated_server) {
        return "dedicated server only";
    }
    if ((row.flags & SF_NOT_DEDICATED) && rf::is_dedicated_server) {
        return "not available on a dedicated server";
    }
    if ((row.flags & SF_NOT_WHILE_IN_GAME) && (rf::level.flags & rf::LEVEL_LOADED)) {
        return "cannot be changed while a level is loaded";
    }
    if ((row.flags & SF_NEEDS_LOCAL_PLAYER) && !rf::local_player) {
        return "no local player";
    }
    if ((row.flags & SF_NEEDS_LARGE_SCREEN) && !g_alpine_game_config.big_hud
        && is_screen_resolution_too_low_for_big_hud()) {
        return "requires at least 1024x768";
    }
    if (row.unavailable_reason) {
        if (const char* reason = row.unavailable_reason()) {
            return reason;
        }
    }
    // A withheld server permission is a state, not an edit lock - see
    // setting_server_permission_blocked. On every permission row the withheld permission is
    // re-checked by the row's apply hook or by the consumer itself before anything is touched.
    return nullptr;
}

bool setting_is_enabled(const SettingRow& row)
{
    return setting_disabled_reason(row) == nullptr;
}

bool setting_server_permission_blocked(const SettingRow& row)
{
    return row.permission != ServerPermission::None && !server_permission_granted(row.permission);
}

void setting_run_apply(const SettingRow& row)
{
    if (row.apply) {
        row.apply();
    }
}

void setting_load_ini_value(const SettingRow& row, const std::string& value, rf::Player* player)
{
    std::visit(
        [&](const auto& acc) {
            using T = std::decay_t<decltype(acc)>;
            if constexpr (std::is_same_v<T, AccBool>) {
                g_alpine_game_config.*acc.member = std::stoi(value) != 0;
            }
            else if constexpr (std::is_same_v<T, AccInt>) {
                const int v = nearest_enum_value(row, std::stoi(value));
                if (acc.setter) {
                    (g_alpine_game_config.*acc.setter)(v);
                }
                else {
                    g_alpine_game_config.*acc.member = clamp_int(v, acc.lo, acc.hi);
                }
            }
            else if constexpr (std::is_same_v<T, AccUint>) {
                // Parsed wide so a negative value floors at 0 instead of wrapping to a huge one.
                const long long parsed =
                    std::clamp<long long>(std::stoll(value), 0, std::numeric_limits<unsigned>::max());
                const auto v = row.enum_labels
                    ? static_cast<unsigned>(nearest_enum_value(
                          row, static_cast<int>(std::min<long long>(parsed, std::numeric_limits<int>::max()))))
                    : static_cast<unsigned>(parsed);
                if (acc.setter) {
                    (g_alpine_game_config.*acc.setter)(v);
                }
                else {
                    g_alpine_game_config.*acc.member = clamp_uint(v, acc.lo, acc.hi);
                }
            }
            else if constexpr (std::is_same_v<T, AccFloat>) {
                const float v = parse_finite_float(value);
                if (acc.setter) {
                    (g_alpine_game_config.*acc.setter)(v);
                }
                else {
                    g_alpine_game_config.*acc.member = clamp_float(v, acc.lo, acc.hi);
                }
            }
            else if constexpr (std::is_same_v<T, AccColor>) {
                const auto color = parse_hex_color_string(value);
                if (!color) {
                    throw std::invalid_argument{"not a color"};
                }
                g_alpine_game_config.*acc.member = *color;
            }
            else if constexpr (std::is_same_v<T, AccOptColor>) {
                const auto color = parse_hex_color_string(value);
                if (!color) {
                    throw std::invalid_argument{"not a color"};
                }
                g_alpine_game_config.*acc.member = color;
            }
            else if constexpr (std::is_same_v<T, AccOptFloat>) {
                const float v = parse_finite_float(value);
                // A stored 1.0 means no override.
                if (v == 1.0f) {
                    (g_alpine_game_config.*acc.clearer)();
                }
                else {
                    (g_alpine_game_config.*acc.setter)(v);
                }
            }
            else if constexpr (std::is_same_v<T, AccString>) {
                if (acc.setter) {
                    (g_alpine_game_config.*acc.setter)(value);
                }
                else {
                    g_alpine_game_config.*acc.member = value;
                }
            }
            else if constexpr (std::is_same_v<T, AccCoreBool>) {
                g_alpine_system_config.*acc.member = std::stoi(value) != 0;
            }
            else if constexpr (std::is_same_v<T, AccFnBool>) {
                if (acc.set) {
                    acc.set(player, std::stoi(value) != 0);
                }
            }
            else if constexpr (std::is_same_v<T, AccFnInt>) {
                if (acc.set) {
                    acc.set(player, clamp_int(nearest_enum_value(row, std::stoi(value)), acc.lo, acc.hi));
                }
            }
            else if constexpr (std::is_same_v<T, AccFnFloat>) {
                if (acc.set) {
                    acc.set(player, clamp_float(parse_finite_float(value), acc.lo, acc.hi));
                }
            }
            else if constexpr (std::is_same_v<T, AccFnString>) {
                if (acc.set) {
                    acc.set(player, value);
                }
            }
        },
        row.access);
}

bool setting_format_ini_value(const SettingRow& row, rf::Player* player, std::string& out)
{
    bool has_value = true;
    std::visit(
        [&](const auto& acc) {
            using T = std::decay_t<decltype(acc)>;
            if constexpr (std::is_same_v<T, AccBool>) {
                out = (g_alpine_game_config.*acc.member) ? "1" : "0";
            }
            else if constexpr (std::is_same_v<T, AccInt>) {
                out = std::to_string(g_alpine_game_config.*acc.member);
            }
            else if constexpr (std::is_same_v<T, AccUint>) {
                out = std::to_string(g_alpine_game_config.*acc.member);
            }
            else if constexpr (std::is_same_v<T, AccFloat>) {
                out = format_float(g_alpine_game_config.*acc.member);
            }
            else if constexpr (std::is_same_v<T, AccColor>) {
                out = format_hex_color_string(g_alpine_game_config.*acc.member);
            }
            else if constexpr (std::is_same_v<T, AccOptColor>) {
                const auto& opt = g_alpine_game_config.*acc.member;
                has_value = opt.has_value();
                if (has_value) {
                    out = format_hex_color_string(*opt);
                }
            }
            else if constexpr (std::is_same_v<T, AccOptFloat>) {
                const auto& opt = g_alpine_game_config.*acc.member;
                has_value = opt.has_value();
                if (has_value) {
                    out = format_float(*opt);
                }
            }
            else if constexpr (std::is_same_v<T, AccString>) {
                out = g_alpine_game_config.*acc.member;
            }
            else if constexpr (std::is_same_v<T, AccCoreBool>) {
                out = (g_alpine_system_config.*acc.member) ? "1" : "0";
            }
            else if constexpr (std::is_same_v<T, AccFnBool>) {
                out = acc.get(player) ? "1" : "0";
            }
            else if constexpr (std::is_same_v<T, AccFnInt>) {
                out = std::to_string(acc.get(player));
            }
            else if constexpr (std::is_same_v<T, AccFnFloat>) {
                out = format_float(acc.get(player));
            }
            else if constexpr (std::is_same_v<T, AccFnString>) {
                out = acc.get(player);
            }
            else if constexpr (std::is_same_v<T, AccRegString>) {
                out = acc.get();
            }
            else {
                has_value = false;
            }
        },
        row.access);
    return has_value;
}

std::string setting_get_as_string(const SettingRow& row, rf::Player* player)
{
    std::string out;
    if (!setting_format_ini_value(row, player, out)) {
        return setting_value_type(row) == SettingValueType::None ? std::string{} : std::string{"default"};
    }
    return out;
}

bool setting_has_setter(const SettingRow& row)
{
    return std::visit(
        [](const auto& acc) {
            using T = std::decay_t<decltype(acc)>;
            if constexpr (std::is_same_v<T, AccFnBool> || std::is_same_v<T, AccFnInt> || std::is_same_v<T, AccFnFloat>
                          || std::is_same_v<T, AccFnString>) {
                return acc.set != nullptr;
            }
            else if constexpr (std::is_same_v<T, AccRegString> || std::is_same_v<T, AccNone>) {
                return false;
            }
            else {
                return true;
            }
        },
        row.access);
}

bool setting_set_from_string(const SettingRow& row, const std::string& value, rf::Player* player)
{
    const auto type = setting_value_type(row);
    if (type == SettingValueType::None || !setting_has_setter(row)) {
        return false;
    }

    // Console friendly spellings. The ini loader deliberately keeps the stricter 0/1 grammar.
    if (type == SettingValueType::Bool) {
        std::optional<bool> enabled = setting_parse_bool_word(value);
        if (!enabled && string_iequals(value, "toggle")) {
            enabled = setting_get_as_string(row, player) != "1";
        }
        if (enabled) {
            setting_load_ini_value(row, *enabled ? "1" : "0", player);
            setting_run_apply(row);
            return true;
        }
    }

    // "default"/"clear" resets an optional colour or scale to its unset state.
    if ((type == SettingValueType::OptionalColor || type == SettingValueType::OptionalFloat)
        && (string_iequals(value, "default") || string_iequals(value, "clear") || string_iequals(value, "none"))) {
        setting_reset_to_default(row);
        return true;
    }

    try {
        setting_load_ini_value(row, value, player);
    }
    catch (const std::exception&) {
        return false;
    }
    setting_run_apply(row);
    return true;
}

bool setting_console_set(std::string_view key_or_id, const std::string& value, rf::Player* player)
{
    const SettingRow* row = settings_find(key_or_id);
    if (!row) {
        xlog::error("[Settings] No setting named {}", key_or_id);
        return false;
    }
    if (!setting_set_from_string(*row, value, player)) {
        rf::console::print("Could not set {} to '{}'.", row->id, value);
        return false;
    }
    return true;
}

bool setting_console_set_bool(std::string_view key_or_id, std::optional<bool> enabled)
{
    return setting_console_set(key_or_id, enabled ? (*enabled ? "1" : "0") : "toggle");
}

std::optional<bool> setting_parse_bool_word(std::string_view value)
{
    for (const char* word : {"1", "on", "true", "yes"}) {
        if (string_iequals(value, word)) {
            return true;
        }
    }
    for (const char* word : {"0", "off", "false", "no"}) {
        if (string_iequals(value, word)) {
            return false;
        }
    }
    return std::nullopt;
}

bool setting_is_modified_from_default(const SettingRow& row)
{
    bool modified = false;
    std::visit(
        [&](const auto& acc) {
            using T = std::decay_t<decltype(acc)>;
            if constexpr (std::is_same_v<T, AccBool> || std::is_same_v<T, AccInt> || std::is_same_v<T, AccUint>
                          || std::is_same_v<T, AccFloat> || std::is_same_v<T, AccColor> || std::is_same_v<T, AccOptColor>
                          || std::is_same_v<T, AccOptFloat> || std::is_same_v<T, AccString>) {
                modified = (g_alpine_game_config.*acc.member) != (g_ags_defaults.*acc.member);
            }
            else if constexpr (std::is_same_v<T, AccCoreBool>) {
                modified = (g_alpine_system_config.*acc.member) != (g_asc_defaults.*acc.member);
            }
            else if constexpr (std::is_same_v<T, AccNone>) {
                modified = false;
            }
            else {
                modified = row.default_str != nullptr && setting_get_as_string(row) != row.default_str;
            }
        },
        row.access);
    return modified;
}

void setting_reset_to_default(const SettingRow& row)
{
    bool handled = true;
    std::visit(
        [&](const auto& acc) {
            using T = std::decay_t<decltype(acc)>;
            if constexpr (std::is_same_v<T, AccBool> || std::is_same_v<T, AccInt> || std::is_same_v<T, AccUint>
                          || std::is_same_v<T, AccFloat> || std::is_same_v<T, AccColor> || std::is_same_v<T, AccOptColor>
                          || std::is_same_v<T, AccOptFloat> || std::is_same_v<T, AccString>) {
                g_alpine_game_config.*acc.member = g_ags_defaults.*acc.member;
            }
            else if constexpr (std::is_same_v<T, AccCoreBool>) {
                g_alpine_system_config.*acc.member = g_asc_defaults.*acc.member;
            }
            else {
                handled = false;
            }
        },
        row.access);

    if (!handled) {
        if (row.default_str) {
            setting_set_from_string(row, row.default_str);
        }
        return;
    }
    setting_run_apply(row);
}

bool setting_hidden_in_menu(const SettingRow& row)
{
    return (row.flags & SF_HIDDEN) || ((row.flags & SF_ADVANCED) && !g_alpine_game_config.show_advanced_settings);
}

void settings_reset_category(SettingCategory category)
{
    for (const auto& row : settings_registry_all()) {
        // Only what the user can see and change: a hidden row would be reset where nobody can see
        // it, and a disabled one would bypass its own gate.
        if (row.category == category && !(row.flags & SF_READ_ONLY) && !setting_hidden_in_menu(row)
            && setting_is_enabled(row)) {
            setting_reset_to_default(row);
        }
    }
}

std::string setting_sanitize_player_name(std::string_view value)
{
    std::string name{trim(value)};
    for (char& c : name) {
        if (c == '%' || c == '\\') {
            c = '_';
        }
    }
    if (name.size() > 31) {
        name.resize(31);
    }
    return name;
}
