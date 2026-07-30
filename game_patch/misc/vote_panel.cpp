#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <vector>
#include <patch_common/AsmWriter.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <common/utils/list-utils.h>
#include <xlog/xlog.h>
#include "vote_panel.h"
#include "player.h"
#include "../hud/hud_internal.h"
#include "../multi/alpine_packets.h"
#include "../multi/vote_client.h"
#include "../os/os.h"
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

// ---------------------------------------------------------------------------
// Frozen RF.exe addresses (multiplayer menu internals, verified in 1.20na).
// The multi menu does not use rf::ui::Container: it keeps its gadgets in two
// counted arrays, one for mouse hit-testing and one for everything else.
// ---------------------------------------------------------------------------

constexpr uintptr_t multi_menu_init_addr = 0x00448C20;
constexpr uintptr_t multi_menu_back_on_click_addr = 0x00448BF0;
constexpr uintptr_t multi_menu_key_handler_addr = 0x00448F80;
constexpr uintptr_t multi_menu_key_handler_push_imm = 0x00448F39;
constexpr uintptr_t multi_menu_mouse_handler_addr = 0x00449180;
constexpr uintptr_t multi_menu_hit_test_count_push = 0x004491A0;
constexpr uintptr_t multi_menu_hit_test_array = 0x0063E1F0;
constexpr uintptr_t multi_menu_hit_test_array_add = 0x00449500;
constexpr uintptr_t multi_menu_nav_next_addr = 0x004490F0;
constexpr uintptr_t multi_menu_nav_prev_addr = 0x00449140;
constexpr uintptr_t multi_menu_render_button_loop = 0x00449476;
constexpr uintptr_t multi_menu_render_button_loop_end = 0x004494A0;
constexpr uintptr_t multi_menu_first_button = 0x0063E0E0;
constexpr int multi_menu_button_stride = 0x44;
constexpr uintptr_t mainmenu_do_frame_addr = 0x00443780;
constexpr uintptr_t mainmenu_do_return_addr = 0x00443C90;

// Every read of the (already full) stock dispatch array `MOV ECX, 0x0063E090`.
// The imm32 lives at site+1. The four init-time add calls are left alone, which
// leaves the stock array populated but unreferenced.
constexpr uintptr_t dispatch_array_sites[] = {
    0x00448EF1, 0x00448F02, 0x00448F15, 0x00448F58, // do_frame
    0x004491F4, 0x0044920C,                         // mouse handler
    0x00448FD8, 0x00448FF4, 0x0044908A, 0x004490A4, // key handler
    0x00448A90, 0x00448AB0,                         // highlight helpers
};

// Set by mainmenu_init from the gameseq state underneath the menu: non-zero
// means the menu was opened from inside a running game (it is what disables the
// stock JOIN GAME / CREATE GAME buttons).
auto& g_menu_opened_in_game = addr_as_ref<uint8_t>(0x0063C11C);
auto& g_multi_menu_focus_index = addr_as_ref<int>(0x0063E0A4);

// Layout-compatible with the stock counted gadget arrays: {count, items[]}.
struct MultiMenuGadgetArray
{
    int32_t count;
    rf::ui::Gadget* items[8];
};

MultiMenuGadgetArray g_multi_menu_gadgets{};
rf::ui::Button g_call_vote_button{};
bool g_call_vote_button_created = false;

// Index in both the AF dispatch array and the stock hit-test array.
constexpr int call_vote_gadget_index = 4;

// --- gameseq state stack internals (RF.exe 1.20na) -------------------------
// gameseq_process applies at most ONE deferred change per frame, and the pop
// branch of gameseq_process_deferred_change (0x00434310) does a bare `idx--`
// with no lower bound. A pop issued against a stale view of the stack therefore
// underflows to idx = -1, where the tail returns states[-1] (BSS, reads 0) and
// the engine sits in state 0 forever -- state 0 has no case in
// gameseq_init_state (0x004B1AC0) or gameseq_close_state (0x004B1BF0).
// Worse, gameseq_set_state (0x00434190) with force = 0 is silently DROPPED
// while a pop is pending, so a pending pop of ours can also swallow the
// server-driven level change outright.
constexpr uintptr_t gameseq_state_stack_addr = 0x00630064;
constexpr uintptr_t gameseq_state_index_addr = 0x005967A4;
constexpr uintptr_t gameseq_pop_pending_addr = 0x006300EA;
constexpr uintptr_t gameseq_push_pending_addr = 0x006300EB;

int gameseq_stack_top()
{
    return addr_as_ref<int32_t>(gameseq_state_index_addr);
}

rf::GameState gameseq_state_at(int index)
{
    return static_cast<rf::GameState>(
        addr_as_ref<int32_t>(gameseq_state_stack_addr + index * 4));
}

// True while any deferred change is queued: the stack we can observe now is not
// the stack that will be current once it lands, so no pop may be issued.
bool gameseq_change_pending()
{
    return addr_as_ref<uint8_t>(gameseq_pop_pending_addr) != 0
        || addr_as_ref<uint8_t>(gameseq_push_pending_addr) != 0
        || static_cast<int>(rf::gameseq_get_pending_state()) != 0;
}

// What the stock RETURN TO GAME button unwinds into. Limbo counts: the menu can
// be opened during the between-levels wait and stock returns there too.
bool is_returnable_bottom_state(rf::GameState state)
{
    return state == rf::GS_GAMEPLAY || state == rf::GS_MULTI_LIMBO;
}

// One-shot, consumed by the next main menu frame. It also expires on its own:
// if the engine takes over (instantly passed vote -> level change -> limbo) the
// main menu may never run again, and a flag left set would otherwise fire
// minutes later and close the ESC menu out from under the player.
constexpr int64_t auto_return_lifetime_ms = 1000;
bool g_auto_return_to_game = false;
int64_t g_auto_return_deadline_ms = 0;

void clear_auto_return()
{
    g_auto_return_to_game = false;
    g_auto_return_deadline_ms = 0;
}

// --- deferred vote send ----------------------------------------------------
// The vote call is stashed on click and only sent once the menu unwind has
// reached a terminal state. gameseq_set_state (0x00434190) silently drops a
// non-forced transition while a deferred change is queued, so sending on click
// let the server's reply race our pops: a vote that passes instantly (sole
// eligible voter) had its enter-limbo transition discarded, leaving the client
// in GS_GAMEPLAY with no traffic -- the connection-interrupted overlay -- for
// the whole limbo. Sending only once the slot is free makes that impossible.
bool g_pending_vote_valid = false;
AfVoteCallParams g_pending_vote;

void drop_pending_vote()
{
    g_pending_vote_valid = false;
    g_pending_vote = AfVoteCallParams{};
}

// --- open context -----------------------------------------------------------
// Which site owns rendering. Exactly one render call happens per frame.
enum class VotePanelContext
{
    None,
    MultiMenu,
    Gameplay,
};

VotePanelContext g_context = VotePanelContext::None;

// Player's local "filter the map list by gametype" choice. Session only, not
// persisted; ignored (forced on) while the server enforces the prefix.
bool g_filter_gametype_local = false;

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

// A stock popup (manual level name, int/float option) takes over input while it
// is up. gameseq_process passes no_input=1 to the state's own frame, but our
// gameplay pump runs outside that, so it has to check explicitly.
bool stock_popup_is_active()
{
    return addr_as_ref<bool()>(0x00456680)();
}

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

    if (player && g_gameplay_mouse_overridden) {
        player->settings.controls.mouse_look = g_gameplay_prev_mouse_look;
    }
    g_gameplay_mouse_overridden = false;
    // Only re-grab the cursor if we are actually still in gameplay; on a level
    // change the engine owns the mouse mode.
    if (rf::gameseq_get_state() == rf::GS_GAMEPLAY && !rf::keep_mouse_centered) {
        rf::mouse_keep_centered_enable();
        rf::mouse_set_visible(false);
    }
}

rf::ui::Button& stock_multi_menu_button(int index)
{
    return addr_as_ref<rf::ui::Button>(multi_menu_first_button + index * multi_menu_button_stride);
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
     "Everyone spawns with the Rail Driver only, and one hit kills. Unlimited ammo, no reloading, "
     "no weapon switching, no item pickups."},
    {"rails",
     "Spawn with only the Riot Stick. All weapon and ammo pickups become the featured weapon and "
     "never despawn. Other items are hidden."},
    {"arena",
     "Spawn with 100 health and 100 armor, a Riot Stick and an Assault Rifle. Each frag refills "
     "your health, armor and ammo. Weapon pickups only."},
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
// ("Damage Control") which overflow the selector at menu-native resolutions.
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
    size_t built_gametypes = 0;
    size_t built_mutators = 0;
    size_t built_levels = 0;

    int type_index = 0;
    int kick_index = 0;
    std::string level_selection; // level NAME; empty = the "Current level" row
    bool manual_level = false;
    std::string manual_level_name;
    int gametype_index = 0; // 0 = server default
    int team_size = 4;
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

bool g_open = false;
FormState g_form;

// Popup input targets. The stock popup callback takes no arguments, so the
// widget that opened it has to be remembered here.
int g_popup_mutator = -1;
int g_popup_option = -1;

enum class PanelMode
{
    NotSupported,
    Loading,
    ActiveVote,
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

void build_form(const VoteOptionsData& options)
{
    g_form.built = true;
    g_form.built_gametypes = options.gametypes.size();
    g_form.built_mutators = options.mutators.size();
    g_form.built_levels = options.levels.size();

    g_form.type_index = 0;
    g_form.kick_index = 0;
    g_form.level_selection.clear();
    g_form.manual_level = false;
    g_form.manual_level_name.clear();
    g_form.gametype_index = 0;
    g_form.team_size = 4;
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
    if (!g_form.built || g_form.built_gametypes != options.gametypes.size()
        || g_form.built_mutators != options.mutators.size()
        || g_form.built_levels != options.levels.size()) {
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
// works from the multi menu and suppresses our input handlers while it is up)
// ---------------------------------------------------------------------------

void manual_level_popup_callback()
{
    char buffer[32] = "";
    rf::ui::popup_get_input(buffer, sizeof(buffer));
    g_form.manual_level_name = buffer;
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
}

// ---------------------------------------------------------------------------
// Panel content
// ---------------------------------------------------------------------------

PanelMode compute_mode()
{
    if (!is_server_minimum_af_version(1, 4)) {
        return PanelMode::NotSupported;
    }
    if (vote_state_get().has_value()) {
        return PanelMode::ActiveVote;
    }
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

void close_panel_and_return_to_game()
{
    vote_panel_close();

    // Only start the unwind when the stack is exactly what we intend to unwind,
    // right now, with nothing already queued. A vote that passes instantly (sole
    // eligible voter) can have the server's level change in flight before we get
    // here, and the engine's transition must win -- if we popped anyway our pop
    // would either swallow its gameseq_set_state or, once it collapsed the stack,
    // underflow the index.
    if (gameseq_change_pending() || gameseq_stack_top() != 2
        || gameseq_state_at(2) != rf::GS_MULTI_MENU
        || gameseq_state_at(1) != rf::GS_MAIN_MENU
        || !is_returnable_bottom_state(gameseq_state_at(0))) {
        clear_auto_return();
        return; // panel is closed; the engine takes it from here
    }

    // The stock BACK handler pops GS_MULTI_MENU (deferred). The second pop, out
    // of GS_MAIN_MENU, has to wait for the next frame because gameseq_pop_state
    // only sets pending flags and is idempotent within one frame.
    AddrCaller{multi_menu_back_on_click_addr}.c_call<void>(0, 0);
    g_auto_return_to_game = true;
    g_auto_return_deadline_ms = timer::get_i64(1000) + auto_return_lifetime_ms;
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
        default:
            break; // parameterless
    }

    // Stashed rather than sent: the packet goes out from vote_send_tick() once
    // the unwind is done and the deferred-change slot is free again.
    g_pending_vote = std::move(params);
    g_pending_vote_valid = true;

    play_click_sound();
    close_panel_and_return_to_game();
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

void do_active_vote(PanelUi& ui, const Layout& lo)
{
    const auto& state = vote_state_get();
    if (!state) {
        return;
    }

    if (ui.draw) {
        const int line_h = rf::gr::get_font_height(lo.font) + lo.gap;
        int y = lo.cy + lo.row_h;
        set_header_color();
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, lo.cx + lo.cw / 2, y,
            "A VOTE IS ALREADY IN PROGRESS", lo.font);
        y += line_h * 2;

        const auto title_lines = wrap_text(state->title, lo.cw, lo.font);
        rf::gr::set_color(255, 255, 255, 255);
        for (const auto& line : title_lines) {
            rf::gr::string_aligned(rf::gr::ALIGN_CENTER, lo.cx + lo.cw / 2, y, line.c_str(), lo.font);
            y += line_h;
        }
        y += line_h;

        const std::string tally = std::format("Yes: {}    No: {}    Waiting: {}",
            static_cast<int>(state->yes), static_cast<int>(state->no),
            static_cast<int>(state->remaining));
        rf::gr::set_color(160, 255, 190, 255);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, lo.cx + lo.cw / 2, y, tally.c_str(), lo.font);
        y += line_h;

        const std::string time_left = std::format("{} seconds remaining", vote_state_seconds_remaining());
        rf::gr::set_color(215, 215, 215, 255);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, lo.cx + lo.cw / 2, y, time_left.c_str(), lo.font);
    }

    if (state->is_owner) {
        const int btn_w = std::min(lo.cw / 2, scaled(200.0f));
        const Rect cancel{lo.cx + (lo.cw - btn_w) / 2, lo.cy + lo.ch - lo.row_h - lo.gap, btn_w, lo.row_h};
        if (ui_button(ui, cancel, "CANCEL VOTE", lo.font)) {
            af_send_vote_cancel();
            play_click_sound();
        }
    }
}

// Total height of every mutator row plus the option rows of expanded mutators.
// Must match the advance in do_mutator_rows exactly (every option consumes a row
// whether or not its type has a widget yet).
int mutator_content_height(const Layout& lo, const VoteOptionsData& options)
{
    int h = 0;
    for (size_t i = 0; i < options.mutators.size() && i < g_form.mutators.size(); ++i) {
        h += lo.row_h;
        if (g_form.mutators[i].enabled) {
            const size_t count =
                std::min(options.mutators[i].options.size(), g_form.mutators[i].options.size());
            h += static_cast<int>(count) * lo.row_h;
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
            }
            if (ui_checkbox(ui, row, selection.enabled, schema.label.c_str(), lo.font)) {
                selection.enabled = !selection.enabled;
                g_form.description_mutator = static_cast<int>(i);
                play_toggle_sound(selection.enabled);
            }
        }
        y += lo.row_h;

        if (!selection.enabled) {
            continue;
        }

        for (size_t o = 0; o < schema.options.size() && o < selection.options.size(); ++o) {
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
                        // Indices, not positions, so scrolling can't misdirect the popup.
                        g_popup_mutator = static_cast<int>(i);
                        g_popup_option = static_cast<int>(o);
                        rf::ui::popup_message(option.label.c_str(), "", mutator_option_popup_callback, 1);
                    }
                    break;
                }
                default:
                    break; // string options have no widget yet
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
        const Rect cb{cb_x, y - 1, std::max(0, col.x + col.w - cb_x), header_h + 2};
        if (ui_checkbox(ui, cb, options.gametype_prefix_restricted || g_filter_gametype_local,
                "Filter gametype", lo.font, !options.gametype_prefix_restricted)) {
            g_filter_gametype_local = !g_filter_gametype_local;
            play_toggle_sound(g_filter_gametype_local);
        }
    }
    y += header_h + lo.gap;

    // Read after the checkbox so a toggle takes effect in the same pass.
    const bool filter_active = options.gametype_prefix_restricted || g_filter_gametype_local;

    // Levels the server would accept with the game type currently selected in the
    // form. valid_gametype_mask always carries real prefix-match data now, so an
    // unenforced server can still be filtered locally on request.
    // Alphabetical rather than the blob's order (rotation order, then extras).
    std::vector<std::string> levels;
    levels.reserve(options.levels.size());
    int hidden_count = 0;
    for (const auto& level : options.levels) {
        const bool allowed = !filter_active
            || (gametype == af_vote_gametype_none
                ? vote_level_allows_default_gametype(level)
                : vote_level_allows_gametype(level, gametype));
        if (allowed) {
            levels.push_back(level.filename);
        }
        else {
            ++hidden_count;
        }
    }
    std::sort(levels.begin(), levels.end(), level_name_less);

    // A game type change can hide the selected map; drop the selection so it can
    // never be sent. Match then falls back to the pinned "Current level" row.
    if (!g_form.level_selection.empty()
        && std::find(levels.begin(), levels.end(), g_form.level_selection) == levels.end()) {
        g_form.level_selection.clear();
    }
    // A Level vote has no pinned row, so keep the top entry active by default.
    if (!allow_current && g_form.level_selection.empty() && !levels.empty()) {
        g_form.level_selection = levels.front();
    }

    const int line_h = rf::gr::get_font_height(lo.font) + 1;
    const bool show_hint = filter_active && hidden_count > 0;
    const int hint_h = show_hint ? 2 * line_h + lo.gap : 0;

    const int manual_rows = 2 * lo.row_h + lo.gap;
    const int list_h = std::max(lo.row_h * 3, col.y + col.h - y - manual_rows - hint_h - lo.gap);
    const Rect list{col.x, y, col.w, list_h};

    if (!allow_current && levels.empty()) {
        // Nothing votable for this game type. Manual entry stays available and
        // unfiltered; CALL VOTE stays inert because the selection is empty.
        if (ui.draw) {
            rf::gr::set_color(8, 8, 8, 235);
            rf::gr::rect(list.x, list.y, list.w, list.h);
            rf::gr::set_color(120, 120, 120, 255);
            hud_rect_border(list.x, list.y, list.w, list.h, 1);
            const int pad = std::max(3, scaled(6.0f));
            draw_wrapped({list.x + pad, list.y + pad, list.w - 2 * pad, list.h - 2 * pad},
                "No maps available for this game type", lo.font, line_h);
        }
    }
    else {
        std::vector<std::string> items;
        items.reserve(levels.size() + 1);
        if (allow_current) {
            // Pinned and always visible; the server adjudicates it at call time.
            items.emplace_back("Current level");
        }
        for (const auto& level : levels) {
            items.push_back(level);
        }

        // Resolve the highlighted row from the stored name so it follows the map
        // through the sort/filter instead of pointing at whatever sits there now.
        const int first_level_row = allow_current ? 1 : 0;
        int sel = -1;
        if (!g_form.manual_level) {
            if (g_form.level_selection.empty()) {
                sel = allow_current ? 0 : -1;
            }
            else {
                for (size_t i = first_level_row; i < items.size(); ++i) {
                    if (items[i] == g_form.level_selection) {
                        sel = static_cast<int>(i);
                        break;
                    }
                }
            }
        }

        const int clicked = ui_listbox(ui, list, items, sel, g_form.level_scroll, lo.font);
        if (clicked >= 0) {
            g_form.level_selection =
                (allow_current && clicked == 0) ? std::string{} : items[clicked];
            g_form.manual_level = false;
            play_click_sound();
        }
    }
    y += list_h + lo.gap;

    if (show_hint) {
        if (ui.draw) {
            draw_wrapped({col.x, y, col.w, 2 * line_h},
                "Some maps hidden: not valid for this game type", lo.font, line_h);
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

        const auto candidates = build_kick_candidates();
        std::vector<std::string> names;
        names.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            names.push_back(candidate.name);
        }
        g_form.kick_index = std::clamp(g_form.kick_index, 0,
            std::max(0, static_cast<int>(names.size()) - 1));

        const Rect list{lo.cx, y, lo.cw, body_bottom - y};
        const int clicked = ui_listbox(ui, list, names, g_form.kick_index, g_form.kick_scroll, lo.font);
        if (clicked >= 0) {
            g_form.kick_index = clicked;
            play_click_sound();
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

        // Title.
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
        case PanelMode::ActiveVote:
            do_active_vote(ui, lo);
            break;
        case PanelMode::Form:
            if (options) {
                do_form(ui, lo, *options);
            }
            break;
    }

    // Footer buttons, sized and centred like the spray picker's Cancel button.
    const int btn_w = std::min(lo.cw / 2 - lo.gap, scaled(240.0f));
    const int btn_h = std::max(lo.row_h, scaled(40.0f));
    const int btn_y = lo.footer_y + (lo.footer_h - btn_h) / 2;
    if (mode == PanelMode::Form) {
        const Rect call{lo.cx + lo.cw / 2 - btn_w - lo.gap, btn_y, btn_w, btn_h};
        if (ui_button(ui, call, "CALL VOTE", lo.font) && options) {
            send_vote_from_form(*options);
            return;
        }
        const Rect close{lo.cx + lo.cw / 2 + lo.gap, btn_y, btn_w, btn_h};
        if (ui_button(ui, close, "CLOSE (Esc)", lo.font)) {
            vote_panel_close();
            play_click_sound();
        }
    }
    else {
        const Rect close{lo.cx + (lo.cw - btn_w) / 2, btn_y, btn_w, btn_h};
        if (ui_button(ui, close, "CLOSE (Esc)", lo.font)) {
            vote_panel_close();
            play_click_sound();
        }
    }
}

// ---------------------------------------------------------------------------
// Multi menu integration
// ---------------------------------------------------------------------------

void call_vote_button_on_click(int, int)
{
    vote_panel_open();
    rf::snd_play(stock_sound_id::menu_select, 0, 0.0f, 1.0f);
}

void update_call_vote_button_enabled()
{
    g_call_vote_button.enabled = rf::is_multi && !rf::is_server && g_menu_opened_in_game != 0;
}

FunHook<void()> multi_menu_init_hook{
    multi_menu_init_addr,
    []() {
        multi_menu_init_hook.call_target();

        if (!g_call_vote_button_created) {
            g_call_vote_button.init();
            g_call_vote_button.create("button_more.tga", "button_selected.tga", 0, 0, rf::KEY_V,
                "CALL VOTE", rf::ui::medium_font_0);
            g_call_vote_button.key = rf::KEY_V;
            g_call_vote_button.on_click = call_vote_button_on_click;

            // Append to the stock hit-test array so the button becomes hit-test
            // index 4; the AF dispatch array must use that same index order.
            AddrCaller{multi_menu_hit_test_array_add}.this_call<int>(
                reinterpret_cast<void*>(multi_menu_hit_test_array), &g_call_vote_button);

            for (int i = 0; i < 4; ++i) {
                g_multi_menu_gadgets.items[i] = &stock_multi_menu_button(i);
            }
            g_multi_menu_gadgets.items[call_vote_gadget_index] = &g_call_vote_button;
            g_multi_menu_gadgets.count = 5;
            g_call_vote_button_created = true;
        }

        update_call_vote_button_enabled();
        vote_panel_close();
        // Re-entering the multi menu means no unwind of ours is in flight.
        clear_auto_return();
        drop_pending_vote();
    },
};

// The stock helpers walk the button block by raw address, so they cannot see a
// fifth button living outside it; they are replaced rather than patched.
void multi_menu_nav(int dir)
{
    const int count = g_multi_menu_gadgets.count;
    if (count <= 0) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        g_multi_menu_focus_index += dir;
        if (g_multi_menu_focus_index >= count) {
            g_multi_menu_focus_index = 0;
        }
        else if (g_multi_menu_focus_index < 0) {
            g_multi_menu_focus_index = count - 1;
        }
        const rf::ui::Gadget* gadget = g_multi_menu_gadgets.items[g_multi_menu_focus_index];
        if (gadget && gadget->enabled) {
            break;
        }
    }
}

FunHook<void()> multi_menu_nav_next_hook{multi_menu_nav_next_addr, []() { multi_menu_nav(1); }};
FunHook<void()> multi_menu_nav_prev_hook{multi_menu_nav_prev_addr, []() { multi_menu_nav(-1); }};

FunHook<void()> multi_menu_mouse_hook{
    multi_menu_mouse_handler_addr,
    []() {
        if (vote_panel_is_open()) {
            vote_panel_handle_mouse();
            return;
        }
        multi_menu_mouse_hook.call_target();
    },
};

// Replaces the stock button render loop so the buttons come from the AF array
// (in visual order) and the vote panel can be drawn before the mouse cursor.
CodeInjection multi_menu_render_injection{
    multi_menu_render_button_loop,
    [](auto& regs) {
        static const int visual_order[] = {0, 1, 2, call_vote_gadget_index, 3};

        update_call_vote_button_enabled();

        const int x = regs.ebx;
        int y = regs.edi;
        for (int index : visual_order) {
            if (index >= g_multi_menu_gadgets.count) {
                continue;
            }
            auto* button = static_cast<rf::ui::Button*>(g_multi_menu_gadgets.items[index]);
            if (!button || !button->enabled) {
                continue; // disabled buttons consume no row, same as stock
            }
            button->x = x;
            button->y = y;
            button->render();
            y += rf::ui::menu_button_offset_y;
        }

        vote_panel_render();

        regs.eip = multi_menu_render_button_loop_end;
    },
};

// Registered every frame by the multi menu do_frame via PUSH <handler>.
void multi_menu_key_handler(int key)
{
    if (vote_panel_is_open()) {
        vote_panel_handle_key(key);
        return;
    }
    // The stock handler forwards every key to the character setup sub-panel
    // while it is open; don't steal the hotkey from it.
    const bool sub_panel_open = addr_as_ref<uint8_t>(0x0063E088) != 0;
    if (!sub_panel_open && key == rf::KEY_V && g_call_vote_button.enabled) {
        g_multi_menu_focus_index = call_vote_gadget_index;
        if (g_call_vote_button.on_click) {
            g_call_vote_button.on_click(0, 0);
        }
        return;
    }
    AddrCaller{multi_menu_key_handler_addr}.c_call<void>(key);
}

// True only when popping GS_MAIN_MENU back to gameplay is provably safe this
// frame. Every other observation abandons the auto-return rather than guessing.
bool consume_auto_return()
{
    if (!g_auto_return_to_game) {
        return false;
    }

    // Left the server, or the engine never gave the main menu another frame.
    if (!rf::is_multi || timer::get_i64(1000) > g_auto_return_deadline_ms) {
        clear_auto_return();
        return false;
    }

    // GS_MULTI_MENU is transparent, so this hook also runs underneath it. The
    // one legitimate in-flight observation is the frame before our own
    // GS_MULTI_MENU pop is applied; anything else means the engine took over.
    const rf::GameState state = rf::gameseq_get_state();
    if (state != rf::GS_MAIN_MENU) {
        const bool our_pop_in_flight = state == rf::GS_MULTI_MENU
            && addr_as_ref<uint8_t>(gameseq_pop_pending_addr) != 0
            && static_cast<int>(rf::gameseq_get_pending_state()) == 0;
        if (!our_pop_in_flight) {
            clear_auto_return();
        }
        return false;
    }

    // Main menu is on top. Require exactly [gameplay, GS_MAIN_MENU] and no
    // queued change, so the pop cannot underflow and cannot race a transition.
    if (gameseq_change_pending() || gameseq_stack_top() != 1
        || gameseq_state_at(1) != rf::GS_MAIN_MENU
        || !is_returnable_bottom_state(gameseq_state_at(0))) {
        clear_auto_return();
        return false;
    }

    clear_auto_return();
    return true;
}

FunHook<void(int)> mainmenu_do_frame_hook{
    mainmenu_do_frame_addr,
    [](int no_input) {
        if (consume_auto_return()) {
            AddrCaller{mainmenu_do_return_addr}.c_call<void>();
            return; // skip the main menu frame entirely so it never flashes
        }
        mainmenu_do_frame_hook.call_target(no_input);
    },
};

// Terminal states of the unwind: no auto-return still owed and nothing queued in
// the deferred-change slot. That covers "unwind completed" (back in gameplay),
// "unwind abandoned, still connected" (the server adjudicates and replies), and
// guarantees our reply can no longer collide with a pop of ours.
void vote_send_tick()
{
    if (!g_pending_vote_valid) {
        return;
    }
    if (!rf::is_multi || rf::is_server) {
        drop_pending_vote(); // disconnected before we got to send it
        return;
    }
    if (g_auto_return_to_game || gameseq_change_pending()) {
        return; // unwind still in flight
    }

    const AfVoteCallParams params = g_pending_vote; // one-shot
    drop_pending_vote();
    af_send_vote_call(params);
}

// Runs every frame regardless of game state, right after the state machine has
// applied this frame's deferred change.
CallHook<rf::GameState()> gameseq_process_hook{
    0x004B2DB1,
    []() -> rf::GameState {
        // Before the state runs its frame, so the overlay sees keys/clicks first.
        vote_panel_decay_attack_swallow();
        vote_panel_gameplay_input();
        const rf::GameState state = gameseq_process_hook.call_target();
        vote_send_tick();
        return state;
    },
};

// ESC during gameplay pushes GS_MAIN_MENU. While the overlay is up, treat that
// as "close the modal" and swallow the push, so ESC behaves like it does in the
// menu context and the ESC menu can never open on top of the overlay.
FunHook<void(rf::GameState, bool, bool)> gameseq_push_state_hook{
    0x00434410,
    [](rf::GameState state, bool transparent, bool pause_beneath) {
        if (state == rf::GS_MAIN_MENU && g_open && g_context == VotePanelContext::Gameplay) {
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
    const bool overlay_up = g_open && g_context == VotePanelContext::Gameplay;
    if (!overlay_up && g_swallow_attack_frames <= 0) {
        return false;
    }
    if (!rf::local_player || ccp != &rf::local_player->settings.controls) {
        return false;
    }
    return action == rf::CC_ACTION_PRIMARY_ATTACK || action == rf::CC_ACTION_SECONDARY_ATTACK;
}

FunHook<bool(rf::ControlConfig*, rf::ControlConfigAction, bool*)> vote_panel_check_pressed_hook{
    0x0043D4F0,
    [](rf::ControlConfig* ccp, rf::ControlConfigAction action, bool* just_pressed) {
        if (gameplay_overlay_blocks_action(ccp, action)) {
            if (just_pressed) {
                *just_pressed = false;
            }
            return false;
        }
        return vote_panel_check_pressed_hook.call_target(ccp, action, just_pressed);
    },
};

FunHook<bool(rf::ControlConfig*, rf::ControlConfigAction)> vote_panel_is_control_down_hook{
    0x00430F40,
    [](rf::ControlConfig* ccp, rf::ControlConfigAction action) {
        if (gameplay_overlay_blocks_action(ccp, action)) {
            return false;
        }
        return vote_panel_is_control_down_hook.call_target(ccp, action);
    },
};

} // namespace

bool vote_panel_is_open()
{
    return g_open;
}

void vote_panel_open()
{
    // Opening from the menu supersedes a gameplay overlay (no double-open).
    if (g_context == VotePanelContext::Gameplay) {
        gameplay_overlay_apply_mouse(false);
    }
    g_open = true;
    g_context = VotePanelContext::MultiMenu;
    g_form.description_mutator = -1;
    g_form.mutator_scroll = 0.0f;
    drop_pending_vote(); // a stash from a previous open can never outlive it
    vote_options_request_if_needed();
}

void vote_panel_close()
{
    if (g_context == VotePanelContext::Gameplay) {
        gameplay_overlay_apply_mouse(false);
        // Rest of this frame plus at least one whole frame, extended while held.
        g_swallow_attack_frames = 2;
    }
    g_open = false;
    g_context = VotePanelContext::None;
}

void vote_panel_render()
{
    if (!g_open || g_context != VotePanelContext::MultiMenu) {
        return;
    }
    if (!rf::is_multi) {
        vote_panel_close();
        clear_auto_return(); // left the server mid-unwind
        drop_pending_vote();
        return;
    }

    int x = 0, y = 0, z = 0;
    rf::mouse_get_pos(x, y, z);

    PanelUi ui;
    ui.draw = true;
    ui.mx = x;
    ui.my = y;
    vote_panel_do(ui);

    // The menu frame + mouse cursor are drawn right after us; never leave a
    // narrowed clip window behind.
    rf::gr::reset_clip();
}

void vote_panel_handle_mouse()
{
    if (!g_open) {
        return;
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

void vote_panel_handle_key(int key)
{
    if (!g_open) {
        return;
    }
    if (key == rf::KEY_ESC) {
        vote_panel_close();
        play_click_sound();
    }
    // Everything else is swallowed while the modal is up.
}

bool vote_panel_is_gameplay_overlay_active()
{
    return g_open && g_context == VotePanelContext::Gameplay;
}

void vote_panel_toggle_gameplay()
{
    if (g_open) {
        // Also covers "menu instance somehow still open": close whatever is up.
        vote_panel_close();
        play_click_sound();
        return;
    }
    if (!rf::is_multi || rf::is_server || rf::gameseq_get_state() != rf::GS_GAMEPLAY) {
        return;
    }

    g_open = true;
    g_context = VotePanelContext::Gameplay;
    g_form.description_mutator = -1;
    g_form.mutator_scroll = 0.0f;
    drop_pending_vote();
    gameplay_overlay_apply_mouse(true);
    vote_options_request_if_needed(); // shared cache, same as the menu path
    rf::snd_play(stock_sound_id::menu_select, 0, 0.0f, 1.0f);
}

void vote_panel_gameplay_input()
{
    if (g_context != VotePanelContext::Gameplay) {
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

    if (stock_popup_is_active()) {
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
    if (!g_open || g_context != VotePanelContext::Gameplay) {
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

void vote_panel_apply_patch()
{
    multi_menu_init_hook.install();
    multi_menu_nav_next_hook.install();
    multi_menu_nav_prev_hook.install();
    multi_menu_mouse_hook.install();
    multi_menu_render_injection.install();
    mainmenu_do_frame_hook.install();
    gameseq_process_hook.install();
    gameseq_push_state_hook.install();
    vote_panel_check_pressed_hook.install();
    vote_panel_is_control_down_hook.install();

    // Seed the stock entries so the patched array is never empty, even if
    // something touches the menu before multi_menu_init has run.
    for (int i = 0; i < 4; ++i) {
        g_multi_menu_gadgets.items[i] = &stock_multi_menu_button(i);
    }
    g_multi_menu_gadgets.count = 4;

    // Hit-test count is hardcoded; our button is index 4.
    AsmWriter{multi_menu_hit_test_count_push}.push(5);

    // Point every read of the (full) stock dispatch array at the AF array.
    for (uintptr_t site : dispatch_array_sites) {
        write_mem<void*>(site + 1, &g_multi_menu_gadgets);
    }

    // Retarget the per-frame key handler registration to the AF wrapper.
    write_mem<void*>(multi_menu_key_handler_push_imm, reinterpret_cast<void*>(&multi_menu_key_handler));
}
