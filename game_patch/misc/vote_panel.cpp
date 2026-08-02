#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <common/utils/list-utils.h>
#include <xlog/xlog.h>
#include "vote_panel.h"
#include "player.h"
#include "../hud/hud_internal.h"
#include "../hud/remote_server_cfg_ui.h"
#include "../input/control_input_filter.h"
#include "../multi/alpine_packets.h"
#include "../multi/vote_client.h"
#include "../rf/gameseq.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/input.h"
#include "../rf/multi.h"
#include "../rf/player/control_config.h"
#include "../rf/player/player.h"
#include "../rf/sound/sound.h"
#include "../rf/ui.h"
#include "../sound/sound.h"

namespace
{

// True while the overlay is up. The overlay is the only way the panel is ever
// shown, so this doubles as "the panel owns input and rendering this frame".
bool g_open = false;

// Player's local "filter the map list by gametype" choice. Session only, not
// persisted; ignored (forced on) while the server enforces the prefix.
bool g_filter_gametype_local = false;

// Description area: hover drives it, a toggle pins it until the next hover.
bool g_hovered_mutator_this_pass = false;
bool g_description_pinned = false;

// Filtered + sorted + deduped display rows for the level list, plus the resolved
// selection row. Both panel passes in a frame reuse this; it is rebuilt only when
// one of its inputs actually changes (see level_list_fingerprint).
struct LevelListCache
{
    bool valid = false;
    uint32_t levels_fp = 0;
    bool filter_active = false;
    uint8_t gametype = af_vote_gametype_none;
    bool allow_current = false;

    std::vector<std::string> items; // display rows, pinned "Current level" included
    int first_level_row = 0;
    int gametype_hidden = 0;    // dropped by the gametype mask (opt-in or enforced)
    int not_allowed_hidden = 0; // dropped by allowed_for_vote (always applied)

    // Selection row resolved from the stored name, cached against it.
    bool sel_valid = false;
    std::string sel_name;
    bool sel_manual = false;
    int sel_row = -1;
};

LevelListCache g_level_cache;

// Allocation-free hash of everything the level display depends on. Filename
// length plus its first/last byte stands in for the whole name: a real map-list
// change cannot preserve all of that alongside the flags.
uint32_t level_list_fingerprint(const VoteOptionsData& options)
{
    uint32_t h = 2166136261u; // FNV-1a
    const auto mix = [&h](uint32_t v) {
        h = (h ^ v) * 16777619u;
    };
    mix(static_cast<uint32_t>(options.levels.size()));
    for (const auto& level : options.levels) {
        mix(level.natural_gametype);
        mix(level.valid_gametype_mask);
        mix(level.allowed_for_vote ? 1u : 0u);
        mix(static_cast<uint32_t>(level.filename.size()));
        if (!level.filename.empty()) {
            mix(static_cast<uint8_t>(level.filename.front()));
            mix(static_cast<uint8_t>(level.filename.back()));
        }
    }
    return h;
}

// Gameplay overlay needs the cursor back and mouse-look off; both are restored
// on every close, including the forced ones (level change, disconnect).
bool g_gameplay_mouse_overridden = false;
bool g_gameplay_prev_mouse_look = false;

// RF latches mouse presses in a per-frame counter (0x018853D4) that
// control_config_check_pressed reads WITHOUT consuming (0x0051E590 / 0x0051E5D0
// are plain reads), while control_is_control_down polls live button state. Our
// attack suppression keys on the panel being open, so the click that dismissed
// the panel was still pending for the remainder of that same frame and fired the
// weapon once gameplay polled its controls later in the frame.
// Armed on every gameplay-context close and held until a later frame sees every
// button released, so it covers a click-and-hold and a press+release inside one
// frame alike. Counts frames rather than latching a bool so it cannot stick.
int g_swallow_attack_frames = 0;

// Only attacks are affected; movement is never blocked.
void vote_panel_decay_attack_swallow()
{
    if (g_swallow_attack_frames <= 0) {
        return;
    }
    if (rf::mouse_button_is_down(0) || rf::mouse_button_is_down(1)) {
        return; // still held: keep swallowing
    }
    --g_swallow_attack_frames;
}

void gameplay_overlay_apply_mouse(bool active)
{
    rf::Player* const player = rf::local_player;

    if (active) {
        if (player && !g_gameplay_mouse_overridden) {
            g_gameplay_prev_mouse_look = player->settings.controls.mouse_look;
            g_gameplay_mouse_overridden = true;
        }
        if (player) {
            player->settings.controls.mouse_look = false;
        }
        if (rf::keep_mouse_centered) {
            rf::mouse_keep_centered_disable();
        }
        rf::mouse_set_visible(true);
        return;
    }

    // mouse_look is a persisted player setting: only drop the override once the
    // saved value is actually back. If the player object is gone (disconnect,
    // kick, level change) keep it pending so the next apply can still restore it
    // rather than losing the user's setting permanently.
    if (g_gameplay_mouse_overridden) {
        if (!player) {
            return;
        }
        player->settings.controls.mouse_look = g_gameplay_prev_mouse_look;
        g_gameplay_mouse_overridden = false;
    }
    // Only re-grab the cursor if we are actually still in gameplay; on a level
    // change the engine owns the mouse mode.
    if (rf::gameseq_get_state() == rf::GS_GAMEPLAY && !rf::keep_mouse_centered) {
        rf::mouse_keep_centered_enable();
        rf::mouse_set_visible(false);
    }
}

// ---------------------------------------------------------------------------
// Mutator descriptions (client-local, keyed by the canonical name the server
// sends in the vote-options blob).
// ---------------------------------------------------------------------------

struct MutatorDescription
{
    const char* name;
    const char* text;
};

// Kept short enough to wrap into the description area's three lines; verified
// against rfpc-medium.vf metrics at the panel's reference scale.
const MutatorDescription mutator_descriptions[] = {
    {"instagib",
     "Spawn with Rail. One-hit kills, unlimited ammo, no reloads, no weapon switching, no pickups."},
    {"rails",
     "Spawn with Baton. Weapon/ammo pickups based on the specified weapon. No other pickups."},
    {"arena",
     "Spawn with AR and 100/100. Auto reload and reset to 100/100 on a frag. Weapon pickups only."},
    {"vampire",
     "Damage enemies to regenerate health and armor."},
};

const char* mutator_description_for(std::string_view name)
{
    for (const auto& entry : mutator_descriptions) {
        if (name == entry.name) {
            return entry.text;
        }
    }
    return "No description available.";
}

// ---------------------------------------------------------------------------
// Vote types
// ---------------------------------------------------------------------------

struct VoteTypeInfo
{
    AfVoteType type;
    const char* label;
    const char* description;
};

// Display order for the vote type buttons (user-specified); enabled_vote_types()
// preserves it and the sequence flows up to fill gaps left by disabled types.
const VoteTypeInfo vote_type_infos[] = {
    {AfVoteType::Level, "Level", "Load a specific level, optionally with a game type and mutators."},
    {AfVoteType::Restart, "Restart", "Restart the level that is currently loaded."},
    {AfVoteType::Next, "Next", "Load the next level in the server's rotation."},
    {AfVoteType::Previous, "Previous", "Load the previous level in the server's rotation."},
    {AfVoteType::Random, "Random", "Load a random level from the server's rotation."},
    {AfVoteType::Extend, "Extend", "Extend the time limit of the round in progress."},
    {AfVoteType::Match, "Match", "Set up a match on the chosen level with the chosen team size."},
    {AfVoteType::CancelMatch, "Cancel Match", "Cancel the match that is currently set up."},
    {AfVoteType::Kick, "Kick", "Kick the selected player from the server."},
};

// Short tag for the game type selector. The blob carries full display names
// ("Damage Control") which overflow the selector at low resolutions.
// Deliberately not multi_game_type_name_short(): that maps every unrecognised id
// to "TDM" and logs a warning, whereas a game type this build predates has to
// fall back to the server-sent name.
const char* gametype_short_name(uint8_t id)
{
    switch (static_cast<rf::NetGameType>(id)) {
        case rf::NG_TYPE_DM: return "DM";
        case rf::NG_TYPE_CTF: return "CTF";
        case rf::NG_TYPE_TEAMDM: return "TDM";
        case rf::NG_TYPE_KOTH: return "KOTH";
        case rf::NG_TYPE_DC: return "DC";
        case rf::NG_TYPE_REV: return "REV";
        case rf::NG_TYPE_RUN: return "RUN";
        case rf::NG_TYPE_ESC: return "ESC";
        case rf::NG_TYPE_BAG: return "BAG";
        case rf::NG_TYPE_TBAG: return "TBAG";
        case rf::NG_TYPE_PIT: return "PIT";
        case rf::NG_TYPE_WO: return "WO";
        case rf::NG_TYPE_GG: return "GG";
        case rf::NG_TYPE_SAL: return "SAL";
        default: return nullptr; // unknown to this build
    }
}

std::vector<const VoteTypeInfo*> enabled_vote_types()
{
    std::vector<const VoteTypeInfo*> out;
    for (const auto& info : vote_type_infos) {
        if (vote_options_is_type_enabled(info.type)) {
            out.push_back(&info);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Panel state
// ---------------------------------------------------------------------------

struct PanelOptionValue
{
    bool bool_value = false;
    uint8_t choice_index = 0;
    int32_t int_value = 0;
    float float_value = 0.0f;
};

struct PanelMutatorSelection
{
    bool enabled = false;
    std::vector<PanelOptionValue> options;
};

struct FormState
{
    bool built = false;
    // Identity of the schema this form was built for. Sizes alone miss a blob
    // that keeps its counts but reorders/renames mutators or changes an option
    // list, which would leave stale values to be sent against different options.
    uint32_t built_fingerprint = 0;

    int type_index = 0;
    int kick_index = 0;
    std::string level_selection; // level NAME; empty = the "Current level" row
    bool manual_level = false;
    std::string manual_level_name;
    int gametype_index = 0; // 0 = server default
    int team_size = 4;
    int extend_minutes = af_vote_extend_default_minutes;
    std::vector<PanelMutatorSelection> mutators;
    int description_mutator = -1;

    float level_scroll = 0.0f;
    float kick_scroll = 0.0f;
    float mutator_scroll = 0.0f;
};

struct KickCandidate
{
    uint8_t id = 0xFF;
    std::string name;
};

// Player list snapshot for the Kick form: refreshed on the hit-test pass and
// reused by the draw pass in the same frame.
std::vector<KickCandidate> g_kick_cache;
std::vector<std::string> g_kick_names;

FormState g_form;

// Popup input targets. The stock popup callback takes no arguments, so the
// widget that opened it has to be remembered here.
int g_popup_mutator = -1;
int g_popup_option = -1;
// Indices alone are unsafe: a popup can outlive a forced close, or the blob can
// refresh between opening it and pressing OK, which would write the typed value
// into a different option. The ids are re-checked on apply.
uint8_t g_popup_mutator_id = 0;
uint8_t g_popup_option_id = 0;

void clear_popup_target()
{
    g_popup_mutator = -1;
    g_popup_option = -1;
}

enum class PanelMode
{
    NotSupported,
    Loading,
    Form,
};

// ---------------------------------------------------------------------------
// Immediate-mode widget helpers. `vote_panel_do` runs twice per frame: once
// from the mouse handler (draw = false, hit-testing only) and once from the
// render injection (draw = true), so layout only exists in one place.
// ---------------------------------------------------------------------------

struct Rect
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

struct PanelUi
{
    bool draw = true;
    int mx = 0;
    int my = 0;
    bool click = false;
    bool rclick = false; // right-click; only the cyclers act on it
    int wheel = 0;

    // Active scroll viewport. Widgets are always laid out in absolute screen
    // space; inside a region the draw pass subtracts the clip origin (set_clip
    // moves the drawing origin) while hit-testing stays absolute but is gated on
    // the viewport, so rows scrolled out of sight are not clickable.
    Rect clip_rect;
    bool clip_active = false;
    int origin_x = 0;
    int origin_y = 0;
};

struct Layout
{
    float scale = 1.0f;
    int px = 0, py = 0, pw = 0, ph = 0; // panel rect
    int cx = 0, cy = 0, cw = 0, ch = 0; // content rect (below title, above footer)
    int title_y = 0;
    int footer_y = 0;
    int footer_h = 0;
    int row_h = 0;
    int gap = 0;
    int font = 0;
    int title_font = 0;
};

// Panel layout reference size, matching spray_picker.cpp.
constexpr float REF_WIDTH = 1280.0f;
constexpr float REF_HEIGHT = 800.0f;

// Uniform panel scale, shared with the widget helpers that only get a Rect.
float g_panel_scale = 1.0f;

int scaled(float value)
{
    return static_cast<int>(value * g_panel_scale);
}

bool point_in(int px, int py, const Rect& r)
{
    return px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h;
}

// Hover test honouring the active scroll viewport.
bool ui_hover(const PanelUi& ui, const Rect& r)
{
    if (ui.clip_active && !point_in(ui.mx, ui.my, ui.clip_rect)) {
        return false;
    }
    return point_in(ui.mx, ui.my, r);
}

// Absolute rect -> coordinates to draw at under the active clip window.
Rect ui_draw_rect(const PanelUi& ui, const Rect& r)
{
    return Rect{r.x - ui.origin_x, r.y - ui.origin_y, r.w, r.h};
}

Layout compute_layout()
{
    Layout lo;
    const int clip_w = rf::gr::clip_width();
    const int clip_h = rf::gr::clip_height();
    lo.scale = std::min(clip_w / REF_WIDTH, clip_h / REF_HEIGHT);
    g_panel_scale = lo.scale;

    // Compact panel that leaves a clear margin at every resolution.
    lo.pw = std::min(scaled(880.0f), clip_w - 80);
    lo.ph = std::min(scaled(600.0f), clip_h - 80);
    lo.px = (clip_w - lo.pw) / 2;
    lo.py = (clip_h - lo.ph) / 2;

    lo.font = rf::ui::medium_font_0;
    lo.title_font = rf::ui::medium_font_0;

    const int pad = scaled(16.0f);
    const int title_h = scaled(46.0f);

    lo.gap = std::max(3, scaled(8.0f));
    lo.row_h = std::max(rf::gr::get_font_height(lo.font) + 4, scaled(24.0f));

    lo.title_y = lo.py + scaled(14.0f);
    lo.footer_h = scaled(58.0f);
    lo.footer_y = lo.py + lo.ph - lo.footer_h;

    lo.cx = lo.px + pad;
    lo.cw = lo.pw - 2 * pad;
    lo.cy = lo.py + title_h;
    lo.ch = lo.footer_y - lo.cy;
    if (lo.ch < lo.row_h) {
        lo.ch = lo.row_h;
    }
    return lo;
}

// Colour conventions on the dark panel: gold for section headers, white for
// widget labels, light grey for secondary/explanatory text.
void set_header_color()
{
    rf::gr::set_color(255, 215, 0, 255);
}

void set_text_color(bool enabled, bool hovered)
{
    if (!enabled) {
        rf::gr::set_color(130, 130, 130, 255);
    }
    else if (hovered) {
        rf::gr::set_color(255, 255, 255, 255);
    }
    else {
        rf::gr::set_color(230, 230, 230, 255);
    }
}

void draw_label(const PanelUi& ui, const Rect& r, const char* text, int font,
                bool secondary = false)
{
    if (secondary) {
        rf::gr::set_color(210, 210, 210, 255);
    }
    else {
        rf::gr::set_color(255, 255, 255, 255);
    }
    const Rect d = ui_draw_rect(ui, r);
    rf::gr::string(d.x, d.y, text, font);
}

// A flat clickable box. Returns true only in the hit-test pass, on a click.
bool ui_button(PanelUi& ui, const Rect& r, const char* text, int font, bool enabled = true,
               bool active = false)
{
    const bool hovered = enabled && ui_hover(ui, r);
    if (ui.draw) {
        const Rect d = ui_draw_rect(ui, r);
        // Active reuses the green the listbox selection and checkbox ticks use.
        if (active) {
            rf::gr::set_color(hovered ? 50 : 35, hovered ? 115 : 90, hovered ? 80 : 60, 255);
        }
        else {
            rf::gr::set_color(hovered ? 70 : 45, hovered ? 70 : 45, hovered ? 70 : 45, 255);
        }
        rf::gr::rect(d.x, d.y, d.w, d.h);
        if (active) {
            rf::gr::set_color(120, 230, 120, 255);
        }
        else {
            rf::gr::set_color(enabled ? 150 : 100, enabled ? 150 : 100, enabled ? 150 : 100, 255);
        }
        hud_rect_border(d.x, d.y, d.w, d.h, 1);
        if (active) {
            rf::gr::set_color(255, 255, 255, 255);
        }
        else {
            set_text_color(enabled, hovered);
        }
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, d.x + d.w / 2,
            d.y + (d.h - rf::gr::get_font_height(font)) / 2, text, font);
        return false;
    }
    return hovered && ui.click;
}

bool ui_checkbox(PanelUi& ui, const Rect& r, bool checked, const char* text, int font,
                 bool enabled = true)
{
    const bool hovered = enabled && ui_hover(ui, r);
    if (ui.draw) {
        const Rect d = ui_draw_rect(ui, r);
        const int box = std::min(d.h - 2, rf::gr::get_font_height(font));
        const int by = d.y + (d.h - box) / 2;
        rf::gr::set_color(hovered ? 70 : 45, hovered ? 70 : 45, hovered ? 70 : 45, 255);
        rf::gr::rect(d.x, by, box, box);
        rf::gr::set_color(enabled ? 170 : 100, enabled ? 170 : 100, enabled ? 170 : 100, 255);
        hud_rect_border(d.x, by, box, box, 1);
        if (checked) {
            // Muted tick for a pinned/non-interactive checkbox.
            if (enabled) {
                rf::gr::set_color(120, 230, 120, 255);
            }
            else {
                rf::gr::set_color(85, 135, 85, 255);
            }
            rf::gr::rect(d.x + 2, by + 2, box - 4, box - 4);
        }
        set_text_color(enabled, hovered);
        rf::gr::string(d.x + box + std::max(3, scaled(5.0f)),
            d.y + (d.h - rf::gr::get_font_height(font)) / 2, text, font);
        return false;
    }
    return hovered && ui.click;
}

int ui_cycler_arrow_width()
{
    return std::max(10, scaled(16.0f));
}

// Space the cycler has for its value text, between the two arrows.
int ui_cycler_value_width(const Rect& r)
{
    return std::max(0, r.w - 2 * ui_cycler_arrow_width());
}

// Middle-truncate so both ends of a long name stay readable.
std::string fit_middle(std::string_view text, int max_w, int font)
{
    if (text.empty() || rf::gr::get_string_size(text, font).first <= max_w) {
        return std::string{text};
    }
    for (size_t keep = text.size(); keep > 0; --keep) {
        const size_t left = (keep + 1) / 2;
        const size_t right = keep - left;
        std::string candidate{text.substr(0, left)};
        candidate += "...";
        candidate += std::string{text.substr(text.size() - right, right)};
        if (rf::gr::get_string_size(candidate, font).first <= max_w) {
            return candidate;
        }
    }
    return std::string{"..."};
}

// Fake cycler: "< value >" with clickable arrows. Returns -1, 0 or +1.
int ui_cycler(PanelUi& ui, const Rect& r, const char* value, int font)
{
    const int arrow_w = ui_cycler_arrow_width();
    const Rect left{r.x, r.y, arrow_w, r.h};
    const Rect right{r.x + r.w - arrow_w, r.y, arrow_w, r.h};
    const Rect middle{r.x + arrow_w, r.y, r.w - 2 * arrow_w, r.h};

    const bool hover_left = ui_hover(ui, left);
    const bool hover_right = ui_hover(ui, right);
    const bool hover_mid = ui_hover(ui, middle);

    if (ui.draw) {
        const Rect d = ui_draw_rect(ui, r);
        const Rect dl = ui_draw_rect(ui, left);
        const Rect dr = ui_draw_rect(ui, right);
        const Rect dm = ui_draw_rect(ui, middle);

        rf::gr::set_color(hover_mid ? 70 : 45, hover_mid ? 70 : 45, hover_mid ? 70 : 45, 255);
        rf::gr::rect(d.x, d.y, d.w, d.h);
        rf::gr::set_color(150, 150, 150, 255);
        hud_rect_border(d.x, d.y, d.w, d.h, 1);

        const int text_y = d.y + (d.h - rf::gr::get_font_height(font)) / 2;
        set_text_color(true, hover_left);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, dl.x + dl.w / 2, text_y, "<", font);
        set_text_color(true, hover_right);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, dr.x + dr.w / 2, text_y, ">", font);

        set_text_color(true, hover_mid);
        if (ui.clip_active) {
            // Already inside a scroll region's clip window, and set_clip replaces
            // rather than nests, so rely on that clip plus the caller's
            // fit_middle() sizing instead of narrowing to the value box here.
            rf::gr::string_aligned(rf::gr::ALIGN_CENTER, dm.x + dm.w / 2,
                dm.y + (dm.h - rf::gr::get_font_height(font)) / 2, value, font);
        }
        else {
            int save_x = 0, save_y = 0, save_w = 0, save_h = 0;
            rf::gr::get_clip(&save_x, &save_y, &save_w, &save_h);
            rf::gr::set_clip(dm.x, dm.y, dm.w, dm.h);
            // set_clip moved the drawing origin, so draw relative to the clip window.
            rf::gr::string_aligned(rf::gr::ALIGN_CENTER, dm.w / 2,
                (dm.h - rf::gr::get_font_height(font)) / 2, value, font);
            rf::gr::set_clip(save_x, save_y, save_w, save_h);
        }
        return 0;
    }

    // Right-click anywhere on the widget steps backwards, so a value can be
    // walked either way without aiming for the small left arrow.
    if (ui.rclick && (hover_left || hover_right || hover_mid)) {
        return -1;
    }
    if (!ui.click) {
        return 0;
    }
    if (hover_left) {
        return -1;
    }
    if (hover_right || hover_mid) {
        return 1;
    }
    return 0;
}

// Scrolling list. Returns the clicked index in the hit-test pass, else -1.
int ui_listbox(PanelUi& ui, const Rect& r, const std::vector<std::string>& items, int sel,
               float& scroll, int font)
{
    const int row_h = rf::gr::get_font_height(font) + 2;
    const int total_h = static_cast<int>(items.size()) * row_h;
    const float max_scroll = static_cast<float>(std::max(0, total_h - r.h));
    const bool inside = point_in(ui.mx, ui.my, r);

    if (!ui.draw && inside && ui.wheel != 0) {
        scroll += (ui.wheel > 0 ? -1.0f : 1.0f) * static_cast<float>(row_h * 3);
    }
    scroll = std::clamp(scroll, 0.0f, max_scroll);

    if (!ui.draw) {
        if (inside && ui.click) {
            const int index = (ui.my - r.y + static_cast<int>(scroll)) / row_h;
            if (index >= 0 && index < static_cast<int>(items.size())) {
                return index;
            }
        }
        return -1;
    }

    rf::gr::set_color(8, 8, 8, 235);
    rf::gr::rect(r.x, r.y, r.w, r.h);
    rf::gr::set_color(120, 120, 120, 255);
    hud_rect_border(r.x, r.y, r.w, r.h, 1);

    int save_x = 0, save_y = 0, save_w = 0, save_h = 0;
    rf::gr::get_clip(&save_x, &save_y, &save_w, &save_h);
    rf::gr::set_clip(r.x, r.y, r.w, r.h);
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        const int iy = r.y + i * row_h - static_cast<int>(scroll);
        if (iy + row_h < r.y || iy > r.y + r.h) {
            continue;
        }
        // Drawing coords are relative to the clip window origin.
        const int dy = iy - r.y;
        const bool hovered = inside && ui.my >= iy && ui.my < iy + row_h;
        if (i == sel) {
            rf::gr::set_color(35, 90, 60, 255);
            rf::gr::rect(0, dy, r.w, row_h);
        }
        else if (hovered) {
            rf::gr::set_color(70, 70, 70, 255);
            rf::gr::rect(0, dy, r.w, row_h);
        }
        if (i == sel) {
            rf::gr::set_color(160, 255, 190, 255);
        }
        else {
            rf::gr::set_color(hovered ? 255 : 230, hovered ? 255 : 230, hovered ? 255 : 230, 255);
        }
        rf::gr::string(3, dy + 1, items[i].c_str(), font);
    }
    rf::gr::set_clip(save_x, save_y, save_w, save_h);

    if (max_scroll > 0.0f) {
        const float ratio = scroll / max_scroll;
        const int bar_w = std::max(3, scaled(6.0f));
        const int bar_h = std::max(bar_w, r.h * r.h / std::max(1, total_h));
        const int bar_y = r.y + static_cast<int>(ratio * (r.h - bar_h));
        rf::gr::set_color(100, 255, 200, 255);
        rf::gr::rect(r.x + r.w - bar_w, bar_y, bar_w, bar_h);
    }
    return -1;
}

std::vector<std::string> wrap_text(std::string_view text, int max_w, int font)
{
    std::vector<std::string> lines;
    std::string current;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t space = text.find(' ', pos);
        const std::string_view word = text.substr(pos, space == std::string_view::npos ? std::string_view::npos
                                                                                       : space - pos);
        std::string candidate = current.empty() ? std::string{word} : current + " " + std::string{word};
        if (!current.empty() && rf::gr::get_string_size(candidate, font).first > max_w) {
            lines.push_back(current);
            current = std::string{word};
        }
        else {
            current = std::move(candidate);
        }
        if (space == std::string_view::npos) {
            break;
        }
        pos = space + 1;
    }
    if (!current.empty()) {
        lines.push_back(current);
    }
    return lines;
}

void draw_wrapped(const Rect& r, std::string_view text, int font, int line_h)
{
    const int max_lines = std::max(1, r.h / line_h);
    auto lines = wrap_text(text, r.w, font);
    if (static_cast<int>(lines.size()) > max_lines) {
        // Mark the cut so an over-long paragraph never just stops mid-sentence.
        lines.resize(max_lines);
        std::string& last = lines.back();
        while (!last.empty() && rf::gr::get_string_size(last + "...", font).first > r.w) {
            last.pop_back();
        }
        last += "...";
    }

    int y = r.y;
    rf::gr::set_color(215, 215, 215, 255);
    for (const auto& line : lines) {
        rf::gr::string(r.x, y, line.c_str(), font);
        y += line_h;
    }
}

// ---------------------------------------------------------------------------
// Data helpers
// ---------------------------------------------------------------------------

std::vector<KickCandidate> build_kick_candidates()
{
    // Re-read every frame: players join and leave while the panel is open.
    std::vector<KickCandidate> out;
    if (!rf::player_list) {
        return out;
    }
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (&player == rf::local_player || !player.net_data) {
            continue;
        }
        KickCandidate candidate;
        candidate.id = player.net_data->player_id;
        candidate.name = player.name.c_str();
        out.push_back(std::move(candidate));
    }
    return out;
}

// Game types offered for the current vote type. Match votes need a team type.
std::vector<const VoteGametypeInfo*> selectable_gametypes(const VoteOptionsData& options, bool team_only)
{
    std::vector<const VoteGametypeInfo*> out;
    for (const auto& gametype : options.gametypes) {
        if (team_only && !gametype.is_team_type) {
            continue;
        }
        out.push_back(&gametype);
    }
    return out;
}

// Types the panel can actually render. Unknown types are already absent from the
// parsed schema (the blob's descriptors are length-prefixed and skippable), but a
// known-yet-unrendered type (String) would otherwise leave a blank row, so the
// row is skipped entirely -- in both the layout and the height calculation.
bool option_has_widget(MutatorOptionType type)
{
    switch (type) {
        case MutatorOptionType::Bool:
        case MutatorOptionType::Choice:
        case MutatorOptionType::Int:
        case MutatorOptionType::Float:
            return true;
        default:
            return false;
    }
}

// Cheap order-sensitive hash over the parts of the schema the form mirrors:
// gametype ids, and every mutator id plus each option's id and type.
uint32_t schema_fingerprint(const VoteOptionsData& options)
{
    uint32_t h = 2166136261u; // FNV-1a
    const auto mix = [&h](uint32_t v) {
        h = (h ^ v) * 16777619u;
    };
    mix(static_cast<uint32_t>(options.gametypes.size()));
    for (const auto& gametype : options.gametypes) {
        mix(gametype.id);
    }
    mix(static_cast<uint32_t>(options.mutators.size()));
    for (const auto& mutator : options.mutators) {
        mix(mutator.id);
        mix(static_cast<uint32_t>(mutator.options.size()));
        for (const auto& option : mutator.options) {
            mix(option.id);
            mix(static_cast<uint32_t>(option.type));
        }
    }
    return h;
}

void build_form(const VoteOptionsData& options)
{
    g_form.built = true;
    g_form.built_fingerprint = schema_fingerprint(options);

    g_form.type_index = 0;
    g_form.kick_index = 0;
    g_form.level_selection.clear();
    g_form.manual_level = false;
    g_form.manual_level_name.clear();
    g_form.gametype_index = 0;
    g_form.team_size = 4;
    g_form.extend_minutes = af_vote_extend_default_minutes;
    g_form.description_mutator = -1;
    g_form.level_scroll = 0.0f;
    g_form.kick_scroll = 0.0f;
    g_form.mutator_scroll = 0.0f;

    g_form.mutators.clear();
    g_form.mutators.reserve(options.mutators.size());
    for (const auto& mutator : options.mutators) {
        PanelMutatorSelection selection;
        selection.options.reserve(mutator.options.size());
        for (const auto& option : mutator.options) {
            PanelOptionValue value;
            value.bool_value = option.default_bool;
            value.choice_index = option.default_choice;
            value.int_value = option.default_int;
            value.float_value = option.default_float;
            selection.options.push_back(value);
        }
        g_form.mutators.push_back(std::move(selection));
    }
}

void ensure_form(const VoteOptionsData& options)
{
    // Deliberately NOT keyed on the level list: the selection is tracked by name,
    // so a background stale-refresh that only changes levels must not wipe the
    // player's in-progress form. The level cache below picks that up instead.
    if (!g_form.built || g_form.built_fingerprint != schema_fingerprint(options)) {
        build_form(options);
    }
}

// The level string sent on the wire. `allow_current` (Match) maps the leading
// "Current level" row to the empty string the server reads as "keep the level".
// Case-insensitive ordering for the level list.
bool level_name_less(const std::string& a, const std::string& b)
{
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
        [](unsigned char l, unsigned char r) { return std::tolower(l) < std::tolower(r); });
}

std::string selected_level()
{
    if (g_form.manual_level) {
        return g_form.manual_level_name;
    }
    // Stored by name, so the display sort (and any blob refresh) cannot desync
    // it. Empty is exactly what the wire wants for Match's "current level".
    return g_form.level_selection;
}

uint8_t selected_gametype(const VoteOptionsData& options, bool team_only)
{
    if (g_form.gametype_index <= 0) {
        return af_vote_gametype_none;
    }
    const auto gametypes = selectable_gametypes(options, team_only);
    const int index = g_form.gametype_index - 1;
    if (index < 0 || index >= static_cast<int>(gametypes.size())) {
        return af_vote_gametype_none;
    }
    return gametypes[index]->id;
}

std::vector<VoteMutatorInput> build_mutator_inputs(const VoteOptionsData& options)
{
    std::vector<VoteMutatorInput> out;
    for (size_t i = 0; i < options.mutators.size() && i < g_form.mutators.size(); ++i) {
        if (!g_form.mutators[i].enabled) {
            continue;
        }
        const VoteMutatorSchema& schema = options.mutators[i];
        VoteMutatorInput input;
        input.mutator_id = schema.id;
        for (size_t o = 0; o < schema.options.size() && o < g_form.mutators[i].options.size(); ++o) {
            const VoteMutatorOptionSchema& option = schema.options[o];
            const PanelOptionValue& value = g_form.mutators[i].options[o];
            VoteMutatorOptionInput option_input;
            option_input.option_id = option.id;
            option_input.type = option.type;
            option_input.bool_value = value.bool_value;
            option_input.choice_index = value.choice_index;
            option_input.int_value = value.int_value;
            option_input.float_value = value.float_value;
            input.options.push_back(std::move(option_input));
        }
        out.push_back(std::move(input));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Popup text input (stock RF popup; gameseq_process drives it globally, so it
// works over the gameplay overlay -- see rf::ui::popup_is_active, which is what
// suppresses our own input handling while it is up)
// ---------------------------------------------------------------------------

void manual_level_popup_callback()
{
    char buffer[32] = "";
    rf::ui::popup_get_input(buffer, sizeof(buffer));
    g_form.manual_level_name = buffer;
}

// Extend's duration entry.
void extend_minutes_popup_callback()
{
    char buffer[32] = "";
    rf::ui::popup_get_input(buffer, sizeof(buffer));
    try {
        g_form.extend_minutes = std::clamp(std::stoi(buffer),
            static_cast<int>(af_vote_extend_min_minutes),
            static_cast<int>(af_vote_extend_max_minutes));
    }
    catch (const std::exception& e) {
        // Non-numeric or out of int range: keep whatever was already selected.
        xlog::info("vote panel: invalid extend duration '{}', reason: {}", buffer, e.what());
    }
}

void mutator_option_popup_callback()
{
    char buffer[32] = "";
    rf::ui::popup_get_input(buffer, sizeof(buffer));

    const VoteOptionsData* options = vote_options_get();
    if (!options || g_popup_mutator < 0 || g_popup_option < 0) {
        return;
    }
    if (g_popup_mutator >= static_cast<int>(options->mutators.size())
        || g_popup_mutator >= static_cast<int>(g_form.mutators.size())) {
        return;
    }
    const auto& schema = options->mutators[g_popup_mutator];
    if (g_popup_option >= static_cast<int>(schema.options.size())
        || g_popup_option >= static_cast<int>(g_form.mutators[g_popup_mutator].options.size())) {
        return;
    }
    // Identity check, not just bounds: the slot must still be the option the
    // popup was opened for.
    if (schema.id != g_popup_mutator_id
        || schema.options[g_popup_option].id != g_popup_option_id) {
        clear_popup_target();
        return;
    }

    PanelOptionValue& value = g_form.mutators[g_popup_mutator].options[g_popup_option];
    try {
        if (schema.options[g_popup_option].type == MutatorOptionType::Int) {
            value.int_value = std::stoi(buffer);
        }
        else {
            value.float_value = std::stof(buffer);
        }
    }
    catch (const std::exception& e) {
        xlog::info("vote panel: invalid option input '{}', reason: {}", buffer, e.what());
    }
    clear_popup_target();
}

// ---------------------------------------------------------------------------
// Panel content
// ---------------------------------------------------------------------------

PanelMode compute_mode()
{
    if (!is_server_minimum_af_version(1, 4)) {
        return PanelMode::NotSupported;
    }
    // No ActiveVote mode any more: a running vote leaves the form in place and
    // is surfaced by the title-row status line plus the footer button states, so
    // a replacement vote can be set up the moment the current one ends.
    if (!vote_options_are_loaded()) {
        return PanelMode::Loading;
    }
    return PanelMode::Form;
}

void play_click_sound()
{
    rf::snd_play(stock_sound_id::panel_button_click, 0, 0.0f, 1.0f);
}

void play_toggle_sound(bool on)
{
    rf::snd_play(on ? stock_sound_id::checkbox_off : stock_sound_id::checkbox_on, 0, 0.0f, 1.0f);
}

// Mirrors send_vote_from_form's validation so the button can be greyed out
// instead of silently doing nothing when pressed.
bool vote_form_is_sendable(const VoteOptionsData& options)
{
    const auto types = enabled_vote_types();
    if (g_form.type_index < 0 || g_form.type_index >= static_cast<int>(types.size())) {
        return false;
    }
    switch (types[g_form.type_index]->type) {
        case AfVoteType::Kick: {
            const auto candidates = build_kick_candidates();
            return g_form.kick_index >= 0
                && g_form.kick_index < static_cast<int>(candidates.size());
        }
        case AfVoteType::Level:
            return !selected_level().empty();
        case AfVoteType::Extend:
            return true;
        default:
            return true; // Match falls back to the current level; rest are parameterless
    }
    (void)options;
}

void send_vote_from_form(const VoteOptionsData& options)
{
    const auto types = enabled_vote_types();
    if (g_form.type_index < 0 || g_form.type_index >= static_cast<int>(types.size())) {
        return;
    }

    AfVoteCallParams params;
    params.type = types[g_form.type_index]->type;

    switch (params.type) {
        case AfVoteType::Kick: {
            const auto candidates = build_kick_candidates();
            if (g_form.kick_index < 0 || g_form.kick_index >= static_cast<int>(candidates.size())) {
                return;
            }
            params.target_player_id = candidates[g_form.kick_index].id;
            break;
        }
        case AfVoteType::Level: {
            params.level = selected_level();
            if (params.level.empty()) {
                return;
            }
            params.gametype = selected_gametype(options, false);
            params.mutators = build_mutator_inputs(options);
            break;
        }
        case AfVoteType::Match: {
            params.team_size = static_cast<uint8_t>(g_form.team_size);
            params.level = selected_level();
            params.gametype = selected_gametype(options, true);
            params.mutators = build_mutator_inputs(options);
            break;
        }
        case AfVoteType::Extend:
            params.extend_minutes = static_cast<uint8_t>(std::clamp(g_form.extend_minutes,
                static_cast<int>(af_vote_extend_min_minutes),
                static_cast<int>(af_vote_extend_max_minutes)));
            break;
        default:
            break; // parameterless
    }

    // Sent the vote call.
    af_send_vote_call(params);

    play_click_sound();
    vote_panel_close();
}

void do_message_block(PanelUi& ui, const Layout& lo, const std::vector<std::string>& lines)
{
    if (!ui.draw) {
        return;
    }
    const int line_h = rf::gr::get_font_height(lo.font) + lo.gap;
    int y = lo.cy + (lo.ch - static_cast<int>(lines.size()) * line_h) / 2;
    rf::gr::set_color(235, 235, 235, 255);
    for (const auto& line : lines) {
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, lo.cx + lo.cw / 2, y, line.c_str(), lo.font);
        y += line_h;
    }
}

// Total height of every mutator row plus the option rows of expanded mutators.
// Must match the advance in do_mutator_rows exactly (rows whose option type has
// no widget consume nothing -- see option_has_widget).
int mutator_content_height(const Layout& lo, const VoteOptionsData& options)
{
    int h = 0;
    for (size_t i = 0; i < options.mutators.size() && i < g_form.mutators.size(); ++i) {
        h += lo.row_h;
        if (g_form.mutators[i].enabled) {
            const size_t count =
                std::min(options.mutators[i].options.size(), g_form.mutators[i].options.size());
            for (size_t o = 0; o < count; ++o) {
                if (option_has_widget(options.mutators[i].options[o].type)) {
                    h += lo.row_h;
                }
            }
        }
    }
    return h;
}

// One mutator checkbox plus, when it is checked, its option widgets indented
// underneath. Rects are absolute screen space with the scroll offset already
// applied by the caller; rows outside `view` are neither drawn nor hit-tested.
void do_mutator_rows(PanelUi& ui, const Layout& lo, const VoteOptionsData& options,
                     const Rect& view, int start_y)
{
    const int indent = std::max(8, scaled(16.0f));
    const int view_bottom = view.y + view.h;
    int y = start_y;

    const auto row_visible = [&](int row_y) {
        return row_y + lo.row_h > view.y && row_y < view_bottom;
    };

    for (size_t i = 0; i < options.mutators.size() && i < g_form.mutators.size(); ++i) {
        const VoteMutatorSchema& schema = options.mutators[i];
        PanelMutatorSelection& selection = g_form.mutators[i];

        if (row_visible(y)) {
            const Rect row{view.x, y, view.w, lo.row_h};
            if (!ui.draw && ui_hover(ui, row)) {
                g_form.description_mutator = static_cast<int>(i);
                g_hovered_mutator_this_pass = true;
            }
            if (ui_checkbox(ui, row, selection.enabled, schema.label.c_str(), lo.font)) {
                selection.enabled = !selection.enabled;
                g_form.description_mutator = static_cast<int>(i);
                g_description_pinned = true;
                play_toggle_sound(selection.enabled);
            }
        }
        y += lo.row_h;

        if (!selection.enabled) {
            continue;
        }

        for (size_t o = 0; o < schema.options.size() && o < selection.options.size(); ++o) {
            if (!option_has_widget(schema.options[o].type)) {
                continue; // consumes no row (see option_has_widget)
            }
            if (!row_visible(y)) {
                y += lo.row_h;
                continue;
            }
            const VoteMutatorOptionSchema& option = schema.options[o];
            PanelOptionValue& value = selection.options[o];
            const Rect opt_row{view.x + indent, y, view.w - indent, lo.row_h};

            switch (option.type) {
                case MutatorOptionType::Bool: {
                    if (ui_checkbox(ui, opt_row, value.bool_value, option.label.c_str(), lo.font)) {
                        value.bool_value = !value.bool_value;
                        play_toggle_sound(value.bool_value);
                    }
                    break;
                }
                case MutatorOptionType::Choice: {
                    const int label_w = opt_row.w / 2;
                    if (ui.draw) {
                        draw_label(ui,
                            {opt_row.x, opt_row.y + (lo.row_h - rf::gr::get_font_height(lo.font)) / 2,
                             label_w, lo.row_h}, option.label.c_str(), lo.font, true);
                    }
                    const Rect value_rect{opt_row.x + label_w, opt_row.y, opt_row.w - label_w, lo.row_h};
                    std::string text = "-";
                    if (value.choice_index < option.choices.size()) {
                        // Long values from a modded server degrade to an ellipsis
                        // rather than clipping mid-glyph.
                        text = fit_middle(option.choices[value.choice_index],
                            ui_cycler_value_width(value_rect), lo.font);
                    }
                    const int delta = ui_cycler(ui, value_rect, text.c_str(), lo.font);
                    if (delta != 0 && !option.choices.empty()) {
                        const int count = static_cast<int>(option.choices.size());
                        int index = static_cast<int>(value.choice_index) + delta;
                        if (index < 0) {
                            index = count - 1;
                        }
                        else if (index >= count) {
                            index = 0;
                        }
                        value.choice_index = static_cast<uint8_t>(index);
                        play_click_sound();
                    }
                    break;
                }
                case MutatorOptionType::Int:
                case MutatorOptionType::Float: {
                    const int label_w = opt_row.w / 2;
                    if (ui.draw) {
                        draw_label(ui,
                            {opt_row.x, opt_row.y + (lo.row_h - rf::gr::get_font_height(lo.font)) / 2,
                             label_w, lo.row_h}, option.label.c_str(), lo.font, true);
                    }
                    const Rect value_rect{opt_row.x + label_w, opt_row.y, opt_row.w - label_w, lo.row_h};
                    const std::string text = option.type == MutatorOptionType::Int
                        ? std::format("{}", value.int_value)
                        : std::format("{:.2f}", value.float_value);
                    if (ui_button(ui, value_rect, text.c_str(), lo.font)) {
                        // Indices, not positions, so scrolling can't misdirect the popup;
                        // ids so a blob refresh before OK can't either.
                        g_popup_mutator = static_cast<int>(i);
                        g_popup_option = static_cast<int>(o);
                        g_popup_mutator_id = schema.id;
                        g_popup_option_id = option.id;
                        rf::ui::popup_message(option.label.c_str(), "", mutator_option_popup_callback, 1);
                    }
                    break;
                }
                default:
                    break; // unreachable: filtered by option_has_widget above
            }
            y += lo.row_h;
        }
    }
}

// Mutator list as a clipped scroll region. When the content fits, nothing is
// clipped, no scrollbar is drawn and the wheel is left alone, so the layout is
// identical to what it was before scrolling existed.
void do_mutator_scroll_region(PanelUi& ui, const Layout& lo, const VoteOptionsData& options,
                              const Rect& view)
{
    if (!ui.draw) {
        g_hovered_mutator_this_pass = false;
    }
    const int content_h = mutator_content_height(lo, options);
    const float max_scroll = static_cast<float>(std::max(0, content_h - view.h));

    if (!ui.draw && max_scroll > 0.0f && ui.wheel != 0 && point_in(ui.mx, ui.my, view)) {
        g_form.mutator_scroll += (ui.wheel > 0 ? -1.0f : 1.0f) * static_cast<float>(lo.row_h * 3);
    }
    // Also clamps after a mutator collapses and the content shrinks.
    g_form.mutator_scroll = std::clamp(g_form.mutator_scroll, 0.0f, max_scroll);

    const int start_y = view.y - static_cast<int>(g_form.mutator_scroll);

    // Gate hit-testing on the viewport in both passes so rows scrolled out of
    // sight can never be clicked.
    const Rect saved_clip_rect = ui.clip_rect;
    const bool saved_clip_active = ui.clip_active;
    const int saved_origin_x = ui.origin_x;
    const int saved_origin_y = ui.origin_y;
    ui.clip_rect = view;
    ui.clip_active = true;

    int save_x = 0, save_y = 0, save_w = 0, save_h = 0;
    if (ui.draw) {
        rf::gr::get_clip(&save_x, &save_y, &save_w, &save_h);
        rf::gr::set_clip(view.x, view.y, view.w, view.h);
        // set_clip moves the drawing origin; widgets subtract it via ui_draw_rect.
        ui.origin_x = view.x;
        ui.origin_y = view.y;
    }

    do_mutator_rows(ui, lo, options, view, start_y);

    // Hovering away clears the description unless the mutator was toggled (a
    // deliberate click keeps its description pinned).
    if (!ui.draw && !g_hovered_mutator_this_pass && !g_description_pinned) {
        g_form.description_mutator = -1;
    }

    if (ui.draw) {
        rf::gr::set_clip(save_x, save_y, save_w, save_h);
    }

    ui.clip_rect = saved_clip_rect;
    ui.clip_active = saved_clip_active;
    ui.origin_x = saved_origin_x;
    ui.origin_y = saved_origin_y;

    // Scrollbar, drawn after the clip is restored so it uses absolute coords
    // (same constants as the level/player listboxes).
    if (ui.draw && max_scroll > 0.0f) {
        const float ratio = g_form.mutator_scroll / max_scroll;
        const int bar_w = std::max(3, scaled(6.0f));
        const int bar_h = std::max(bar_w, view.h * view.h / std::max(1, content_h));
        const int bar_y = view.y + static_cast<int>(ratio * (view.h - bar_h));
        rf::gr::set_color(100, 255, 200, 255);
        rf::gr::rect(view.x + view.w - bar_w, bar_y, bar_w, bar_h);
    }
}

void do_level_column(PanelUi& ui, const Layout& lo, const VoteOptionsData& options, const Rect& col,
                     bool allow_current, uint8_t gametype)
{
    const int header_h = rf::gr::get_font_height(lo.font);
    int y = col.y;
    if (ui.draw) {
        set_header_color();
        rf::gr::string(col.x, y, "Level", lo.font);
    }

    // Filter toggle on the header line. A server that enforces the gametype
    // prefix pins it checked and non-interactive; otherwise it is the player's
    // choice, off by default, and kept for the session only.
    {
        const int header_w = rf::gr::get_string_size("Level", lo.font).first;
        const int cb_x = col.x + header_w + lo.gap * 2;
        // ui_checkbox draws at d.x regardless of width, so a clamped-to-zero
        // width would leave a visible but unclickable control.
        const int cb_w = std::max(rf::gr::get_string_size("Filter gametype", lo.font).first
                + header_h + std::max(3, scaled(5.0f)),
            col.x + col.w - cb_x);
        const Rect cb{cb_x, y - 1, cb_w, header_h + 2};
        if (ui_checkbox(ui, cb, options.gametype_prefix_restricted || g_filter_gametype_local,
                "Filter gametype", lo.font, !options.gametype_prefix_restricted)) {
            g_filter_gametype_local = !g_filter_gametype_local;
            play_toggle_sound(g_filter_gametype_local);
        }
    }
    y += header_h + lo.gap;

    // Read after the checkbox so a toggle takes effect in the same pass.
    const bool filter_active = options.gametype_prefix_restricted || g_filter_gametype_local;

    // Rebuild the display rows only when an input changed. allowed_for_vote is an
    // independent axis from the gametype mask: it is applied ALWAYS, because a
    // level the server vote_level allow-list refuses is rejected whatever game
    // type is picked. Hence it is not gated on filter_active.
    const uint32_t levels_fp = level_list_fingerprint(options);
    LevelListCache& cache = g_level_cache;
    if (!cache.valid || cache.levels_fp != levels_fp || cache.filter_active != filter_active
        || cache.gametype != gametype || cache.allow_current != allow_current) {
        cache.levels_fp = levels_fp;
        cache.filter_active = filter_active;
        cache.gametype = gametype;
        cache.allow_current = allow_current;
        cache.gametype_hidden = 0;
        cache.not_allowed_hidden = 0;
        cache.sel_valid = false;

        std::vector<std::string> levels;
        levels.reserve(options.levels.size());
        for (const auto& level : options.levels) {
            if (!level.allowed_for_vote) {
                ++cache.not_allowed_hidden;
                continue;
            }
            if (filter_active) {
                const bool matches = gametype == af_vote_gametype_none
                    ? vote_level_allows_default_gametype(level)
                    : vote_level_allows_gametype(level, gametype);
                if (!matches) {
                    ++cache.gametype_hidden;
                    continue;
                }
            }
            levels.push_back(level.filename);
        }

        // Alphabetical rather than the blob order (rotation order, then extras).
        std::sort(levels.begin(), levels.end(), level_name_less);
        // The blob can list the same file twice (rotation entry + vote-allowed
        // extra); two identical rows would both resolve to the first by name.
        levels.erase(std::unique(levels.begin(), levels.end(),
                         [](const std::string& a, const std::string& b) {
                             return !level_name_less(a, b) && !level_name_less(b, a);
                         }),
            levels.end());

        cache.items.clear();
        cache.items.reserve(levels.size() + 1);
        if (allow_current) {
            // Pinned and always visible; the server adjudicates it at call time.
            cache.items.emplace_back("Current level");
        }
        for (auto& level : levels) {
            cache.items.push_back(std::move(level));
        }
        cache.first_level_row = allow_current ? 1 : 0;
        cache.valid = true;
    }

    const int level_row_count = static_cast<int>(cache.items.size()) - cache.first_level_row;

    // A game type change (or a refreshed allow-list) can hide the selected map;
    // drop the selection so it can never be sent. Match then falls back to the
    // pinned "Current level" row.
    if (!g_form.level_selection.empty()
        && std::find(cache.items.begin() + cache.first_level_row, cache.items.end(),
               g_form.level_selection)
            == cache.items.end()) {
        g_form.level_selection.clear();
        cache.sel_valid = false;
    }
    // A Level vote has no pinned row, so keep the top entry active by default.
    if (!allow_current && g_form.level_selection.empty() && level_row_count > 0) {
        g_form.level_selection = cache.items[cache.first_level_row];
        cache.sel_valid = false;
    }

    // Resolve the highlighted row from the stored name so it follows the map
    // through the sort/filter instead of pointing at whatever sits there now.
    if (!cache.sel_valid || cache.sel_name != g_form.level_selection
        || cache.sel_manual != g_form.manual_level) {
        cache.sel_name = g_form.level_selection;
        cache.sel_manual = g_form.manual_level;
        cache.sel_row = -1;
        if (!g_form.manual_level) {
            if (g_form.level_selection.empty()) {
                cache.sel_row = allow_current ? 0 : -1;
            }
            else {
                for (size_t i = cache.first_level_row; i < cache.items.size(); ++i) {
                    if (cache.items[i] == g_form.level_selection) {
                        cache.sel_row = static_cast<int>(i);
                        break;
                    }
                }
            }
        }
        cache.sel_valid = true;
    }

    const int line_h = rf::gr::get_font_height(lo.font) + 1;
    // Attribute the hint correctly: a map hidden by the allow-list has nothing to
    // do with the game type, and changing the game type will not bring it back.
    const bool show_hint = cache.gametype_hidden > 0 || cache.not_allowed_hidden > 0;
    const char* hint_text = cache.gametype_hidden > 0
        ? "Some maps hidden: not valid for this game type"
        : "Some maps hidden: not on the server vote list";
    const int hint_h = show_hint ? 2 * line_h + lo.gap : 0;

    const int manual_rows = 2 * lo.row_h + lo.gap;
    const int list_h = std::max(lo.row_h * 3, col.y + col.h - y - manual_rows - hint_h - lo.gap);
    const Rect list{col.x, y, col.w, list_h};

    if (!allow_current && level_row_count == 0) {
        // Nothing votable for this game type. Manual entry stays available and
        // unfiltered; CALL VOTE stays inert because the selection is empty.
        if (ui.draw) {
            rf::gr::set_color(8, 8, 8, 235);
            rf::gr::rect(list.x, list.y, list.w, list.h);
            rf::gr::set_color(120, 120, 120, 255);
            hud_rect_border(list.x, list.y, list.w, list.h, 1);
            const int pad = std::max(3, scaled(6.0f));
            draw_wrapped({list.x + pad, list.y + pad, list.w - 2 * pad, list.h - 2 * pad},
                cache.gametype_hidden > 0 ? "No maps available for this game type"
                                          : "No maps available for voting on this server",
                lo.font, line_h);
        }
    }
    else {
        const int clicked =
            ui_listbox(ui, list, cache.items, cache.sel_row, g_form.level_scroll, lo.font);
        if (clicked >= 0) {
            g_form.level_selection =
                (allow_current && clicked == 0) ? std::string{} : cache.items[clicked];
            g_form.manual_level = false;
            cache.sel_valid = false;
            play_click_sound();
        }
    }
    y += list_h + lo.gap;

    if (show_hint) {
        if (ui.draw) {
            draw_wrapped({col.x, y, col.w, 2 * line_h}, hint_text, lo.font, line_h);
        }
        y += hint_h;
    }

    const Rect manual_row{col.x, y, col.w, lo.row_h};
    if (ui_checkbox(ui, manual_row, g_form.manual_level, "Enter level name manually", lo.font)) {
        g_form.manual_level = !g_form.manual_level;
        play_toggle_sound(g_form.manual_level);
    }
    y += lo.row_h;

    const Rect manual_value{col.x, y, col.w, lo.row_h};
    const std::string manual_text = g_form.manual_level_name.empty()
        ? std::string{"(click to type a level name)"}
        : g_form.manual_level_name;
    if (ui_button(ui, manual_value, manual_text.c_str(), lo.font, g_form.manual_level)
        && g_form.manual_level) {
        rf::ui::popup_message("Enter level file name:", "", manual_level_popup_callback, 1);
    }
}

void do_form(PanelUi& ui, const Layout& lo, const VoteOptionsData& options)
{
    ensure_form(options);

    const auto types = enabled_vote_types();
    if (types.empty()) {
        do_message_block(ui, lo, {"This server has no votes enabled."});
        return;
    }
    g_form.type_index = std::clamp(g_form.type_index, 0, static_cast<int>(types.size()) - 1);

    int y = lo.cy;

    // Vote type buttons across the top, flowing left-to-right and wrapping. One
    // uniform width sized to the widest label keeps the grid aligned; the form
    // below starts under however many rows that produces.
    {
        const int hpad = std::max(4, scaled(10.0f));
        int widest = 0;
        for (const VoteTypeInfo* info : types) {
            widest = std::max(widest, rf::gr::get_string_size(info->label, lo.font).first);
        }
        const int btn_w = widest + 2 * hpad;
        const int btn_h = lo.row_h;
        const int per_row = std::max(1, (lo.cw + lo.gap) / (btn_w + lo.gap));

        for (int i = 0; i < static_cast<int>(types.size()); ++i) {
            const Rect btn{lo.cx + (i % per_row) * (btn_w + lo.gap),
                           y + (i / per_row) * (btn_h + lo.gap), btn_w, btn_h};
            const bool active = i == g_form.type_index;
            if (ui_button(ui, btn, types[i]->label, lo.font, true, active)) {
                if (!active) {
                    g_form.type_index = i;
                    g_form.mutator_scroll = 0.0f; // the section is rebuilt for the new type
                }
                play_click_sound();
            }
        }

        const int rows = (static_cast<int>(types.size()) + per_row - 1) / per_row;
        y += rows * (btn_h + lo.gap) + lo.gap;
    }

    const AfVoteType type = types[g_form.type_index]->type;
    const int body_bottom = lo.cy + lo.ch;

    if (type == AfVoteType::Kick) {
        if (ui.draw) {
            set_header_color();
            rf::gr::string(lo.cx, y, "Player to kick", lo.font);
        }
        y += rf::gr::get_font_height(lo.font) + lo.gap;

        // Still re-read every frame (players join and leave while the panel is
        // open), but once per frame: the hit-test pass refreshes it and the draw
        // pass that follows in the same frame reuses it.
        if (!ui.draw) {
            g_kick_cache = build_kick_candidates();
            g_kick_names.clear();
            g_kick_names.reserve(g_kick_cache.size());
            for (const auto& candidate : g_kick_cache) {
                g_kick_names.push_back(candidate.name);
            }
        }
        g_form.kick_index = std::clamp(g_form.kick_index, 0,
            std::max(0, static_cast<int>(g_kick_names.size()) - 1));

        const Rect list{lo.cx, y, lo.cw, body_bottom - y};
        const int clicked =
            ui_listbox(ui, list, g_kick_names, g_form.kick_index, g_form.kick_scroll, lo.font);
        if (clicked >= 0) {
            g_form.kick_index = clicked;
            play_click_sound();
        }
        return;
    }

    if (type == AfVoteType::Extend) {
        const int font_h = rf::gr::get_font_height(lo.font);
        const int line_h = font_h + 1;

        if (ui.draw) {
            set_header_color();
            rf::gr::string(lo.cx, y, "Duration", lo.font);
        }
        y += font_h + lo.gap;

        // Numeric entry.
        static constexpr const char* minutes_label = "Minutes (1-60)";
        const int label_w = rf::gr::get_string_size(minutes_label, lo.font).first + lo.gap * 2;
        const int value_w =
            std::min(std::max(scaled(96.0f), lo.row_h * 3), std::max(lo.row_h, lo.cw - label_w));
        if (ui.draw) {
            draw_label(ui, {lo.cx, y + (lo.row_h - font_h) / 2, label_w, lo.row_h}, minutes_label,
                lo.font);
        }
        const Rect value_rect{lo.cx + label_w, y, value_w, lo.row_h};
        const std::string value_text = std::format("{}", g_form.extend_minutes);
        if (ui_button(ui, value_rect, value_text.c_str(), lo.font)) {
            rf::ui::popup_message("Minutes to extend (1-60):", "", extend_minutes_popup_callback, 1);
        }
        y += lo.row_h + lo.gap * 2;

        if (ui.draw) {
            draw_wrapped({lo.cx, y, lo.cw, std::max(line_h, body_bottom - y)},
                types[g_form.type_index]->description, lo.font, line_h);
        }
        return;
    }

    if (type != AfVoteType::Level && type != AfVoteType::Match) {
        do_message_block(ui, lo, {types[g_form.type_index]->description});
        return;
    }

    const bool is_match = type == AfVoteType::Match;
    const int col_gap = std::max(6, scaled(16.0f));
    const int left_w = (lo.cw - col_gap) / 2;
    const Rect left_col{lo.cx, y, left_w, body_bottom - y};
    const Rect right_col{lo.cx + left_w + col_gap, y, lo.cw - left_w - col_gap, body_bottom - y};

    // Filtered live against the game type currently selected in the form; for
    // Match that cycler already offers only team types, so the two compose.
    do_level_column(ui, lo, options, left_col, is_match, selected_gametype(options, is_match));

    int ry = right_col.y;
    if (is_match) {
        const int label_w = right_col.w / 2;
        if (ui.draw) {
            draw_label(ui, {right_col.x, ry + (lo.row_h - rf::gr::get_font_height(lo.font)) / 2, label_w,
                        lo.row_h}, "Team size", lo.font);
        }
        const Rect cycler{right_col.x + label_w, ry, right_col.w - label_w, lo.row_h};
        const std::string text = std::format("{}v{}", g_form.team_size, g_form.team_size);
        const int delta = ui_cycler(ui, cycler, text.c_str(), lo.font);
        if (delta != 0) {
            g_form.team_size += delta;
            if (g_form.team_size < 1) {
                g_form.team_size = 8;
            }
            else if (g_form.team_size > 8) {
                g_form.team_size = 1;
            }
            play_click_sound();
        }
        ry += lo.row_h + lo.gap;
    }

    {
        const auto gametypes = selectable_gametypes(options, is_match);
        const int count = static_cast<int>(gametypes.size()) + 1; // + "Default"
        g_form.gametype_index = std::clamp(g_form.gametype_index, 0, count - 1);

        const int label_w = right_col.w / 2;
        if (ui.draw) {
            draw_label(ui, {right_col.x, ry + (lo.row_h - rf::gr::get_font_height(lo.font)) / 2, label_w,
                        lo.row_h}, "Game type", lo.font);
        }
        const Rect cycler{right_col.x + label_w, ry, right_col.w - label_w, lo.row_h};
        std::string text = "Default";
        if (g_form.gametype_index > 0) {
            const VoteGametypeInfo& gametype = *gametypes[g_form.gametype_index - 1];
            const char* short_name = gametype_short_name(gametype.id);
            text = short_name != nullptr
                ? std::string{short_name}
                // A game type this build predates has no short tag, so fall back
                // to the server-sent display name, truncated to the selector.
                : fit_middle(gametype.name, ui_cycler_value_width(cycler), lo.font);
        }
        const int delta = ui_cycler(ui, cycler, text.c_str(), lo.font);
        if (delta != 0) {
            g_form.gametype_index = (g_form.gametype_index + delta + count) % count;
            play_click_sound();
        }
        ry += lo.row_h + lo.gap * 2;
    }

    if (ui.draw) {
        set_header_color();
        rf::gr::string(right_col.x, ry, "Mutators", lo.font);
    }
    ry += rf::gr::get_font_height(lo.font) + lo.gap;

    // Reserve exactly three lines at the bottom of the column for the description;
    // everything between the header and it scrolls.
    const int desc_line_h = rf::gr::get_font_height(lo.font) + 1;
    const int desc_h = 3 * desc_line_h;
    const int mutator_bottom = right_col.y + right_col.h - desc_h - lo.gap;
    const Rect mutator_view{right_col.x, ry, right_col.w, std::max(lo.row_h, mutator_bottom - ry)};
    do_mutator_scroll_region(ui, lo, options, mutator_view);

    if (ui.draw) {
        const Rect desc{right_col.x, right_col.y + right_col.h - desc_h, right_col.w, desc_h};
        const char* text = "Hover or toggle a mutator to read what it does.";
        if (g_form.description_mutator >= 0
            && g_form.description_mutator < static_cast<int>(options.mutators.size())) {
            text = mutator_description_for(options.mutators[g_form.description_mutator].name);
        }
        draw_wrapped(desc, text, lo.font, desc_line_h);
    }
}

void vote_panel_do(PanelUi& ui)
{
    const Layout lo = compute_layout();

    if (ui.draw) {
        // Full-screen dim.
        rf::gr::set_color(0, 0, 0, 192);
        rf::gr::rect(0, 0, rf::gr::clip_width(), rf::gr::clip_height());

        // Panel background + border.
        rf::gr::set_color(20, 20, 20, 235);
        rf::gr::rect(lo.px, lo.py, lo.pw, lo.ph);
        rf::gr::set_color(120, 120, 120, 255);
        hud_rect_border(lo.px, lo.py, lo.pw, lo.ph, std::max(1, scaled(2.0f)));

        // Title. Always the panel name: a running vote is conveyed by the HUD
        // notification and by the footer button states, not in here.
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, lo.px + lo.pw / 2, lo.title_y, "CALL VOTE",
            lo.title_font);
    }

    const PanelMode mode = compute_mode();
    const VoteOptionsData* options = vote_options_get();

    switch (mode) {
        case PanelMode::NotSupported:
            do_message_block(ui, lo, {"This server does not support menu voting",
                                      "(requires Alpine Faction 1.4 or newer)"});
            break;
        case PanelMode::Loading:
            if (ui.draw) {
                vote_options_request_if_needed();
            }
            do_message_block(ui, lo, {"Loading server vote options..."});
            break;
        case PanelMode::Form:
            if (options) {
                do_form(ui, lo, *options);
            }
            break;
    }

    // Footer buttons, sized and centred like the spray picker's Cancel button.
    const int btn_h = std::max(lo.row_h, scaled(40.0f));
    const int btn_y = lo.footer_y + (lo.footer_h - btn_h) / 2;
    if (mode == PanelMode::Form) {
        // Three equal slots across the content width.
        const int btn_w = std::min((lo.cw - 2 * lo.gap) / 3, scaled(240.0f));
        const int row_w = 3 * btn_w + 2 * lo.gap;
        const int row_x = lo.cx + (lo.cw - row_w) / 2;

        const auto& state = vote_state_get();
        const bool vote_active = state.has_value();
        // Cannot call one while one runs; can only cancel one you own.
        const bool can_send =
            !vote_active && options != nullptr && vote_form_is_sendable(*options);
        const bool can_cancel = vote_active && state->is_owner;

        const Rect call{row_x, btn_y, btn_w, btn_h};
        if (ui_button(ui, call, "CALL VOTE", lo.font, can_send) && can_send) {
            send_vote_from_form(*options);
            return;
        }

        const Rect cancel{row_x + btn_w + lo.gap, btn_y, btn_w, btn_h};
        if (ui_button(ui, cancel, "CANCEL VOTE", lo.font, can_cancel) && can_cancel) {
            // Sent immediately: cancelling changes no level, so none of the
            // deferred-send or gameseq unwind machinery applies. The panel stays
            // open so a replacement vote can be set up as soon as the server's
            // end event flips these buttons back.
            af_send_vote_cancel();
            play_click_sound();
        }

        const Rect close{row_x + 2 * (btn_w + lo.gap), btn_y, btn_w, btn_h};
        if (ui_button(ui, close, "CLOSE (Esc)", lo.font)) {
            vote_panel_close();
            play_click_sound();
        }
    }
    else {
        const int btn_w = std::min(lo.cw / 2 - lo.gap, scaled(240.0f));
        const Rect close{lo.cx + (lo.cw - btn_w) / 2, btn_y, btn_w, btn_h};
        if (ui_button(ui, close, "CLOSE (Esc)", lo.font)) {
            vote_panel_close();
            play_click_sound();
        }
    }
}

// ---------------------------------------------------------------------------
// Gameplay overlay driving
// ---------------------------------------------------------------------------

void vote_panel_gameplay_input()
{
    if (!g_open) {
        return;
    }

    // Force-close on anything that is not a live multiplayer gameplay frame:
    // level change / limbo entry, multi_stop, becoming a server.
    if (!rf::is_multi || rf::is_server || rf::gameseq_get_state() != rf::GS_GAMEPLAY) {
        vote_panel_close();
        return;
    }

    // Re-assert every frame; respawns and camera changes reset mouse mode.
    gameplay_overlay_apply_mouse(true);

    // A stock popup (manual level name, int/float option) takes over input while
    // it is up. gameseq_process passes no_input=1 to the state's own frame, but
    // this gameplay pump runs outside that, so it has to check explicitly.
    if (rf::ui::popup_is_active()) {
        return; // the popup owns input; panel keeps rendering underneath
    }

    // Read the wheel accumulator before touching the mouse API.
    const int wheel = rf::mouse_dz;

    int x = 0, y = 0, z = 0;
    rf::mouse_get_pos(x, y, z);

    PanelUi ui;
    ui.draw = false;
    ui.mx = x;
    ui.my = y;
    ui.click = rf::mouse_was_button_pressed(0) != 0;
    ui.rclick = rf::mouse_was_button_pressed(1) != 0;
    ui.wheel = wheel;
    vote_panel_do(ui);
}

void vote_panel_gameplay_render()
{
    if (!g_open) {
        return;
    }
    // The input pump runs before gameseq_process, so on the frame a level change
    // lands it still saw gameplay; re-check here so the overlay never draws over
    // limbo or a loading screen even once.
    if (!rf::is_multi || rf::is_server || rf::gameseq_get_state() != rf::GS_GAMEPLAY) {
        vote_panel_close();
        return;
    }

    int x = 0, y = 0, z = 0;
    rf::mouse_get_pos(x, y, z);

    PanelUi ui;
    ui.draw = true;
    ui.mx = x;
    ui.my = y;
    vote_panel_do(ui);

    rf::gr::reset_clip();

    // gameseq_process re-hides the cursor every gameplay frame (mouse_set_visible
    // (false) at 0x004342E6), which undoes what the input pump sets because the
    // pump runs before it. This hook sits after gameseq_process and before the
    // stock cursor render (game_do_frame -> 0x004355F0 -> 0x00435460), so
    // re-asserting here is what actually makes the cursor appear -- and it draws
    // after us, so it lands on top of the panel for free.
    rf::mouse_set_visible(true);
}

// Runs every frame regardless of game state, right before the state machine
// applies this frame's deferred change and runs the current state's frame.
CallHook<rf::GameState()> gameseq_process_hook{
    0x004B2DB1,
    []() -> rf::GameState {
        // Before the state runs its frame, so the overlay sees keys/clicks first.
        vote_panel_decay_attack_swallow();
        vote_panel_gameplay_input();
        return gameseq_process_hook.call_target();
    },
};

// ESC during gameplay pushes GS_MAIN_MENU. While the overlay is up, treat that
// as "close the modal" and swallow the push, so ESC dismisses the panel and the
// ESC menu can never open on top of the overlay.
FunHook<void(rf::GameState, bool, bool)> gameseq_push_state_hook{
    0x00434410,
    [](rf::GameState state, bool transparent, bool pause_beneath) {
        if (state == rf::GS_MAIN_MENU && g_open) {
            vote_panel_close();
            play_click_sound();
            return;
        }
        gameseq_push_state_hook.call_target(state, transparent, pause_beneath);
    },
};

// Clicking in the panel must not also fire the weapon. Movement is left alone
// (the panel does not pause a multiplayer game), matching the waypoint editor.
bool gameplay_overlay_blocks_action(rf::ControlConfig* ccp, rf::ControlConfigAction action)
{
    if (!g_open && g_swallow_attack_frames <= 0) {
        return false;
    }
    if (!rf::local_player || ccp != &rf::local_player->settings.controls) {
        return false;
    }
    return action == rf::CC_ACTION_PRIMARY_ATTACK || action == rf::CC_ACTION_SECONDARY_ATTACK;
}

// control_input_filter owns the hooks and installs them at startup; the panel
// only contributes its veto, and until it does the filter has nothing to do.
// Registration must happen once, hence the flag: the overlay can be opened any
// number of times.
bool g_control_veto_registered = false;

void ensure_control_veto_registered()
{
    if (g_control_veto_registered) {
        return;
    }
    control_input_filter_add_veto(&gameplay_overlay_blocks_action);
    g_control_veto_registered = true;
}

// The dispatcher recurses into itself (the self-call at 0x004343E4) to draw the
// states stacked under a transparent one, so the hook below re-enters once per
// state in that chain. Counting entries and exits picks out the outermost
// dispatch, which is the one that must render the panel -- exactly once a frame.
int g_gameseq_dispatch_depth = 0;

struct GameseqDispatchDepth
{
    GameseqDispatchDepth() { ++g_gameseq_dispatch_depth; }
    ~GameseqDispatchDepth() { --g_gameseq_dispatch_depth; }
    GameseqDispatchDepth(const GameseqDispatchDepth&) = delete;
    GameseqDispatchDepth& operator=(const GameseqDispatchDepth&) = delete;

    [[nodiscard]] bool is_outermost() const { return g_gameseq_dispatch_depth == 1; }
};

// gameseq_process renders an active popup itself, at 0x004342CD, i.e. INSIDE the
// call this hook wraps -- so anything drawn from the after-frame hook
// (0x004B2DC2) lands on top of the popup and hides it. Rendering the overlay
// here instead puts it before the popup render, so a popup is drawn over the
// panel and stays usable.
FunHook<void(int, int)> gameseq_state_do_frame_hook{
    0x004343C0,
    [](int state_index, int no_input) {
        GameseqDispatchDepth depth;
        gameseq_state_do_frame_hook.call_target(state_index, no_input);
        if (depth.is_outermost()) {
            vote_panel_gameplay_render();
        }
    },
};

} // namespace

void vote_panel_close()
{
    if (g_open) {
        gameplay_overlay_apply_mouse(false);
        // Rest of this frame plus at least one whole frame, extended while held.
        g_swallow_attack_frames = 2;
    }
    g_open = false;
    clear_popup_target();
}

bool vote_panel_is_gameplay_overlay_active()
{
    return g_open;
}

void vote_panel_toggle_gameplay()
{
    if (g_open) {
        vote_panel_close();
        play_click_sound();
        return;
    }
    if (!rf::is_multi || rf::is_server || rf::gameseq_get_state() != rf::GS_GAMEPLAY) {
        return;
    }

    // Mutually exclusive with the remote server config overlay: both are
    // full-screen and both read the same non-consuming mouse state.
    if (g_remote_server_cfg_popup.is_active()) {
        g_remote_server_cfg_popup.toggle();
    }

    ensure_control_veto_registered();
    g_open = true;
    g_form.description_mutator = -1;
    g_form.mutator_scroll = 0.0f;
    clear_popup_target();
    g_level_cache.valid = false; // never show another blob's rows
    gameplay_overlay_apply_mouse(true);
    rf::snd_play(stock_sound_id::menu_select, 0, 0.0f, 1.0f);
}

void vote_panel_apply_patch()
{
    gameseq_process_hook.install();
    gameseq_push_state_hook.install();
    gameseq_state_do_frame_hook.install();
}
