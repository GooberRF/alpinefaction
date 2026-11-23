#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <common/utils/string-utils.h>
#include "column_menu.h"
#include "form_view.h"
#include "menu_shell.h"
#include "reference_data.h"
#include "settings_menu.h"
#include "settings_skin.h"
#include "ui_kit.h"
#include "../input/bind_context.h"
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
#include "../rf/localize.h"
#include "../rf/multi.h"
#include "../rf/os/console.h"
#include "../rf/player/control_config.h"
#include "../rf/player/player.h"
#include "../rf/ui.h"
#include "../rf/weapon.h"

namespace
{

static_assert(static_cast<int>(SettingCategory::_count) + 1 <= menu_panel_slots);

// The category column, top to bottom.
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
    .count = mp_character_count,
    .bitmap = [](int i) {
        return i >= 0 && i < mp_character_count() ? rf::mp_characters[i].image_handle : -1;
    },
    .caption = mp_character_caption,
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
UiId g_picker_scrolled_to = 0;

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
    std::vector<std::string> choices; // a string Enum row's
};

std::unordered_map<const SettingRow*, RowState> g_row_state;

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

FormTextSpec text_row_config(const SettingRow& row, const std::string& text)
{
    FormTextSpec cfg;
    const std::string_view id{row.id};
    if (id == "player_name") {
        cfg.filter = player_name_char_allowed;
        cfg.max_len = player_name_field_len;
        cfg.hint = "Player";
        cfg.warn = player_name_warning(text);
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

// The device the columns describe. Only the keyboard/mouse pair has a runtime; the other two,
// whose caps are inert, are not offered until 1.6 (see the device strip).
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
        case BindDevice::Controller: return "Controller support is not available in this build.";
        case BindDevice::VR: return "VR support is not available in this build.";
        default: return "Keyboard and mouse bindings.";
    }
}

bool bind_device_live()
{
    return g_bind_device == BindDevice::Keyboard;
}

// A row carries the OVERRIDE badge when the edited context holds its own values for it.
bool bind_is_override(int index)
{
    return bind_device_live() && g_bind_context != BindContextId::Global
        && bind_context_override(g_bind_context, index) != nullptr;
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
    for (int i = 0; i < total; ++i) {
        const rf::ControlConfigItem& item = cc->bindings[i];
        const BindOverride& then = g_bind_baseline.global[i];
        if (item.scan_codes[0] != then.scan_codes[0] || item.scan_codes[1] != then.scan_codes[1]
            || item.mouse_btn_id != then.mouse_btn_id || item.aux_btn_id != g_bind_baseline.global_aux[i]) {
            return false;
        }
    }
    for (int c = 1; c < static_cast<int>(g_bind_baseline.overrides.size()); ++c) {
        const auto& overrides = g_bind_baseline.overrides[c];
        for (int i = 0; i < static_cast<int>(overrides.size()); ++i) {
            const BindOverride* now = bind_context_override(static_cast<BindContextId>(c), i);
            const std::optional<BindOverride>& then = overrides[i];
            if (!now || !then) {
                if (now || then) {
                    return false;
                }
                continue;
            }
            if (now->scan_codes[0] != then->scan_codes[0] || now->scan_codes[1] != then->scan_codes[1]
                || now->mouse_btn_id != then->mouse_btn_id) {
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
    return BindOverride{{bind_inherit, bind_inherit}, bind_inherit};
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
    g_capture = BindCapture{};
}

void bind_begin_capture(UiContext& ctx, int index, bool mouse, UiId id)
{
    g_capture.row = index;
    g_capture.mouse = mouse;
    g_capture.armed = false;
    g_capture.id = id;
    ctx.begin_key_capture(id);
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
    if (ctx.take_key_capture(g_capture.id, scan, mouse, cancelled)) {
        if (!cancelled) {
            bind_assign(ctx, g_capture.row, scan, mouse);
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

// MP / SP / HOST say where a setting applies, not whether it can be edited.
int collect_badges(const SettingRow& row, FormBadge* out, int max_badges)
{
    int n = 0;
    const auto add = [&](const char* text, rf::Color color, const char* icon, bool lamp) {
        if (n < max_badges) {
            out[n++] = FormBadge{text, color, icon, lamp};
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
    FormTree tree = FormTree::None;
};

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
            entries.push_back(Entry{EntryKind::SubHeader, last_sub, nullptr, nullptr, -1, form_sub_header_h});
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
        Entry entry{EntryKind::Row, nullptr, row, nullptr, -1, skin::row_h};
        if (row->sub_option_shown) {
            entry.tree = FormTree::Branch;
        }
        entries.push_back(entry);
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].tree == FormTree::Branch && (i + 1 == entries.size() || entries[i + 1].tree == FormTree::None)) {
            entries[i].tree = FormTree::Last;
        }
    }
    return entries;
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
                                        nullptr, nullptr, -1, form_sub_header_h});
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
    if (bind_device_live()) {
        bool captioned = false;
        const int total = bind_context_binding_count();
        for (int i = 0; i < total; ++i) {
            const std::string_view name{bind_action_name(i)};
            const auto word_matches = [&](std::string_view word) { return string_icontains(name, word); };
            if (name.empty() || !all_words_match(query, word_matches)) {
                continue;
            }
            if (!captioned) {
                entries.push_back(Entry{EntryKind::SubHeader, "Key bindings", nullptr, nullptr, -1, form_sub_header_h});
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
void do_picker_row_widget(UiContext& ctx, const SettingRow& row, const GridPicker& picker, const FormLine& line,
                          const Fonts& fonts, UiId id)
{
    const int index = static_cast<int>(std::lround(row_float(row, -1.0f)));
    const char* caption = index >= 0 && index < picker.count() ? picker.caption(index) : nullptr;
    if (form_select_box(ctx, line, id, caption && caption[0] != '\0' ? caption : "-", fonts.body)) {
        g_picker = &picker;
        g_picker_row = &row;
        g_picker_scroll = 0.0f;
        g_picker_scrolled_to = 0;
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
void do_action_choice_widget(UiContext& ctx, const SettingRow& row, const FormLine& line, int font, UiId id)
{
    const ActionChoices& choices = action_choices();
    const int value = static_cast<int>(std::lround(row_float(row, -1.0f)));
    const auto found = std::find(choices.actions.begin(), choices.actions.end(), value);
    int pos = found != choices.actions.end() ? static_cast<int>(found - choices.actions.begin()) : -1;
    if (form_dropdown(ctx, line, id, choices.items, pos, font)) {
        write_row(row, std::to_string(choices.actions[pos]));
    }
}

void do_row_widget(UiContext& ctx, const SettingRow& row, const FormLine& line, const Fonts& fonts,
                   int slider_readout_w, bool sync_pass)
{
    const bool enabled = line.enabled;
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
            bool shown = inverted ? !stored : stored;
            if (form_toggle(ctx, line, id, shown, font)) {
                write_row(row, (inverted ? !shown : shown) ? "1" : "0");
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
                form_slider(ctx, line, id, state.f, lo, hi, integral && step <= 0.0f ? 1.0f : step,
                            presentation.ui_nudge, fmt, font, slider_readout_w);
            }
            else {
                // No bounded range to draw a track for.
                form_stepper(ctx, line, id, state.f, -1.0e9f, 1.0e9f, 1.0f, fmt, font, integral);
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
            form_stepper(ctx, line, id, state.f, lo, hi, step > 0.0f ? step : 1.0f, row_number_format(row), font,
                         integral);
            commit_row_float(ctx, row, state, enabled, integral);
            break;
        }
        case SettingWidget::Enum: {
            if (row.string_choices) {
                // Listed once a visit to the menu, as the list can take a packfile search.
                if (first_seen) {
                    state.choices = row.string_choices();
                }
                const std::string value = setting_get_as_string(row);
                const auto it = std::ranges::find_if(state.choices, [&](const std::string& choice) {
                    return string_iequals(choice, value);
                });
                int pos = it == state.choices.end() ? -1 : static_cast<int>(it - state.choices.begin());
                if (form_choice(ctx, line, id, state.choices, pos, font)) {
                    write_row(row, state.choices[pos]);
                }
                break;
            }
            if (const GridPicker* picker = picker_for_row(row)) {
                do_picker_row_widget(ctx, row, *picker, line, fonts, id);
                break;
            }
            if (row.flags & SF_ACTION_CHOICE) {
                do_action_choice_widget(ctx, row, line, font, id);
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
                form_stepper(ctx, line, id, state.f, lo, hi, 1.0f, UiNumberFormat{}, font, true);
                commit_row_float(ctx, row, state, enabled, true);
                break;
            }
            // A row may hide values the hardware cannot do (MSAA levels), so the widget walks
            // the offered subset rather than the raw label array.
            const std::vector<int> offered = setting_enum_offered(row);
            const int count = static_cast<int>(offered.size());
            const int value = static_cast<int>(std::lround(row_float(row,
                static_cast<float>(setting_enum_value(row, offered[0])))));
            int pos = 0;
            for (int i = 0; i < count; ++i) {
                if (setting_enum_value(row, offered[i]) == value) {
                    pos = i;
                    break;
                }
            }
            std::vector<std::string> items;
            items.reserve(count);
            for (const int i : offered) {
                items.emplace_back(row.enum_labels[i]);
            }
            if (form_choice(ctx, line, id, items, pos, font)) {
                write_row(row, std::to_string(setting_enum_value(row, offered[pos])));
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
            const UiRect r = form_control_rect(ctx, line, skin::control_w, 0.0f);
            if (skin_swatch(ctx, r, state.color, fonts.caption, enabled, id, line.hot)) {
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
            form_stepper(ctx, line, id, state.f, 0.1f, 10.0f, 0.05f, UiNumberFormat{2, 1.0f, ""}, font, false);
            commit_row_float(ctx, row, state, enabled, false);
            break;
        }
        case SettingWidget::Text: {
            if (sync_state) {
                state.text = setting_get_as_string(row);
            }
            // The buffer is already up to date here: UiContext::apply_pending_keys ran at the top
            // of the pass, so the validation below sees exactly what the field will draw.
            // player_name commits through the engine and notifies the server, so never per keystroke.
            const SettingRow* target = &row;
            RowState* buffer = &state;
            form_text(ctx, line, id, state.text, text_row_config(row, state.text), fonts.body, g_prev_text_focus,
                      [target, buffer] {
                          if (text_row_config(*target, buffer->text).warn == nullptr) {
                              write_row(*target, buffer->text);
                          }
                      });
            break;
        }
        case SettingWidget::Action: {
            const std::string_view action{row.id};
            if (action == "bindings_reset") {
                // The control column's width, or the label plus the danger button's LED if wider.
                const int w = skin_button_width(ctx, "RESET BINDINGS", fonts.body_bold, skin::control_w - 30.0f)
                            + skin_px(ctx, 30.0f);
                const int h = skin_px(ctx, 44.0f);
                const UiRect r{line.ctl_right - w, line.rect.y + (line.rect.h - h) / 2, w, h};
                if (skin_button(ctx, r, "RESET BINDINGS", fonts.body_bold, SkinButton::Danger,
                                enabled && bind_device_live(), id)) {
                    g_modal = Modal::ResetBindings;
                    ui_play_click();
                }
            }
            break;
        }
        case SettingWidget::Label: {
            if (ctx.draw) {
                form_value_text(ctx, line, setting_get_as_string(row).c_str(), font);
            }
            break;
        }
        case SettingWidget::Keybind:
        case SettingWidget::ListReorder: {
            // Both expand into a block of their own inside their category; a search hit just
            // offers the jump.
            const UiRect r = form_control_rect(ctx, line, skin::control_w, 44.0f);
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

void do_row(UiContext& ctx, const SettingRow& row, const UiRect& row_rect, const char* crumb, FormTree tree,
            const Fonts& fonts, int slider_readout_w, bool sync_state)
{
    FormBadge badges[8];
    const int badge_count = ctx.draw ? collect_badges(row, badges, 8) : 0;
    const FormRow form{
        .rect = row_rect,
        .id = ui_id(row.id),
        .label = row.label,
        .crumb = crumb,
        .enabled = row_widget_enabled(row),
        .modified = setting_is_modified_from_default(row),
        .reset_id = ui_id("rowreset", desc_id_for(row)),
        .badges = std::span{badges, static_cast<std::size_t>(badge_count)},
        .desc_id = desc_id_for(row),
        .tree = tree,
    };
    const FormLine line = form_row(ctx, form, FormFonts{fonts.body, fonts.body_bold, fonts.caption});
    if (line.reset) {
        setting_reset_to_default(row);
        note_rows_changed();
        ui_play_click();
    }
    do_row_widget(ctx, row, line, fonts, slider_readout_w, sync_state);
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
    const int pad = skin_px(ctx, form_row_inset);
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
    rf::gr::string(d.x + skin_px(ctx, form_row_inset), dy, "ACTION", fonts.caption);
    rf::gr::string(d.x + (key_x - r.x), dy, col1, fonts.caption);
    rf::gr::string(d.x + (mouse_x - r.x), dy, col2, fonts.caption);
    rf::gr::set_color(skin::border_dim);
    rf::gr::rect(d.x + skin_px(ctx, form_row_inset), d.y + d.h - std::max(1, skin_px(ctx, 1.0f)),
                 d.w - 2 * skin_px(ctx, form_row_inset), std::max(1, skin_px(ctx, 1.0f)));
}

void do_bind_row(UiContext& ctx, const UiRect& r, int index, const Fonts& fonts)
{
    rf::ControlConfig* cc = bind_context_controls();
    if (!cc || index < 0 || index >= bind_context_binding_count()) {
        return;
    }
    const int pad = skin_px(ctx, form_row_inset);
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
        form_draw_row_backing(ctx, d, focused);
        rf::gr::set_color(form_label_color(true, hovered, focused));
        const int label_w = std::max(0, (key_x - r.x) - pad - skin_px(ctx, 12.0f));
        rf::gr::string(d.x + pad, d.y + (d.h - rf::gr::get_font_height(fonts.body)) / 2,
                       ui_fit_middle(bind_action_name(index), label_w, fonts.body).c_str(),
                       fonts.body);
    }

    // Context override affordance: gold chip plus the revert arrow, which drops the override and
    // puts the row back on its Global binding.
    if (bind_is_override(index)) {
        const int badge_h = skin_px(ctx, skin::badge_h);
        const int badge_w = skin_badge_width(ctx, "OVERRIDE", fonts.caption, false);
        const int icon = skin_px(ctx, skin::icon_size);
        const UiRect badge{key_x - badge_w - icon - skin_px(ctx, 20.0f),
                           r.y + (r.h - badge_h) / 2, badge_w, badge_h};
        skin_badge(ctx, badge, "OVERRIDE", skin::badge_override, fonts.caption, nullptr, false);
        const UiRect revert{badge.x + badge_w + skin_px(ctx, 6.0f), r.y + (r.h - icon) / 2, icon, icon};
        if (skin_icon_button(ctx, revert, "af_revert_icon", skin::amber, skin::ink_title,
                             ui_id("bindrevert", index))) {
            bind_context_clear_override(g_bind_context, index);
            bind_note_changed();
            ui_play_click();
        }
    }

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
    const int pad = skin_px(ctx, form_row_inset);
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
        rf::gr::set_color(form_label_color(enabled, hovered, false));
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
        case SettingCategory::Bindings: return "Key and mouse bindings for every action.";
        case SettingCategory::Input: return "Mouse, keyboard and scope behavior.";
        case SettingCategory::Gameplay: return "Single-player options, view, autoswitch, gore, weapons and vehicles.";
        case SettingCategory::Multiplayer: return "Identity, handicap, sprays, hosting and FactionFiles account.";
        default: return "";
    }
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
            if (g_bind_context != BindContextId::Global) {
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

void build_modal(UiContext& ctx, const Fonts& fonts)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };

    const ModalCopy copy = modal_copy();
    const std::string& body = copy.body;
    const std::string& note = copy.note;
    const char* confirm = copy.confirm;

    const MenuDialog frame = menu_dialog(ctx, s(760.0f), s(280.0f), copy.title, fonts.section);
    const UiRect& dialog = frame.frame;
    const UiRect& band = frame.band;
    const int pad = s(14.0f);
    const UiRect body_box{dialog.x + pad, band.y + band.h + s(12.0f), dialog.w - 2 * pad, s(120.0f)};
    const int btn_h = s(44.0f);
    const int btn_y = body_box.y + body_box.h + s(14.0f);
    const int confirm_w = skin_button_width(ctx, confirm, fonts.body_bold, 260.0f) + s(30.0f);
    const int cancel_w = skin_button_width(ctx, "CANCEL", fonts.body_bold, 180.0f);
    const UiRect confirm_rect{dialog.x + dialog.w - pad - confirm_w, btn_y, confirm_w, btn_h};
    const UiRect cancel_rect{confirm_rect.x - s(16.0f) - cancel_w, btn_y, cancel_w, btn_h};

    if (ctx.draw) {
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

void close_modal(UiContext& ctx)
{
    // A picker opens from a click on its row, which pinned the row's description; as a dropdown's list does, it takes
    // the pin with it.
    if (g_modal == Modal::GridPicker || g_modal == Modal::ColorPicker) {
        ctx.description().unpin();
    }
    g_modal = Modal::None;
}

void build_grid_picker_modal(UiContext& ctx, const Fonts& fonts)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    const GridPicker& picker = *g_picker;
    const int count = picker.count();

    const MenuDialog frame = menu_dialog(ctx, s(1000.0f), s(760.0f), picker.title, fonts.section);
    const UiRect& dialog = frame.frame;
    const UiRect& band = frame.band;
    const int pad = s(24.0f);
    const int footer_h = s(68.0f);
    const int top = band.y + band.h + s(12.0f);
    const UiRect grid_view{dialog.x + pad, top, dialog.w - 2 * pad, dialog.y + dialog.h - footer_h - pad - top};

    if (ctx.draw) {
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
    const SkinPickerGrid grid{
        .count = count,
        .selected = selected,
        .tile_w = picker.tile_w,
        .tile_h = picker.tile_h,
        .bitmap = picker.bitmap,
        .caption = picker.caption,
        .caption_line = picker.caption_line,
        .font = fonts.caption,
        .min_cols = 2,
        .max_cols = 6,
        .id_name = "picker",
    };
    const int chosen = skin_picker_grid(ctx, grid_view, grid, g_picker_scroll, g_picker_scrolled_to);

    const int btn_h = s(44.0f);
    const UiRect close{dialog.x + dialog.w - pad - s(200.0f), dialog.y + dialog.h - pad - btn_h,
                       s(200.0f), btn_h};
    if (skin_button(ctx, close, "CLOSE", fonts.body_bold, SkinButton::Secondary, true,
                    ui_id("picker_close"))) {
        close_modal(ctx);
        ui_play_click();
    }

    if (chosen >= 0 && g_picker_row) {
        write_row(*g_picker_row, std::to_string(chosen));
        ui_play_click();
        close_modal(ctx);
    }
}

constexpr uint32_t color_presets[] = {
    0xFFFFFFFFu, 0xC0C0C0FFu, 0x808080FFu, 0x404040FFu, 0x000000FFu, 0xFF0000FFu,
    0xFF8000FFu, 0xFFFF00FFu, 0x00FF00FFu, 0x00FFFFFFu, 0x0080FFFFu, 0xFF00FFFFu,
};

// Hue/saturation/value plus alpha, with the old and new colours side by side. Nothing is written
// until APPLY; CANCEL and ESC leave the row as it was.
void build_color_picker_modal(UiContext& ctx, const Fonts& fonts)
{
    const auto s = [&](float v) { return skin_px(ctx, v); };
    ColorPick& pick = g_color_pick;

    const MenuDialog frame = menu_dialog(ctx, s(900.0f), s(540.0f), "SELECT COLOR", fonts.section);
    const UiRect& dialog = frame.frame;
    const UiRect& band = frame.band;
    const int pad = s(24.0f);
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
        close_modal(ctx);
        ui_play_click();
    }
    if (skin_button(ctx, apply_rect, "APPLY", fonts.body_bold, SkinButton::Secondary, true,
                    ui_id("color_apply"))) {
        if (!pick.unset) {
            write_row(*pick.row, format_hex_color_string(pick.rgba));
        }
        close_modal(ctx);
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
    // Action buttons carry enable conditions of their own; a stand-in for a button that then
    // refuses focus would drop the cursor back to the top.
    if (entry.kind == EntryKind::Row
        && (!row_widget_enabled(*entry.row) || entry.row->widget == SettingWidget::Action)) {
        return;
    }
    if (entry.kind == EntryKind::Bind && (!bind_device_live() || g_capture.row >= 0)) {
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
    // A short On / Off cycler: the rows' column width does not fit beside the search box.
    const SkinCyclerRects adv = skin_header_cycler_rects(ctx, line, adv_text, 80.0f, fonts.body);
    const UiRect search{adv.label.x - s(28.0f) - s(340.0f), line.y + (line.h - s(44.0f)) / 2, s(340.0f), s(44.0f)};

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
    if (skin_labeled_cycler(ctx, adv.label, adv.toggle, adv_text, show_advanced ? "On" : "Off", fonts.body, true,
                            ui_id("show_advanced"), skin::ink_title, skin::ink_status) != 0) {
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

    // Controller and VR bindings are planned for 1.6; until then the keyboard alone is offered, which needs no strip.
    // constexpr int dev_count = static_cast<int>(BindDevice::_count);
    constexpr int dev_count = 1;
    static const char* const dev_long[] = {"KEYBOARD", "CONTROLLER", "VR"};
    static const char* const dev_short[] = {"KB", "PAD", "VR"};
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
    // A single device needs no strip.
    bool show_devices = dev_count > 1;
    int ctx_left = show_devices ? avail_x0 + dev_total + strip_gap : avail_x0;
    if (show_devices && ctx_left + std::min(ctx_total, ctx_min * ctx_count) > avail_x1) {
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
    const int crumb_h = form_crumb_line_h(ctx, fonts.caption);
    // A crumb's line takes the font's own height, which need not track the reference canvas.
    const auto entry_h = [&](const Entry& entry) { return s(entry.ref_h) + (entry.crumb ? crumb_h : 0); };
    int content_h = 0;
    for (const Entry& entry : entries) {
        content_h += entry_h(entry);
    }

    FormScroll scroll = form_scroll_begin(ctx, view, content_h, g_content_scroll, row_h * 3, ui_id("content"));
    const int inner_w = scroll.inner_w;
    const int slider_readout_w = panel_slider_readout_w(ctx, entries,
        searching ? std::nullopt : std::optional{g_category}, fonts.body);
    int y = scroll.region.start_y();
    for (const Entry& entry : entries) {
        const int h = entry_h(entry);
        const UiRect rect{view.x, y, inner_w, h};
        y += h;
        if (entry_owns_focus(entry, ctx.focus_id())) {
            scroll.focus_rect = rect;
        }
        if (form_scroll_culled(scroll, rect)) {
            // Still reachable by keyboard: the focus can step onto it and scroll it into view.
            register_offscreen_focus(ctx, entry);
            continue;
        }
        switch (entry.kind) {
            case EntryKind::SubHeader:
                form_draw_sub_header(ctx, rect, entry.header, fonts.body_bold);
                break;
            case EntryKind::Row:
                do_row(ctx, *entry.row, rect, entry.crumb, entry.tree, fonts, slider_readout_w, sync_state);
                break;
            case EntryKind::BindHeader:
                draw_bind_header(ctx, rect, fonts);
                break;
            case EntryKind::BindGroup:
                form_draw_sub_header(ctx, rect, entry.header, fonts.body_bold);
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
        rf::gr::string(d.x + s(form_row_inset), d.y + s(24.0f), empty.c_str(), fonts.body);
    }

    form_scroll_end(ctx, scroll, g_scrolled_to_focus);
}

void draw_description_strip(UiContext& ctx, const UiRect& desc, const Fonts& fonts)
{
    // A hovered or pinned registry row, else the category summary. Blocks that are not
    // registry rows supply their own line.
    const SettingRow* desc_row = desc_row_from_id(ctx.description().id);
    std::string text;
    if (!g_desc_custom.empty()) {
        text = g_desc_custom;
    }
    else if (desc_row) {
        text = desc_row->tooltip ? desc_row->tooltip : "";
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
    form_draw_description(ctx, desc, text, fonts.caption);
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
                    !searching && (bindings ? bind_device_live() : category_has_resettable_rows(g_category)),
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
    const MenuFrame frame = menu_frame(ctx, g_slide);
    const MenuLayout& lay = frame.lay;
    const auto s = [&](float v) { return ctx.scaled(v); };
    const auto cy = [&](float v) { return canvas_y + s(v); };
    const int panel_dx = frame.enter_dx + static_cast<int>(g_switch * static_cast<float>(frame.slide_w));
    const int top = frame.top;

    if (ctx.draw) {
        menu_draw_title_bar(ctx, lay.panel_x + frame.enter_dx, lay.panel_r + frame.enter_dx, canvas_y,
                            menu_stock_label(rf::strings::options).c_str());
    }

    build_category_column(ctx, lay, lay.column_x + frame.col_dx, top, fonts);

    const MenuPanel panel = menu_panel(ctx, lay, panel_dx, top, canvas_y);
    if (ctx.draw) {
        menu_draw_panel(ctx, panel);
        // Search results belong to no category, so nothing is connected.
        if (g_search.empty()) {
            menu_draw_connector(ctx, panel,
                                top + menu_slot_mid_y(ctx, MenuButtonSize::Small, category_slot(g_category)));
        }
    }
    // A panel on its way out is only painted: anything done on it would land on the category
    // about to replace it.
    if (!ctx.draw && g_switch_to) {
        return;
    }

    const int content_x = panel.content_x;
    const int content_r = panel.content_r;
    const UiRect& header = panel.header;
    const int title_right = build_header_controls(ctx, header, fonts);
    const bool searching = !g_search.empty();

    std::vector<std::string> crumbs;
    int match_count = 0;
    const std::vector<Entry> entries = searching
        ? build_search_entries(g_search, crumbs, match_count)
        : build_category_entries(g_category);

    if (ctx.draw) {
        draw_header_title(ctx, header, title_right, searching, match_count, fonts);
        menu_draw_panel_ridge(ctx, panel);
    }

    // The header line has no room left beside the search box, so Bindings takes a second one.
    float view_top = menu_panel_body_y;
    if (!searching && g_category == SettingCategory::Bindings && bind_context_controls() != nullptr) {
        build_bindings_header(ctx, UiRect{content_x, cy(menu_panel_body_y), content_r - content_x, s(36.0f)}, fonts);
        view_top = menu_panel_body_y + 44.0f;
    }
    else if (!ctx.draw) {
        // No strips on screen means no way to see or change what is being edited, so the edit
        // target may not survive a search or a trip through another category.
        g_bind_context = BindContextId::Global;
        g_bind_device = BindDevice::Keyboard;
    }

    const FormPanelAreas areas = form_panel_areas(ctx, panel, true);
    // From cy() rather than the body's y plus an offset, which can round a pixel apart.
    const int body_y = cy(view_top);
    const UiRect body{areas.body.x, body_y, areas.body.w, areas.body.y + areas.body.h - body_y};
    build_content(ctx, form_rows_view(ctx, body), entries, searching, fonts, sync_state);
    if (ctx.draw) {
        draw_description_strip(ctx, areas.desc, fonts);
    }
    build_panel_footer(ctx, areas.footer, searching, fonts);
}

// The menus above, `crumbs` from menu_crumbs, then Options and the panel's category, or its search.
std::vector<std::string> frame_path(std::vector<std::string> crumbs)
{
    std::vector<std::string> path = std::move(crumbs);
    path.push_back(menu_stock_label(rf::strings::options));
    path.push_back(g_search.empty() ? string_to_upper(setting_category_name(g_category)) : "SEARCH");
    return path;
}

// The menus above Options, and the player, unless a dialog or a listening key cap takes the clicks or the menu is
// sliding.
MenuStripLinks frame_links(Modal modal, std::size_t crumbs)
{
    if (modal != Modal::None || g_closing || g_slide < 1.0f || g_capture.row >= 0 || rf::ui::popup_is_active()) {
        return {};
    }
    return MenuStripLinks{crumbs, menu_player_reachable()};
}

// A dialog dims the hint bar, and takes the keys itself. The exit animation takes none.
std::vector<MenuHint> frame_hints(const UiContext& ctx, Modal modal)
{
    std::vector<MenuHint> hints;
    if (rf::ui::popup_is_active() || g_closing) {
        return hints;
    }
    // A listening key cap takes every key, Esc as its cancel.
    if (g_capture.row >= 0) {
        hints.push_back({"ESC", "Cancel"});
    }
    else if (modal == Modal::None) {
        menu_add_nav_hints(ctx, hints, g_search.empty() ? "Back" : "Clear search");
    }
    return hints;
}

void settings_menu_build(UiContext& ctx)
{
    const float scale = menu_screen_scale();
    ctx.set_scale(scale);
    const int canvas_y = (rf::gr::clip_height() - static_cast<int>(menu_ref_h * scale)) / 2;

    Fonts fonts;
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
        // Under the dialogs and their dim.
        if (ctx.draw) {
            const std::vector<std::string> crumbs = menu_crumbs({});
            menu_draw_frame(ctx, frame_path(crumbs), frame_hints(ctx, modal), frame_links(modal, crumbs.size()));
        }
    }
    if (modal == Modal::GridPicker) {
        if (g_picker) {
            build_grid_picker_modal(ctx, fonts);
        }
    }
    else if (modal == Modal::ColorPicker) {
        if (g_color_pick.row) {
            build_color_picker_modal(ctx, fonts);
        }
    }
    else if (modal != Modal::None) {
        build_modal(ctx, fonts);
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
            close_modal(g_ui);
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
    ui_drop_text_focus_under_console(g_ui);
    if (rf::console::console_is_visible() && g_capture.row >= 0) {
        g_ui.cancel_key_capture();
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

    // As drawn last frame, before the pass's clicks change it.
    const std::vector<std::string> crumbs = menu_crumbs({});
    const std::vector<std::string> path = frame_path(crumbs);
    const MenuStripLinks links = frame_links(g_modal, crumbs.size());
    g_ui.begin_frame(x, y, capturing ? 0 : wheel, !capturing && rf::mouse_button_is_down(0),
        !capturing && rf::mouse_was_button_pressed(0) != 0,
        !capturing && rf::mouse_was_button_pressed(1) != 0);
    g_ui.begin_pass(UiPass::HitTest);
    settings_menu_build(g_ui);
    g_ui.end_pass();
    write_committed_values();

    // A step of the path, or the player, goes back up the menus, Options closing as for Back.
    if (const int link = menu_strip_click(g_ui, path, links); link != -1) {
        if (link == menu_strip_player) {
            column_menu_jump_to_player();
        }
        else {
            column_menu_jump_to_step(static_cast<std::size_t>(link));
        }
        ui_play_click();
        request_close();
    }

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
    g_picker_scrolled_to = 0;
    g_color_pick = ColorPick{};
    g_baseline.clear();
    for (const SettingRow& row : settings_registry_all()) {
        g_baseline.emplace(&row, setting_get_as_string(row));
    }
    capture_session_baseline();
    g_changed.clear();
    // The context first: reset() releases a text focus, and a stale commit callback would still
    // be holding a pointer into g_row_state.
    menu_ui_reset(g_ui);
    g_row_state.clear();
}

void settings_menu_do_frame(bool paused)
{
    advance_animation();

    // paused = 1 means a stock popup owns input this frame; it renders after us and on top.
    if (!paused && !g_closing) {
        settings_menu_input();
    }

    menu_run_draw(g_ui, settings_menu_build);
    // Below the path strip.
    g_ui.draw_toasts(menu_font(false, 17.0f, menu_screen_scale()), skin_px(g_ui, menu_strip_h));
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
