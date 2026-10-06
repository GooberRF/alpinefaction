#include <algorithm>
#include <format>
#include <string>
#include "demo.h"
#include "demo_ui.h"
#include "../../misc/vote_panel.h"
#include "../../hud/hud.h"
#include "../../hud/hud_internal.h"
#include "../../hud/remote_server_cfg_ui.h"
#include "../../input/control_input_filter.h"
#include "../../input/input.h"
#include "../../sound/sound.h"
#include "../../rf/gameseq.h"
#include "../../rf/gr/gr.h"
#include "../../rf/gr/gr_font.h"
#include "../../rf/input.h"
#include "../../rf/player/player.h"
#include "../../rf/sound/sound.h"

namespace
{

bool g_open = false;
bool g_dragging = false;
double g_preview_ms = 0.0;
// A backward seek restarts the whole session, which force-closes the bar when
// gameplay tears down. Remember that and reopen once the restarted session has
// fast-forwarded back to the target, so a rewind doesn't eat the UI.
bool g_reopen_pending = false;
// Cursor position saved when a rewind force-closes the bar; restored on reopen
// (the session restart re-centers the cursor).
bool g_reopen_cursor_valid = false;
int g_reopen_cursor_x = 0;
int g_reopen_cursor_y = 0;
// Same idiom as the vote panel: the click that ends a drag is still latched for
// the rest of the frame and would fire the weapon / cycle the spectated player.
int g_swallow_attack_frames = 0;
bool g_veto_registered = false;

struct Rect
{
    int x, y, w, h;
};

bool point_in(int px, int py, const Rect& rect)
{
    return px >= rect.x && px < rect.x + rect.w && py >= rect.y && py < rect.y + rect.h;
}

struct Layout
{
    Rect strip;     // translucent backing
    Rect track;     // scrubber track (visual)
    Rect track_hit; // scrubber hit area (padded)
    Rect btn_back;  // -10s
    Rect btn_pause; // play/pause
    Rect btn_fwd;   // +10s
};

// Reference-resolution scaling, like the other Alpine overlays.
constexpr float ref_width = 1280.0f;
constexpr float ref_height = 800.0f;

Layout compute_layout()
{
    const float scale =
        std::min(rf::gr::clip_width() / ref_width, rf::gr::clip_height() / ref_height);
    const auto scaled = [scale](float value) {
        return static_cast<int>(value * scale + 0.5f);
    };

    Layout lay{};
    lay.strip.w = std::min(scaled(700.0f), rf::gr::clip_width() - scaled(120.0f));
    lay.strip.h = scaled(70.0f);
    lay.strip.x = (rf::gr::clip_width() - lay.strip.w) / 2;
    lay.strip.y = rf::gr::clip_height() - scaled(110.0f);

    lay.track.x = lay.strip.x + scaled(16.0f);
    lay.track.y = lay.strip.y + scaled(12.0f);
    lay.track.w = lay.strip.w - 2 * scaled(16.0f);
    lay.track.h = scaled(10.0f);
    const int pad = scaled(6.0f);
    lay.track_hit = {lay.track.x - pad, lay.track.y - pad, lay.track.w + 2 * pad, lay.track.h + 2 * pad};

    const int btn_w = scaled(64.0f);
    const int btn_h = scaled(24.0f);
    const int gap = scaled(10.0f);
    const int row_w = 3 * btn_w + 2 * gap;
    const int row_x = lay.strip.x + (lay.strip.w - row_w) / 2;
    const int row_y = lay.strip.y + scaled(34.0f);
    lay.btn_back = {row_x, row_y, btn_w, btn_h};
    lay.btn_pause = {row_x + btn_w + gap, row_y, btn_w, btn_h};
    lay.btn_fwd = {row_x + 2 * (btn_w + gap), row_y, btn_w, btn_h};
    return lay;
}

// Scrub-bar denominator: footerless (still-recording) files report duration 0 and
// grow it as records stream in, so the clock is the only reliable lower bound.
double effective_duration_ms()
{
    return std::max({static_cast<double>(demo_playback_duration_ms()), demo_playback_clock_ms(), 1.0});
}

double ms_from_x(int mx, const Rect& track)
{
    const int rel = std::clamp(mx - track.x, 0, track.w);
    return static_cast<double>(rel) / static_cast<double>(track.w) * effective_duration_ms();
}

std::string format_time_ms(double time_ms)
{
    const auto total_sec = static_cast<int>(time_ms / 1000.0);
    return std::format("{}:{:02}", total_sec / 60, total_sec % 60);
}

void play_click_sound()
{
    rf::snd_play(stock_sound_id::panel_button_click, 0, 0.0f, 1.0f);
}

void decay_attack_swallow()
{
    if (g_swallow_attack_frames <= 0) {
        return;
    }
    if (rf::mouse_button_is_down(0) || rf::mouse_button_is_down(1)) {
        return; // still held: keep swallowing
    }
    --g_swallow_attack_frames;
}

// Clicking the bar must not also fire the weapon - primary/secondary attack is
// what drives spectate target cycling during playback.
bool controls_ui_blocks_action(rf::ControlConfig* ccp, rf::ControlConfigAction action)
{
    if (!g_open && g_swallow_attack_frames <= 0) {
        return false;
    }
    if (!rf::local_player || ccp != &rf::local_player->settings.controls) {
        return false;
    }
    return action == rf::CC_ACTION_PRIMARY_ATTACK || action == rf::CC_ACTION_SECONDARY_ATTACK;
}

void ensure_control_veto_registered()
{
    if (g_veto_registered) {
        return;
    }
    control_input_filter_add_veto(&controls_ui_blocks_action);
    g_veto_registered = true;
}

void open(bool play_sound = true)
{
    if (!demo_playback_active() || !demo_playback_can_seek() || demo_playback_is_seeking()
        || rf::gameseq_get_state() != rf::GS_GAMEPLAY) {
        return;
    }
    // Mutually exclusive with the other gameplay overlays: all read the same
    // non-consuming mouse state and share the cursor override.
    if (vote_panel_is_gameplay_overlay_active()) {
        vote_panel_close();
    }
    if (g_remote_server_cfg_popup.is_active()) {
        g_remote_server_cfg_popup.toggle();
    }
    ensure_control_veto_registered();
    g_open = true;
    g_dragging = false;
    g_reopen_pending = false;
    gameplay_overlay_apply_mouse(true);
    if (play_sound) {
        rf::snd_play(stock_sound_id::menu_select, 0, 0.0f, 1.0f);
    }
}

// True when the overlay must not stay open: playback gone (demo_stop), the
// backward-seek session restart left gameplay, or a level change landed.
bool must_force_close()
{
    return !demo_playback_active() || !demo_playback_can_seek()
        || rf::gameseq_get_state() != rf::GS_GAMEPLAY;
}

// Force-close from the input/render passes. A close caused by a queued session
// restart (rewind) arms the reopen so the bar comes back after the restart;
// every other cause (demo_stop, demo ended into limbo) closes for good.
void force_close()
{
    demo_controls_ui_close();
    g_reopen_pending = demo_playback_restart_pending();
    if (g_reopen_pending) {
        int z;
        rf::mouse_get_pos(g_reopen_cursor_x, g_reopen_cursor_y, z);
        g_reopen_cursor_valid = true;
    }
}

// Reopen path for a rewind that force-closed the bar: wait out the restart and
// the fast-forward, then reopen silently. Cancel when playback died without a
// restart in flight (failed restart, demo_stop during the wait).
void reopen_do_frame()
{
    if (!g_reopen_pending || g_open) {
        return;
    }
    if (demo_playback_active()) {
        if (demo_playback_can_seek() && !demo_playback_is_seeking()
            && rf::gameseq_get_state() == rf::GS_GAMEPLAY) {
            open(/* play_sound */ false);
            if (g_open && g_reopen_cursor_valid) {
                // mouse_set_pos updates the internal position globals; mouse_force_pos
                // moves the OS cursor. Both are needed: with the bar open the engine is
                // in windowed-mouse mode (mouse_process re-reads GetCursorPos each
                // frame), and mouse_set_pos alone can hit its already-at-position
                // early-out against globals DirectInput left stale during the seek.
                rf::mouse_set_pos(g_reopen_cursor_x, g_reopen_cursor_y);
                rf::mouse_force_pos(g_reopen_cursor_x, g_reopen_cursor_y);
            }
            g_reopen_cursor_valid = false;
        }
    }
    else if (!demo_playback_restart_pending()) {
        g_reopen_pending = false;
        g_reopen_cursor_valid = false;
    }
}

struct ButtonStyle
{
    bool hovered;
    bool enabled;
};

void draw_button(const Rect& rect, const char* text, const ButtonStyle& style)
{
    const int fill = style.hovered ? 70 : 45;
    rf::gr::set_color(fill, fill, fill, 255);
    rf::gr::rect(rect.x, rect.y, rect.w, rect.h);
    const int border = style.enabled ? 170 : 100;
    rf::gr::set_color(border, border, border, 255);
    hud_rect_border(rect.x, rect.y, rect.w, rect.h, 1);
    const int font = hud_get_default_font();
    const int text_gray = style.enabled ? (style.hovered ? 255 : 200) : 110;
    rf::gr::set_color(text_gray, text_gray, text_gray, 255);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, rect.x + rect.w / 2,
                           rect.y + (rect.h - rf::gr::get_font_height(font)) / 2, text, font);
}

} // namespace

bool demo_controls_ui_is_open()
{
    return g_open;
}

void demo_controls_ui_close()
{
    if (!g_open) {
        return;
    }
    g_open = false;
    g_dragging = false;
    gameplay_overlay_apply_mouse(false);
    // Rest of this frame plus at least one whole frame, extended while held.
    g_swallow_attack_frames = 2;
}

bool demo_controls_ui_execute_action(rf::ControlConfigAction action, bool was_pressed)
{
    if (!demo_playback_active()) {
        return false;
    }
    // Alpine action indices are runtime values (they start after the stock and
    // weapon bindings), so no switch here.
    const bool rewind = action == get_af_control(rf::AlpineControlConfigAction::AF_ACTION_VOTE_YES);
    const bool forward = action == get_af_control(rf::AlpineControlConfigAction::AF_ACTION_VOTE_NO);
    if (action == rf::CC_ACTION_USE) {
        if (was_pressed) {
            if (g_open) {
                demo_controls_ui_close();
                play_click_sound();
            }
            else {
                open();
            }
        }
    }
    else if (rewind || forward) {
        // Keyboard shortcuts for the popup's -10s/+10s buttons. Voting does not
        // exist during playback, so the vote keys are free for reuse.
        if (was_pressed && demo_playback_can_seek() && !demo_playback_is_seeking()) {
            play_click_sound();
            demo_playback_request_seek(demo_playback_clock_ms() + (rewind ? -10000.0 : 10000.0));
        }
    }
    else if (action == rf::CC_ACTION_RELOAD) {
        // Keyboard shortcut for the popup's play/pause button; meaningless once
        // the demo has finished (a seek restarts the session instead).
        if (was_pressed && !demo_playback_finished()) {
            play_click_sound();
            demo_playback_toggle_pause();
        }
    }
    else {
        return false;
    }
    // These actions have no other purpose during playback - swallow press and
    // release alike so the phantom spectator player never acts on them.
    return true;
}

void demo_controls_ui_input()
{
    decay_attack_swallow();
    reopen_do_frame();
    if (!g_open) {
        return;
    }
    if (must_force_close()) {
        force_close();
        return;
    }
    // Re-assert every frame: respawns and camera cuts re-enable mouse-look.
    gameplay_overlay_apply_mouse(true);
    if (demo_playback_is_seeking()) {
        // The full-screen SEEKING overlay owns the screen; drop any drag so a
        // stale release cannot fire a second seek when the bar comes back.
        g_dragging = false;
        return;
    }

    const Layout lay = compute_layout();
    int mx = 0;
    int my = 0;
    int mz = 0;
    rf::mouse_get_pos(mx, my, mz);

    if (g_dragging) {
        if (rf::mouse_button_is_down(0)) {
            g_preview_ms = ms_from_x(mx, lay.track); // preview only - no seek yet
        }
        else {
            // Released: commit exactly once. The seek itself runs from the pump
            // (networking phase); request_seek never seeks synchronously.
            g_dragging = false;
            play_click_sound();
            demo_playback_request_seek(g_preview_ms);
        }
        return;
    }

    if (!rf::mouse_was_button_pressed(0)) {
        return;
    }
    if (point_in(mx, my, lay.track_hit)) {
        g_dragging = true;
        g_preview_ms = ms_from_x(mx, lay.track);
    }
    else if (point_in(mx, my, lay.btn_back)) {
        play_click_sound();
        demo_playback_request_seek(demo_playback_clock_ms() - 10000.0);
    }
    else if (point_in(mx, my, lay.btn_pause) && !demo_playback_finished()) {
        play_click_sound();
        demo_playback_toggle_pause();
    }
    else if (point_in(mx, my, lay.btn_fwd)) {
        play_click_sound();
        demo_playback_request_seek(demo_playback_clock_ms() + 10000.0);
    }
}

void demo_controls_ui_render()
{
    if (!g_open) {
        return;
    }
    // The input pump runs before gameseq_process; a level change or teardown can
    // land mid-frame, so re-check here (vote panel precedent).
    if (must_force_close()) {
        force_close();
        return;
    }
    if (demo_playback_is_seeking()) {
        return; // the full-screen SEEKING overlay takes over
    }

    const Layout lay = compute_layout();
    int mx = 0;
    int my = 0;
    int mz = 0;
    rf::mouse_get_pos(mx, my, mz);
    const int font = hud_get_default_font();
    const int font_h = rf::gr::get_font_height(font);

    // Backing
    rf::gr::set_color(0, 0, 0, 150);
    rf::gr::rect(lay.strip.x, lay.strip.y, lay.strip.w, lay.strip.h);
    rf::gr::set_color(170, 170, 170, 255);
    hud_rect_border(lay.strip.x, lay.strip.y, lay.strip.w, lay.strip.h, 1);

    // Track: frame, unplayed remainder, played fill, handle (colors match the
    // fast-forward progress bar in demo_playback.cpp)
    const double shown_ms = g_dragging ? g_preview_ms : demo_playback_clock_ms();
    const double progress = std::clamp(shown_ms / effective_duration_ms(), 0.0, 1.0);
    const int fill_w = static_cast<int>(progress * lay.track.w);
    rf::gr::set_color(0x40, 0x40, 0x40, 255);
    rf::gr::rect(lay.track.x - 2, lay.track.y - 2, lay.track.w + 4, lay.track.h + 4);
    rf::gr::set_color(0, 0, 0, 255);
    rf::gr::rect(lay.track.x, lay.track.y, lay.track.w, lay.track.h);
    rf::gr::set_color(0, 0x80, 0, 255);
    rf::gr::rect(lay.track.x, lay.track.y, fill_w, lay.track.h);
    const int handle_w = std::max(4, lay.track.h - 2);
    const int handle_h = lay.track.h + 8;
    const int handle_x = std::clamp(lay.track.x + fill_w - handle_w / 2, lay.track.x,
                                    lay.track.x + lay.track.w - handle_w);
    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::rect(handle_x, lay.track.y - 4, handle_w, handle_h);

    // Time labels: current on the left, total on the right ("-:--" until a
    // footerless file's duration is known)
    const int label_y = lay.btn_back.y + (lay.btn_back.h - font_h) / 2;
    rf::gr::set_color(200, 200, 200, 255);
    rf::gr::string(lay.track.x, label_y, format_time_ms(demo_playback_clock_ms()).c_str(), font);
    const std::string total = demo_playback_duration_ms() > 0
        ? format_time_ms(demo_playback_duration_ms()) : std::string{"-:--"};
    rf::gr::string_aligned(rf::gr::ALIGN_RIGHT, lay.track.x + lay.track.w, label_y, total.c_str(), font);

    // Buttons; pause is meaningless once the demo has finished (a seek restarts
    // the session instead)
    const bool paused = demo_playback_paused();
    const bool pause_enabled = !demo_playback_finished();
    draw_button(lay.btn_back, "-10s", {point_in(mx, my, lay.btn_back), true});
    draw_button(lay.btn_pause, paused ? ">" : "II",
                {pause_enabled && point_in(mx, my, lay.btn_pause), pause_enabled});
    draw_button(lay.btn_fwd, "+10s", {point_in(mx, my, lay.btn_fwd), true});

    // Drag preview timestamp above the handle
    if (g_dragging) {
        const std::string preview = format_time_ms(g_preview_ms);
        const auto [text_w, text_h] = rf::gr::get_string_size(preview, font);
        const int cx = std::clamp(handle_x + handle_w / 2, lay.strip.x + text_w / 2 + 4,
                                  lay.strip.x + lay.strip.w - text_w / 2 - 4);
        const int ty = lay.strip.y - text_h - 6;
        rf::gr::set_color(0, 0, 0, 200);
        rf::gr::rect(cx - text_w / 2 - 4, ty - 2, text_w + 8, text_h + 4);
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, cx, ty, preview.c_str(), font);
    }

    // gameseq_process re-hides the cursor every gameplay frame; re-asserting here
    // (after it, before the stock cursor render) makes the cursor draw on top.
    rf::mouse_set_visible(true);
}
