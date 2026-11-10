#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <common/utils/string-utils.h>
#include "menu_shell.h"
#include "settings_menu.h"
#include "settings_skin.h"
#include "ui_kit.h"
#include "../graphics/ui_draw.h"
#include "../input/bind_context.h"
#include "../input/gamepad.h"
#include "../input/input.h"
#include "../main/main.h"
#include "../misc/alpine_settings.h"
#include "../misc/settings_registry.h"
#include "../multi/sprays.h"
#include "../rf/gameseq.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/input.h"
#include "../rf/level.h"
#include "../rf/multi.h"
#include "../rf/os/console.h"
#include "../rf/os/frametime.h"
#include "../rf/player/control_config.h"
#include "../rf/player/player.h"
#include "../rf/ui.h"
#include "../rf/weapon.h"

namespace
{

static_assert(static_cast<int>(SettingCategory::_count) + 1 <= menu_panel_slots);
// Left inset of a row's label and right inset of its controls.
constexpr float row_inset = 32.0f;

// The category column, top to bottom. The longest labels take the top slots, beside the side
// bar's flat stretch and clear of the brackets lower down.
constexpr std::array<SettingCategory, static_cast<std::size_t>(SettingCategory::_count)> category_order{
    SettingCategory::Multiplayer, SettingCategory::Gameplay, SettingCategory::Graphics,
    SettingCategory::Interface,   SettingCategory::Hud,      SettingCategory::Audio,
    SettingCategory::Input,       SettingCategory::Bindings,
};

constexpr bool category_order_complete()
{
    for (int c = 0; c < static_cast<int>(SettingCategory::_count); ++c) {
        if (std::ranges::count(category_order, static_cast<SettingCategory>(c)) != 1) {
            return false;
        }
    }
    return true;
}
static_assert(category_order_complete(), "every category needs exactly one slot in category_order");

int category_slot(SettingCategory category)
{
    return static_cast<int>(std::ranges::find(category_order, category) - category_order.begin());
}

enum class Modal
{
    None,
    ResetCategory,
    ResetBindings,
    RevertSession,
    GridPicker,
    ColorPicker,
};

UiContext g_ui;
SettingCategory g_category = category_order.front();
bool g_closing = false;
UiId g_prev_text_focus = 0;
float g_content_scroll = 0.0f;
// Where each category was left, kept for the whole game session; a search borrows the panel and
// hands it back.
std::array<float, static_cast<std::size_t>(SettingCategory::_count)> g_category_scroll{};
bool g_search_holds_panel = false;
// 0 = fully off canvas, 1 = seated. Rises on entry, falls again once Back/ESC is taken.
float g_slide = 0.0f;
// 0 = seated, 1 = off the right edge, where g_switch_to replaces the category.
float g_switch = 0.0f;
std::optional<SettingCategory> g_switch_to;
// A close can skip the is_closed poll, so the suspend depth only moves through this flag.
bool g_bind_suspended = false;
// The focus the content panel last scrolled to, so only a focus change scrolls.
UiId g_scrolled_to_focus = 0;
std::string g_search;
Modal g_modal = Modal::None;

std::unordered_set<const SettingRow*> g_changed;
// Every row's value as the menu opened, so a row put back where it started stops counting as a
// change. Rebuilt by settings_menu_enter; see note_rows_changed.
std::unordered_map<const SettingRow*, std::string> g_baseline;
// The non-row state REVERT also puts back: every binding (Global and each context's overrides)
// and the autoswitch order, as they were when the menu opened.
struct BindBaseline
{
    std::vector<BindOverride> global;
    std::vector<int16_t> global_aux;
    std::vector<std::vector<std::optional<BindOverride>>> overrides; // [context][binding]
};
BindBaseline g_bind_baseline;
constexpr std::size_t weapon_pref_count = std::extent_v<decltype(rf::Player::weapon_prefs)>;
std::optional<std::array<int, weapon_pref_count>> g_weapon_prefs_baseline;

// The description line of a block that is not a registry row (binding rows, the context strip).
std::string g_desc_custom;

// Weapon autoswitch reorder: the selected slot is an index into Player::weapon_prefs.
int g_weapon_pref_sel = -1;

// `tile_h` is the image area; a captioned tile adds its caption line to it.
struct GridPicker
{
    const char* row_id;
    const char* title;
    const char* empty_text;
    float tile_w;
    float tile_h;
    int (*count)();
    int (*bitmap)(int);
    const char* (*caption)(int);
    bool caption_line;
};

int character_count()
{
    return std::clamp(rf::num_multi_characters, 0, rf::MAX_MP_CHARACTERS);
}

const char* character_caption(int index)
{
    if (index < 0 || index >= character_count()) {
        return nullptr;
    }
    const rf::MpCharacterInfo& info = rf::mp_characters[index];
    if (info.screen_name && info.screen_name[0] != '\0') {
        return info.screen_name;
    }
    return info.name[0] != '\0' ? info.name : nullptr;
}

constexpr GridPicker spray_picker{
    .row_id = "selected_spray_index",
    .title = "SELECT SPRAY",
    .empty_text = "No sprays are installed.",
    .tile_w = skin::spray_tile,
    .tile_h = skin::spray_tile,
    .count = [] { return spray_count(); },
    .bitmap = [](int i) { return spray_get_bitmap(static_cast<uint16_t>(i)); },
    .caption = [](int i) { return spray_texture_name(static_cast<uint16_t>(i)); },
    .caption_line = false,
};

constexpr GridPicker character_picker{
    .row_id = "multi_character",
    .title = "SELECT CHARACTER",
    .empty_text = "No multiplayer characters are installed.",
    .tile_w = skin::portrait_tile,
    .tile_h = skin::portrait_tile,
    .count = character_count,
    .bitmap = [](int i) {
        return i >= 0 && i < character_count() ? rf::mp_characters[i].image_handle : -1;
    },
    .caption = character_caption,
    .caption_line = true,
};

const GridPicker* picker_for_row(const SettingRow& row)
{
    const std::string_view id{row.id};
    if (id == spray_picker.row_id) {
        return &spray_picker;
    }
    if (id == character_picker.row_id) {
        return &character_picker;
    }
    return nullptr;
}

// The row the grid modal is editing, and which picker it is showing.
const SettingRow* g_picker_row = nullptr;
const GridPicker* g_picker = nullptr;
float g_picker_scroll = 0.0f;

// The colour modal: the row it edits, the value it opened with and the pick in progress. `rgba`
// is what APPLY writes; `hsva` keeps the hue through greys, where RGB alone would lose it. An
// optional colour opened with no override stays `unset` (APPLY writes nothing) until a pick.
struct ColorPick
{
    const SettingRow* row = nullptr;
    bool had_value = false;
    bool unset = false;
    uint32_t original = 0;
    uint32_t rgba = 0;
    SkinHsva hsva;
    std::string hex;
};
ColorPick g_color_pick;

// Widget-local copies of every row that feeds a widget taking a reference. The kit's inline
// numeric/hex editors keep a pointer to that reference in a commit callback which fires during
// the NEXT frame's hit-test pass, so the storage has to outlive the frame that built the widget.
struct RowState
{
    float f = 0.0f;
    std::optional<uint32_t> color;
    std::string text;
    // Last value read from or written to storage. A typed value is committed when its box loses
    // the focus, which can happen at the top of the pass, so a change is detected against this
    // rather than against the value the widget started the pass with.
    float stored_f = 0.0f;
    std::optional<uint32_t> stored_color;
    bool held = false; // an SF_COMMIT_ON_RELEASE drag not yet written
};

std::unordered_map<const SettingRow*, RowState> g_row_state;

// Options sits on the main menu, which sits on gameplay or limbo.
int state_under_menu()
{
    const int top = rf::gameseq_stack_top_index;
    if (top < 2 || top >= static_cast<int>(std::size(rf::gameseq_state_stack))) {
        return -1;
    }
    return rf::gameseq_state_stack[top - 2];
}

bool level_is_running()
{
    const int under = state_under_menu();
    return (rf::level.flags & rf::LEVEL_LOADED)
        && (under == rf::GS_GAMEPLAY || under == rf::GS_MULTI_LIMBO);
}

bool in_limbo()
{
    return state_under_menu() == rf::GS_MULTI_LIMBO;
}

void remember_category_scroll()
{
    if (!g_search_holds_panel) {
        g_category_scroll[static_cast<std::size_t>(g_category)] = g_content_scroll;
    }
}

// Drops any search and shows `category` where it was last left.
void show_category(SettingCategory category)
{
    g_search.clear();
    g_search_holds_panel = false;
    g_category = category;
    g_content_scroll = g_category_scroll[static_cast<std::size_t>(category)];
}

// The category the column lights: where a switch is heading, else the one on the panel. Search
// results belong to no category.
std::optional<SettingCategory> selected_category()
{
    if (g_switch_to) {
        return g_switch_to;
    }
    if (!g_search.empty()) {
        return std::nullopt;
    }
    return g_category;
}

void write_committed_values();

void request_close()
{
    if (g_closing) {
        return;
    }
    // A slider still being dragged is written rather than lost.
    g_ui.release_capture();
    write_committed_values();
    g_closing = true;
    remember_category_scroll();
    rf::gameseq_pop_state();
}

struct Fonts
{
    int title = -1;
    int heading = -1;
    int section = -1;
    int body = -1;
    int body_bold = -1;
    int caption = -1;
    int caption_bold = -1;
};

float row_float(const SettingRow& row, float fallback)
{
    const std::string text = setting_get_as_string(row);
    float value = fallback;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return ec == std::errc{} ? value : fallback;
}

bool is_integral_row(const SettingRow& row)
{
    const auto type = setting_value_type(row);
    return type == SettingValueType::Int || type == SettingValueType::Unsigned;
}

UiNumberFormat row_number_format(const SettingRow& row)
{
    UiNumberFormat fmt;
    fmt.decimals = row.ui_decimals >= 0 ? row.ui_decimals : (is_integral_row(row) ? 0 : 2);
    fmt.display_scale = row.ui_display_scale;
    fmt.suffix = row.ui_suffix ? row.ui_suffix : "";
    fmt.zero_label = row.ui_zero_label;
    return fmt;
}

// Keeps g_changed in step with the baseline captured at menu entry. A write can move other rows
// too (a preset expands over its members, a member edit makes it Custom), so every row is checked.
// Rows without a setter (bindings, the autoswitch order) are tracked by their own blocks.
void note_rows_changed()
{
    for (const SettingRow& row : settings_registry_all()) {
        if (!setting_has_setter(row)) {
            continue;
        }
        const auto it = g_baseline.find(&row);
        if (it == g_baseline.end() || it->second != setting_get_as_string(row)) {
            g_changed.insert(&row);
        }
        else {
            g_changed.erase(&row);
        }
    }
}

void note_block_changed(const char* id, bool at_baseline)
{
    if (const SettingRow* row = settings_find(id)) {
        if (at_baseline) {
            g_changed.erase(row);
        }
        else {
            g_changed.insert(row);
        }
    }
}

void write_row(const SettingRow& row, const std::string& value);
bool row_widget_enabled(const SettingRow& row);

std::string format_row_value(float value, bool integral)
{
    return integral ? std::to_string(static_cast<int>(std::lround(value))) : std::format("{}", value);
}

// A typed value box that loses the focus without its row being drawn (category switch, row
// scrolled away) commits into g_row_state after every widget has run, so nothing compared it
// with storage; write it here before the next pass resyncs the row from storage.
void write_committed_values()
{
    for (auto& [row, state] : g_row_state) {
        if (state.held) {
            if (g_ui.captured_id() == ui_id(row->id)) {
                continue;
            }
            state.held = false;
            if (!row_widget_enabled(*row)) {
                state.f = state.stored_f;
                continue;
            }
        }
        switch (row->widget) {
            case SettingWidget::Slider:
            case SettingWidget::Stepper:
            case SettingWidget::OptionalScale:
                if (state.f != state.stored_f) {
                    state.stored_f = state.f;
                    write_row(*row, format_row_value(state.f, is_integral_row(*row)));
                }
                break;
            case SettingWidget::Color:
            case SettingWidget::OptionalColor:
                if (state.color != state.stored_color) {
                    state.stored_color = state.color;
                    if (state.color) {
                        write_row(*row, format_hex_color_string(*state.color));
                    }
                }
                break;
            default:
                break;
        }
    }
}

void write_row(const SettingRow& row, const std::string& value)
{
    if (setting_set_from_string(row, value)) {
        note_rows_changed();
    }
}

// ---------------------------------------------------------------------------
// Text rows
// ---------------------------------------------------------------------------

bool char_filter_player_name(char c)
{
    // player_change_name would substitute both.
    return c >= 0x20 && c < 0x7F && c != '%' && c != '\\';
}

struct TextRowConfig
{
    UiCharFilter filter = ui_char_filter_filename;
    size_t max_len = 63;
    const char* hint = "";
    const char* warn = nullptr; // non-null while the field content cannot be committed
};

TextRowConfig text_row_config(const SettingRow& row, const std::string& text)
{
    TextRowConfig cfg;
    const std::string_view id{row.id};
    if (id == "player_name") {
        cfg.filter = char_filter_player_name;
        cfg.max_len = 31; // the ini cap; the engine's own commit is stricter, see below
        cfg.hint = "Player";
        const std::string clean = setting_sanitize_player_name(text);
        if (clean.empty()) {
            cfg.warn = "Enter a name";
        }
        else if (clean.size() > player_name_max_len) {
            cfg.warn = "20 characters maximum";
        }
    }
    return cfg;
}

int desc_id_for(const SettingRow& row)
{
    return static_cast<int>(&row - settings_registry_all().data());
}

const SettingRow* desc_row_from_id(int id)
{
    const auto rows = settings_registry_all();
    return id >= 0 && id < static_cast<int>(rows.size()) ? &rows[id] : nullptr;
}

// ---------------------------------------------------------------------------
// Bindings (Bindings screen)
// ---------------------------------------------------------------------------

// The context the Bindings screen is editing. Global edits the live table (which is the true
// Global for as long as the menu holds the runtime suspended); the others edit that context's
// override store.
BindContextId g_bind_context = BindContextId::Global;

const char* bind_context_description(BindContextId ctx)
{
    switch (ctx) {
        case BindContextId::Vehicles: return "Overrides used while in any vehicle or turret seat.";
        case BindContextId::Spectator: return "Overrides used while spectating.";
        case BindContextId::Demos: return "Overrides used during demo playback.";
        default: return "Bindings used everywhere.";
    }
}

// The device the columns describe. The keyboard/mouse pair and the controller have a runtime; VR
// is selectable but every cap is inert.
enum class BindDevice
{
    Keyboard,
    Controller,
    VR,
    _count,
};

BindDevice g_bind_device = BindDevice::Keyboard;

const char* bind_device_description(BindDevice device)
{
    switch (device) {
        case BindDevice::Controller: return "Controller bindings.";
        case BindDevice::VR: return "VR support is not available in this build.";
        default: return "Keyboard and mouse bindings.";
    }
}

bool bind_device_live()
{
    return g_bind_device == BindDevice::Keyboard;
}

bool bind_pad_live()
{
    return g_bind_device == BindDevice::Controller;
}

// True when the action rows can be edited on the current device.
bool bind_rows_live()
{
    return bind_device_live() || bind_pad_live();
}

// A row carries the OVERRIDE badge when the edited context holds its own values for the shown
// device. Each device has its own fields, so overriding keys leaves the controller alone.
bool bind_is_override(int index)
{
    if (g_bind_context == BindContextId::Global || !bind_rows_live()) {
        return false;
    }
    const BindOverride* value = bind_context_override(g_bind_context, index);
    if (!value) {
        return false;
    }
    if (bind_pad_live()) {
        return value->pad_codes[0] != bind_inherit || value->pad_codes[1] != bind_inherit;
    }
    return value->scan_codes[0] != bind_inherit || value->scan_codes[1] != bind_inherit
        || value->mouse_btn_id != bind_inherit;
}

std::string bind_key_name(int scan)
{
    if (scan < 0) {
        return {};
    }
    rf::String out;
    if (rf::control_config_get_key_name(&out, scan) != 0) {
        return {};
    }
    return std::string{out.c_str()};
}

std::string bind_mouse_name(int btn)
{
    if (btn < 0) {
        return {};
    }
    rf::String out;
    if (rf::control_config_get_mouse_button_name(&out, btn) != 0) {
        return {};
    }
    return std::string{out.c_str()};
}

// Mouse 4 and up arrive as key events (input.h), so they live in a key slot. The MOUSE cap owns
// them: they are parked in the second key slot, which has no cap of its own.
std::string bind_mouse_cap_name(const BindOverride& value)
{
    if (value.mouse_btn_id >= 0) {
        return bind_mouse_name(value.mouse_btn_id);
    }
    return ctrl_is_extra_mouse_scan(value.scan_codes[1]) ? bind_key_name(value.scan_codes[1]) : std::string{};
}

struct BindGroup
{
    const char* caption;
    int first;
    int last; // exclusive
};

std::vector<BindGroup> bind_groups()
{
    std::vector<BindGroup> groups;
    const int total = bind_context_binding_count();
    if (total <= 0) {
        return groups;
    }
    const int alpine = alpine_control_start_index();
    // The fixed stock actions end after the four weapon-category selects.
    constexpr int stock_action_count = rf::CC_ACTION_SELECT_WEAPON_FIRST + 4;
    const int stock_end = std::min(total, stock_action_count);
    const int weapon_end = alpine > 0 ? std::min(total, alpine) : total;
    // Alpine's actions are listed ahead of the long weapon list; only the menu order changes.
    groups.push_back(BindGroup{"GAME ACTIONS", 0, stock_end});
    if (total > weapon_end) {
        groups.push_back(BindGroup{"ALPINE ACTIONS", weapon_end, total});
    }
    if (weapon_end > stock_end) {
        groups.push_back(BindGroup{"WEAPONS", stock_end, weapon_end});
    }
    return groups;
}

const char* bind_action_name(int index)
{
    const rf::ControlConfig* cc = bind_context_controls();
    if (!cc || index < 0 || index >= bind_context_binding_count()) {
        return "";
    }
    const char* name = cc->bindings[index].name.c_str();
    return name ? name : "";
}

// Capture state. Exactly one row can be capturing, into exactly one of its two caps.
struct BindCapture
{
    int row = -1;
    bool mouse = false;
    // True while the row waits for a controller button or trigger instead of a key or mouse input.
    bool pad = false;
    int pad_slot = 0; // which controller cap listens: 0 = BUTTON, 1 = ALT
    // False for the frame the capture opened: the click or Enter that started it is still visible
    // to the mouse press counters, and must not become the new binding.
    bool armed = false;
    UiId id = 0;
};

BindCapture g_capture;

// Stock's exclusion set, less Alt and Mouse 4+, which Alpine binds. ESC is the cancel.
bool bind_scan_excluded(int scan)
{
    return scan == rf::KEY_PRINT_SCRN || scan == rf::KEY_PAUSE || scan == rf::KEY_BREAK
        || scan == rf::KEY_LAPOSTRO_DBG;
}

void bind_suspend_contexts()
{
    if (!g_bind_suspended) {
        g_bind_suspended = true;
        bind_context_suspend();
    }
}

void bind_resume_contexts()
{
    // A controller capture must not outlive the menu, whichever way it closes.
    gamepad_bind_capture_end();
    if (g_bind_suspended) {
        g_bind_suspended = false;
        bind_context_resume();
    }
}

bool bindings_at_baseline()
{
    const rf::ControlConfig* cc = bind_context_controls();
    const int total = bind_context_binding_count();
    if (!cc || total != static_cast<int>(g_bind_baseline.global.size())) {
        return false;
    }
    const auto same_binding = [](const BindOverride& lhs, const BindOverride& rhs) {
        return lhs.scan_codes[0] == rhs.scan_codes[0] && lhs.scan_codes[1] == rhs.scan_codes[1]
            && lhs.mouse_btn_id == rhs.mouse_btn_id && lhs.pad_codes[0] == rhs.pad_codes[0]
            && lhs.pad_codes[1] == rhs.pad_codes[1];
    };
    for (int i = 0; i < total; ++i) {
        const rf::ControlConfigItem& item = cc->bindings[i];
        const BindOverride& then = g_bind_baseline.global[i];
        const BindOverride global = bind_context_global_binding(i);
        const BindOverride now{{item.scan_codes[0], item.scan_codes[1]}, item.mouse_btn_id,
                               {global.pad_codes[0], global.pad_codes[1]}};
        if (!same_binding(now, then) || item.aux_btn_id != g_bind_baseline.global_aux[i]) {
            return false;
        }
    }
    for (int c = 1; c < static_cast<int>(g_bind_baseline.overrides.size()); ++c) {
        const auto& overrides = g_bind_baseline.overrides[c];
        for (int i = 0; i < static_cast<int>(overrides.size()); ++i) {
            const BindOverride* now = bind_context_override(static_cast<BindContextId>(c), i);
            const std::optional<BindOverride>& then = overrides[i];
            if ((now != nullptr) != then.has_value() || (now && !same_binding(*now, *then))) {
                return false;
            }
        }
    }
    return true;
}

void bind_note_changed()
{
    note_block_changed("bindings", bindings_at_baseline());
}

void bind_toast_unbound(UiContext& ctx, const std::vector<std::string>& cleared)
{
    for (const std::string& name : cleared) {
        ctx.push_toast(std::format("UNBOUND FROM {}", name.empty() ? "another action" : name));
    }
}

// What the edited context actually resolves an action to: its own fields where it has them, else
// the Global values. Dedupe and every edit work off this, so a context is exclusive within itself.
BindOverride bind_effective(int index)
{
    if (g_bind_context != BindContextId::Global) {
        return bind_context_effective(g_bind_context, index);
    }
    return bind_context_global_binding(index);
}

// The value an edit starts from: the stored override, or a triple that changes nothing yet, so
// touching one cap never pins the other two to whatever Global happened to hold.
BindOverride bind_override_or_inherit(int index)
{
    if (const BindOverride* value = bind_context_override(g_bind_context, index)) {
        return *value;
    }
    return BindOverride{{bind_inherit, bind_inherit}, bind_inherit, {bind_inherit, bind_inherit}};
}

// Exclusive assignment inside a non-Global context: the conflicting action gets an override of
// its own that unbinds the field here, leaving its Global binding untouched.
void bind_assign_context(UiContext& ctx, int index, int scan, int mouse_btn)
{
    const int total = bind_context_binding_count();
    BindOverride self = bind_override_or_inherit(index);
    std::vector<std::string> cleared;

    if (scan >= 0) {
        const int slot = ctrl_is_extra_mouse_scan(scan) ? 1 : 0;
        if (bind_effective(index).scan_codes[1 - slot] == scan) {
            self.scan_codes[1 - slot] = -1;
        }
        self.scan_codes[slot] = static_cast<int16_t>(scan);
        if (slot == 1) {
            self.mouse_btn_id = -1; // the MOUSE cap holds one input
        }
        for (int i = 0; i < total; ++i) {
            if (i == index) {
                continue;
            }
            const BindOverride effective = bind_effective(i);
            BindOverride other = bind_override_or_inherit(i);
            bool changed = false;
            for (int j = 0; j < 2; ++j) {
                if (effective.scan_codes[j] == scan) {
                    other.scan_codes[j] = -1;
                    changed = true;
                }
            }
            if (changed) {
                bind_context_set_override(g_bind_context, i, other);
                cleared.push_back(std::string{bind_action_name(i)});
            }
        }
    }
    else if (mouse_btn >= 0) {
        self.mouse_btn_id = static_cast<int16_t>(mouse_btn);
        if (ctrl_is_extra_mouse_scan(bind_effective(index).scan_codes[1])) {
            self.scan_codes[1] = -1;
        }
        for (int i = 0; i < total; ++i) {
            if (i == index || bind_effective(i).mouse_btn_id != mouse_btn) {
                continue;
            }
            BindOverride other = bind_override_or_inherit(i);
            other.mouse_btn_id = -1;
            bind_context_set_override(g_bind_context, i, other);
            cleared.push_back(std::string{bind_action_name(i)});
        }
    }
    else {
        return;
    }

    bind_context_set_override(g_bind_context, index, self);
    bind_note_changed();
    bind_toast_unbound(ctx, cleared);
}

// Exclusive assignment. Unlike stock only the captured cap is written: the key and the mouse bind
// each have their own cap and clear.
void bind_assign(UiContext& ctx, int index, int scan, int mouse_btn)
{
    rf::ControlConfig* cc = bind_context_controls();
    const int total = bind_context_binding_count();
    if (!cc || index < 0 || index >= total || !bind_device_live()) {
        return;
    }
    if (g_bind_context != BindContextId::Global) {
        bind_assign_context(ctx, index, scan, mouse_btn);
        return;
    }
    std::vector<std::string> cleared;
    const auto note = [&](int other) {
        std::string name{bind_action_name(other)};
        if (std::find(cleared.begin(), cleared.end(), name) == cleared.end()) {
            cleared.push_back(std::move(name));
        }
    };

    if (scan >= 0) {
        const int slot = ctrl_is_extra_mouse_scan(scan) ? 1 : 0;
        for (int i = 0; i < total; ++i) {
            for (int j = 0; j < 2; ++j) {
                if (cc->bindings[i].scan_codes[j] != scan || (i == index && j == slot)) {
                    continue;
                }
                // scan_codes[1] is live at runtime and persisted whether or not a cap shows it,
                // so it has to lose the code too or the bind would not be exclusive.
                cc->bindings[i].scan_codes[j] = -1;
                if (i != index) {
                    note(i);
                }
            }
        }
        cc->bindings[index].scan_codes[slot] = static_cast<int16_t>(scan);
        if (slot == 1) {
            cc->bindings[index].mouse_btn_id = -1; // the MOUSE cap holds one input
        }
    }
    else if (mouse_btn >= 0) {
        for (int i = 0; i < total; ++i) {
            if (i == index || cc->bindings[i].mouse_btn_id != mouse_btn) {
                continue;
            }
            cc->bindings[i].mouse_btn_id = -1;
            note(i);
        }
        cc->bindings[index].mouse_btn_id = static_cast<int16_t>(mouse_btn);
        if (ctrl_is_extra_mouse_scan(cc->bindings[index].scan_codes[1])) {
            cc->bindings[index].scan_codes[1] = -1;
        }
    }
    else {
        return;
    }

    // Stock's "latest assignment wins" reaches into the contexts too: an override still holding
    // the code just assigned here becomes unbound there.
    const bool ctx_unbound = bind_context_dedupe_against_global();
    bind_note_changed();
    bind_toast_unbound(ctx, cleared);
    if (ctx_unbound) {
        ctx.push_toast("CONTEXT OVERRIDES UNBOUND");
    }
}

void bind_clear_cap(int16_t (&scan_codes)[2], int16_t& mouse_btn_id, bool slot1_is_mouse, bool mouse)
{
    if (mouse) {
        mouse_btn_id = -1;
        if (slot1_is_mouse) {
            scan_codes[1] = -1;
        }
    }
    else {
        // A plain key in scan_codes[1] has no cap of its own but is live at runtime, so it goes too.
        scan_codes[0] = -1;
        if (!slot1_is_mouse) {
            scan_codes[1] = -1;
        }
    }
}

void bind_clear(int index, bool mouse)
{
    rf::ControlConfig* cc = bind_context_controls();
    if (!cc || index < 0 || index >= bind_context_binding_count() || !bind_device_live()) {
        return;
    }
    if (g_bind_context != BindContextId::Global) {
        // Clearing an inherited cap is itself an override: "explicitly unbound here". The other
        // fields keep following Global.
        BindOverride value = bind_override_or_inherit(index);
        bind_clear_cap(value.scan_codes, value.mouse_btn_id,
                       ctrl_is_extra_mouse_scan(bind_effective(index).scan_codes[1]), mouse);
        bind_context_set_override(g_bind_context, index, value);
    }
    else {
        rf::ControlConfigItem& item = cc->bindings[index];
        bind_clear_cap(item.scan_codes, item.mouse_btn_id, ctrl_is_extra_mouse_scan(item.scan_codes[1]), mouse);
    }
    bind_note_changed();
}

// Controller bindings are one table for every context, and an input serves one action.
// Exclusive assignment inside a non-Global context, as for keys: the conflicting action gets an
// override of its own that unbinds the slot here, leaving its Global binding untouched.
void bind_pad_assign_context(UiContext& ctx, int index, int slot, int code)
{
    const int total = bind_context_binding_count();
    const bool menu_only = gamepad_is_menu_only_action(index);
    BindOverride self = bind_override_or_inherit(index);
    if (bind_effective(index).pad_codes[1 - slot] == code) {
        self.pad_codes[1 - slot] = -1;
    }
    self.pad_codes[slot] = static_cast<int16_t>(code);

    std::vector<std::string> cleared;
    for (int i = 0; i < total; ++i) {
        // Gameplay and menu-only actions read separate tables, so they never compete.
        if (i == index || gamepad_is_menu_only_action(i) != menu_only) {
            continue;
        }
        const BindOverride effective = bind_effective(i);
        BindOverride other = bind_override_or_inherit(i);
        bool changed = false;
        for (int j = 0; j < 2; ++j) {
            if (effective.pad_codes[j] == code) {
                other.pad_codes[j] = -1;
                changed = true;
            }
        }
        if (changed) {
            bind_context_set_override(g_bind_context, i, other);
            cleared.push_back(std::string{bind_action_name(i)});
        }
    }
    bind_context_set_override(g_bind_context, index, self);
    bind_note_changed();
    bind_toast_unbound(ctx, cleared);
}

void bind_pad_assign(UiContext& ctx, int index, int slot, int code)
{
    if (!bind_pad_live() || index < 0 || index >= bind_context_binding_count()) {
        return;
    }
    if (g_bind_context != BindContextId::Global) {
        bind_pad_assign_context(ctx, index, slot, code);
        return;
    }
    std::vector<std::string> cleared;
    const int displaced = gamepad_bind_action(index, slot, code);
    if (displaced >= 0) {
        cleared.push_back(std::string{bind_action_name(displaced)});
    }
    // The newest Global assignment stays exclusive inside every context too.
    const bool ctx_unbound = bind_context_dedupe_against_global();
    bind_note_changed();
    bind_toast_unbound(ctx, cleared);
    if (ctx_unbound) {
        ctx.push_toast("CONTEXT OVERRIDES UNBOUND");
    }
}

void bind_pad_clear(int index, int slot)
{
    if (!bind_pad_live() || index < 0 || index >= bind_context_binding_count()) {
        return;
    }
    if (g_bind_context != BindContextId::Global) {
        // Clearing an inherited cap is itself an override: "explicitly unbound here".
        BindOverride value = bind_override_or_inherit(index);
        value.pad_codes[slot] = -1;
        bind_context_set_override(g_bind_context, index, value);
    }
    else {
        gamepad_bind_action(index, slot, -1);
    }
    bind_note_changed();
}

// Not stock's reset, which leaves scan_codes[1] alone and cannot restore the Alpine rows.
void bindings_reset_defaults(UiContext& ctx)
{
    rf::ControlConfig* cc = bind_context_controls();
    const int total = bind_context_binding_count();
    if (!cc) {
        return;
    }
    for (int i = 0; i < total; ++i) {
        rf::ControlConfigItem& item = cc->bindings[i];
        item.scan_codes[0] = item.default_scan_codes[0];
        item.scan_codes[1] = item.default_scan_codes[1];
        item.mouse_btn_id = item.default_mouse_btn_id;
        item.aux_btn_id = item.default_aux_btn_id;
    }
    // What the ini load path guarantees after applying binds: first row wins, later duplicates
    // are unbound. The factory table is conflict free, so this only ever runs as a backstop.
    resolve_scan_code_conflicts(*cc);
    resolve_mouse_button_conflicts(*cc);
    if (bind_context_dedupe_against_global()) {
        ctx.push_toast("CONTEXT OVERRIDES UNBOUND");
    }
    bind_note_changed();
}

void bind_end_capture()
{
    if (g_capture.pad) {
        gamepad_bind_capture_end();
    }
    g_capture = BindCapture{};
}

void bind_begin_capture(UiContext& ctx, int index, bool mouse, UiId id, bool pad = false, int pad_slot = 0)
{
    g_capture.row = index;
    g_capture.mouse = mouse;
    g_capture.pad = pad;
    g_capture.pad_slot = pad_slot;
    g_capture.armed = false;
    g_capture.id = id;
    ctx.begin_key_capture(id);
    if (pad) {
        gamepad_bind_capture_begin();
    }
    // Anything already held - the Enter that activated the cap, a key held from gameplay - has
    // its press counted. Draining every counter makes the next real press the first one seen,
    // and leaves autorepeat words (which never bump the counter) at zero.
    for (int i = 0; i < 256; ++i) {
        rf::key_get_and_reset_down_counter(static_cast<rf::Key>(i));
    }
}

// Runs at the top of every hit-test pass, whether or not the capturing row is on screen, so a
// capture can never outlive the frame that resolved it.
void bind_poll_capture(UiContext& ctx)
{
    if (g_capture.row < 0) {
        return;
    }
    int scan = -1;
    int mouse = -1;
    bool cancelled = false;
    if (g_capture.pad && g_capture.armed) {
        int pad_code = -1;
        if (gamepad_bind_capture_take(pad_code)) {
            ctx.feed_capture(pad_code, -1);
        }
    }
    if (ctx.take_key_capture(g_capture.id, scan, mouse, cancelled)) {
        if (!cancelled) {
            if (g_capture.pad) {
                bind_pad_assign(ctx, g_capture.row, g_capture.pad_slot, scan);
            }
            else {
                bind_assign(ctx, g_capture.row, scan, mouse);
            }
        }
        bind_end_capture();
    }
    else if (ctx.key_capture_id() != g_capture.id) {
        // The kit dropped the capture (context reset, another widget): never keep waiting.
        bind_end_capture();
    }
}

// Key words during a capture, straight off the menu pump before UiContext ever sees them.
bool bind_handle_capture_key(int key)
{
    if (g_capture.row < 0) {
        return false;
    }
    const int code = key & rf::KEY_MASK; // chords cannot be bound: modifier bits are masked off
    if (code == rf::KEY_ESC) {
        g_ui.cancel_key_capture();
        return true;
    }
    if (g_capture.pad) {
        return true; // a controller capture ignores the keyboard
    }
    if (!g_capture.armed) {
        return true;
    }
    if (rf::key_get_and_reset_down_counter(static_cast<rf::Key>(code)) <= 0) {
        return true; // autorepeat: the ring repeats, the press counter does not
    }
    // Mouse 4+ arrive as key words and belong to the MOUSE cap; every other key to the KEY cap.
    if (g_capture.mouse != ctrl_is_extra_mouse_scan(code) || code == 0 || bind_scan_excluded(code)
        || bind_key_name(code).empty()) {
        // Stock plays the error sound and stops waiting; so do we, so no path can eat keys.
        ui_play_error();
        g_ui.cancel_key_capture();
        return true;
    }
    g_ui.feed_capture(code, -1);
    return true;
}

void bind_poll_capture_mouse(int wheel)
{
    if (g_capture.row < 0 || !g_capture.armed) {
        return;
    }
    if (!g_capture.mouse) {
        // Stock cancels a key capture when the mouse is clicked anywhere.
        if (rf::mouse_was_button_pressed(0) || rf::mouse_was_button_pressed(1)
            || rf::mouse_was_button_pressed(2)) {
            g_ui.cancel_key_capture();
        }
        return;
    }
    int btn = -1;
    for (int i = 0; i < 3 && btn < 0; ++i) {
        if (rf::mouse_was_button_pressed(i)) {
            btn = i;
        }
    }
    if (btn < 0) {
        constexpr int wheel_up_btn = 3;
        constexpr int wheel_down_btn = 4;
        btn = wheel > 0 ? wheel_up_btn : (wheel < 0 ? wheel_down_btn : -1);
    }
    if (btn < 0) {
        return;
    }
    if (bind_mouse_name(btn).empty()) {
        ui_play_error();
        g_ui.cancel_key_capture();
        return;
    }
    g_ui.feed_capture(-1, btn);
}

// ---------------------------------------------------------------------------
// Weapon autoswitch priority
// ---------------------------------------------------------------------------

// Stock's own visibility rule for the priority list: a slot counts when it names a real weapon
// whose display name is not empty and whose class carries WTF_PLAYER_WEP.
bool weapon_pref_visible(int weapon_id)
{
    if (weapon_id < 0 || weapon_id >= rf::num_weapon_types || weapon_id >= rf::max_weapon_types) {
        return false;
    }
    const rf::WeaponInfo& info = rf::weapon_types[weapon_id];
    const char* name = info.display_name.c_str();
    return name && *name && (info.flags & rf::WTF_PLAYER_WEP) != 0;
}

// Slots of Player::weapon_prefs that the list shows, in order.
std::vector<int> weapon_pref_slots()
{
    std::vector<int> slots;
    if (!rf::local_player) {
        return slots;
    }
    for (int i = 0; i < static_cast<int>(weapon_pref_count); ++i) {
        if (weapon_pref_visible(rf::local_player->weapon_prefs[i])) {
            slots.push_back(i);
        }
    }
    return slots;
}

bool weapon_prefs_at_baseline()
{
    return rf::local_player && g_weapon_prefs_baseline
        && std::equal(g_weapon_prefs_baseline->begin(), g_weapon_prefs_baseline->end(),
                      std::begin(rf::local_player->weapon_prefs));
}

void weapon_pref_move(int direction)
{
    if (!rf::local_player) {
        return;
    }
    const std::vector<int> slots = weapon_pref_slots();
    const auto it = std::find(slots.begin(), slots.end(), g_weapon_pref_sel);
    if (it == slots.end()) {
        return;
    }
    const auto index = static_cast<int>(std::distance(slots.begin(), it));
    const int target = index + direction;
    if (target < 0 || target >= static_cast<int>(slots.size())) {
        return;
    }
    std::swap(rf::local_player->weapon_prefs[slots[index]], rf::local_player->weapon_prefs[slots[target]]);
    g_weapon_pref_sel = slots[target];
    note_block_changed("weapon_prefs", weapon_prefs_at_baseline());
}

// ---------------------------------------------------------------------------
// Badges
// ---------------------------------------------------------------------------

struct Badge
{
    const char* text;
    rf::Color color;
    const char* icon;
    bool lamp;
};

// MP / SP / HOST say where a setting applies, not whether it can be edited.
int collect_badges(const SettingRow& row, Badge* out, int max_badges)
{
    int n = 0;
    const auto add = [&](const char* text, rf::Color color, const char* icon, bool lamp) {
        if (n < max_badges) {
            out[n++] = Badge{text, color, icon, lamp};
        }
    };
    // A read only row cannot be edited from here, so a timing chip would promise an apply that
    // this screen can never make; its description strip says where the value comes from instead.
    const bool read_only = (row.flags & SF_READ_ONLY) != 0;
    if ((row.flags & SF_RESTART_REQUIRED) && !read_only) {
        add("RESTART", skin::warn, nullptr, true);
    }
    if ((row.flags & SF_APPLIES_NEXT_LEVEL) && !read_only) {
        add("NEXT LEVEL", skin::badge_sp, nullptr, false);
    }
    if (row.flags & SF_LEGACY_RENDERER_ONLY) {
        add("LEGACY", skin::badge_d3d11, nullptr, false);
    }
    if (row.flags & SF_MP_ONLY) {
        add("MP", skin::badge_mp, nullptr, false);
    }
    if (row.flags & SF_SP_ONLY) {
        add("SP", skin::badge_sp, nullptr, false);
    }
    if (row.flags & SF_HOST_ONLY) {
        add("HOST", skin::badge_sp, nullptr, false);
    }
    if (row.flags & SF_ADVANCED) {
        add("ADV", skin::badge_d3d11, nullptr, false);
    }
    if (setting_server_permission_blocked(row)) {
        add("SERVER", skin::badge_server, "af_padlock_icon", false);
    }
    return n;
}

// ---------------------------------------------------------------------------
// Entries
// ---------------------------------------------------------------------------

enum class EntryKind
{
    SubHeader,  // subcategory or search group caption
    Row,        // one registry row
    BindHeader, // ACTION | KEY | MOUSE captions
    BindGroup,  // caption inside the binding list
    Bind,       // one binding row
    Reorder,    // the weapon autoswitch block: label row, list and its buttons
};

struct Entry
{
    EntryKind kind = EntryKind::Row;
    const char* header = nullptr;
    const SettingRow* row = nullptr;
    const char* crumb = nullptr;  // "Category > Subcategory", search results only
    int bind_index = -1;
    float ref_h = skin::row_h;    // reference-canvas height
};

constexpr float sub_header_ref_h = 40.0f;
constexpr float bind_header_ref_h = 30.0f;
constexpr float bind_group_ref_h = 34.0f;

float reorder_block_ref_h()
{
    // The floor keeps room for the Move up / Move down pair beside a very short list.
    const auto count = static_cast<int>(weapon_pref_slots().size());
    return skin::row_h + static_cast<float>(std::max(3, count)) * skin::list_row_h + 16.0f;
}

void push_binding_entries(std::vector<Entry>& entries)
{
    entries.push_back(Entry{EntryKind::BindHeader, nullptr, nullptr, nullptr, -1, bind_header_ref_h});
    for (const BindGroup& group : bind_groups()) {
        bool captioned = false;
        for (int i = group.first; i < group.last; ++i) {
            const char* name = bind_action_name(i);
            if (!name || !*name) {
                continue; // weapon slots the tbl never named have no usable row
            }
            if (!captioned) {
                entries.push_back(Entry{EntryKind::BindGroup, group.caption, nullptr, nullptr, -1, bind_group_ref_h});
                captioned = true;
            }
            entries.push_back(Entry{EntryKind::Bind, nullptr, nullptr, nullptr, i, skin::bind_row_h});
        }
    }
}

std::vector<Entry> build_category_entries(SettingCategory category)
{
    std::vector<const SettingRow*> rows;
    for (const SettingRow* row : settings_in_category(category)) {
        if (!setting_hidden_in_menu(*row)) {
            rows.push_back(row);
        }
    }
    // Group by subcategory without disturbing the order inside a group. Groups come out in the
    // order their subcategory first appears in the category's table, so the panel reads the way
    // the row table is authored rather than alphabetically.
    std::vector<std::string_view> first_seen;
    for (const SettingRow* row : rows) {
        const std::string_view sub{row->subcategory ? row->subcategory : ""};
        if (std::find(first_seen.begin(), first_seen.end(), sub) == first_seen.end()) {
            first_seen.push_back(sub);
        }
    }
    const auto group_order = [&first_seen](const SettingRow* row) {
        return std::find(first_seen.begin(), first_seen.end(),
                         std::string_view{row->subcategory ? row->subcategory : ""})
             - first_seen.begin();
    };
    std::stable_sort(rows.begin(), rows.end(), [&group_order](const SettingRow* a, const SettingRow* b) {
        return group_order(a) < group_order(b);
    });

    std::vector<Entry> entries;
    const char* last_sub = nullptr;
    for (const SettingRow* row : rows) {
        if (row->subcategory && *row->subcategory
            && (!last_sub || std::string_view{last_sub} != row->subcategory)) {
            last_sub = row->subcategory;
            entries.push_back(Entry{EntryKind::SubHeader, last_sub, nullptr, nullptr, -1, sub_header_ref_h});
        }
        const std::string_view id{row->id};
        // Two rows are whole blocks rather than a label plus one widget.
        if (id == "bindings" && bind_context_controls() != nullptr) {
            push_binding_entries(entries);
            continue;
        }
        if (id == "weapon_prefs" && rf::local_player != nullptr) {
            entries.push_back(Entry{EntryKind::Reorder, nullptr, row, nullptr, -1, reorder_block_ref_h()});
            continue;
        }
        entries.push_back(Entry{EntryKind::Row, nullptr, row, nullptr, -1, skin::row_h});
    }
    return entries;
}

// Every space-separated word of the query has to appear somewhere, in any order.
template<typename Fn>
bool all_words_match(std::string_view query, Fn&& word_matches)
{
    const std::vector<std::string_view> words = string_split(query);
    return !words.empty() && std::all_of(words.begin(), words.end(), word_matches);
}

bool row_matches(const SettingRow& row, std::string_view query)
{
    return all_words_match(query, [&](std::string_view word) {
        return string_icontains(row.label ? row.label : "", word)
            || string_icontains(row.tooltip ? row.tooltip : "", word)
            || string_icontains(row.id ? row.id : "", word)
            || string_icontains(row.key ? row.key : "", word)
            || string_icontains(row.console_cmd ? row.console_cmd : "", word)
            || string_icontains(row.subcategory ? row.subcategory : "", word)
            || string_icontains(setting_category_name(row.category), word);
    });
}

std::vector<Entry> build_search_entries(std::string_view query, std::vector<std::string>& crumbs,
                                        int& match_count)
{
    std::vector<Entry> entries;
    crumbs.clear();
    match_count = 0;
    // Reserving up front keeps the crumb strings stable while entries point at them.
    crumbs.reserve(settings_registry_all().size());
    // In column order, so the results read the way the categories are laid out.
    for (const SettingCategory category : category_order) {
        bool captioned = false;
        for (const SettingRow* row : settings_in_category(category)) {
            if ((row->flags & SF_HIDDEN) || !row_matches(*row, query)) {
                continue;
            }
            if (!captioned) {
                entries.push_back(Entry{EntryKind::SubHeader, setting_category_name(category),
                                        nullptr, nullptr, -1, sub_header_ref_h});
                captioned = true;
            }
            crumbs.push_back(row->subcategory && *row->subcategory
                                 ? std::format("{} > {}", setting_category_name(category),
                                               row->subcategory)
                                 : std::string{setting_category_name(category)});
            entries.push_back(Entry{EntryKind::Row, nullptr, row, crumbs.back().c_str(), -1,
                                    skin::row_h});
            ++match_count;
        }
    }
    // Actions are searchable by name too, editable right in the results.
    if (bind_rows_live()) {
        bool captioned = false;
        const int total = bind_context_binding_count();
        for (int i = 0; i < total; ++i) {
            const std::string_view name{bind_action_name(i)};
            const auto word_matches = [&](std::string_view word) { return string_icontains(name, word); };
            if (name.empty() || !all_words_match(query, word_matches)) {
                continue;
            }
            if (!captioned) {
                entries.push_back(Entry{EntryKind::SubHeader,
                                        g_bind_device == BindDevice::Controller ? "Controller bindings" : "Key bindings",
                                        nullptr, nullptr, -1, sub_header_ref_h});
                captioned = true;
            }
            entries.push_back(Entry{EntryKind::Bind, nullptr, nullptr, nullptr, i, skin::bind_row_h});
            ++match_count;
        }
    }
    return entries;
}

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

SettingRow slider_presentation(const SettingRow& row)
{
    SettingRow presentation = row;
    if (row.ui_adjust) {
        row.ui_adjust(presentation);
    }
    return presentation;
}

int row_slider_readout_w(const UiContext& ctx, const SettingRow& row, int font)
{
    if (row.widget != SettingWidget::Slider) {
        return 0;
    }
    const SettingRow presentation = slider_presentation(row);
    float lo = 0.0f;
    float hi = 0.0f;
    float step = 0.0f;
    if (!setting_ui_range(presentation, lo, hi, step)) {
        return 0;
    }
    return skin_slider_readout_w(ctx, lo, hi, row_number_format(presentation), font);
}

// One readout width for every slider on the panel, so their tracks line up in a column. A category
// measures its advanced rows even while they are hidden, so toggling them does not move the column.
int panel_slider_readout_w(const UiContext& ctx, const std::vector<Entry>& entries,
                           std::optional<SettingCategory> category, int font)
{
    int out = 0;
    if (!category) {
        for (const Entry& entry : entries) {
            if (entry.kind == EntryKind::Row) {
                out = std::max(out, row_slider_readout_w(ctx, *entry.row, font));
            }
        }
    }
    else {
        for (const SettingRow* row : settings_in_category(*category)) {
            if (!(row->flags & SF_HIDDEN)) {
                out = std::max(out, row_slider_readout_w(ctx, *row, font));
            }
        }
    }
    return out;
}

UiRect right_rect(const UiContext& ctx, int right, const UiRect& row, float w_ref, float h_ref)
{
    const int w = skin_px(ctx, w_ref);
    const int h = h_ref > 0.0f ? skin_px(ctx, h_ref) : row.h;
    return UiRect{right - w, row.y + (row.h - h) / 2, w, h};
}

// Writes a numeric widget's edit through once the hit-test pass has run it.
void commit_row_float(const UiContext& ctx, const SettingRow& row, RowState& state, bool enabled, bool integral)
{
    if (!ctx.draw && enabled && state.f != state.stored_f) {
        write_row(row, format_row_value(state.f, integral));
        state.stored_f = state.f;
    }
}

// A picture index (spray, character) is picked in the modal grid, not from a list of labels; the
// row's box names the current pick and opens the grid.
void do_picker_row_widget(UiContext& ctx, const SettingRow& row, const GridPicker& picker, const UiRect& row_rect,
                          int ctl_right, const Fonts& fonts, bool enabled, UiId id, bool hot)
{
    const UiRect r = right_rect(ctx, ctl_right, row_rect, skin::control_w, skin::widget_h);
    const int index = static_cast<int>(std::lround(row_float(row, -1.0f)));
    const char* caption = index >= 0 && index < picker.count() ? picker.caption(index) : nullptr;
    if (skin_select_box(ctx, r, caption && caption[0] != '\0' ? caption : "-", fonts.body, enabled, id, hot)) {
        g_picker = &picker;
        g_picker_row = &row;
        g_picker_scroll = 0.0f;
        g_modal = Modal::GridPicker;
        ui_play_click();
    }
}

struct ActionChoices
{
    const rf::ControlConfig* controls = nullptr;
    int count = -1;
    std::vector<std::string> items;
    std::vector<int> actions; // per item, -1 for None
};
ActionChoices g_action_choices;

const ActionChoices& action_choices()
{
    const rf::ControlConfig* cc = bind_context_controls();
    const int count = bind_context_binding_count();
    if (g_action_choices.controls != cc || g_action_choices.count != count) {
        g_action_choices = ActionChoices{cc, count, {"None"}, {-1}};
        for (int i = 0; i < count; ++i) {
            const char* name = bind_action_name(i);
            if (*name) {
                g_action_choices.items.emplace_back(name);
                g_action_choices.actions.push_back(i);
            }
        }
    }
    return g_action_choices;
}

// An action index, chosen by name from the live binding table, or None.
void do_action_choice_widget(UiContext& ctx, const SettingRow& row, const UiRect& row_rect, int ctl_right, int font,
                             bool enabled, UiId id, bool hot)
{
    const ActionChoices& choices = action_choices();
    const int value = static_cast<int>(std::lround(row_float(row, -1.0f)));
    const auto found = std::find(choices.actions.begin(), choices.actions.end(), value);
    const int pos = found != choices.actions.end() ? static_cast<int>(found - choices.actions.begin()) : -1;
    int sel = pos;
    const UiRect r = right_rect(ctx, ctl_right, row_rect, skin::control_w, skin::widget_h);
    if (skin_dropdown(ctx, r, choices.items, sel, font, enabled, id, hot) && enabled && sel != pos) {
        write_row(row, std::to_string(choices.actions[sel]));
    }
}

// Most enums with at most this many offered values cycle in place.
constexpr int cycler_max_options = 4;

bool is_controller_debug_action(std::string_view action)
{
    return action == "gyro_reset_partial" || action == "gyro_reset_full" || action == "joy_reset";
}

void do_row_widget(UiContext& ctx, const SettingRow& row, const UiRect& row_rect, int ctl_right,
                   const Fonts& fonts, int slider_readout_w, bool enabled, bool sync_pass, bool hot)
{
    const UiId id = ui_id(row.id);
    // A row first seen in a pass that may not sync (the search box holds the text focus while
    // results appear) must still start from the stored value, not a zeroed state.
    const auto [state_it, first_seen] = g_row_state.try_emplace(&row);
    RowState& state = state_it->second;
    const bool sync_state = sync_pass || first_seen;
    const int font = fonts.body;

    switch (row.widget) {
        case SettingWidget::Toggle: {
            const bool stored = setting_get_as_string(row) == "1";
            const bool inverted = (row.flags & SF_INVERTED) != 0;
            const bool shown = inverted ? !stored : stored;
            const UiRect r = right_rect(ctx, ctl_right, row_rect, skin::control_w, skin::widget_h);
            if (skin_cycler(ctx, r, shown ? "On" : "Off", font, enabled, id, hot) != 0) {
                write_row(row, (inverted ? shown : !shown) ? "1" : "0");
                ui_play_toggle(!shown);
            }
            break;
        }
        case SettingWidget::Slider: {
            const SettingRow presentation = slider_presentation(row);
            float lo = 0.0f;
            float hi = 1.0f;
            float step = 0.0f;
            const bool ranged = setting_ui_range(presentation, lo, hi, step);
            if (sync_state && !state.held) {
                state.f = row_float(row, lo);
                state.stored_f = state.f;
            }
            const bool integral = is_integral_row(row);
            const UiNumberFormat fmt = row_number_format(presentation);
            if (ranged) {
                const int w = skin_px(ctx, skin::control_w);
                const UiRect r{ctl_right - w, row_rect.y, w, row_rect.h};
                skin_slider(ctx, r, state.f, lo, hi, integral && step <= 0.0f ? 1.0f : step, presentation.ui_nudge,
                            fmt, font, slider_readout_w, enabled, id, hot);
            }
            else {
                // No bounded range to draw a track for.
                const UiRect r = right_rect(ctx, ctl_right, row_rect, skin::control_w, skin::widget_h);
                skin_stepper(ctx, r, state.f, -1.0e9f, 1.0e9f, 1.0f, fmt, font, enabled, id, integral, hot);
            }
            if ((row.flags & SF_COMMIT_ON_RELEASE) && ctx.captured_id() == id) {
                state.held = true;
            }
            else {
                commit_row_float(ctx, row, state, enabled, integral);
            }
            break;
        }
        case SettingWidget::Stepper: {
            float lo = -1.0e9f;
            float hi = 1.0e9f;
            float step = 1.0f;
            setting_ui_range(row, lo, hi, step);
            const bool integral = is_integral_row(row);
            if (sync_state) {
                state.f = row_float(row, 0.0f);
                state.stored_f = state.f;
            }
            const UiRect r = right_rect(ctx, ctl_right, row_rect, skin::control_w, skin::widget_h);
            skin_stepper(ctx, r, state.f, lo, hi, step > 0.0f ? step : 1.0f, row_number_format(row),
                         font, enabled, id, integral, hot);
            commit_row_float(ctx, row, state, enabled, integral);
            break;
        }
        case SettingWidget::Enum: {
            if (const GridPicker* picker = picker_for_row(row)) {
                do_picker_row_widget(ctx, row, *picker, row_rect, ctl_right, fonts, enabled, id, hot);
                break;
            }
            if (row.flags & SF_ACTION_CHOICE) {
                do_action_choice_widget(ctx, row, row_rect, ctl_right, font, enabled, id, hot);
                break;
            }
            if (!row.enum_labels || row.enum_count <= 0) {
                if (sync_state) {
                    state.f = row_float(row, 0.0f);
                    state.stored_f = state.f;
                }
                float lo = -1.0e9f;
                float hi = 1.0e9f;
                float step = 1.0f;
                setting_ui_range(row, lo, hi, step);
                const UiRect r = right_rect(ctx, ctl_right, row_rect, skin::control_w, skin::widget_h);
                skin_stepper(ctx, r, state.f, lo, hi, 1.0f, UiNumberFormat{}, font, enabled, id, true, hot);
                commit_row_float(ctx, row, state, enabled, true);
                break;
            }
            // A row may hide values the hardware cannot do (MSAA levels), so the widget walks
            // the offered subset rather than the raw label array.
            const std::vector<int> offered = setting_enum_offered(row);
            const int count = static_cast<int>(offered.size());
            const int value = static_cast<int>(std::lround(row_float(row,
                static_cast<float>(setting_enum_value(row, offered[0])))));
            int pos = -1;
            for (int i = 0; i < count; ++i) {
                if (setting_enum_value(row, offered[i]) == value) {
                    pos = i;
                    break;
                }
            }
            if (count <= cycler_max_options && pos >= 0
                && std::string_view{row.id} != "gamepad_gyro_autocalibration"
                && std::string_view{row.id} != "gamepad_gyro_ratcheting") {
                const UiRect r = right_rect(ctx, ctl_right, row_rect, skin::control_w, skin::widget_h);
                const int delta = skin_cycler(ctx, r, row.enum_labels[offered[pos]], font, enabled, id, hot);
                if (delta != 0 && enabled) {
                    pos = (pos + delta + count) % count;
                    write_row(row, std::to_string(setting_enum_value(row, offered[pos])));
                    ui_play_click();
                }
            }
            else {
                std::vector<std::string> items;
                items.reserve(count);
                for (const int i : offered) {
                    items.emplace_back(row.enum_labels[i]);
                }
                const UiRect r = right_rect(ctx, ctl_right, row_rect, skin::control_w, skin::widget_h);
                int sel = pos;
                if (skin_dropdown(ctx, r, items, sel, font, enabled, id, hot) && enabled && sel != pos) {
                    write_row(row, std::to_string(setting_enum_value(row, offered[sel])));
                }
            }
            break;
        }
        case SettingWidget::Color:
        case SettingWidget::OptionalColor: {
            if (sync_state) {
                state.color = parse_hex_color_string(setting_get_as_string(row));
                state.stored_color = state.color;
            }
            // An optional colour is cleared back to "no override" by the row's reset icon.
            const UiRect r = right_rect(ctx, ctl_right, row_rect, skin::control_w, 0.0f);
            if (skin_swatch(ctx, r, state.color, fonts.caption, enabled, id, hot)) {
                // An optional colour with no override has nothing to start from; white is neutral.
                const uint32_t start = state.color.value_or(0xFFFFFFFFu);
                g_color_pick = ColorPick{&row, state.color.has_value(), !state.color.has_value(), start, start,
                                         skin_rgba_to_hsva(start),
                                         state.color ? format_hex_color_string(start) : std::string{}};
                g_modal = Modal::ColorPicker;
                ui_play_click();
            }
            if (!ctx.draw && enabled && state.color != state.stored_color) {
                state.stored_color = state.color;
                if (state.color) {
                    write_row(row, format_hex_color_string(*state.color));
                }
            }
            break;
        }
        case SettingWidget::OptionalScale: {
            // The stored 1.0 sentinel means "no override"; the row's reset icon clears it.
            if (sync_state) {
                state.f = row_float(row, 1.0f);
                state.stored_f = state.f;
            }
            const UiRect r = right_rect(ctx, ctl_right, row_rect, skin::control_w, skin::widget_h);
            skin_stepper(ctx, r, state.f, 0.1f, 10.0f, 0.05f, UiNumberFormat{2, 1.0f, ""},
                         font, enabled, id, false, hot);
            commit_row_float(ctx, row, state, enabled, false);
            break;
        }
        case SettingWidget::Text: {
            if (sync_state) {
                state.text = setting_get_as_string(row);
            }
            // The buffer is already up to date here: UiContext::apply_pending_keys ran at the top
            // of the pass, so the validation below sees exactly what the field will draw.
            const TextRowConfig cfg = text_row_config(row, state.text);
            const UiRect r = right_rect(ctx, ctl_right, row_rect, skin::control_w, 32.0f);
            skin_field(ctx, r, id, state.text, cfg.max_len, cfg.filter, cfg.hint, fonts.body, enabled,
                       enabled && cfg.warn != nullptr, cfg.warn);
            // The value is committed when the box releases the focus (Enter, click away, the row
            // scrolling out of the pass), never per keystroke: player_name commits through the
            // engine and notifies the server.
            if (!ctx.draw && enabled && ctx.focused_text_id() == id && g_prev_text_focus != id) {
                const SettingRow* target = &row;
                RowState* buffer = &state;
                ctx.set_text_commit_callback([target, buffer](bool commit) {
                    if (!commit) {
                        return;
                    }
                    if (text_row_config(*target, buffer->text).warn == nullptr) {
                        write_row(*target, buffer->text);
                    }
                });
            }
            break;
        }
        case SettingWidget::Action: {
            const std::string_view action{row.id};
            if (action == "bindings_reset") {
                // The control column's width, or the label plus the danger button's LED if wider.
                const int w = skin_button_width(ctx, "RESET BINDINGS", fonts.body_bold, skin::control_w - 30.0f)
                            + skin_px(ctx, 30.0f);
                const int h = skin_px(ctx, 44.0f);
                const UiRect r{ctl_right - w, row_rect.y + (row_rect.h - h) / 2, w, h};
                if (skin_button(ctx, r, "RESET BINDINGS", fonts.body_bold, SkinButton::Danger,
                                enabled && bind_rows_live(), id)) {
                    g_modal = Modal::ResetBindings;
                    ui_play_click();
                }
            }
            else if (is_controller_debug_action(action)) {
                const UiRect r = right_rect(ctx, ctl_right, row_rect, skin::control_w, 44.0f);
                if (skin_button(ctx, r, "RESET", fonts.body_bold, SkinButton::Secondary, enabled, id)) {
                    if (action == "gyro_reset_partial") {
                        rf::console::do_command("gyro_reset partial");
                    }
                    else if (action == "gyro_reset_full") {
                        rf::console::do_command("gyro_reset full");
                    }
                    else {
                        rf::console::do_command("joy_reset");
                    }
                    ui_play_click();
                }
            }
            break;
        }
        case SettingWidget::Label: {
            if (ctx.draw) {
                const UiRect d = ui_draw_rect(ctx, row_rect);
                rf::gr::set_color(hot ? skin::ink_title : skin::ink_status);
                rf::gr::string_aligned(rf::gr::ALIGN_RIGHT,
                    d.x + (ctl_right - row_rect.x),
                    d.y + (d.h - rf::gr::get_font_height(font)) / 2,
                    setting_get_as_string(row).c_str(), font);
            }
            break;
        }
        case SettingWidget::Keybind:
        case SettingWidget::ListReorder: {
            // Both expand into a block of their own inside their category; a search hit just
            // offers the jump.
            const UiRect r = right_rect(ctx, ctl_right, row_rect, skin::control_w, 44.0f);
            if (skin_button(ctx, r, "OPEN", fonts.body_bold, SkinButton::Secondary, enabled, id)) {
                g_switch_to = row.category;
                ui_play_click();
            }
            break;
        }
    }
}

bool row_widget_enabled(const SettingRow& row)
{
    // Keybind / ListReorder / Action rows have no accessor: their gates alone decide.
    const bool bespoke_widget = row.widget == SettingWidget::Action
        || row.widget == SettingWidget::Keybind || row.widget == SettingWidget::ListReorder;
    return setting_disabled_reason(row) == nullptr && (bespoke_widget || setting_has_setter(row));
}

// The keyboard-focused row carries the stock list selection, faded out well before the controls.
// Any other row sits over a ridge, as stock's panel rows do.
void draw_row_backing(const UiContext& ctx, const UiRect& d, bool focused)
{
    if (focused) {
        const int inset = skin_px(ctx, 3.0f);
        skin_selection_bar(ctx, d.x, d.y + inset, std::min(d.w, skin_px(ctx, 900.0f)), d.h - 2 * inset);
        return;
    }
    const int pad = skin_px(ctx, row_inset);
    skin_ridge(ctx, d.x + pad / 2, d.y + d.h - std::max(1, skin_px(ctx, 1.0f)), d.w - pad);
}

// A row label: dark ink on the steel, white on the focused row's selection glow.
rf::Color row_label_color(bool enabled, bool hot, bool focused)
{
    if (!enabled) {
        return skin::ink_dim;
    }
    if (focused) {
        return skin::txt_hi;
    }
    return hot ? skin::ink_title : skin::ink_status;
}

void do_row(UiContext& ctx, const SettingRow& row, const UiRect& row_rect, const char* crumb,
            const Fonts& fonts, int slider_readout_w, bool sync_state)
{
    const bool enabled = row_widget_enabled(row);
    const bool modified = setting_is_modified_from_default(row);
    const UiId id = ui_id(row.id);
    const int pad = skin_px(ctx, row_inset);
    const int gap = skin_px(ctx, 12.0f);
    const int reset_w = skin_px(ctx, skin::icon_size);

    const bool hovered = ui_hover(ctx, row_rect);
    const bool focused = ctx.focus_visible() && ctx.focus_id() == id;
    if (!ctx.draw) {
        if (hovered || focused) {
            ctx.description().hover(desc_id_for(row));
        }
        // A click anywhere on the row pins its description until the strip moves on.
        if (hovered && ctx.click) {
            ctx.description().pin(desc_id_for(row));
        }
    }

    if (ctx.draw) {
        const UiRect d = ui_draw_rect(ctx, row_rect);
        draw_row_backing(ctx, d, focused);

        int tx = d.x + pad;
        if (modified) {
            // In the margin, so the label stays in line with the others; clear of the focus LED.
            const int dot = skin_px(ctx, 8.0f);
            ui_rounded_rect(static_cast<float>(tx - skin_px(ctx, 6.0f) - dot),
                            static_cast<float>(d.y + (d.h - dot) / 2), static_cast<float>(dot),
                            static_cast<float>(dot), static_cast<float>(dot) * 0.5f, skin::amber);
        }
        // Search results stack the label over a "Category > Subcategory" crumb.
        const int label_h = rf::gr::get_font_height(fonts.body);
        const int label_y = crumb ? d.y + d.h / 2 - label_h - skin_px(ctx, 2.0f)
                                  : d.y + (d.h - label_h) / 2;
        rf::gr::set_color(row_label_color(enabled, hovered || modified, focused));
        rf::gr::string(tx, label_y, row.label, fonts.body);
        if (crumb) {
            rf::gr::set_color(focused ? skin::txt : skin::ink_hint);
            rf::gr::string(tx, d.y + d.h / 2 + skin_px(ctx, 2.0f), crumb, fonts.caption);
        }
        tx += rf::gr::get_string_size(row.label, fonts.body).first + gap;

        Badge badges[8];
        const int badge_count = collect_badges(row, badges, 8);
        const int badge_h = skin_px(ctx, skin::badge_h);
        // Beside the label: centred on the row, or on the label line when a crumb sits under it.
        const int badge_y = crumb ? row_rect.y + (label_y - d.y) + (label_h - badge_h) / 2
                                  : row_rect.y + (row_rect.h - badge_h) / 2;
        for (int i = 0; i < badge_count; ++i) {
            const int w = skin_badge_width(ctx, badges[i].text, fonts.caption, badges[i].lamp
                                                                                || badges[i].icon);
            const UiRect br{row_rect.x + (tx - d.x), badge_y, w, badge_h};
            skin_badge(ctx, br, badges[i].text, badges[i].color, fonts.caption, badges[i].icon,
                       badges[i].lamp);
            tx += w + skin_px(ctx, 8.0f);
        }
    }

    const int ctl_right = row_rect.x + row_rect.w - pad;
    const UiRect reset_rect{ctl_right - reset_w, row_rect.y + (row_rect.h - reset_w) / 2, reset_w,
                            reset_w};
    if (modified && enabled) {
        if (skin_icon_button(ctx, reset_rect, "af_reset_icon", skin::amber, skin::ink_title,
                             ui_id("rowreset", desc_id_for(row)))) {
            setting_reset_to_default(row);
            note_rows_changed();
            ui_play_click();
        }
    }

    do_row_widget(ctx, row, row_rect, ctl_right - reset_w - gap, fonts, slider_readout_w, enabled, sync_state,
                  hovered || focused);
}

// ---------------------------------------------------------------------------
// Bindings blocks
// ---------------------------------------------------------------------------

// The pad and dot put the selected tab's LED where skin_selection_bar lights it.
constexpr float strip_tab_dot = 10.0f;
constexpr float strip_tab_pad = 7.0f;
constexpr float strip_tab_gap = 10.0f;

// Everything in a tab that is not the label. Measurement and draw must round the four terms the
// same way or the label lands a pixel short of its measured width and gets ellipsised.
int strip_tab_chrome(const UiContext& ctx)
{
    return skin_px(ctx, strip_tab_pad) + skin_px(ctx, strip_tab_dot) + skin_px(ctx, strip_tab_gap)
        + skin_px(ctx, strip_tab_pad);
}

// The active tab's LED is always reserved, so selecting a tab never resizes the strip.
int strip_tab_width(const UiContext& ctx, const char* label, int font)
{
    return rf::gr::get_string_size(std::string_view{label}, font).first + strip_tab_chrome(ctx);
}

struct StripTab
{
    const char* label;
    const char* description;
};

// Returns the tab clicked this pass, or -1. `widths` holds one measured width per tab and must
// sum to r.w.
int do_segment_strip(UiContext& ctx, const UiRect& r, const StripTab* tabs, const int* widths,
                     int count, int active, const char* id_key, int font)
{
    int clicked = -1;

    if (ctx.draw) {
        const UiRect d = ui_draw_rect(ctx, r);
        skin_slot(ctx, d.x, d.y, d.w, d.h);
    }

    int x = r.x;
    for (int i = 0; i < count; ++i) {
        const UiRect tab{x, r.y, widths[i], r.h};
        x += widths[i];
        if (widths[i] <= 0) {
            // A zero-width tab cannot be hit or read; registering it would only trap the focus.
            continue;
        }
        const UiId id = ui_id(id_key, i);
        const bool hovered = ui_hover(ctx, tab, id);
        const bool is_active = i == active;

        if (!ctx.draw) {
            if (hovered) {
                g_desc_custom = tabs[i].description;
            }
            ctx.register_focusable(id, UiFocusKind::Activate);
            if (ctx.consume_activation(id) || (hovered && ctx.click)) {
                clicked = i;
            }
            continue;
        }

        const UiRect d = ui_draw_rect(ctx, tab);
        if (i > 0) {
            rf::gr::set_color(skin::field_edge);
            rf::gr::rect(d.x, d.y, std::max(1, skin_px(ctx, 1.0f)), d.h);
        }
        if (is_active) {
            const int inset = skin_px(ctx, 2.0f);
            skin_selection_bar(ctx, d.x, d.y + inset, d.w, d.h - 2 * inset);
        }
        const int tx = d.x + skin_px(ctx, strip_tab_pad) + skin_px(ctx, strip_tab_dot) + skin_px(ctx, strip_tab_gap);
        const int label_w = std::max(0, d.w - strip_tab_chrome(ctx));
        rf::gr::set_color(is_active || hovered ? skin::txt_hi : skin::txt);
        rf::gr::string(tx, d.y + (d.h - rf::gr::get_font_height(font)) / 2,
                       ui_fit_middle(tabs[i].label, label_w, font).c_str(), font);
        if (ctx.focus_visible() && ctx.focus_id() == id) {
            skin_focus_ring(ctx, d.x, d.y, d.w, d.h);
        }
    }
    return clicked;
}

void bind_columns(const UiContext& ctx, const UiRect& r, int& key_x, int& mouse_x, int& col_w)
{
    const int pad = skin_px(ctx, row_inset);
    col_w = skin_px(ctx, skin::bind_col_w);
    mouse_x = r.x + r.w - pad - col_w;
    key_x = mouse_x - col_w;
}

void draw_bind_header(const UiContext& ctx, const UiRect& r, const Fonts& fonts)
{
    if (!ctx.draw) {
        return;
    }
    int key_x = 0;
    int mouse_x = 0;
    int col_w = 0;
    bind_columns(ctx, r, key_x, mouse_x, col_w);
    const UiRect d = ui_draw_rect(ctx, r);
    const int dy = d.y + (d.h - rf::gr::get_font_height(fonts.caption)) / 2;
    rf::gr::set_color(skin::ink_hint);
    const char* col1 = "KEY";
    const char* col2 = "MOUSE";
    if (g_bind_device == BindDevice::Controller) {
        col1 = "BUTTON";
        col2 = "ALT";
    }
    else if (g_bind_device == BindDevice::VR) {
        col1 = "LEFT HAND";
        col2 = "RIGHT HAND";
    }
    rf::gr::string(d.x + skin_px(ctx, row_inset), dy, "ACTION", fonts.caption);
    rf::gr::string(d.x + (key_x - r.x), dy, col1, fonts.caption);
    rf::gr::string(d.x + (mouse_x - r.x), dy, col2, fonts.caption);
    rf::gr::set_color(skin::border_dim);
    rf::gr::rect(d.x + skin_px(ctx, row_inset), d.y + d.h - std::max(1, skin_px(ctx, 1.0f)),
                 d.w - 2 * skin_px(ctx, row_inset), std::max(1, skin_px(ctx, 1.0f)));
}

void draw_sub_header(const UiContext& ctx, const UiRect& r, const char* header, const Fonts& fonts)
{
    if (!ctx.draw) {
        return;
    }
    const UiRect d = ui_draw_rect(ctx, r);
    skin_led(ctx, d.x + skin_px(ctx, 12.0f), d.y + d.h / 2, 7.0f, 22.0f, true);
    rf::gr::set_color(skin::ink_title);
    rf::gr::string(d.x + skin_px(ctx, row_inset), d.y + (d.h - rf::gr::get_font_height(fonts.body_bold)) / 2,
                   string_to_upper(header ? header : "").c_str(), fonts.body_bold);
}

// Context override affordance: gold chip plus the revert arrow, which drops the override of the
// shown device and puts the row back on its Global binding.
void do_bind_override_badge(UiContext& ctx, const UiRect& r, int key_x, int index, const Fonts& fonts)
{
    if (!bind_is_override(index)) {
        return;
    }
    const int badge_h = skin_px(ctx, skin::badge_h);
    const int badge_w = skin_badge_width(ctx, "OVERRIDE", fonts.caption, false);
    const int icon = skin_px(ctx, skin::icon_size);
    const UiRect badge{key_x - badge_w - icon - skin_px(ctx, 20.0f),
                       r.y + (r.h - badge_h) / 2, badge_w, badge_h};
    skin_badge(ctx, badge, "OVERRIDE", skin::badge_override, fonts.caption, nullptr, false);
    const UiRect revert{badge.x + badge_w + skin_px(ctx, 6.0f), r.y + (r.h - icon) / 2, icon, icon};
    if (skin_icon_button(ctx, revert, "af_revert_icon", skin::amber, skin::ink_title,
                         ui_id("bindrevert", index))) {
        // An all-inherit value drops the override, so only the other device's fields can survive.
        BindOverride value = bind_override_or_inherit(index);
        if (bind_pad_live()) {
            value.pad_codes[0] = value.pad_codes[1] = bind_inherit;
        }
        else {
            value.scan_codes[0] = value.scan_codes[1] = value.mouse_btn_id = bind_inherit;
        }
        bind_context_set_override(g_bind_context, index, value);
        bind_note_changed();
        ui_play_click();
    }
}

// A controller row: BUTTON and ALT each hold one button or trigger.
void do_pad_bind_row(UiContext& ctx, const UiRect& r, int index, const Fonts& fonts)
{
    const int inset = skin_px(ctx, row_inset);
    int key_x = 0;
    int mouse_x = 0;
    int col_w = 0;
    bind_columns(ctx, r, key_x, mouse_x, col_w);
    const int cap_w = skin_px(ctx, skin::keycap_w);
    const int cap_h = skin_px(ctx, skin::keycap_h);
    const int cap_y = r.y + (r.h - cap_h) / 2;

    const bool live = bind_pad_live();
    const bool capturing_main = live && g_capture.row == index && g_capture.pad && g_capture.pad_slot == 0;
    const bool capturing_alt = live && g_capture.row == index && g_capture.pad && g_capture.pad_slot == 1;
    // While one row listens, no other cap may start a second capture.
    const bool enabled = live && (g_capture.row < 0 || g_capture.row == index);

    const bool hovered = ui_hover(ctx, r);
    const bool focused = ctx.focus_visible()
        && (ctx.focus_id() == ui_id("bindkey", index) || ctx.focus_id() == ui_id("bindmouse", index));
    if (!ctx.draw && hovered) {
        g_desc_custom = std::format(
            "Click a cap, then press a controller button or trigger. ESC Key or {} Button cancels.",
            gamepad_get_scan_code_name(CTRL_GAMEPAD_SCAN_BASE + SDL_GAMEPAD_BUTTON_START));
    }

    if (ctx.draw) {
        const UiRect d = ui_draw_rect(ctx, r);
        draw_row_backing(ctx, d, focused);
        rf::gr::set_color(row_label_color(true, hovered, focused));
        const int label_w = std::max(0, (key_x - r.x) - inset - skin_px(ctx, 12.0f));
        rf::gr::string(d.x + inset, d.y + (d.h - rf::gr::get_font_height(fonts.body)) / 2,
                       ui_fit_middle(bind_action_name(index), label_w, fonts.body).c_str(), fonts.body);
    }

    do_bind_override_badge(ctx, r, key_x, index, fonts);

    // A cap the context does not override shows the Global value dimmed; inheritance is per cap.
    const BindOverride* override_value = g_bind_context != BindContextId::Global
        ? bind_context_override(g_bind_context, index) : nullptr;
    const BindOverride values = bind_effective(index);
    std::string texts[2];
    bool bound[2];
    bool inherited[2];
    for (int slot = 0; slot < 2; ++slot) {
        const int code = values.pad_codes[slot];
        const std::string name = code >= 0 ? std::string{gamepad_get_scan_code_name(code)} : std::string{};
        bound[slot] = !name.empty();
        inherited[slot] = g_bind_context != BindContextId::Global
            && (!override_value || override_value->pad_codes[slot] == bind_inherit);
        const std::string shown = name.empty() ? std::string{"-"} : name;
        texts[slot] = inherited[slot] ? std::format("Global: {}", shown) : shown;
    }
    // A listening cap takes both columns and the sibling stands down.
    const int span_w = mouse_x + col_w - key_x;

    if (!capturing_alt) {
        const UiRect key_rect{key_x, cap_y, capturing_main ? span_w : cap_w, cap_h};
        const SkinKeyCapResult key_cap = skin_key_cap(ctx, key_rect,
            capturing_main ? "PRESS A BUTTON..." : texts[0].c_str(), bound[0], capturing_main, enabled,
            ui_id("bindkey", index), fonts.body, inherited[0]);
        if (key_cap.activated) {
            bind_begin_capture(ctx, index, false, ui_id("bindkey", index), true, 0);
            ui_play_click();
        }
        else if (key_cap.cleared) {
            bind_pad_clear(index, 0);
            ui_play_click();
        }
    }

    if (!capturing_main) {
        const UiRect alt_rect{capturing_alt ? key_x : mouse_x, cap_y, capturing_alt ? span_w : cap_w, cap_h};
        const SkinKeyCapResult alt_cap = skin_key_cap(ctx, alt_rect,
            capturing_alt ? "PRESS A BUTTON..." : texts[1].c_str(), bound[1], capturing_alt, enabled,
            ui_id("bindmouse", index), fonts.body, inherited[1]);
        if (alt_cap.activated) {
            bind_begin_capture(ctx, index, false, ui_id("bindmouse", index), true, 1);
            ui_play_click();
        }
        else if (alt_cap.cleared) {
            bind_pad_clear(index, 1);
            ui_play_click();
        }
    }
}

void do_bind_row(UiContext& ctx, const UiRect& r, int index, const Fonts& fonts)
{
    rf::ControlConfig* cc = bind_context_controls();
    if (!cc || index < 0 || index >= bind_context_binding_count()) {
        return;
    }
    if (g_bind_device == BindDevice::Controller) {
        do_pad_bind_row(ctx, r, index, fonts);
        return;
    }
    const int pad = skin_px(ctx, row_inset);
    int key_x = 0;
    int mouse_x = 0;
    int col_w = 0;
    bind_columns(ctx, r, key_x, mouse_x, col_w);
    const int cap_w = skin_px(ctx, skin::keycap_w);
    const int cap_h = skin_px(ctx, skin::keycap_h);
    const int cap_y = r.y + (r.h - cap_h) / 2;

    const bool live = bind_device_live();
    const bool capturing_key = live && g_capture.row == index && !g_capture.mouse;
    const bool capturing_mouse = live && g_capture.row == index && g_capture.mouse;
    // While one row listens, no other cap may start a second capture.
    const bool enabled = live && (g_capture.row < 0 || g_capture.row == index);

    const bool hovered = ui_hover(ctx, r);
    const bool focused = ctx.focus_visible()
        && (ctx.focus_id() == ui_id("bindkey", index) || ctx.focus_id() == ui_id("bindmouse", index));
    if (!ctx.draw && hovered) {
        g_desc_custom = live
            ? "Click a cap to rebind it. ESC cancels; the x or a right click unbinds."
            : bind_device_description(g_bind_device);
    }

    if (ctx.draw) {
        const UiRect d = ui_draw_rect(ctx, r);
        draw_row_backing(ctx, d, focused);
        rf::gr::set_color(row_label_color(true, hovered, focused));
        const int label_w = std::max(0, (key_x - r.x) - pad - skin_px(ctx, 12.0f));
        rf::gr::string(d.x + pad, d.y + (d.h - rf::gr::get_font_height(fonts.body)) / 2,
                       ui_fit_middle(bind_action_name(index), label_w, fonts.body).c_str(),
                       fonts.body);
    }

    // Context override affordance: gold chip plus the revert arrow, which drops the override and
    // puts the row back on its Global binding.
    do_bind_override_badge(ctx, r, key_x, index, fonts);

    // A cap the context does not override shows the Global value dimmed; the non-keyboard devices
    // have nothing to show at all. Inheritance is per field, so one cap can be overridden while the
    // other still follows Global.
    const BindOverride* override_value = live && g_bind_context != BindContextId::Global
        ? bind_context_override(g_bind_context, index) : nullptr;
    const bool key_inherited = live && g_bind_context != BindContextId::Global
        && (!override_value || override_value->scan_codes[0] == bind_inherit);
    // The MOUSE cap also shows a Mouse 4+ in scan_codes[1], so an override of that slot counts when
    // either it or the Global value there is such a button.
    const bool slot1_overrides_mouse = override_value && override_value->scan_codes[1] != bind_inherit
        && (ctrl_is_extra_mouse_scan(override_value->scan_codes[1])
            || ctrl_is_extra_mouse_scan(bind_context_global_binding(index).scan_codes[1]));
    const bool mouse_inherited = live && g_bind_context != BindContextId::Global
        && (!override_value || (override_value->mouse_btn_id == bind_inherit && !slot1_overrides_mouse));
    const BindOverride values = bind_effective(index);
    std::string key_name;
    std::string mouse_name;
    if (live) {
        key_name = bind_key_name(values.scan_codes[0]);
        mouse_name = bind_mouse_cap_name(values);
    }
    const std::string key_text = key_inherited
        ? std::format("Global: {}", key_name.empty() ? "-" : key_name)
        : (key_name.empty() ? std::string{"-"} : key_name);
    const std::string mouse_text = mouse_inherited
        ? std::format("Global: {}", mouse_name.empty() ? "-" : mouse_name)
        : (mouse_name.empty() ? std::string{"-"} : mouse_name);

    // A listening cap takes both columns and the sibling stands down: the prompt is far wider
    // than one column, and the sibling could otherwise start a second capture underneath it.
    const int span_w = mouse_x + col_w - key_x;

    if (!capturing_mouse) {
        const UiRect key_rect{key_x, cap_y, capturing_key ? span_w : cap_w, cap_h};
        const SkinKeyCapResult key_cap = skin_key_cap(ctx, key_rect,
            capturing_key ? "PRESS A KEY..." : key_text.c_str(),
            !key_name.empty(), capturing_key, enabled, ui_id("bindkey", index), fonts.body,
            key_inherited);
        if (key_cap.activated) {
            bind_begin_capture(ctx, index, false, ui_id("bindkey", index));
            ui_play_click();
        }
        else if (key_cap.cleared) {
            bind_clear(index, false);
            ui_play_click();
        }
    }

    if (!capturing_key) {
        const UiRect mouse_rect{capturing_mouse ? key_x : mouse_x, cap_y,
                                capturing_mouse ? span_w : cap_w, cap_h};
        const SkinKeyCapResult mouse_cap = skin_key_cap(ctx, mouse_rect,
            capturing_mouse ? "PRESS A BUTTON..." : mouse_text.c_str(),
            !mouse_name.empty(), capturing_mouse, enabled, ui_id("bindmouse", index), fonts.body,
            mouse_inherited);
        if (mouse_cap.activated) {
            bind_begin_capture(ctx, index, true, ui_id("bindmouse", index));
            ui_play_click();
        }
        else if (mouse_cap.cleared) {
            bind_clear(index, true);
            ui_play_click();
        }
    }
}

void do_reorder_block(UiContext& ctx, const UiRect& r, const SettingRow& row, const Fonts& fonts)
{
    const int pad = skin_px(ctx, row_inset);
    const int label_h = skin_px(ctx, skin::row_h);
    const int list_row_h = skin_px(ctx, skin::list_row_h);
    // Wide enough for MOVE DOWN, so both buttons' labels take the same size.
    const int btn_w = skin_button_width(ctx, "MOVE DOWN", fonts.body_bold, 200.0f);
    const int btn_h = skin_px(ctx, 44.0f);
    const int inset = std::max(1, skin_px(ctx, 2.0f));
    const bool enabled = setting_disabled_reason(row) == nullptr && rf::local_player != nullptr;
    const UiId drag_id = ui_id("wpref_drag");

    const std::vector<int> slots = weapon_pref_slots();
    const UiRect list{r.x + pad, r.y + label_h,
                      std::max(0, r.w - 2 * pad - btn_w - skin_px(ctx, 16.0f)),
                      static_cast<int>(slots.size()) * list_row_h + 2 * inset};

    const bool hovered = ui_hover(ctx, r);
    if (!ctx.draw && hovered) {
        ctx.description().hover(desc_id_for(row));
    }

    if (ctx.draw) {
        const UiRect d = ui_draw_rect(ctx, UiRect{r.x, r.y, r.w, label_h});
        rf::gr::set_color(row_label_color(enabled, hovered, false));
        rf::gr::string(d.x + pad, d.y + (label_h - rf::gr::get_font_height(fonts.body)) / 2,
                       row.label, fonts.body);
        const UiRect dl = ui_draw_rect(ctx, list);
        skin_slot(ctx, dl.x, dl.y, dl.w, std::max(list_row_h, dl.h));
        if (slots.empty()) {
            rf::gr::set_color(skin::txt_hint);
            rf::gr::string(dl.x + skin_px(ctx, 12.0f),
                           dl.y + (list_row_h - rf::gr::get_font_height(fonts.caption)) / 2,
                           "No weapons to order yet.", fonts.caption);
        }
    }

    for (size_t i = 0; i < slots.size(); ++i) {
        const int slot = slots[i];
        const UiRect rr{list.x + inset, list.y + inset + static_cast<int>(i) * list_row_h,
                        std::max(0, list.w - 2 * inset), list_row_h};
        const char* name = rf::weapon_types[rf::local_player->weapon_prefs[slot]].display_name.c_str();
        if (skin_reorder_row(ctx, rr, name ? name : "", slot == g_weapon_pref_sel, fonts.body,
                             ui_id("wpref", slot))) {
            g_weapon_pref_sel = slot;
            ui_play_click();
            // Pressing a weapon also picks it up; it is dropped where the button is released.
            if (enabled && ctx.click) {
                ctx.capture(drag_id);
            }
        }
    }

    // While held, the weapon follows the pointer through the list, one place at a time.
    if (!ctx.draw && enabled && ctx.captured_id() == drag_id && list_row_h > 0) {
        const auto held = std::find(slots.begin(), slots.end(), g_weapon_pref_sel);
        if (held != slots.end()) {
            const int from = static_cast<int>(std::distance(slots.begin(), held));
            const int to = std::clamp((ctx.my - list.y - inset) / list_row_h, 0, static_cast<int>(slots.size()) - 1);
            const int step = to > from ? 1 : -1;
            for (int i = from; i != to; i += step) {
                weapon_pref_move(step);
            }
        }
    }

    const int bx = r.x + r.w - pad - btn_w;
    const auto it = std::find(slots.begin(), slots.end(), g_weapon_pref_sel);
    const bool has_sel = it != slots.end();
    const bool can_up = has_sel && it != slots.begin();
    const bool can_down = has_sel && std::next(it) != slots.end();
    if (skin_button(ctx, UiRect{bx, list.y, btn_w, btn_h}, "MOVE UP", fonts.body_bold,
                    SkinButton::Secondary, enabled && can_up, ui_id("wpref_up"))) {
        weapon_pref_move(-1);
        ui_play_click();
    }
    if (skin_button(ctx, UiRect{bx, list.y + btn_h + skin_px(ctx, 10.0f), btn_w, btn_h}, "MOVE DOWN",
                    fonts.body_bold, SkinButton::Secondary, enabled && can_down,
                    ui_id("wpref_down"))) {
        weapon_pref_move(1);
        ui_play_click();
    }
}

// ---------------------------------------------------------------------------
// Chrome
// ---------------------------------------------------------------------------

const char* category_summary(SettingCategory category)
{
    switch (category) {
        case SettingCategory::Graphics: return "Display, quality, lighting, shadows, effects and vehicles.";
        case SettingCategory::Hud:
            return "Heads-up display, crosshair, scopes, minimap, world markers, spectating and demos.";
        case SettingCategory::Interface: return "Menus, notifications, downloads and the console.";
        case SettingCategory::Audio: return "Volumes, voice messages and audio feedback.";
        case SettingCategory::Bindings: return "Keyboard, mouse and controller binding for every action.";
        case SettingCategory::Input: return "Mouse, keyboard, controller and scope behavior.";
        case SettingCategory::Gameplay: return "Single-player options, view, autoswitch, gore, weapons and vehicles.";
        case SettingCategory::Multiplayer: return "Identity, handicap, sprays, hosting and FactionFiles account.";
        default: return "";
    }
}

std::string status_strip_text()
{
    if (!level_is_running()) {
        return {};
    }
    if (in_limbo()) {
        return "ROUND ENDED";
    }
    if (!rf::is_multi) {
        return "GAME PAUSED";
    }
    return std::format("CONNECTED TO {} - GAME CONTINUES", rf::netgame.name.c_str());
}

// The status sits on the steel title bar, so it is dark lettering like the title.
rf::Color status_strip_color()
{
    return in_limbo() ? skin::ink_alert : skin::ink_status;
}

// Hidden rows such as the detail preset follow visible edits and are reverted with them, but are
// not counted, so one edit never reads as two.
int visible_change_count()
{
    return static_cast<int>(std::count_if(g_changed.begin(), g_changed.end(),
                                          [](const SettingRow* row) { return !(row->flags & SF_HIDDEN); }));
}

std::string footer_status_text()
{
    const int total = visible_change_count();
    if (total == 0) {
        return {};
    }
    int next_level = 0;
    for (const SettingRow* row : g_changed) {
        if ((row->flags & SF_APPLIES_NEXT_LEVEL) && !(row->flags & SF_HIDDEN)) {
            ++next_level;
        }
    }
    // Edits apply as they are made, so this counts what changed, not what is waiting.
    std::string text = std::format("{} setting{} changed", total, total == 1 ? "" : "s");
    if (next_level > 0) {
        text += std::format(" - {} appl{} on map change", next_level, next_level == 1 ? "ies" : "y");
    }
    return text;
}

// ---------------------------------------------------------------------------
// Modal
// ---------------------------------------------------------------------------

struct ModalCopy
{
    const char* title;
    std::string body;
    std::string note;
    const char* confirm;
};

ModalCopy modal_copy()
{
    switch (g_modal) {
        case Modal::ResetBindings:
            if (g_bind_device == BindDevice::Controller) {
                if (g_bind_context != BindContextId::Global) {
                    if (g_bind_context == BindContextId::Vehicles) {
                        return ModalCopy{"RESET BINDINGS",
                                         std::string{"Restore vehicle controller bindings to their defaults?"},
                                         std::string{"Change View uses D-Pad Left; other controller overrides are cleared."},
                                         "RESET BINDINGS"};
                    }
                    return ModalCopy{"RESET BINDINGS",
                                     std::format("Remove every controller override in the {} context?",
                                                 bind_context_label(g_bind_context)),
                                     std::string{"Every action here falls back to its global controller binding."},
                                     "RESET OVERRIDES"};
                }
                return ModalCopy{"RESET BINDINGS",
                                 std::string{"Restore every controller binding to its default?"},
                                 std::string{"Keyboard and mouse bindings are not touched."},
                                 "RESET BINDINGS"};
            }
            if (g_bind_context != BindContextId::Global) {
                return ModalCopy{"RESET BINDINGS",
                                 std::format("Remove every override in the {} context?",
                                             bind_context_label(g_bind_context)),
                                 std::string{"Every action here falls back to its global binding."},
                                 "RESET OVERRIDES"};
            }
            return ModalCopy{"RESET BINDINGS",
                             std::string{"Restore every key and mouse binding to its default?"},
                             std::string{"Weapon and Alpine actions are reset too."},
                             "RESET BINDINGS"};
        case Modal::RevertSession:
            return ModalCopy{"REVERT CHANGES",
                             std::format("Put back the {} setting{} changed since opening this menu?",
                                         visible_change_count(), visible_change_count() == 1 ? "" : "s"),
                             std::string{"Includes bindings and the autoswitch order."},
                             "REVERT"};
        default:
            return ModalCopy{"RESET CATEGORY",
                             std::format("Reset every setting in {} to its default?",
                                         setting_category_name(g_category)),
                             std::string{"Settings in other categories are not touched."}, "RESET"};
    }
}

void capture_session_baseline()
{
    g_bind_baseline = BindBaseline{};
    const int total = bind_context_binding_count();
    const rf::ControlConfig* cc = bind_context_controls();
    for (int i = 0; i < total; ++i) {
        g_bind_baseline.global.push_back(bind_context_global_binding(i));
        g_bind_baseline.global_aux.push_back(cc->bindings[i].aux_btn_id);
    }
    g_bind_baseline.overrides.resize(static_cast<std::size_t>(BindContextId::_count));
    for (int c = 1; c < static_cast<int>(BindContextId::_count); ++c) {
        for (int i = 0; i < total; ++i) {
            const BindOverride* value = bind_context_override(static_cast<BindContextId>(c), i);
            g_bind_baseline.overrides[c].push_back(value ? std::optional<BindOverride>{*value} : std::nullopt);
        }
    }
    g_weapon_prefs_baseline.reset();
    if (rf::local_player) {
        g_weapon_prefs_baseline.emplace();
        std::copy(std::begin(rf::local_player->weapon_prefs), std::end(rf::local_player->weapon_prefs),
                  g_weapon_prefs_baseline->begin());
    }
}

// Every row that left its baseline is set back in table order, so the detail preset lands before
// the rows it expands into and they then restore their own values over it.
void revert_session(UiContext& ctx)
{
    for (const SettingRow& row : settings_registry_all()) {
        if (std::string_view{row.id} == "bindings" && g_changed.count(&row)) {
            rf::ControlConfig* cc = bind_context_controls();
            const int total = std::min(bind_context_binding_count(), static_cast<int>(g_bind_baseline.global.size()));
            for (int i = 0; cc && i < total; ++i) {
                const BindOverride& value = g_bind_baseline.global[i];
                cc->bindings[i].scan_codes[0] = value.scan_codes[0];
                cc->bindings[i].scan_codes[1] = value.scan_codes[1];
                cc->bindings[i].mouse_btn_id = value.mouse_btn_id;
                cc->bindings[i].aux_btn_id = g_bind_baseline.global_aux[i];
                gamepad_set_action_code(i, 0, value.pad_codes[0]);
                gamepad_set_action_code(i, 1, value.pad_codes[1]);
            }
            for (int c = 1; c < static_cast<int>(g_bind_baseline.overrides.size()); ++c) {
                const auto context = static_cast<BindContextId>(c);
                bind_context_clear_all(context);
                for (int i = 0; i < static_cast<int>(g_bind_baseline.overrides[c].size()); ++i) {
                    if (const auto& value = g_bind_baseline.overrides[c][i]) {
                        bind_context_set_override(context, i, *value);
                    }
                }
            }
        }
        else if (std::string_view{row.id} == "weapon_prefs" && g_changed.count(&row)) {
            if (rf::local_player && g_weapon_prefs_baseline) {
                std::copy(g_weapon_prefs_baseline->begin(), g_weapon_prefs_baseline->end(),
                          std::begin(rf::local_player->weapon_prefs));
            }
        }
        else if (const auto it = g_baseline.find(&row);
                 it != g_baseline.end() && setting_has_setter(row) && it->second != setting_get_as_string(row)) {
            setting_set_from_string(row, it->second);
        }
    }
    g_changed.clear();
    note_rows_changed();
    ctx.push_toast("CHANGES REVERTED");
}

// Mirrors what settings_reset_category touches.
bool category_has_resettable_rows(SettingCategory category)
{
    for (const SettingRow* row : settings_in_category(category)) {
        if (!(row->flags & SF_READ_ONLY) && !setting_hidden_in_menu(*row) && setting_has_setter(*row)
            && setting_is_enabled(*row)) {
            return true;
        }
    }
    return false;
}

void modal_confirmed(UiContext& ctx)
{
    switch (g_modal) {
        case Modal::ResetBindings:
            if (g_bind_device == BindDevice::Controller) {
                if (g_bind_context != BindContextId::Global) {
                    // Only the controller fields go; a keyboard override in the same row stays.
                    for (int i = 0; i < bind_context_binding_count(); ++i) {
                        if (const BindOverride* stored = bind_context_override(g_bind_context, i)) {
                            BindOverride value = *stored;
                            value.pad_codes[0] = value.pad_codes[1] = bind_inherit;
                            bind_context_set_override(g_bind_context, i, value);
                        }
                    }
                    if (g_bind_context == BindContextId::Vehicles) {
                        const int index = static_cast<int>(get_af_control(rf::AF_ACTION_SPECTATE_CHANGE_VIEW));
                        if (alpine_control_start_index() >= 0 && index < bind_context_binding_count()) {
                            bind_pad_assign_context(ctx, index, 0, gamepad_vehicle_change_view_default_code());
                        }
                        else {
                            ctx.push_toast("CHANGE VIEW BINDING UNAVAILABLE");
                        }
                    }
                    else if (g_bind_context == BindContextId::Spectator) {
                        for (const auto& [action, code] : gamepad_spectator_default_bindings()) {
                            if (alpine_control_start_index() >= 0 && action >= 0
                                && action < bind_context_binding_count()) {
                                bind_pad_assign_context(ctx, action, 0, code);
                            }
                            else {
                                ctx.push_toast("SPECTATOR BINDING UNAVAILABLE");
                            }
                        }
                    }
                }
                else {
                    gamepad_reset_to_defaults();
                    if (bind_context_dedupe_against_global()) {
                        ctx.push_toast("CONTEXT OVERRIDES UNBOUND");
                    }
                }
                bind_note_changed();
            }
            else if (g_bind_context != BindContextId::Global) {
                bind_context_clear_all(g_bind_context);
                bind_note_changed();
            }
            else {
                bindings_reset_defaults(ctx);
            }
            break;
        case Modal::RevertSession:
            g_ui.release_text_focus(false);
            revert_session(ctx);
            g_row_state.clear();
            break;
        default:
            // A live text commit callback points into g_row_state; drop it before the map goes.
            g_ui.release_text_focus(false);
            settings_reset_category(g_category);
            note_rows_changed();
            g_row_state.clear();
            break;
    }
    g_modal = Modal::None;
}

constexpr rf::Color modal_dim{0, 0, 0, 191};

void draw_modal_dim()
{
    rf::gr::set_color(modal_dim);
    rf::gr::rect(0, 0, rf::gr::clip_width(), rf::gr::clip_height());
}

void build_modal(UiContext& ctx, int canvas_x, int canvas_y, const Fonts& fonts)
{
    const auto cx = [&](float v) { return canvas_x + static_cast<int>(v * ctx.scale()); };
    const auto cy = [&](float v) { return canvas_y + static_cast<int>(v * ctx.scale()); };
    const auto s = [&](float v) { return skin_px(ctx, v); };

    const ModalCopy copy = modal_copy();
    const char* title = copy.title;
    const std::string& body = copy.body;
    const std::string& note = copy.note;
    const char* confirm = copy.confirm;

    const UiRect dialog{cx(580.0f), cy(400.0f), s(760.0f), s(280.0f)};
    const int pad = s(14.0f);
    const int edge = s(1.0f);
    const UiRect band{dialog.x + edge, dialog.y + edge, dialog.w - 2 * edge, s(56.0f)};
    const UiRect body_box{dialog.x + pad, band.y + band.h + s(12.0f), dialog.w - 2 * pad, s(120.0f)};
    const int btn_h = s(44.0f);
    const int btn_y = body_box.y + body_box.h + s(14.0f);
    const int confirm_w = skin_button_width(ctx, confirm, fonts.body_bold, 260.0f) + s(30.0f);
    const int cancel_w = skin_button_width(ctx, "CANCEL", fonts.body_bold, 180.0f);
    const UiRect confirm_rect{dialog.x + dialog.w - pad - confirm_w, btn_y, confirm_w, btn_h};
    const UiRect cancel_rect{confirm_rect.x - s(16.0f) - cancel_w, btn_y, cancel_w, btn_h};

    if (ctx.draw) {
        draw_modal_dim();
        skin_popup(ctx, dialog, band, title, fonts.section);
        rf::gr::set_color(skin::txt_hi);
        // The body names a context or category, so its length varies; it shrinks to fit like a label.
        const SkinText fitted = skin_fit_text(ctx, body, fonts.body, body_box.w - 2 * s(24.0f));
        rf::gr::string(body_box.x + s(24.0f), body_box.y + s(24.0f), fitted.text.c_str(), fitted.font);
        ui_draw_line_block(UiRect{body_box.x + s(24.0f), body_box.y + s(62.0f),
                                  body_box.w - 2 * s(24.0f), s(56.0f)},
                           {note}, fonts.caption, rf::gr::get_font_height(fonts.caption) + s(4.0f),
                           skin::txt.red, skin::txt.green, skin::txt.blue);
    }

    if (skin_button(ctx, cancel_rect, "CANCEL", fonts.body_bold, SkinButton::Secondary, true,
                    ui_id("modal_cancel"))) {
        g_modal = Modal::None;
        ui_play_click();
    }
    if (skin_button(ctx, confirm_rect, confirm, fonts.body_bold, SkinButton::Danger, true,
                    ui_id("modal_confirm"))) {
        ui_play_click();
        modal_confirmed(ctx);
    }
}

void build_grid_picker_modal(UiContext& ctx, int canvas_x, int canvas_y, const Fonts& fonts)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const GridPicker& picker = *g_picker;
    const int count = picker.count();
    const int tile_w = s(picker.tile_w);
    const int tile_h = s(picker.tile_h)
        + (picker.caption_line ? skin_picker_caption_h(ctx, fonts.caption) : 0);
    const int gap = s(12.0f);

    const int dialog_w = s(1000.0f);
    const int dialog_h = s(760.0f);
    const UiRect dialog{canvas_x + (static_cast<int>(menu_ref_w * ctx.scale()) - dialog_w) / 2,
                        canvas_y + (static_cast<int>(menu_ref_h * ctx.scale()) - dialog_h) / 2, dialog_w,
                        dialog_h};
    const int pad = s(24.0f);
    const int band_h = s(56.0f);
    const int footer_h = s(68.0f);
    const int edge = s(1.0f);
    const UiRect band{dialog.x + edge, dialog.y + edge, dialog.w - 2 * edge, band_h};
    const UiRect grid_view{dialog.x + pad, band.y + band.h + s(12.0f), dialog.w - 2 * pad,
                           dialog.h - edge - band_h - s(12.0f) - footer_h - pad};

    const int cols = std::clamp((grid_view.w + gap) / std::max(1, tile_w + gap), 2, 6);
    const int rows = count > 0 ? (count + cols - 1) / cols : 0;
    const int content_h = rows * (tile_h + gap) + gap;
    const int grid_w = cols * tile_w + (cols - 1) * gap;

    if (ctx.draw) {
        draw_modal_dim();
        skin_popup(ctx, dialog, band, picker.title, fonts.section);
        skin_slot(ctx, grid_view.x, grid_view.y, grid_view.w, grid_view.h);
        if (count == 0) {
            rf::gr::set_color(skin::txt_hint);
            rf::gr::string(grid_view.x + s(16.0f), grid_view.y + s(16.0f), picker.empty_text,
                           fonts.body);
        }
    }

    const int selected = g_picker_row
        ? static_cast<int>(std::lround(row_float(*g_picker_row, -1.0f)))
        : -1;
    int chosen = -1;

    UiScrollRegion region = ui_scroll_begin(ctx, grid_view, content_h, g_picker_scroll, tile_h + gap,
                                            ui_id("picker_grid"), true);
    for (int i = 0; i < count; ++i) {
        const int col = i % cols;
        const int row = i / cols;
        const UiRect r{grid_view.x + (grid_view.w - grid_w) / 2 + col * (tile_w + gap),
                       region.start_y() + gap + row * (tile_h + gap), tile_w, tile_h};
        if (r.y + tile_h < grid_view.y || r.y > grid_view.y + grid_view.h) {
            continue;
        }
        if (skin_picker_tile(ctx, r, picker.bitmap(i), i, i == selected, picker.caption(i),
                             picker.caption_line, fonts.caption, ui_id("picker", i))) {
            chosen = i;
        }
    }
    ui_scroll_end(ctx, region);

    if (ctx.draw) {
        skin_scrollbar(ctx, grid_view, content_h, g_picker_scroll,
                       ctx.captured_id() == ui_id("picker_grid"));
    }

    const int btn_h = s(44.0f);
    const UiRect close{dialog.x + dialog.w - pad - s(200.0f), dialog.y + dialog.h - pad - btn_h,
                       s(200.0f), btn_h};
    if (skin_button(ctx, close, "CLOSE", fonts.body_bold, SkinButton::Secondary, true,
                    ui_id("picker_close"))) {
        g_modal = Modal::None;
        ui_play_click();
    }

    if (chosen >= 0 && g_picker_row) {
        write_row(*g_picker_row, std::to_string(chosen));
        ui_play_click();
        g_modal = Modal::None;
    }
}

constexpr uint32_t color_presets[] = {
    0xFFFFFFFFu, 0xC0C0C0FFu, 0x808080FFu, 0x404040FFu, 0x000000FFu, 0xFF0000FFu,
    0xFF8000FFu, 0xFFFF00FFu, 0x00FF00FFu, 0x00FFFFFFu, 0x0080FFFFu, 0xFF00FFFFu,
};

// Hue/saturation/value plus alpha, with the old and new colours side by side. Nothing is written
// until APPLY; CANCEL and ESC leave the row as it was.
void build_color_picker_modal(UiContext& ctx, int canvas_x, int canvas_y, const Fonts& fonts)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    ColorPick& pick = g_color_pick;

    const int dialog_w = s(900.0f);
    const int dialog_h = s(540.0f);
    const UiRect dialog{canvas_x + (static_cast<int>(menu_ref_w * ctx.scale()) - dialog_w) / 2,
                        canvas_y + (static_cast<int>(menu_ref_h * ctx.scale()) - dialog_h) / 2, dialog_w,
                        dialog_h};
    const int pad = s(24.0f);
    const int edge = s(1.0f);
    const UiRect band{dialog.x + edge, dialog.y + edge, dialog.w - 2 * edge, s(56.0f)};
    const int top = band.y + band.h + pad;
    const int area_h = s(360.0f);
    const UiRect sv{dialog.x + pad, top, s(360.0f), area_h};
    const UiRect hue{sv.x + sv.w + pad, top, s(32.0f), area_h};
    const UiRect alpha{hue.x + hue.w + s(20.0f), top, s(32.0f), area_h};
    const int col_x = alpha.x + alpha.w + s(36.0f);
    const int col_w = dialog.x + dialog.w - pad - col_x;
    const int caption_h = s(26.0f);
    const int section_gap = s(28.0f);
    const int preview_w = (col_w - s(20.0f)) / 2;
    const UiRect preview_new{col_x, top + caption_h, preview_w, s(72.0f)};
    const UiRect preview_old{col_x + col_w - preview_w, preview_new.y, preview_w, preview_new.h};
    const int hex_y = preview_new.y + preview_new.h + section_gap;
    const UiRect hex_box{col_x, hex_y + caption_h, s(200.0f), s(skin::widget_h)};
    const int presets_y = hex_box.y + hex_box.h + section_gap;
    constexpr int preset_cols = 6;
    const int chip_gap = s(12.0f);
    const int chip = (col_w - (preset_cols - 1) * chip_gap) / preset_cols;
    const int btn_h = s(44.0f);
    const int btn_y = dialog.y + dialog.h - pad - btn_h;

    if (ctx.draw) {
        draw_modal_dim();
        skin_popup(ctx, dialog, band, "SELECT COLOR", fonts.section);
        const int title_w = rf::gr::get_string_size("SELECT COLOR", fonts.section).first;
        const SkinText label = skin_fit_text(ctx, pick.row->label, fonts.body,
                                             std::max(0, band.w - title_w - s(84.0f)));
        rf::gr::set_color(skin::txt);
        rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, band.x + band.w - s(16.0f),
                               band.y + (band.h - rf::gr::get_font_height(label.font)) / 2,
                               label.text.c_str(), label.font);
        rf::gr::string(preview_new.x, top, "NEW", fonts.caption_bold);
        rf::gr::string(preview_old.x, top, "CURRENT", fonts.caption_bold);
        rf::gr::string(col_x, hex_y, "HEX", fonts.caption_bold);
        rf::gr::string(col_x, presets_y, "PRESETS", fonts.caption_bold);
        skin_color_preview(ctx, preview_new, pick.unset ? std::nullopt : std::optional{pick.rgba}, fonts.caption);
    }

    // Greys have no hue and black no saturation of their own, so taking one keeps what the square
    // and strip were left on.
    const auto take = [&](uint32_t rgba) {
        const SkinHsva kept = pick.hsva;
        pick.unset = false;
        pick.rgba = rgba;
        pick.hsva = skin_rgba_to_hsva(rgba);
        if (pick.hsva.v == 0.0f) {
            pick.hsva.h = kept.h;
            pick.hsva.s = kept.s;
        }
        else if (pick.hsva.s == 0.0f) {
            pick.hsva.h = kept.h;
        }
    };

    bool dragged = skin_sv_square(ctx, sv, pick.hsva, ui_id("color_sv"));
    dragged = skin_hue_strip(ctx, hue, pick.hsva, ui_id("color_hue")) || dragged;
    dragged = skin_alpha_strip(ctx, alpha, pick.hsva, ui_id("color_alpha")) || dragged;
    if (dragged) {
        pick.unset = false;
        pick.rgba = skin_hsva_to_rgba(pick.hsva);
    }

    const std::optional<uint32_t> current = pick.had_value ? std::optional{pick.original} : std::nullopt;
    if (skin_color_chip(ctx, preview_old, current, fonts.caption, ui_id("color_current"))) {
        take(pick.original);
        pick.unset = !pick.had_value;
        ui_play_click();
    }
    for (int i = 0; i < static_cast<int>(std::size(color_presets)); ++i) {
        const UiRect r{col_x + (i % preset_cols) * (chip + chip_gap),
                       presets_y + caption_h + (i / preset_cols) * (chip + chip_gap), chip, chip};
        // A preset sets the colour, not the alpha the user already chose.
        if (skin_color_chip(ctx, r, color_presets[i], fonts.caption, ui_id("color_preset", i))) {
            take((color_presets[i] & 0xFFFFFF00u) | (pick.rgba & 0xFFu));
            ui_play_click();
        }
    }

    const UiId hex_id = ui_id("color_hex");
    if (skin_field(ctx, hex_box, hex_id, pick.hex, 8, ui_char_filter_hex, "RRGGBB", fonts.body, true)) {
        if (const std::optional<uint32_t> parsed = parse_hex_color_string(pick.hex)) {
            take(*parsed);
        }
    }
    if (!ctx.draw && ctx.focused_text_id() == hex_id && g_prev_text_focus != hex_id) {
        // Typing applies as it parses, so ESC puts back the pick the edit started from.
        ctx.set_text_commit_callback([before = pick](bool commit) {
            if (!commit) {
                g_color_pick = before;
            }
        });
    }
    // Outside an edit the box always shows the pick, which also drops text that never parsed.
    if (!ctx.draw && ctx.focused_text_id() != hex_id) {
        pick.hex = pick.unset ? std::string{} : format_hex_color_string(pick.rgba);
    }

    const int apply_w = skin_button_width(ctx, "APPLY", fonts.body_bold, 180.0f);
    const int cancel_w = skin_button_width(ctx, "CANCEL", fonts.body_bold, 180.0f);
    const UiRect apply_rect{dialog.x + dialog.w - pad - apply_w, btn_y, apply_w, btn_h};
    const UiRect cancel_rect{apply_rect.x - s(16.0f) - cancel_w, btn_y, cancel_w, btn_h};
    if (skin_button(ctx, cancel_rect, "CANCEL", fonts.body_bold, SkinButton::Secondary, true,
                    ui_id("color_cancel"))) {
        g_modal = Modal::None;
        ui_play_click();
    }
    if (skin_button(ctx, apply_rect, "APPLY", fonts.body_bold, SkinButton::Secondary, true,
                    ui_id("color_apply"))) {
        if (!pick.unset) {
            write_row(*pick.row, format_hex_color_string(pick.rgba));
        }
        g_modal = Modal::None;
        ui_play_click();
    }
}

// ---------------------------------------------------------------------------
// Screen
// ---------------------------------------------------------------------------

// The id a culled entry stands in with for keyboard navigation: its first focusable widget.
UiId entry_focus_id(const Entry& entry, bool mouse_column)
{
    if (entry.kind == EntryKind::Row && entry.row->widget != SettingWidget::Label) {
        return ui_id(entry.row->id);
    }
    if (entry.kind == EntryKind::Bind) {
        return ui_id(mouse_column ? "bindmouse" : "bindkey", entry.bind_index);
    }
    if (entry.kind == EntryKind::Reorder) {
        const std::vector<int> slots = weapon_pref_slots();
        return slots.empty() ? 0 : ui_id("wpref", slots.front());
    }
    return 0;
}

bool entry_owns_focus(const Entry& entry, UiId focus)
{
    if (focus == 0) {
        return false;
    }
    if (entry.kind == EntryKind::Reorder) {
        const std::vector<int> slots = weapon_pref_slots();
        return focus == ui_id("wpref_up") || focus == ui_id("wpref_down")
            || std::any_of(slots.begin(), slots.end(), [focus](int slot) { return ui_id("wpref", slot) == focus; });
    }
    return entry_focus_id(entry, false) == focus || entry_focus_id(entry, true) == focus;
}

void register_offscreen_focus(UiContext& ctx, const Entry& entry)
{
    // Other action buttons can have extra enable conditions the row alone does not capture.
    if (entry.kind == EntryKind::Row
        && (!row_widget_enabled(*entry.row)
            || (entry.row->widget == SettingWidget::Action && !is_controller_debug_action(entry.row->id)))) {
        return;
    }
    if (entry.kind == EntryKind::Bind && (!bind_rows_live() || g_capture.row >= 0)) {
        return;
    }
    const bool adjustable = entry.kind == EntryKind::Row
        && (entry.row->widget == SettingWidget::Slider || entry.row->widget == SettingWidget::Stepper
            || entry.row->widget == SettingWidget::OptionalScale
            || (entry.row->widget == SettingWidget::Enum && !picker_for_row(*entry.row)));
    const UiFocusKind kind = adjustable ? UiFocusKind::Adjustable : UiFocusKind::Activate;
    for (const bool mouse_column : {false, true}) {
        if (const UiId id = entry_focus_id(entry, mouse_column); id != 0) {
            ctx.register_focusable(id, kind);
        }
        if (entry.kind != EntryKind::Bind) {
            break;
        }
    }
}

void draw_title_bar(const UiContext& ctx, int x, int right, int clear_r, int canvas_y, const Fonts& fonts)
{
    const UiRect free = menu_draw_title_bar(ctx, x, right, clear_r, canvas_y, "Options", fonts.title);
    const std::string status = status_strip_text();
    if (status.empty()) {
        return;
    }
    const SkinText fitted = skin_fit_text(ctx, status, fonts.caption_bold, free.w);
    rf::gr::set_color(status_strip_color());
    rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, free.x + free.w,
                           free.y + free.h / 2 - rf::gr::get_font_height(fitted.font) / 2, fitted.text.c_str(),
                           fitted.font);
}

void build_category_column(UiContext& ctx, const MenuLayout& lay, int col_x, int col_y, const Fonts& fonts)
{
    constexpr int category_count = static_cast<int>(SettingCategory::_count);
    const std::optional<SettingCategory> selected = selected_category();
    std::array<MenuColumnItem, category_count> items;
    for (int i = 0; i < category_count; ++i) {
        const SettingCategory category = category_order[i];
        items[i] = MenuColumnItem{string_to_upper(setting_category_name(category)), selected == category};
    }
    const int clicked = menu_build_column(ctx, lay, col_x, col_y, items, "BACK", fonts.body_bold, "category");
    if (clicked < 0) {
        return;
    }
    ui_play_click();
    if (clicked == category_count) {
        request_close();
        return;
    }
    const SettingCategory category = category_order[clicked];
    if (selected != category) {
        // Picking the panel's own category again turns a slide-out around.
        if (g_switch_to && category == g_category && g_search.empty()) {
            g_switch_to.reset();
        }
        else {
            g_switch_to = category;
        }
        ctx.description().pinned = false;
    }
}

// Search and Show advanced, right-aligned on the header line. Returns where they start, which is
// as far as the title may run.
int build_header_controls(UiContext& ctx, const UiRect& line, const Fonts& fonts)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const char* adv_text = "Show advanced";
    const int adv_text_w = rf::gr::get_string_size(adv_text, fonts.body).first;
    // A short On / Off cycler: the rows' column width does not fit beside the search box.
    const int adv_w = s(2.0f * (skin::cycler_arrow + skin::cycler_gap) + 80.0f);
    const UiRect adv_toggle{line.x + line.w - adv_w, line.y + (line.h - s(skin::widget_h)) / 2, adv_w,
                            s(skin::widget_h)};
    const UiRect adv_label{adv_toggle.x - s(14.0f) - adv_text_w, line.y, adv_text_w + s(14.0f), line.h};
    const UiRect search{adv_label.x - s(28.0f) - s(340.0f), line.y + (line.h - s(44.0f)) / 2, s(340.0f), s(44.0f)};

    if (skin_search_box(ctx, search, ui_id("search"), g_search, fonts.body)) {
        if (!g_search.empty() && !g_search_holds_panel) {
            remember_category_scroll();
            g_search_holds_panel = true;
        }
        if (g_search.empty() && g_search_holds_panel) {
            show_category(g_category);
        }
        else {
            g_content_scroll = 0.0f;
        }
        ctx.description().pinned = false;
    }

    const bool show_advanced = g_alpine_game_config.show_advanced_settings;
    const bool label_hovered = ui_hover(ctx, adv_label);
    if (ctx.draw) {
        const bool lit = label_hovered || (ctx.focus_visible() && ctx.focus_id() == ui_id("show_advanced"));
        rf::gr::set_color(lit ? skin::ink_title : skin::ink_status);
        rf::gr::string(adv_label.x, line.y + (line.h - rf::gr::get_font_height(fonts.body)) / 2, adv_text,
                       fonts.body);
    }
    bool flip_advanced = skin_cycler(ctx, adv_toggle, show_advanced ? "On" : "Off", fonts.body, true,
                                     ui_id("show_advanced"), label_hovered) != 0;
    // The label is part of the control too.
    if (!ctx.draw && !flip_advanced && ctx.click && label_hovered) {
        flip_advanced = true;
    }
    if (flip_advanced) {
        // A view preference, not a setting change: its baseline follows it, so it never counts.
        if (const SettingRow* row = settings_find("show_advanced_settings")) {
            setting_set_from_string(*row, show_advanced ? "0" : "1");
            g_baseline[row] = setting_get_as_string(*row);
        }
        ui_play_toggle(!show_advanced);
    }
    return search.x;
}

void draw_header_title(const UiContext& ctx, const UiRect& line, int right_x, bool searching, int match_count,
                       const Fonts& fonts)
{
    const std::string title = searching
        ? std::format("{} RESULT{}", match_count, match_count == 1 ? "" : "S")
        : string_to_upper(setting_category_name(g_category));
    const SkinText fitted =
        skin_fit_text(ctx, title, fonts.heading, std::max(0, right_x - skin_px(ctx, 24.0f) - line.x));
    rf::gr::set_color(skin::ink_title);
    rf::gr::string(line.x, line.y + (line.h - rf::gr::get_font_height(fitted.font)) / 2, fitted.text.c_str(),
                   fitted.font);
}

// The Bindings header's second line carries the device selector (left) and the bind context
// selector (right). Both are sized from measured labels so they cannot overlap each other; the
// device strip is the one that gives ground when the line runs out of room.
void build_bindings_header(UiContext& ctx, const UiRect& line, const Fonts& fonts)
{
    const auto s = [&](float v) { return ctx.scaled(v); };

    const int strip_h = line.h;
    const int strip_y = line.y;
    const int avail_x0 = line.x;
    const int avail_x1 = line.x + line.w;
    const int strip_gap = s(16.0f);

    const int ctx_count = static_cast<int>(BindContextId::_count);
    StripTab ctx_tabs[static_cast<int>(BindContextId::_count)];
    int ctx_w[static_cast<int>(BindContextId::_count)];
    int ctx_total = 0;
    for (int i = 0; i < ctx_count; ++i) {
        const auto id = static_cast<BindContextId>(i);
        ctx_tabs[i] = StripTab{bind_context_label(id), bind_context_description(id)};
        ctx_w[i] = strip_tab_width(ctx, ctx_tabs[i].label, fonts.caption_bold);
        ctx_total += ctx_w[i];
    }

    constexpr int dev_count = static_cast<int>(BindDevice::_count);
    static const char* const dev_long[dev_count] = {"KEYBOARD", "CONTROLLER", "VR"};
    static const char* const dev_short[dev_count] = {"KB", "PAD", "VR"};
    const char* const* dev_labels = dev_long;
    int dev_font = fonts.caption_bold;
    int dev_w[dev_count];
    int dev_total = 0;
    const auto measure_devices = [&] {
        dev_total = 0;
        for (int i = 0; i < dev_count; ++i) {
            dev_w[i] = strip_tab_width(ctx, dev_labels[i], dev_font);
            dev_total += dev_w[i];
        }
    };
    measure_devices();
    // Shrink the device strip before anything else: the context strip is the functional one.
    if (avail_x0 + dev_total + strip_gap + ctx_total > avail_x1) {
        dev_font = fonts.caption;
        measure_devices();
    }
    if (avail_x0 + dev_total + strip_gap + ctx_total > avail_x1) {
        dev_labels = dev_short;
        measure_devices();
    }
    // A context tab never goes below its dot plus an ellipsis; if the strips cannot both hold
    // their floor the device strip goes away entirely rather than squeezing the useful one.
    const int ctx_min = rf::gr::get_string_size(std::string_view{"..."}, fonts.caption_bold).first
        + strip_tab_chrome(ctx);
    int ctx_left = avail_x0 + dev_total + strip_gap;
    bool show_devices = true;
    if (ctx_left + std::min(ctx_total, ctx_min * ctx_count) > avail_x1) {
        show_devices = false;
        ctx_left = avail_x0;
    }
    // Still short: the context tabs take an equal share of what is left and ellipsize.
    int ctx_x = std::max(ctx_left, avail_x1 - ctx_total);
    if (ctx_x + ctx_total > avail_x1) {
        const int share = std::max(ctx_min, (avail_x1 - ctx_x) / ctx_count);
        ctx_total = share * ctx_count;
        for (int i = 0; i < ctx_count; ++i) {
            ctx_w[i] = share;
        }
    }

    if (show_devices) {
        StripTab dev_tabs[dev_count];
        for (int i = 0; i < dev_count; ++i) {
            dev_tabs[i] =
                StripTab{dev_labels[i], bind_device_description(static_cast<BindDevice>(i))};
        }
        const int dev_clicked = do_segment_strip(ctx,
            UiRect{avail_x0, strip_y, dev_total, strip_h}, dev_tabs, dev_w, dev_count,
            static_cast<int>(g_bind_device), "binddev", dev_font);
        if (dev_clicked >= 0) {
            if (static_cast<BindDevice>(dev_clicked) != g_bind_device && g_capture.row >= 0) {
                ctx.cancel_key_capture();
            }
            g_bind_device = static_cast<BindDevice>(dev_clicked);
            ui_play_click();
        }
    }

    const int ctx_clicked = do_segment_strip(ctx, UiRect{ctx_x, strip_y, ctx_total, strip_h},
        ctx_tabs, ctx_w, ctx_count, static_cast<int>(g_bind_context), "bindctx",
        fonts.caption_bold);
    if (ctx_clicked >= 0) {
        if (static_cast<BindContextId>(ctx_clicked) != g_bind_context && g_capture.row >= 0) {
            ctx.cancel_key_capture();
        }
        g_bind_context = static_cast<BindContextId>(ctx_clicked);
        ui_play_click();
    }
}

void build_content(UiContext& ctx, const UiRect& view, const std::vector<Entry>& entries, bool searching,
                   const Fonts& fonts, bool sync_state)
{
    const auto s = [&](float v) { return ctx.scaled(v); };

    const int row_h = s(skin::row_h);
    int content_h = 0;
    for (const Entry& entry : entries) {
        content_h += s(entry.ref_h);
    }

    UiScrollRegion region = ui_scroll_begin(ctx, view, content_h, g_content_scroll, row_h * 3,
        ui_id("content"), true);

    const int gutter = std::max(ctx.style.scrollbar_w_min, ctx.scaled(ctx.style.scrollbar_w));
    const int inner_w = view.w - gutter - s(8.0f);
    const int slider_readout_w = panel_slider_readout_w(ctx, entries,
        searching ? std::nullopt : std::optional{g_category}, fonts.body);
    std::optional<UiRect> focus_rect;
    int y = region.start_y();
    for (const Entry& entry : entries) {
        const int h = s(entry.ref_h);
        const UiRect rect{view.x, y, inner_w, h};
        y += h;
        if (entry_owns_focus(entry, ctx.focus_id())) {
            focus_rect = rect;
        }
        if (rect.y + h < view.y || rect.y > view.y + view.h) {
            // Still reachable by keyboard: the focus can step onto it and scroll it into view.
            register_offscreen_focus(ctx, entry);
            continue;
        }
        switch (entry.kind) {
            case EntryKind::SubHeader:
                draw_sub_header(ctx, rect, entry.header, fonts);
                break;
            case EntryKind::Row:
                do_row(ctx, *entry.row, rect, entry.crumb, fonts, slider_readout_w, sync_state);
                break;
            case EntryKind::BindHeader:
                draw_bind_header(ctx, rect, fonts);
                break;
            case EntryKind::BindGroup:
                draw_sub_header(ctx, rect, entry.header, fonts);
                break;
            case EntryKind::Bind:
                do_bind_row(ctx, rect, entry.bind_index, fonts);
                break;
            case EntryKind::Reorder:
                do_reorder_block(ctx, rect, *entry.row, fonts);
                break;
        }
    }

    if (ctx.draw && entries.empty()) {
        const UiRect d = ui_draw_rect(ctx, UiRect{view.x, view.y, inner_w, row_h});
        const std::string empty = searching ? std::format("No results for \"{}\"", g_search)
                                            : std::string{"Nothing to configure here."};
        rf::gr::set_color(skin::ink_hint);
        rf::gr::string(d.x + s(row_inset), d.y + s(24.0f), empty.c_str(), fonts.body);
    }

    ui_scroll_end(ctx, region);

    ui_scroll_focus_into_view(ctx, view, focus_rect, g_content_scroll, g_scrolled_to_focus);

    if (ctx.draw) {
        skin_scrollbar(ctx, view, content_h, g_content_scroll, ctx.captured_id() == ui_id("content"));
    }
}

void draw_description_strip(UiContext& ctx, const UiRect& desc, const Fonts& fonts)
{
    const auto s = [&](float v) { return ctx.scaled(v); };

    skin_slot(ctx, desc.x, desc.y, desc.w, desc.h);
    // A hovered or pinned registry row, else the category summary. Blocks that are not
    // registry rows supply their own line.
    const SettingRow* desc_row = desc_row_from_id(ctx.description().id);
    std::string text;
    if (!g_desc_custom.empty()) {
        text = g_desc_custom;
    }
    else if (desc_row) {
        text = desc_row->tooltip ? desc_row->tooltip : "";
        if (desc_row->describe) {
            desc_row->describe(text);
        }
        // A row can be both locked out and blocked by the server; saying only the first
        // reason hides the one the player would act on.
        const char* reason = setting_disabled_reason(*desc_row);
        const bool blocked = setting_server_permission_blocked(*desc_row);
        if (reason && blocked) {
            text += std::format("  ({}; this server does not currently allow this)", reason);
        }
        else if (reason) {
            text += std::format("  ({})", reason);
        }
        else if (blocked) {
            text += "  (this server does not currently allow this)";
        }
    }
    else {
        text = category_summary(g_category);
    }
    const int caption_h = rf::gr::get_font_height(fonts.caption);
    const int line_h = std::max(1, caption_h + s(6.0f));
    // In line with the row labels above, which start row_inset into a view 16 left of the strip.
    const int text_x = desc.x + s(row_inset - 16.0f);
    const int text_w = std::max(1, desc.x + desc.w - s(18.0f) - text_x);
    const int max_lines = std::max(1, (desc.h - 2 * s(12.0f)) / line_h);
    const int lines = std::clamp(static_cast<int>(ui_wrap_text(text, text_w, fonts.caption).size()),
                                 1, max_lines);
    const int block_h = lines * line_h;
    const int text_y = desc.y + (desc.h - block_h) / 2;
    rf::gr::set_color(skin::txt);
    ui_draw_wrapped(ctx, UiRect{text_x, text_y, text_w, block_h}, text, fonts.caption, line_h);
}

void build_panel_footer(UiContext& ctx, const UiRect& line, bool searching, const Fonts& fonts)
{
    const int btn_gap = skin_px(ctx, 16.0f);
    const int revert_w = skin_button_width(ctx, "REVERT", fonts.body_bold, 140.0f);
    const int reset_w = skin_button_width(ctx, "RESET CATEGORY", fonts.body_bold, 220.0f);
    const UiRect revert{line.x + line.w - revert_w, line.y, revert_w, line.h};
    const UiRect reset_cat{revert.x - btn_gap - reset_w, line.y, reset_w, line.h};

    // Bindings has its own reset (the current context's overrides, or the factory bindings).
    const bool bindings = g_category == SettingCategory::Bindings;
    if (skin_button(ctx, reset_cat, "RESET CATEGORY", fonts.body_bold, SkinButton::Secondary,
                    !searching && (bindings ? bind_rows_live() : category_has_resettable_rows(g_category)),
                    ui_id("reset_category"))) {
        g_modal = bindings ? Modal::ResetBindings : Modal::ResetCategory;
        ui_play_click();
    }
    if (skin_button(ctx, revert, "REVERT", fonts.body_bold, SkinButton::Secondary, visible_change_count() > 0,
                    ui_id("revert_session"))) {
        g_modal = Modal::RevertSession;
        ui_play_click();
    }

    if (ctx.draw) {
        const std::string status = footer_status_text();
        if (!status.empty()) {
            const SkinText fitted =
                skin_fit_text(ctx, status, fonts.caption, std::max(0, reset_cat.x - btn_gap - line.x));
            rf::gr::set_color(skin::ink_status);
            rf::gr::string(line.x, line.y + (line.h - rf::gr::get_font_height(fitted.font)) / 2,
                           fitted.text.c_str(), fitted.font);
        }
    }
}

void build_screen(UiContext& ctx, int canvas_y, const Fonts& fonts, bool sync_state)
{
    const float scale = ctx.scale();
    const auto s = [&](float v) { return static_cast<int>(v * scale); };
    const auto cy = [&](float v) { return canvas_y + s(v); };
    const MenuLayout lay = menu_layout(ctx);

    const int slide_w = rf::gr::clip_width() - lay.panel_x;
    const int enter_dx = static_cast<int>((1.0f - g_slide) * static_cast<float>(slide_w));
    const int panel_dx = enter_dx + static_cast<int>(g_switch * static_cast<float>(slide_w));
    const int col_dx = -static_cast<int>((1.0f - g_slide) * static_cast<float>(lay.column_x + lay.column_w));

    const int button_h = menu_button_h(ctx);
    const int top = cy(menu_column_y);

    if (ctx.draw) {
        draw_title_bar(ctx, lay.panel_x + enter_dx, lay.panel_r + enter_dx, lay.rail_right + enter_dx, canvas_y,
                       fonts);
    }

    build_category_column(ctx, lay, lay.column_x + col_dx, top, fonts);

    const MenuPanel panel = menu_panel(ctx, lay, panel_dx, top);
    const UiRect& inset = panel.inset;
    if (ctx.draw) {
        menu_draw_panel(ctx, panel);
        // Search results belong to no category, so nothing is connected.
        if (g_search.empty()) {
            menu_draw_connector(ctx, panel, top + category_slot(g_category) * button_h + button_h / 2);
        }
    }
    // A panel on its way out is only painted: anything done on it would land on the category
    // about to replace it.
    if (!ctx.draw && g_switch_to) {
        return;
    }

    const int content_x = panel.content_x;
    const int content_r = panel.content_r;
    const UiRect header{content_x, cy(menu_panel_header_y), content_r - content_x, s(menu_panel_header_h)};
    const int title_right = build_header_controls(ctx, header, fonts);
    const bool searching = !g_search.empty();

    std::vector<std::string> crumbs;
    int match_count = 0;
    const std::vector<Entry> entries = searching
        ? build_search_entries(g_search, crumbs, match_count)
        : build_category_entries(g_category);

    if (ctx.draw) {
        draw_header_title(ctx, header, title_right, searching, match_count, fonts);
        skin_ridge(ctx, inset.x + s(8.0f), header.y + header.h + s(1.0f), inset.w - s(16.0f));
    }

    // The header line has no room left beside the search box, so Bindings takes a second one.
    float view_top = menu_panel_body_y;
    if (!searching && g_category == SettingCategory::Bindings && bind_context_controls() != nullptr) {
        build_bindings_header(ctx, UiRect{content_x, cy(menu_panel_body_y), content_r - content_x, s(36.0f)},
                              fonts);
        view_top = 262.0f;
    }
    else if (!ctx.draw) {
        // No strips on screen means no way to see or change what is being edited, so the edit
        // target may not survive a search or a trip through another category.
        g_bind_context = BindContextId::Global;
        g_bind_device = BindDevice::Keyboard;
    }

    const int footer_y = inset.y + inset.h - s(menu_panel_footer_inset);
    const int desc_y = footer_y - s(74.0f);
    // The rows start left of the content edge so a focused row's LED lines up with the headers'.
    const UiRect view{content_x - s(16.0f), cy(view_top), content_r - content_x + s(16.0f),
                      desc_y - s(8.0f) - cy(view_top)};
    build_content(ctx, view, entries, searching, fonts, sync_state);
    if (ctx.draw) {
        draw_description_strip(ctx, UiRect{content_x, desc_y, content_r - content_x, s(66.0f)}, fonts);
    }
    build_panel_footer(ctx, UiRect{content_x, footer_y, content_r - content_x, s(menu_panel_footer_h)}, searching,
                       fonts);
}

void settings_menu_build(UiContext& ctx)
{
    const int screen_w = rf::gr::clip_width();
    const int screen_h = rf::gr::clip_height();
    const float scale = menu_screen_scale();
    ctx.set_scale(scale);

    const int canvas_x = (screen_w - static_cast<int>(menu_ref_w * scale)) / 2;
    const int canvas_y = (screen_h - static_cast<int>(menu_ref_h * scale)) / 2;

    Fonts fonts;
    fonts.title = menu_font(true, 46.0f, scale);
    fonts.heading = menu_font(true, 32.0f, scale);
    fonts.section = menu_font(true, 24.0f, scale);
    fonts.body = menu_font(false, 20.0f, scale);
    fonts.body_bold = menu_font(true, 20.0f, scale);
    fonts.caption = menu_font(false, 17.0f, scale);
    fonts.caption_bold = menu_font(true, 17.0f, scale);

    // Hit-test pass only: hold the widget-local copies while a text box is being edited, and for
    // one further frame so the commit that release_text_focus() ran can be written through.
    const bool sync_state = ctx.draw ? false : (ctx.focused_text_id() == 0 && g_prev_text_focus == 0);

    if (!ctx.draw) {
        ctx.description().begin_pass();
        g_desc_custom.clear();
        // Resolved here rather than in the row widget, so a capture whose row scrolled out of
        // view still ends.
        bind_poll_capture(ctx);
    }

    // Captured before the screen runs: a click that opens a modal must not also be delivered to
    // the dialog that click created.
    const Modal modal = g_modal;

    // A modal or the exit animation owns the frame: the screen is still painted (dimmed, behind
    // the dialog) but never hit-tested, so nothing behind can be clicked or focused.
    const bool screen_interactive = modal == Modal::None && !g_closing;
    if (ctx.draw || screen_interactive) {
        build_screen(ctx, canvas_y, fonts, sync_state);
    }
    // Stock draws its side bars over its menus; the dialogs, and their dim, go over the bars.
    if (ctx.draw) {
        rf::gr::reset_clip();
        rf::ui::menu_draw_side_bars();
    }
    if (modal == Modal::GridPicker) {
        if (g_picker) {
            build_grid_picker_modal(ctx, canvas_x, canvas_y, fonts);
        }
    }
    else if (modal == Modal::ColorPicker) {
        if (g_color_pick.row) {
            build_color_picker_modal(ctx, canvas_x, canvas_y, fonts);
        }
    }
    else if (modal != Modal::None) {
        build_modal(ctx, canvas_x, canvas_y, fonts);
    }

    if (!ctx.draw) {
        ctx.description().end_pass();
        g_prev_text_focus = ctx.focused_text_id();
    }
}

void __cdecl settings_menu_key_callback(int key)
{
    if (key == rf::KEY_NONE) {
        return;
    }

    // A listening key cap owns every word, ESC (its cancel) included.
    if (bind_handle_capture_key(key)) {
        return;
    }

    // A focused text box owns everything; the context derives typed characters from the queue
    // word through rf::key_to_ascii and replays them at the start of the next hit-test pass.
    if (g_ui.focused_text_id() != 0) {
        g_ui.push_key(static_cast<rf::Key>(key));
        return;
    }

    if ((key & rf::KEY_MASK) == rf::KEY_ESC) {
        if (g_modal != Modal::None) {
            g_modal = Modal::None;
            ui_play_click();
            return;
        }
        if (g_ui.popup_id() != 0) {
            g_ui.close_popup();
            ui_play_click();
            return;
        }
        // ESC unwinds the search before it leaves the menu.
        if (!g_search.empty()) {
            show_category(g_category);
            ui_play_click();
            return;
        }
        ui_play_click();
        request_close();
        return;
    }

    // Tab / Shift+Tab (0x100F) / arrows / Enter, resolved against the previous frame's widgets.
    g_ui.handle_nav_key(static_cast<rf::Key>(key));
}

void settings_menu_input()
{
    rf::game_poll(settings_menu_key_callback);

    // The console reads the same key queue a focused box drains, so a box would type into both -
    // and the menu pump skips the key loop entirely while it is open, which would leave a capture
    // listening with nothing able to end it.
    if (rf::console::console_is_visible()) {
        if (g_ui.focused_text_id() != 0) {
            g_ui.release_text_focus(true);
        }
        if (g_capture.row >= 0) {
            g_ui.cancel_key_capture();
        }
    }

    const int wheel = rf::mouse_dz;
    int x = 0;
    int y = 0;
    int z = 0;
    rf::mouse_get_pos(x, y, z);

    // While a cap listens the host owns the mouse: the press that would become a UI click is
    // either the new binding or the cancel.
    bind_poll_capture_mouse(wheel);
    const bool capturing = g_capture.row >= 0;

    g_ui.begin_frame(x, y, capturing ? 0 : wheel, !capturing && rf::mouse_button_is_down(0),
        !capturing && rf::mouse_was_button_pressed(0) != 0,
        !capturing && rf::mouse_was_button_pressed(1) != 0);
    if (!capturing && !g_ui.lmb_down && !rf::console::console_is_visible()) {
        constexpr float scroll_speed = 880.0f;
        g_ui.analog_scroll = gamepad_consume_menu_scroll() * scroll_speed * menu_screen_scale()
            * std::max(0.0f, rf::frametime);
    }
    g_ui.begin_pass(UiPass::HitTest);
    settings_menu_build(g_ui);
    g_ui.end_pass();
    write_committed_values();

    // The click or Enter that opened a capture is still visible to the counters this frame; the
    // capture only starts listening on the next one.
    if (g_capture.row >= 0) {
        g_capture.armed = true;
    }
}

void advance_animation()
{
    const float step = menu_slide_step();
    if (g_closing) {
        g_slide = std::max(0.0f, g_slide - step);
    }
    else {
        g_slide = std::min(1.0f, g_slide + step);
    }
    if (g_switch_to) {
        g_switch = std::min(1.0f, g_switch + step);
        if (g_switch >= 1.0f) {
            remember_category_scroll();
            show_category(*g_switch_to);
            g_switch_to.reset();
        }
    }
    else {
        g_switch = std::max(0.0f, g_switch - step);
    }
}

bool exit_animation_done()
{
    return g_slide <= 0.0f;
}

} // namespace

void settings_menu_enter()
{
    g_closing = false;
    g_prev_text_focus = 0;
    // The menu reopens on the category and scroll position it was left at.
    g_content_scroll = g_category_scroll[static_cast<std::size_t>(g_category)];
    g_search_holds_panel = false;
    g_scrolled_to_focus = 0;
    g_slide = 0.0f;
    g_switch = 0.0f;
    g_switch_to.reset();
    g_search.clear();
    g_modal = Modal::None;
    g_desc_custom.clear();
    g_capture = BindCapture{};
    g_bind_context = BindContextId::Global;
    g_bind_device = BindDevice::Keyboard;
    // The live ControlConfig has to be the true Global for as long as the menu can edit or save
    // it, so no context may stay applied over it while we are open. A close that never polled
    // is_closed leaves the flag set; drop that suspend first so entry is always balanced.
    bind_resume_contexts();
    bind_suspend_contexts();
    g_weapon_pref_sel = -1;
    g_picker_row = nullptr;
    g_picker = nullptr;
    g_picker_scroll = 0.0f;
    g_color_pick = ColorPick{};
    g_baseline.clear();
    for (const SettingRow& row : settings_registry_all()) {
        g_baseline.emplace(&row, setting_get_as_string(row));
    }
    capture_session_baseline();
    g_changed.clear();
    // The context first: reset() releases a text focus, and a stale commit callback would still
    // be holding a pointer into g_row_state.
    g_ui.reset();
    g_row_state.clear();
    g_ui.style = settings_menu_style();
    g_ui.set_hover_sound_enabled(true);
}

void settings_menu_do_frame(bool paused)
{
    advance_animation();

    // paused = 1 means a stock popup owns input this frame; it renders after us and on top.
    if (!paused && !g_closing) {
        settings_menu_input();
    }

    rf::gr::reset_clip();
    rf::ui::menu_draw_background();

    int x = 0;
    int y = 0;
    int z = 0;
    rf::mouse_get_pos(x, y, z);
    g_ui.set_mouse_pos(x, y);
    g_ui.begin_pass(UiPass::Draw);
    settings_menu_build(g_ui);
    g_ui.end_pass();
    g_ui.draw_overlays();
    g_ui.draw_toasts(menu_font(false, 17.0f, menu_screen_scale()));
    rf::gr::reset_clip();
}

void settings_menu_abandon()
{
    bind_resume_contexts();
}

bool settings_menu_is_closed()
{
    // A queued state change never runs our frame again, so it cannot wait for the exit animation.
    if (rf::gameseq_get_pending_state() != 0) {
        bind_resume_contexts();
        return true;
    }
    if (!g_closing) {
        return false;
    }
    if (exit_animation_done()) {
        // Every close path lands here; the flag makes the resume a no-op once the menu's suspend
        // has already been lifted, so a repeated poll cannot unbalance it.
        bind_resume_contexts();
        return true;
    }
    return false;
}
