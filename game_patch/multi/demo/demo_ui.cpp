#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <vector>
#include "demo.h"
#include "demo_internal.h"
#include "demo_ui.h"
#include "../awards.h"
#include "../gametype.h"
#include "../../misc/vote_panel.h"
#include "../../hud/hud.h"
#include "../../hud/hud_internal.h"
#include "../../hud/multi_spectate.h"
#include "../../hud/remote_server_cfg_ui.h"
#include "../../input/control_input_filter.h"
#include "../../input/input.h"
#include "../../misc/alpine_settings.h"
#include "../../os/console.h"
#include "../../sound/sound.h"
#include "../../rf/gameseq.h"
#include "../../rf/gr/gr.h"
#include "../../rf/gr/gr_font.h"
#include "../../rf/input.h"
#include "../../rf/multi.h"
#include "../../rf/player/player.h"
#include "../../rf/sound/sound.h"

namespace
{

// Expanded: the interactive controls over the nameplate row, with a free cursor.
// Otherwise the panel is compact: a progress line over the nameplate row.
bool g_open = false;
// The panel expands by default whenever it can; USE compacts it until pressed again.
// Survives the backward-seek session restart (which force-closes the panel when
// gameplay tears down), so a rewind brings the controls back; reset once playback ends.
bool g_compact = false;
// Expanded with USE while in a mouse-look camera; see auto_close_in_mouse_look().
bool g_opened_in_mouse_look = false;
bool g_dragging = false;
double g_preview_ms = 0.0;
// Cursor position saved when a rewind force-closes the bar; restored on reopen
// (the session restart re-centers the cursor).
bool g_reopen_cursor_valid = false;
int g_reopen_cursor_x = 0;
int g_reopen_cursor_y = 0;
// Same idiom as the vote panel: armed by a click on the panel, so neither that press
// nor the release ending a drag also cycles the spectated player. Clicks elsewhere
// pass through.
int g_swallow_attack_frames = 0;
bool g_veto_registered = false;
// The viewer's own seek bar marker, set with the Quick Save bind or the panel's marker button,
// jumped to with the Quick Load bind or a click on it.
// Survives the backward-seek session restart like g_compact; cleared once playback ends.
std::optional<uint32_t> g_custom_marker_ms;

struct Rect
{
    int x, y, w, h;
};

// The expanded panel's backing as of the last input pass. The attack veto tests the
// cursor against it: the engine can read a click before the input pass sees it.
Rect g_expanded_strip{};

bool point_in(int px, int py, const Rect& rect)
{
    return px >= rect.x && px < rect.x + rect.w && py >= rect.y && py < rect.y + rect.h;
}

struct Layout
{
    Rect strip;     // translucent backing
    Rect nameplate; // bottom row: the spectate nameplate
    Rect track;     // scrubber track (visual); the progress line when compact
    Rect track_hit; // scrubber hit area (padded)
    Rect btn_back;  // -10s
    Rect btn_pause; // play/pause
    Rect btn_fwd;   // +10s
    Rect btn_speed; // playback speed
    Rect btn_mark;  // set the custom marker
    Rect btn_prev;  // nameplate row, left: previous player / camera
    Rect btn_next;  // nameplate row, right: next player / camera
    int keycap_y;     // transport binds, under their buttons
    int dot_r;        // marker dot radius
    int pickup_dot_y; // pickup dot row (above the track), center
    int award_dot_y;  // award dot row (below the track), center
};

// Key binds on the panel: the transport buttons carry their keyboard shortcuts, and
// the nameplate arrows the attack binds that cycle players. Empty when unbound.
struct PanelLabels
{
    std::string back_bind;
    std::string pause_bind;
    std::string fwd_bind;
    std::string mark_bind;
    std::string jump_bind;
    std::string prev_bind;
    std::string next_bind;
};

std::string bind_name(rf::ControlConfigAction action)
{
    std::string name = get_action_bind_name(action).c_str();
    return name == "?" ? std::string{} : name; // "?" = unbound
}

PanelLabels panel_labels()
{
    return {
        bind_name(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_VOTE_YES)),
        bind_name(rf::CC_ACTION_RELOAD),
        bind_name(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_VOTE_NO)),
        bind_name(rf::CC_ACTION_QUICK_SAVE),
        bind_name(rf::CC_ACTION_QUICK_LOAD),
        bind_name(rf::CC_ACTION_SECONDARY_ATTACK),
        bind_name(rf::CC_ACTION_PRIMARY_ATTACK),
    };
}

// Binds draw as keycaps - dark text on a light chip - so they read as keys rather
// than as part of a caption.
int keycap_pad_x()
{
    return g_alpine_game_config.big_hud ? 6 : 4;
}

int keycap_w(const std::string& bind)
{
    return bind.empty() ? 0 : rf::gr::get_string_size(bind, hud_get_small_font()).first + 2 * keycap_pad_x();
}

int keycap_h()
{
    return rf::gr::get_font_height(hud_get_small_font()) + 2;
}

void draw_keycap(int x, int y, const std::string& bind, bool bright)
{
    if (bind.empty()) {
        return;
    }
    const int fill = bright ? 235 : 170;
    rf::gr::set_color(fill, fill, fill, 255);
    rf::gr::rect(x, y, keycap_w(bind), keycap_h());
    rf::gr::set_color(20, 20, 20, 255);
    rf::gr::string(x + keycap_pad_x(), y + 1, bind.c_str(), hud_get_small_font());
}

// Playback speeds the speed button steps through
constexpr float speed_steps[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f, 8.0f};
// The widest label the button sizes for, so it doesn't resize as the speed changes
constexpr const char* widest_speed_label = "0.25x";

std::string speed_label(float scale)
{
    return std::format("{:.2g}x", scale);
}

constexpr const char* prev_caption = "< Previous";
constexpr const char* next_caption = "Next >";

// Free look has nothing to cycle (its attack binds zoom and drop cameras); compact,
// the arrows are hints and follow the minimal-UI setting like the stock ones.
bool nameplate_arrows_shown(bool expanded)
{
    return (multi_spectate_is_following_player() || multi_spectate_is_static())
        && (expanded || !g_alpine_game_config.spectate_mode_minimal_ui);
}

// Minimal compact bar (spectate_minimalbar): compact, the panel is just the stock plaque
// around the nameplate text, without its "Spectating" lead, the player's quick-bind numbers or
// the progress line.
bool minimal_compact(bool expanded)
{
    return !expanded && g_alpine_game_config.demo_minimal_compact_bar;
}

// Reference-resolution scaling, like the other Alpine overlays.
constexpr float ref_width = 1280.0f;
constexpr float ref_height = 800.0f;

Layout compute_layout(bool expanded, const PanelLabels& labels)
{
    const float scale =
        std::min(rf::gr::clip_width() / ref_width, rf::gr::clip_height() / ref_height);
    const auto scaled = [scale](float value) {
        return static_cast<int>(value * scale + 0.5f);
    };

    Layout lay{};
    lay.strip.w = std::min(scaled(700.0f), rf::gr::clip_width() - scaled(120.0f));
    // Keep clear of the spectate hint column at the bottom left
    if (const int hints_right = hud_control_hints_right_x(); hints_right > 0) {
        const int clear_w = rf::gr::clip_width() - 2 * (hints_right + scaled(12.0f));
        lay.strip.w = std::min(lay.strip.w, std::max(clear_w, scaled(360.0f)));
    }
    const bool minimal = minimal_compact(expanded);
    if (const int plaque_w = multi_spectate_nameplate_row_width(true); minimal && plaque_w > 0) {
        lay.strip.w = plaque_w;
    }
    lay.strip.x = (rf::gr::clip_width() - lay.strip.w) / 2;
    // The nameplate row takes the stock plaque's spot at the bottom of the screen
    lay.nameplate = {lay.strip.x, multi_spectate_nameplate_top_y(), lay.strip.w, multi_spectate_nameplate_height()};
    // Expanded: track with its marker dot rows, transport buttons, their keycaps. Dot rows
    // clear the handle, which overhangs the track by 4px each side.
    const int track_y = scaled(18.0f);
    const int track_h = scaled(10.0f);
    const int dot_r = std::max(2, scaled(3.0f));
    const int award_row_bottom = track_y + track_h + 4 + scaled(2.0f) + 2 * dot_r;
    const int btn_row_y = award_row_bottom + scaled(6.0f);
    const int btn_h = scaled(28.0f);
    const int keycap_gap = scaled(4.0f);
    int controls_h = 0;
    if (expanded) {
        controls_h = btn_row_y + btn_h + keycap_gap + keycap_h() + scaled(8.0f);
    }
    else if (!minimal) {
        controls_h = std::max(3, scaled(4.0f)); // the progress line
    }
    lay.strip.y = lay.nameplate.y - controls_h;
    lay.strip.h = controls_h + lay.nameplate.h;

    const auto arrow_w = [&](const char* caption, const std::string& bind) {
        return std::max(rf::gr::get_string_size(caption, hud_get_small_font()).first, keycap_w(bind))
            + scaled(24.0f);
    };
    const int prev_w = arrow_w(prev_caption, labels.prev_bind);
    const int next_w = arrow_w(next_caption, labels.next_bind);
    if (minimal) {
        // The plaque only fits its text, so the arrows flank it like the stock hints
        lay.btn_prev = {lay.nameplate.x - prev_w, lay.nameplate.y, prev_w, lay.nameplate.h};
        lay.btn_next = {lay.nameplate.x + lay.nameplate.w, lay.nameplate.y, next_w, lay.nameplate.h};
    }
    else {
        lay.btn_prev = {lay.nameplate.x, lay.nameplate.y, prev_w, lay.nameplate.h};
        lay.btn_next = {lay.nameplate.x + lay.nameplate.w - next_w, lay.nameplate.y, next_w, lay.nameplate.h};
    }

    if (!expanded) {
        lay.track = {lay.strip.x, lay.strip.y, lay.strip.w, controls_h};
        return lay;
    }

    lay.track.x = lay.strip.x + scaled(16.0f);
    lay.track.y = lay.strip.y + track_y;
    lay.track.w = lay.strip.w - 2 * scaled(16.0f);
    lay.track.h = track_h;
    const int pad = scaled(6.0f);
    lay.track_hit = {lay.track.x - pad, lay.track.y - pad, lay.track.w + 2 * pad, lay.track.h + 2 * pad};

    // Media-player cluster: skip back, play/pause, skip forward, speed, then set marker set apart,
    // wide enough for their keycaps and the speed label
    const int skip_w = std::max({scaled(56.0f), keycap_w(labels.back_bind), keycap_w(labels.fwd_bind)});
    const int pause_w = std::max(scaled(44.0f), keycap_w(labels.pause_bind));
    const int speed_w = std::max(scaled(44.0f),
        rf::gr::get_string_size(widest_speed_label, hud_get_default_font()).first + scaled(12.0f));
    const int mark_w = std::max(scaled(44.0f), keycap_w(labels.mark_bind));
    const int gap = scaled(8.0f);
    const int row_w = 2 * skip_w + pause_w + speed_w + mark_w + 5 * gap;
    const int row_x = lay.strip.x + (lay.strip.w - row_w) / 2;
    const int row_y = lay.strip.y + btn_row_y;
    lay.btn_back = {row_x, row_y, skip_w, btn_h};
    lay.btn_pause = {lay.btn_back.x + skip_w + gap, row_y, pause_w, btn_h};
    lay.btn_fwd = {lay.btn_pause.x + pause_w + gap, row_y, skip_w, btn_h};
    lay.btn_speed = {lay.btn_fwd.x + skip_w + gap, row_y, speed_w, btn_h};
    lay.btn_mark = {lay.btn_speed.x + speed_w + 2 * gap, row_y, mark_w, btn_h};
    lay.keycap_y = row_y + btn_h + keycap_gap;

    lay.dot_r = dot_r;
    lay.pickup_dot_y = lay.track.y - 4 - scaled(2.0f) - lay.dot_r;
    lay.award_dot_y = lay.track.y + lay.track.h + 4 + scaled(2.0f) + lay.dot_r;
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
    snd_play_unscaled(stock_sound_id::panel_button_click, 0, 0.0f, 1.0f);
}

// Next speed step up (up) or down from the current speed, wrapping back to normal speed
// past either end. A console-set speed between steps moves to the neighbouring step.
void step_timescale(bool up)
{
    play_click_sound();
    const float cur = demo_playback_timescale();
    float next = 1.0f;
    if (up) {
        for (const float step : speed_steps) {
            if (step > cur + 0.001f) {
                next = step;
                break;
            }
        }
    }
    else {
        for (const float step : speed_steps) {
            if (step < cur - 0.001f) {
                next = step;
            }
        }
    }
    demo_playback_set_timescale(next);
}

// Replaces any earlier custom marker
void set_custom_marker()
{
    play_click_sound();
    g_custom_marker_ms = static_cast<uint32_t>(demo_playback_clock_ms());
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

bool cursor_over_expanded_panel()
{
    int mx = 0;
    int my = 0;
    int mz = 0;
    rf::mouse_get_pos(mx, my, mz);
    return point_in(mx, my, g_expanded_strip);
}

// Clicking the panel must not also fire the weapon - primary/secondary attack is
// what drives spectate target cycling during playback. Clicks off the panel keep
// cycling, except under the full-screen SEEKING overlay.
bool controls_ui_blocks_action(rf::ControlConfig* ccp, rf::ControlConfigAction action)
{
    if (g_swallow_attack_frames <= 0
        && !(g_open && (demo_playback_is_seeking() || cursor_over_expanded_panel()))) {
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

// Free look and third-person orbit steer the camera with the mouse, which the expanded
// panel's cursor takes over: there the panel stays compact unless the user expands it
// with USE.
bool camera_uses_mouse_look()
{
    return multi_spectate_is_freelook() || multi_spectate_is_third_person_orbit();
}

void open(bool play_sound = true)
{
    if (!demo_playback_active() || !demo_playback_can_seek() || demo_playback_is_seeking()
        || !demo_playback_in_level_view()) {
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
    gameplay_overlay_apply_mouse(true);
    if (play_sound) {
        snd_play_unscaled(stock_sound_id::menu_select, 0, 0.0f, 1.0f);
    }
}

// True when the overlay must not stay open: playback gone (demo_stop), the
// backward-seek session restart left gameplay, or a level change landed.
bool must_force_close()
{
    return !demo_playback_active() || !demo_playback_can_seek()
        || !demo_playback_in_level_view();
}

// Force-close from the input/render passes. A close caused by a queued session
// restart (rewind) saves the cursor for the reopen after the restart.
void force_close()
{
    demo_controls_ui_close();
    if (demo_playback_restart_pending()) {
        int z;
        rf::mouse_get_pos(g_reopen_cursor_x, g_reopen_cursor_y, z);
        g_reopen_cursor_valid = true;
    }
}

// Expands the panel silently whenever it is compact but not by the user's choice: at
// playback start, and after a rewind's restart and fast-forward. Playback dying
// without a restart in flight (demo_stop, failed restart) resets to the default.
void auto_open_do_frame()
{
    if (g_open) {
        return;
    }
    if (demo_playback_active()) {
        // Never steal the screen from another overlay (open() would close it)
        if (!g_compact && !camera_uses_mouse_look() && demo_playback_can_seek() && !demo_playback_is_seeking()
            && demo_playback_in_level_view() && !vote_panel_is_gameplay_overlay_active()
            && !g_remote_server_cfg_popup.is_active()) {
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
        g_compact = false;
        g_reopen_cursor_valid = false;
        g_custom_marker_ms.reset();
    }
}

// Switching into a mouse-look camera compacts the panel (not as the user's choice, so
// it expands again on leaving), unless the user expanded it there on purpose.
void auto_close_in_mouse_look()
{
    if (g_open && !g_opened_in_mouse_look && camera_uses_mouse_look()) {
        demo_controls_ui_close();
    }
}

struct ButtonStyle
{
    bool hovered;
    bool enabled;
};

// Markers for the player spectated in first person: a tick on the track at each kill,
// powerup pickup dots, objective squares (hollow when taken, filled when captured) and
// point capture diamonds above it, and award dots below. Clicking one seeks to just before it. The custom marker, a pennant
// on a pole across all rows, shows in every camera and seeks exactly to it.
struct Marker
{
    int x; // center
    uint32_t t_ms;
    DemoMarkerKind kind;
    int team;       // objective and point markers: team whose color it takes (0 red, 1 blue, -1 none)
    uint8_t detail; // award markers: the AwardId earned; point captures: the hill uid
};

// Rewind ahead of the moment so the lead-up plays instead of landing on it.
constexpr double marker_seek_lead_ms = 2000.0;

int marker_x(uint32_t t_ms, const Rect& track)
{
    const double pos = std::clamp(t_ms / effective_duration_ms(), 0.0, 1.0);
    return track.x + static_cast<int>(pos * track.w);
}

std::vector<Marker> panel_markers(const Rect& track)
{
    std::vector<Marker> markers;
    const rf::Player* target = multi_spectate_is_first_person() ? multi_spectate_get_target_player() : nullptr;
    if (target && target != rf::local_player && target->net_data) {
        // A CTF player only ever takes or captures the other team's flag; the other game types'
        // objectives are neutral. A point is captured for the player's own team.
        const int objective_team = rf::multi_get_game_type() == rf::NG_TYPE_CTF ? (target->team == 0 ? 1 : 0) : -1;
        for (const auto& marker : demo_playback_player_markers(target->net_data->player_id)) {
            const int team = marker.kind == DemoMarkerKind::point_captured ? target->team : objective_team;
            markers.push_back({marker_x(marker.t_ms, track), marker.t_ms, marker.kind, team, marker.detail});
        }
    }
    // Last, so it draws over the player's markers
    if (g_custom_marker_ms) {
        markers.push_back({marker_x(*g_custom_marker_ms, track), *g_custom_marker_ms, DemoMarkerKind::custom, 0, 0});
    }
    return markers;
}

bool marker_is_dot(const Marker& marker)
{
    return marker.kind != DemoMarkerKind::kill && marker.kind != DemoMarkerKind::custom;
}

// The custom marker's pole spans the pickup row to the award row
int custom_marker_top(const Layout& lay)
{
    return lay.pickup_dot_y - lay.dot_r;
}

int custom_marker_bottom(const Layout& lay)
{
    return lay.award_dot_y + lay.dot_r + 1;
}

int marker_dot_y(const Marker& marker, const Layout& lay)
{
    return marker.kind == DemoMarkerKind::award ? lay.award_dot_y : lay.pickup_dot_y;
}

// Marker nearest the cursor, within a few pixels of reach so the small markers stay
// clickable; null when no marker is close.
const Marker* hovered_marker(const std::vector<Marker>& markers, int mx, int my, const Layout& lay)
{
    const int tick_reach = std::max(3, lay.track.h / 2);
    const int dot_reach = lay.dot_r + 2;
    const Marker* best = nullptr;
    for (const auto& marker : markers) {
        const int dist = std::abs(marker.x - mx);
        bool in_reach = false;
        if (marker.kind == DemoMarkerKind::custom) {
            in_reach = dist <= tick_reach && my >= custom_marker_top(lay) - 2 && my < custom_marker_bottom(lay) + 2;
        }
        else if (marker_is_dot(marker)) {
            in_reach = dist <= dot_reach && std::abs(marker_dot_y(marker, lay) - my) <= dot_reach;
        }
        else {
            in_reach = dist <= tick_reach && point_in(mx, my, lay.track_hit);
        }
        if (in_reach && (!best || dist < std::abs(best->x - mx))) {
            best = &marker;
        }
    }
    return best;
}

// Anti-aliased fill, in the current color, of a shape given as its horizontal span
// [left, right) at each y within [top, bottom) (pixel units; pixel x covers [x, x + 1)).
// Each pixel row averages sub-scanlines and blends edge pixels by coverage, so small
// shapes don't stair-step.
template<typename SpanFn>
void fill_shape_aa(float top, float bottom, SpanFn span)
{
    constexpr int sub_rows = 4;
    const rf::gr::Color color = rf::gr::screen.current_color;
    std::pair<float, float> spans[sub_rows];
    for (int y = static_cast<int>(std::floor(top)); y < static_cast<int>(std::ceil(bottom)); ++y) {
        int count = 0;
        float row_left = 0.0f;
        float row_right = 0.0f;
        for (int i = 0; i < sub_rows; ++i) {
            const float sample_y = y + (i + 0.5f) / sub_rows;
            if (sample_y < top || sample_y >= bottom) {
                continue;
            }
            const auto [left, right] = span(sample_y);
            if (right <= left) {
                continue;
            }
            row_left = count ? std::min(row_left, left) : left;
            row_right = count ? std::max(row_right, right) : right;
            spans[count++] = {left, right};
        }
        if (!count) {
            continue;
        }
        const auto coverage = [&](int x) {
            float sum = 0.0f;
            for (int i = 0; i < count; ++i) {
                sum += std::clamp(std::min(spans[i].second, x + 1.0f) - std::max(spans[i].first, static_cast<float>(x)),
                                  0.0f, 1.0f);
            }
            return sum / sub_rows;
        };
        const int x_end = static_cast<int>(std::ceil(row_right));
        for (int x = static_cast<int>(std::floor(row_left)); x < x_end;) {
            const float c = coverage(x);
            if (c >= 0.99f) {
                // Fully covered run in one rect
                int run_end = x + 1;
                while (run_end < x_end && coverage(run_end) >= 0.99f) {
                    ++run_end;
                }
                rf::gr::set_color(color);
                rf::gr::rect(x, y, run_end - x, 1);
                x = run_end;
                continue;
            }
            if (c > 0.02f) {
                rf::gr::set_color(color.red, color.green, color.blue, static_cast<rf::ubyte>(color.alpha * c + 0.5f));
                rf::gr::rect(x, y, 1, 1);
            }
            ++x;
        }
    }
    rf::gr::set_color(color);
}

// Filled disc of radius r + 0.5 centered on pixel (cx, cy)
void fill_dot(int cx, int cy, int r)
{
    const float center_x = cx + 0.5f;
    const float center_y = cy + 0.5f;
    const float radius = r + 0.5f;
    fill_shape_aa(center_y - radius, center_y + radius, [=](float y) {
        const float dy = y - center_y;
        const float half = std::sqrt(std::max(0.0f, radius * radius - dy * dy));
        return std::pair{center_x - half, center_x + half};
    });
}

// Filled diamond of half-diagonal r + 0.5 centered on pixel (cx, cy)
void fill_diamond(int cx, int cy, int r)
{
    const float center_x = cx + 0.5f;
    const float center_y = cy + 0.5f;
    const float radius = r + 0.5f;
    fill_shape_aa(center_y - radius, center_y + radius, [=](float y) {
        const float half = std::max(0.0f, radius - std::abs(y - center_y));
        return std::pair{center_x - half, center_x + half};
    });
}

bool marker_is_objective(const Marker& marker)
{
    return marker.kind == DemoMarkerKind::objective_taken || marker.kind == DemoMarkerKind::objective_captured;
}

void set_marker_color(const Marker& marker)
{
    switch (marker.kind) {
    case DemoMarkerKind::kill:
        rf::gr::set_color(255, 170, 0, 255);
        break;
    case DemoMarkerKind::damage_amp:
        rf::gr::set_color(180, 70, 255, 255);
        break;
    case DemoMarkerKind::invulnerability:
        rf::gr::set_color(0, 230, 255, 255);
        break;
    case DemoMarkerKind::fusion:
        rf::gr::set_color(70, 255, 70, 255);
        break;
    case DemoMarkerKind::award:
        rf::gr::set_color(255, 215, 0, 255);
        break;
    case DemoMarkerKind::objective_taken:
    case DemoMarkerKind::objective_captured:
    case DemoMarkerKind::point_captured:
        if (marker.team == 0) {
            rf::gr::set_color(255, 70, 70, 255);
        }
        else if (marker.team == 1) {
            rf::gr::set_color(80, 140, 255, 255);
        }
        else {
            rf::gr::set_color(225, 190, 120, 255);
        }
        break;
    case DemoMarkerKind::custom:
        rf::gr::set_color(255, 64, 160, 255);
        break;
    }
}

const char* objective_label()
{
    if (gt_is_bagman_any()) {
        return "Bag";
    }
    return gt_is_scavenger() ? "Cartridge" : "Flag";
}

std::string marker_label(const Marker& marker)
{
    switch (marker.kind) {
    case DemoMarkerKind::kill:
        return "Kill";
    case DemoMarkerKind::damage_amp:
        return "Damage Amp";
    case DemoMarkerKind::invulnerability:
        return "Invulnerability";
    case DemoMarkerKind::fusion:
        return "Fusion";
    case DemoMarkerKind::award:
        // An id from a newer build than this one has no name here
        if (const char* name = award_name(marker.detail)) {
            return std::format("Award: {}", name);
        }
        return "Award";
    case DemoMarkerKind::objective_taken:
        return std::format("{} Taken", objective_label());
    case DemoMarkerKind::objective_captured:
        return std::format("{} Capture", objective_label());
    case DemoMarkerKind::point_captured: {
        const char* label = gt_is_koth() ? "Hill Capture" : "Point Capture";
        if (const HillInfo* hill = koth_find_hill_by_uid(marker.detail); hill && !hill->name.empty()) {
            return std::format("{}: {}", label, hill->name);
        }
        return label;
    }
    case DemoMarkerKind::custom:
        return "Marker";
    }
    return "";
}

// Filled triangle pointing left or right, spanning rows cy - half_h to cy + half_h; x is
// its left edge
void fill_arrow_triangle(int x, int cy, int half_h, int len, bool left)
{
    const float center_y = cy + 0.5f;
    const float half = half_h + 0.5f;
    fill_shape_aa(center_y - half, center_y + half, [=](float y) {
        const float w = len * (1.0f - std::abs(y - center_y) / half);
        return left ? std::pair{x + len - w, static_cast<float>(x + len)} : std::pair{static_cast<float>(x), x + w};
    });
}

// Pole from top to bottom (exclusive) at x, with a right-pointing pennant at its top
void draw_pennant(int x, int top, int bottom, int pole_w, int half_h)
{
    rf::gr::rect(x, top, pole_w, bottom - top);
    fill_arrow_triangle(x + pole_w, top + half_h, half_h, half_h * 2, false);
}

void draw_markers(const std::vector<Marker>& markers, const Marker* hovered, const Layout& lay)
{
    const Rect& track = lay.track;
    const int tick_w = std::max(1, track.h / 5);
    for (const auto& marker : markers) {
        if (&marker == hovered) {
            rf::gr::set_color(255, 255, 255, 255);
        }
        else {
            set_marker_color(marker);
        }
        if (marker.kind == DemoMarkerKind::custom) {
            const int pole_w = std::max(2, tick_w);
            draw_pennant(marker.x - pole_w / 2, custom_marker_top(lay), custom_marker_bottom(lay), pole_w,
                         lay.dot_r);
        }
        else if (marker.kind == DemoMarkerKind::point_captured) {
            fill_diamond(marker.x, lay.pickup_dot_y, lay.dot_r);
        }
        else if (marker_is_objective(marker)) {
            const int size = 2 * lay.dot_r + 1;
            const int x = marker.x - lay.dot_r;
            const int y = lay.pickup_dot_y - lay.dot_r;
            if (marker.kind == DemoMarkerKind::objective_captured) {
                rf::gr::rect(x, y, size, size);
            }
            else {
                hud_rect_border(x, y, size, size, std::max(1, lay.dot_r / 2));
            }
        }
        else if (marker_is_dot(marker)) {
            fill_dot(marker.x, marker_dot_y(marker, lay), lay.dot_r);
        }
        else {
            const int x = std::clamp(marker.x - tick_w / 2, track.x, track.x + track.w - tick_w);
            rf::gr::rect(x, track.y - 2, tick_w, track.h + 4);
        }
    }
}

// Label above the strip, centered on cx
void draw_tooltip(const Rect& strip, int cx, const std::string& text, int font)
{
    const auto [text_w, text_h] = rf::gr::get_string_size(text, font);
    cx = std::clamp(cx, strip.x + text_w / 2 + 4, strip.x + strip.w - text_w / 2 - 4);
    const int ty = strip.y - text_h - 6;
    rf::gr::set_color(0, 0, 0, 200);
    rf::gr::rect(cx - text_w / 2 - 4, ty - 2, text_w + 8, text_h + 4);
    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, cx, ty, text.c_str(), font);
}

enum class TransportIcon
{
    skip_back,
    play,
    pause,
    skip_forward,
    speed,
    set_marker,
};

std::string transport_tooltip(TransportIcon icon)
{
    switch (icon) {
    case TransportIcon::skip_back:
        return "Rewind 10 seconds";
    case TransportIcon::play:
        return "Play";
    case TransportIcon::pause:
        return "Pause";
    case TransportIcon::skip_forward:
        return "Forward 10 seconds";
    case TransportIcon::speed:
        return std::format("Speed {} (click faster, right-click slower)", speed_label(demo_playback_timescale()));
    case TransportIcon::set_marker:
        return g_custom_marker_ms ? "Move marker here" : "Set marker here";
    }
    return "";
}

// Flat icon button with its keycap centered underneath
void draw_transport_button(const Rect& rect, TransportIcon icon, const std::string& bind, int keycap_y,
                           const ButtonStyle& style)
{
    const int fill = style.hovered ? 80 : 40;
    rf::gr::set_color(fill, fill, fill, 220);
    rf::gr::rect(rect.x, rect.y, rect.w, rect.h);
    if (style.hovered) {
        rf::gr::set_color(200, 200, 200, 255);
        hud_rect_border(rect.x, rect.y, rect.w, rect.h, 1);
    }

    const int gray = style.enabled ? (style.hovered ? 255 : 220) : 100;
    rf::gr::set_color(gray, gray, gray, 255);
    const int cx = rect.x + rect.w / 2;
    const int cy = rect.y + rect.h / 2;
    const int half_h = std::max(3, rect.h * 2 / 9);
    switch (icon) {
    case TransportIcon::play: {
        const int len = half_h * 7 / 4;
        fill_arrow_triangle(cx - len / 2 + 1, cy, half_h, len, false);
        break;
    }
    case TransportIcon::pause: {
        const int bar_w = std::max(2, half_h * 2 / 3);
        rf::gr::rect(cx - bar_w - bar_w / 2, cy - half_h, bar_w, 2 * half_h + 1);
        rf::gr::rect(cx + bar_w / 2 + 1, cy - half_h, bar_w, 2 * half_h + 1);
        break;
    }
    case TransportIcon::skip_back:
    case TransportIcon::skip_forward: {
        // Double arrow; the step size is in the tooltip
        const int len = half_h * 5 / 4;
        const int x = cx - len;
        fill_arrow_triangle(x, cy, half_h, len, icon == TransportIcon::skip_back);
        fill_arrow_triangle(x + len, cy, half_h, len, icon == TransportIcon::skip_back);
        break;
    }
    case TransportIcon::speed: {
        // Normal speed reads dimmer, so a changed speed stands out
        if (style.enabled && demo_playback_timescale() == 1.0f && !style.hovered) {
            rf::gr::set_color(160, 160, 160, 255);
        }
        const int font = hud_get_default_font();
        const int text_y = cy - rf::gr::get_font_height(font) / 2;
        rf::gr::string_aligned(rf::gr::ALIGN_CENTER, cx, text_y, speed_label(demo_playback_timescale()).c_str(), font);
        break;
    }
    case TransportIcon::set_marker: {
        // The custom marker's pennant, in its color
        const int pole_w = std::max(2, half_h / 3);
        if (style.enabled) {
            rf::gr::set_color(255, style.hovered ? 120 : 64, style.hovered ? 200 : 160, 255);
        }
        draw_pennant(cx - half_h / 2, cy - half_h, cy + half_h + 1, pole_w, half_h / 2);
        break;
    }
    }

    if (style.enabled) {
        draw_keycap(cx - keycap_w(bind) / 2, keycap_y, bind, style.hovered);
    }
}

// Keycap over caption, centered in the arrow's slot at the nameplate row's edge
ConsoleCommand2 spectate_minimalbar_cmd{
    "spectate_minimalbar",
    []() {
        g_alpine_game_config.demo_minimal_compact_bar = !g_alpine_game_config.demo_minimal_compact_bar;
        rf::console::print("Minimal compact demo bar is {}",
                           g_alpine_game_config.demo_minimal_compact_bar ? "enabled" : "disabled");
    },
    "Toggle shrinking the compact demo playback bar to the plain spectate nameplate (no progress line)",
    "spectate_minimalbar",
};

void draw_nameplate_arrow(const Rect& rect, const std::string& bind, const char* caption, bool hovered)
{
    const int font = hud_get_small_font();
    const int font_h = rf::gr::get_font_height(font);
    const int line_gap = g_alpine_game_config.big_hud ? 2 : 1;
    const int cx = rect.x + rect.w / 2;
    const int y = rect.y + (rect.h - (keycap_h() + font_h + line_gap)) / 2;
    if (hovered) {
        rf::gr::set_color(255, 255, 255, 40);
        rf::gr::rect(rect.x + 1, rect.y + 1, rect.w - 2, rect.h - 2);
    }
    draw_keycap(cx - keycap_w(bind) / 2, y, bind, hovered);
    rf::gr::set_color(0xFF, 0xFF, 0xFF, hovered ? 0xFF : 0x80);
    rf::gr::string_aligned(rf::gr::ALIGN_CENTER, cx, y + keycap_h() + line_gap, caption, font);
}

} // namespace

void demo_controls_ui_do_patch()
{
    spectate_minimalbar_cmd.register_cmd();
}

bool demo_controls_ui_is_open()
{
    return g_open;
}

bool demo_controls_ui_has_custom_marker()
{
    return g_custom_marker_ms.has_value();
}

void demo_controls_ui_close()
{
    if (!g_open) {
        return;
    }
    g_open = false;
    g_dragging = false;
    g_opened_in_mouse_look = false;
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
                g_compact = true;
                demo_controls_ui_close();
                play_click_sound();
            }
            else {
                g_compact = false;
                open();
                g_opened_in_mouse_look = g_open && camera_uses_mouse_look();
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
    else if (action == rf::CC_ACTION_QUICK_SAVE) {
        // Keyboard shortcut for the popup's marker button. Saving does not exist in
        // multiplayer, so the Quick Save key is free for reuse.
        if (was_pressed && demo_playback_can_seek() && !demo_playback_is_seeking()) {
            set_custom_marker();
        }
    }
    else if (action == rf::CC_ACTION_QUICK_LOAD) {
        // Jumps exactly to the custom marker, like clicking it - the Quick Load to the
        // marker's Quick Save.
        if (was_pressed && g_custom_marker_ms && demo_playback_can_seek() && !demo_playback_is_seeking()) {
            play_click_sound();
            demo_playback_request_seek(*g_custom_marker_ms);
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
    auto_close_in_mouse_look();
    auto_open_do_frame();
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

    const Layout lay = compute_layout(true, panel_labels());
    g_expanded_strip = lay.strip;
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

    const bool left_pressed = rf::mouse_was_button_pressed(0);
    if ((left_pressed || rf::mouse_was_button_pressed(1)) && point_in(mx, my, lay.strip)) {
        // Rest of this frame plus at least one whole frame, extended while held.
        g_swallow_attack_frames = 2;
    }
    if (!left_pressed) {
        if (rf::mouse_was_button_pressed(1) && point_in(mx, my, lay.btn_speed)) {
            step_timescale(false);
        }
        return;
    }
    // hovered_marker points into this vector, so it must outlive the seek below
    const auto markers = panel_markers(lay.track);
    if (const Marker* marker = hovered_marker(markers, mx, my, lay)) {
        play_click_sound();
        const double lead_ms = marker->kind == DemoMarkerKind::custom ? 0.0 : marker_seek_lead_ms;
        demo_playback_request_seek(marker->t_ms - lead_ms);
    }
    else if (point_in(mx, my, lay.track_hit)) {
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
    else if (point_in(mx, my, lay.btn_speed)) {
        step_timescale(true);
    }
    else if (point_in(mx, my, lay.btn_mark)) {
        set_custom_marker();
    }
    else if (nameplate_arrows_shown(true) && point_in(mx, my, lay.btn_prev)) {
        play_click_sound();
        multi_spectate_execute_action(rf::CC_ACTION_SECONDARY_ATTACK, true);
    }
    else if (nameplate_arrows_shown(true) && point_in(mx, my, lay.btn_next)) {
        play_click_sound();
        multi_spectate_execute_action(rf::CC_ACTION_PRIMARY_ATTACK, true);
    }
}

bool demo_controls_ui_draws_nameplate()
{
    // Compact is HUD and hides with it; expanded stays up for seeking
    return demo_playback_active() && demo_playback_in_level_view()
        && (g_open || !is_hud_effectively_hidden());
}

void demo_controls_ui_render()
{
    // The input pump runs before gameseq_process; a level change or teardown can
    // land mid-frame, so re-check here (vote panel precedent).
    if (g_open && must_force_close()) {
        force_close();
    }
    if (!demo_controls_ui_draws_nameplate() || demo_playback_is_seeking()) {
        return; // the full-screen SEEKING overlay takes over
    }

    const bool expanded = g_open;
    const PanelLabels labels = panel_labels();
    const Layout lay = compute_layout(expanded, labels);
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

    const Marker* hovered = nullptr;
    std::optional<std::pair<Rect, TransportIcon>> hovered_button;
    int handle_cx = 0;
    if (expanded) {
        // Track: frame, unplayed remainder, played fill, markers, handle (colors match the
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
        const auto markers = panel_markers(lay.track);
        hovered = g_dragging ? nullptr : hovered_marker(markers, mx, my, lay);
        draw_markers(markers, hovered, lay);
        const int handle_w = std::max(4, lay.track.h - 2);
        const int handle_h = lay.track.h + 8;
        const int handle_x = std::clamp(lay.track.x + fill_w - handle_w / 2, lay.track.x,
                                        lay.track.x + lay.track.w - handle_w);
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::rect(handle_x, lay.track.y - 4, handle_w, handle_h);
        handle_cx = handle_x + handle_w / 2;

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
        const TransportIcon pause_icon = demo_playback_paused() ? TransportIcon::play : TransportIcon::pause;
        const bool pause_enabled = !demo_playback_finished();
        const bool back_hovered = point_in(mx, my, lay.btn_back);
        const bool pause_hovered = pause_enabled && point_in(mx, my, lay.btn_pause);
        const bool fwd_hovered = point_in(mx, my, lay.btn_fwd);
        const bool speed_hovered = point_in(mx, my, lay.btn_speed);
        const bool mark_hovered = point_in(mx, my, lay.btn_mark);
        draw_transport_button(lay.btn_back, TransportIcon::skip_back, labels.back_bind, lay.keycap_y,
                              {back_hovered, true});
        draw_transport_button(lay.btn_pause, pause_icon, labels.pause_bind, lay.keycap_y,
                              {pause_hovered, pause_enabled});
        draw_transport_button(lay.btn_fwd, TransportIcon::skip_forward, labels.fwd_bind, lay.keycap_y,
                              {fwd_hovered, true});
        draw_transport_button(lay.btn_speed, TransportIcon::speed, {}, lay.keycap_y, {speed_hovered, true});
        draw_transport_button(lay.btn_mark, TransportIcon::set_marker, labels.mark_bind, lay.keycap_y,
                              {mark_hovered, true});
        if (back_hovered) {
            hovered_button = {lay.btn_back, TransportIcon::skip_back};
        }
        else if (pause_hovered) {
            hovered_button = {lay.btn_pause, pause_icon};
        }
        else if (fwd_hovered) {
            hovered_button = {lay.btn_fwd, TransportIcon::skip_forward};
        }
        else if (speed_hovered) {
            hovered_button = {lay.btn_speed, TransportIcon::speed};
        }
        else if (mark_hovered) {
            hovered_button = {lay.btn_mark, TransportIcon::set_marker};
        }

        // Divider over the nameplate row
        rf::gr::set_color(90, 90, 90, 255);
        rf::gr::rect(lay.strip.x + 1, lay.nameplate.y, lay.strip.w - 2, 1);
    }
    else if (!minimal_compact(expanded)) {
        // Progress line across the top edge
        const double progress = std::clamp(demo_playback_clock_ms() / effective_duration_ms(), 0.0, 1.0);
        rf::gr::set_color(0, 0x80, 0, 255);
        rf::gr::rect(lay.track.x, lay.track.y, static_cast<int>(progress * lay.track.w), lay.track.h);
        if (g_custom_marker_ms) {
            rf::gr::set_color(255, 64, 160, 255);
            const int x = std::clamp(marker_x(*g_custom_marker_ms, lay.track) - 1, lay.track.x,
                                     lay.track.x + lay.track.w - 2);
            rf::gr::rect(x, lay.track.y, 2, lay.track.h);
        }
    }

    // Nameplate row
    multi_spectate_render_nameplate_row(lay.nameplate.x + lay.nameplate.w / 2, lay.nameplate.y,
                                        minimal_compact(expanded));
    if (nameplate_arrows_shown(expanded)) {
        draw_nameplate_arrow(lay.btn_prev, labels.prev_bind, prev_caption, expanded && point_in(mx, my, lay.btn_prev));
        draw_nameplate_arrow(lay.btn_next, labels.next_bind, next_caption, expanded && point_in(mx, my, lay.btn_next));
    }

    if (!expanded) {
        return;
    }

    // Drag preview timestamp above the handle, what the hovered marker is and when, or
    // what the hovered button does
    if (g_dragging) {
        draw_tooltip(lay.strip, handle_cx, format_time_ms(g_preview_ms), font);
    }
    else if (hovered) {
        std::string text = std::format("{} {}", marker_label(*hovered), format_time_ms(hovered->t_ms));
        if (hovered->kind == DemoMarkerKind::custom && !labels.jump_bind.empty()) {
            text += std::format(" ({} jumps here)", labels.jump_bind);
        }
        draw_tooltip(lay.strip, hovered->x, text, font);
    }
    else if (hovered_button) {
        const auto& [rect, icon] = *hovered_button;
        draw_tooltip(lay.strip, rect.x + rect.w / 2, transport_tooltip(icon), font);
    }

    // gameseq_process re-hides the cursor every gameplay frame; re-asserting here
    // (after it, before the stock cursor render) makes the cursor draw on top.
    rf::mouse_set_visible(true);
}
