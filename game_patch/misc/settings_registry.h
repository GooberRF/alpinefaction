#pragma once

// Declarative table describing every user-facing setting: where the value lives, how it is
// persisted, how it is applied and how it should be presented. The ini loader/saver, the
// console commands and the options menu all read this table instead of hardcoding keys.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <common/config/AlpineCoreConfig.h>
#include "alpine_settings.h"

namespace rf
{
    struct Player;
}

enum class SettingWidget
{
    Toggle,
    Slider,
    Stepper,
    Enum,
    Color,
    OptionalColor,
    OptionalScale,
    Text,
    Keybind,
    ListReorder,
    Action,
    Label,
};

enum class SettingCategory
{
    Graphics,
    Hud,
    Interface,
    Audio,
    Bindings,
    Input,
    Gameplay,
    Multiplayer,
    _count,
};

enum SettingFlags : uint32_t
{
    SF_NONE = 0,
    SF_D3D11_ONLY = 1u << 0,
    SF_LEGACY_RENDERER_ONLY = 1u << 1,
    SF_MP_ONLY = 1u << 2,
    SF_SP_ONLY = 1u << 3,
    SF_HOST_ONLY = 1u << 4,
    SF_DEDICATED_ONLY = 1u << 5,
    SF_NOT_DEDICATED = 1u << 6,
    SF_NOT_WHILE_IN_GAME = 1u << 7,
    SF_NEEDS_LOCAL_PLAYER = 1u << 8,
    SF_HIDDEN = 1u << 9,
    SF_RESTART_REQUIRED = 1u << 10,
    SF_APPLIES_NEXT_LEVEL = 1u << 11,
    SF_READ_ONLY = 1u << 12,
    // Shown inverted from the stored field (stored "disable X", presented as "X").
    SF_INVERTED = 1u << 13,
    // ini I/O is hand written; the generic load/save path must skip this row.
    SF_BESPOKE_INI = 1u << 14,
    // The ini loader calls this row's apply hook.
    SF_APPLY_ON_LOAD = 1u << 15,
    // Needs a screen of at least 1024x768 (big HUD).
    SF_NEEDS_LARGE_SCREEN = 1u << 16,
    // Stores a control action index, -1 for none; the menu offers the action names.
    SF_ACTION_CHOICE = 1u << 17,
    // Listed only while "Show advanced settings" is on (search always finds it).
    SF_ADVANCED = 1u << 18,
    // The menu writes a dragged slider once, when the drag ends, not on every step.
    SF_COMMIT_ON_RELEASE = 1u << 19,
};

// Server permission that must be granted for a setting to have any effect in multiplayer.
enum class ServerPermission : uint8_t
{
    None,
    FullbrightMeshes,   // allow_fb_mesh
    LightmapsOnly,      // allow_lmap
    NoScreenShake,      // allow_no_ss
    NoMuzzleFlash,      // allow_no_mf
    Footsteps,          // allow_footsteps
    Outlines,           // allow_outlines
    OutlinesXray,       // allow_outlines_xray
};

enum class SettingPersistence : uint8_t
{
    None,      // session only
    Afs,       // alpine_settings.ini
    Afcc,      // alpine_system.ini
    Registry,  // GameConfig, read only (the launcher owns it)
};

enum class SettingValueType : uint8_t
{
    None,
    Bool,
    Int,
    Unsigned,
    Float,
    Color,
    OptionalColor,
    OptionalFloat,
    String,
};

// Value accessors. AlpineGameSettings/AlpineCoreConfig members are reached through
// pointers-to-member (optionally through the existing clamping setter); everything else
// through capture-less function pairs that receive the player the ini layer is working on
// (null outside load/save, in which case they fall back to rf::local_player).
struct AccBool
{
    bool AlpineGameSettings::* member;
};
struct AccInt
{
    int AlpineGameSettings::* member;
    void (AlpineGameSettings::*setter)(int) = nullptr;
    int lo = 0;
    int hi = -1;  // hi < lo means "no clamp"
};
struct AccUint
{
    unsigned AlpineGameSettings::* member;
    void (AlpineGameSettings::*setter)(unsigned) = nullptr;
    unsigned lo = 1;
    unsigned hi = 0;  // hi < lo means "no clamp"
};
struct AccFloat
{
    float AlpineGameSettings::* member;
    void (AlpineGameSettings::*setter)(float) = nullptr;
    float lo = 0.0f;
    float hi = -1.0f;
};
struct AccColor
{
    uint32_t AlpineGameSettings::* member;
};
struct AccOptColor
{
    std::optional<uint32_t> AlpineGameSettings::* member;
};
struct AccOptFloat
{
    std::optional<float> AlpineGameSettings::* member;
    void (AlpineGameSettings::*setter)(float);
    void (AlpineGameSettings::*clearer)();
};
struct AccString
{
    std::string AlpineGameSettings::* member;
    void (AlpineGameSettings::*setter)(const std::string&) = nullptr;
};
struct AccCoreBool
{
    bool AlpineCoreConfig::* member;
};
struct AccFnBool
{
    bool (*get)(rf::Player*);
    void (*set)(rf::Player*, bool) = nullptr;
};
struct AccFnInt
{
    int (*get)(rf::Player*);
    void (*set)(rf::Player*, int) = nullptr;
    int lo = 0;
    int hi = -1;
};
struct AccFnFloat
{
    float (*get)(rf::Player*);
    void (*set)(rf::Player*, float) = nullptr;
    float lo = 0.0f;
    float hi = -1.0f;
};
struct AccFnString
{
    std::string (*get)(rf::Player*);
    void (*set)(rf::Player*, const std::string&) = nullptr;
};
struct AccRegString
{
    std::string (*get)();
};
struct AccNone
{
};

using SettingAccess = std::variant<AccBool, AccInt, AccUint, AccFloat, AccColor, AccOptColor, AccOptFloat,
                                   AccString, AccCoreBool, AccFnBool, AccFnInt, AccFnFloat, AccFnString,
                                   AccRegString, AccNone>;

struct SettingRow
{
    const char* id;                          // stable catalog id, unique, snake_case
    const char* key = nullptr;               // persistence key (ini/registry value name)
    const char* label = "";                  // <= 36 chars
    const char* tooltip = "";
    SettingCategory category = SettingCategory::Graphics;
    const char* subcategory = "";
    SettingWidget widget = SettingWidget::Toggle;
    SettingAccess access = AccNone{};
    uint32_t flags = SF_NONE;
    ServerPermission permission = ServerPermission::None;
    SettingPersistence persistence = SettingPersistence::Afs;
    void (*apply)() = nullptr;
    const char* const* enum_labels = nullptr;
    int enum_count = 0;
    // Stored value per label, for enums whose values are not consecutive. Null means the index.
    const int* enum_values = nullptr;
    // Menu filter: labels whose value fails this are not offered. Console access is unfiltered.
    bool (*enum_offered)(int value) = nullptr;
    const char* console_cmd = nullptr;
    // Default in ini text form, for rows whose storage is not AlpineGameSettings /
    // AlpineCoreConfig / GameConfig (those compare against a default constructed struct instead).
    const char* default_str = nullptr;
    // Presentation range for slider rows: what the track spans and how the readout reads. The
    // accessor's own clamp is the storage contract and is never widened by these; a slider whose
    // ui range reaches past the clamp simply cannot store the extreme. ui_max < ui_min means the
    // row has no ui range, in which case the menu falls back to the accessor clamp (slider) or a
    // stepper (no clamp either).
    float ui_min = 0.0f;
    float ui_max = -1.0f;
    float ui_step = 0.0f;
    // Arrow-key step for an unstepped slider; 0 nudges by a twentieth of the range.
    float ui_nudge = 0.0f;
    float ui_display_scale = 1.0f;
    int ui_decimals = -1; // -1 = derive from the value type
    const char* ui_suffix = "";
    // Shown instead of the number when the stored value is 0.
    const char* ui_zero_label = nullptr;
    // Availability gate for state the flags cannot express (controller hardware, a related
    // toggle). Returns why the row is unavailable, or nullptr when it is available. Read by
    // setting_disabled_reason, so the menu greys the row and the console refuses the edit.
    const char* (*unavailable_reason)() = nullptr;
};

// Table access
std::span<const SettingRow> settings_registry_all();
std::vector<const SettingRow*> settings_in_category(SettingCategory category);
const SettingRow* settings_find(std::string_view key_or_id);
const char* setting_category_name(SettingCategory category);
SettingValueType setting_value_type(const SettingRow& row);
// Slider presentation range: the row's own ui range when it has one, else the accessor clamp.
// False when neither exists, in which case the row has no bounded range to draw a track for.
bool setting_ui_range(const SettingRow& row, float& lo, float& hi, float& step);
// Stored value of enum label `index`.
int setting_enum_value(const SettingRow& row, int index);
// Label indices the menu should offer, in table order. Falls back to every label when the row's
// filter rejects all of them.
std::vector<int> setting_enum_offered(const SettingRow& row);

// True when the setting can be edited right now. Only hard constraints disable a row: the
// renderer it needs, the screen size it needs, a missing local player, read-only storage, the
// dedicated/headless split and "not while a level is loaded". Context flags (SF_MP_ONLY,
// SF_SP_ONLY, SF_HOST_ONLY) and server permissions are presentation only - they say where the
// setting applies, never whether it can be changed. Hidden rows are still "enabled"; visibility
// is a separate concern.
bool setting_is_enabled(const SettingRow& row);
// True when the menu leaves the row out of its category: SF_HIDDEN, or SF_ADVANCED while advanced
// settings are not shown.
bool setting_hidden_in_menu(const SettingRow& row);
// Reason the row is disabled, or nullptr when it is enabled.
const char* setting_disabled_reason(const SettingRow& row);
// True when the row has a server permission and the server the client is on currently withholds
// it. A state indicator for the padlock badge, not an edit lock: the value is still editable and
// takes effect wherever it is allowed.
bool setting_server_permission_blocked(const SettingRow& row);

// False for read-only rows (launcher-owned registry values, labels, bespoke widgets).
bool setting_has_setter(const SettingRow& row);
std::string setting_get_as_string(const SettingRow& row, rf::Player* player = nullptr);
// Parses, clamps and stores the value, then runs the apply hook. False on a parse error.
bool setting_set_from_string(const SettingRow& row, const std::string& value, rf::Player* player = nullptr);
void setting_run_apply(const SettingRow& row);
// Console command helper: resolves the row and forwards to setting_set_from_string.
bool setting_console_set(std::string_view key_or_id, const std::string& value, rf::Player* player = nullptr);
// Same for a bool row: a value sets it, nullopt toggles it.
bool setting_console_set_bool(std::string_view key_or_id, std::optional<bool> enabled);
// Console spellings of a bool: 1/0, on/off, true/false, yes/no (case insensitive).
std::optional<bool> setting_parse_bool_word(std::string_view value);

bool setting_is_modified_from_default(const SettingRow& row);
void setting_reset_to_default(const SettingRow& row);
void settings_reset_category(SettingCategory category);

// ini layer helpers (used by alpine_settings.cpp)
// Throws std::invalid_argument / std::out_of_range on a malformed value.
void setting_load_ini_value(const SettingRow& row, const std::string& value, rf::Player* player);
// False when an optional value is unset, in which case the key must not be written.
bool setting_format_ini_value(const SettingRow& row, rf::Player* player, std::string& out);

// Sanitises a player name from the menu or alpine_settings.ini: outer whitespace trimmed, percent and
// backslash replaced with '_', capped at 31 characters. Empty or longer than 20 characters is rejected by
// the engine.
std::string setting_sanitize_player_name(std::string_view value);
constexpr size_t player_name_max_len = 20;
