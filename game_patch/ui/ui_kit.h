#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "../rf/gr/gr.h"
#include "../rf/input.h"

// Immediate-mode UI toolkit shared by the Alpine overlays and the settings menu.
//
// Two-pass contract: every screen is laid out by ONE function that runs twice per frame,
// once with UiPass::HitTest (input + state mutation, no drawing) and once with UiPass::Draw
// (drawing only). Anything the draw pass needs must be cached during the hit-test pass of the
// same frame. Widgets return their "activated" result only in the hit-test pass.
//
// The kit installs no hooks: a host owns its hooks and feeds the context through
// begin_frame/set_mouse_pos/push_key and brackets each pass with begin_pass/end_pass. A host
// that uses popup widgets calls draw_overlays() after the draw pass so they land on top.
//
// Widget ids: 0 means "anonymous" - the widget takes no part in keyboard focus, hover audio or
// popup modality. Non-zero ids are produced by ui_id(): a 32-bit FNV-1a hash of a caller-supplied
// label, optionally mixed with an integer for repeated rows ("row", i). Ids only have to be unique
// within one screen and stable across the two passes of a frame.

using UiId = uint32_t;

UiId ui_id(std::string_view label);
UiId ui_id(std::string_view label, int index);

struct UiRect
{
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

bool ui_point_in(int px, int py, const UiRect& r);

enum class UiPass
{
    HitTest,
    Draw,
};

// Every colour and metric the widgets use. Defaults are the vote panel's palette.
struct UiStyle
{
    // Chrome
    rf::Color header{255, 215, 0, 255};

    // Text
    rf::Color text_normal{230, 230, 230, 255};
    rf::Color text_hover{255, 255, 255, 255};
    rf::Color text_disabled{130, 130, 130, 255};
    rf::Color label{255, 255, 255, 255};
    rf::Color label_secondary{210, 210, 210, 255};
    rf::Color body_text{215, 215, 215, 255};

    // Button
    rf::Color button_fill{45, 45, 45, 255};
    rf::Color button_fill_hover{70, 70, 70, 255};
    rf::Color button_border{150, 150, 150, 255};
    rf::Color button_border_disabled{100, 100, 100, 255};
    rf::Color button_active_fill{35, 90, 60, 255};
    rf::Color button_active_fill_hover{50, 115, 80, 255};
    rf::Color button_active_border{120, 230, 120, 255};
    rf::Color button_active_text{255, 255, 255, 255};

    // Checkbox
    rf::Color check_border{170, 170, 170, 255};
    rf::Color check_border_disabled{100, 100, 100, 255};
    rf::Color check_mark{120, 230, 120, 255};
    rf::Color check_mark_disabled{85, 135, 85, 255};

    // Cycler
    rf::Color cycler_border{150, 150, 150, 255};

    // Lists
    rf::Color list_bg{8, 8, 8, 235};
    rf::Color list_border{120, 120, 120, 255};
    rf::Color list_selected_bg{35, 90, 60, 255};
    rf::Color list_hover_bg{70, 70, 70, 255};
    rf::Color list_selected_text{160, 255, 190, 255};
    rf::Color scrollbar{100, 255, 200, 255};
    rf::Color scrollbar_track{0, 0, 0, 0};
    rf::Color scrollbar_hover{160, 255, 220, 255};

    // Text input
    rf::Color input_bg{8, 8, 8, 235};
    rf::Color input_border{120, 120, 120, 255};
    rf::Color input_border_focused{220, 220, 220, 255};
    rf::Color input_border_disabled{80, 80, 80, 255};
    rf::Color input_hint{140, 140, 140, 255};
    rf::Color input_hint_disabled{90, 90, 90, 255};
    rf::Color input_text{255, 255, 255, 255};
    rf::Color input_text_disabled{120, 120, 120, 255};

    // Danger affordances (close button, delete zones)
    rf::Color danger_fill{150, 40, 40, 255};
    rf::Color danger_border{220, 120, 120, 255};

    // Toasts
    rf::Color toast_bg{20, 20, 20, 235};
    rf::Color toast_border{120, 230, 120, 255};
    rf::Color toast_text{240, 240, 240, 255};
    float toast_seconds = 3.0f;
    float toast_fade_seconds = 0.5f;

    // Metrics, in reference-resolution pixels unless the name says otherwise.
    float text_pad = 6.0f;
    int text_pad_min = 3;
    float check_text_gap = 5.0f;
    int check_text_gap_min = 3;
    float cycler_arrow_w = 16.0f;
    int cycler_arrow_w_min = 10;
    float scrollbar_w = 6.0f;
    int scrollbar_w_min = 3;
    // Length of a step box at each end of a scroll region's gutter. The thumb travels between
    // them, and a click on one scrolls by the region's wheel step (draggable regions only).
    float scrollbar_cap = 0.0f;

    // Timing for UiContext::repeat_tick.
    int repeat_delay_ms = 400;
    int repeat_interval_ms = 70;
};

struct UiTextInputResult
{
    bool changed = false;      // `text` was edited since the previous pass
    bool gained_focus = false; // this click focused the box; caller seeds state
};

using UiCharFilter = bool (*)(char);

bool ui_char_filter_filename(char c); // alnum plus - _ . space
bool ui_char_filter_numeric(char c);  // digits plus - .
bool ui_char_filter_digit(char c);
bool ui_char_filter_hex(char c);

// Formatting for the numeric widgets. A printf-style string would have to be a runtime format for
// std::format, so the pieces are spelled out instead.
struct UiNumberFormat
{
    int decimals = 0;
    float display_scale = 1.0f; // e.g. 100 to show a 0..1 value as a percentage
    const char* suffix = "";
    const char* zero_label = nullptr; // replaces the number for a value of exactly 0
};

std::string ui_format_number(float value, const UiNumberFormat& fmt);

// Hover-driven description area: an id (caller's own numbering, -1 = none), a "something was
// hovered this pass" latch and a pin that a deliberate click sets.
struct UiDescriptionState
{
    int id = -1;
    bool pinned = false;
    bool seen_this_pass = false;

    void begin_pass()
    {
        seen_this_pass = false;
    }

    void hover(int new_id)
    {
        if (id != new_id) {
            pinned = false;
        }
        id = new_id;
        seen_this_pass = true;
    }

    void pin(int new_id)
    {
        id = new_id;
        pinned = true;
    }

    void end_pass()
    {
        if (!seen_this_pass && !pinned) {
            id = -1;
        }
    }
};

// What a focusable widget accepts from the keyboard.
enum class UiFocusKind
{
    Activate,   // Enter presses it
    Adjustable, // Left/Right steps it, Enter presses it
    Text,       // Enter starts editing
};

// Saved/restored state of one clipped scroll region; opaque to callers apart from start_y().
struct UiScrollRegion
{
    UiRect view;
    float* scroll = nullptr;
    int content_h = 0;
    float max_scroll = 0.0f;
    UiId id = 0;
    bool draggable = false;
    int wheel_step = 0;

    UiRect saved_clip_rect;
    bool saved_clip_active = false;
    int saved_origin_x = 0;
    int saved_origin_y = 0;
    int saved_gr_x = 0, saved_gr_y = 0, saved_gr_w = 0, saved_gr_h = 0;

    // Absolute y the first row of content sits at.
    [[nodiscard]] int start_y() const
    {
        return view.y - static_cast<int>(*scroll);
    }
};

class UiContext
{
public:
    // -------- per-pass input snapshot; widgets read these directly --------
    bool draw = true;
    int mx = 0;
    int my = 0;
    bool click = false;
    bool rclick = false;
    int wheel = 0;
    bool lmb_down = false;

    // Active scroll viewport. Widgets lay out in absolute screen space; inside a region the draw
    // pass subtracts the clip origin (set_clip moves the drawing origin) while hit-testing stays
    // absolute but is gated on the viewport. Managed by ui_scroll_begin / ui_scroll_end.
    UiRect clip_rect;
    bool clip_active = false;
    int origin_x = 0;
    int origin_y = 0;

    UiStyle style;

    // -------- host feed --------
    // Called once per frame, before the hit-test pass, with that frame's input edges.
    void begin_frame(int mouse_x, int mouse_y, int wheel_delta, bool lmb_is_down,
                     bool lmb_was_pressed, bool rmb_was_pressed = false);
    // The draw pass runs later in the frame and re-reads the cursor; edges stay consumed.
    void set_mouse_pos(int mouse_x, int mouse_y);

    // begin_pass resets the clip window, and for the hit-test pass folds in the keys the host
    // pushed since the previous pass. end_pass commits and drops a text focus no widget claimed.
    void begin_pass(UiPass pass);
    void end_pass();

    // Deferred popup layer (open dropdowns, tooltips): drawn by the host after the draw pass.
    void draw_overlays();

    // Key events from the host's key hook. Handles ESC/ENTER/BACKSPACE/Ctrl+V and routes printable
    // keys through rf::key_to_ascii.
    void push_key(rf::Key key);

    // Forget every transient (focus, capture, popup, toasts, scroll offsets).
    void reset();

    // -------- scale --------
    void set_scale(float s)
    {
        scale_ = s;
    }
    [[nodiscard]] float scale() const
    {
        return scale_;
    }
    [[nodiscard]] int scaled(float value) const
    {
        return static_cast<int>(value * scale_);
    }

    // -------- hover --------
    // Honours the active scroll viewport and an open popup's modality.
    [[nodiscard]] bool hover(const UiRect& r, UiId id = 0) const;
    // Latched per pass by the widgets; drives the optional hover blip and, while the mouse is the
    // live input device, moves the keyboard focus without showing the ring.
    void note_hovered(UiId id);
    void set_hover_sound_enabled(bool enabled)
    {
        hover_sound_ = enabled;
    }

    // -------- mouse capture (slider / scrollbar drags) --------
    void capture(UiId id)
    {
        captured_id_ = id;
    }
    [[nodiscard]] UiId captured_id() const
    {
        return captured_id_;
    }
    void release_capture()
    {
        captured_id_ = 0;
    }

    // -------- keyboard focus --------
    void register_focusable(UiId id, UiFocusKind kind);
    [[nodiscard]] UiId focus_id() const
    {
        return focus_id_;
    }
    [[nodiscard]] bool focus_visible() const
    {
        return focus_visible_;
    }
    // Tab / Shift+Tab (0x100F) / Up / Down / Left / Right / Enter against the previous frame's
    // registration order; Left / Right only reach an Adjustable widget. While a popup is open the
    // arrows and Enter go to its owner and Tab closes it. Returns true when the key was consumed.
    bool handle_nav_key(rf::Key key);
    // One-shot results a widget picks up in the hit-test pass.
    bool consume_activation(UiId id);
    int consume_adjust(UiId id); // -1, 0 or +1

    // -------- text input --------
    [[nodiscard]] UiId focused_text_id() const
    {
        return text_focus_;
    }
    // Start editing a box from outside the widget (Enter on a focused field, a host shortcut).
    // Commits whatever box held the focus first; the box binds its buffer on the NEXT pass,
    // exactly like the click path does.
    void focus_text(UiId id);
    [[nodiscard]] std::string& text_buffer()
    {
        return text_buffer_;
    }
    // Runs the commit callback (if any) and drops the focus. String boxes edit live, so for them
    // this only drops focus.
    void release_text_focus(bool commit);
    // Set from a box's gained_focus branch; called with true on ENTER / click-away / focus loss and
    // with false on ESC, before the focus is cleared.
    void set_text_commit_callback(std::function<void(bool commit)> fn);

    // -------- binding capture --------
    void begin_key_capture(UiId id);
    void cancel_key_capture();
    [[nodiscard]] UiId key_capture_id() const
    {
        return capture_id_;
    }
    // Host-supplied event. Exactly one of scan_code / mouse_btn is meaningful; the host owns all
    // filtering rules (auto-repeat, excluded scans, modifier masking).
    void feed_capture(int scan_code, int mouse_btn);
    // Ends the capture `id` started, once the host fed an event or cancelled it. True on the pass
    // the capture ends; `cancelled` says which way and scan_code / mouse_btn carry the value
    // (-1 / -1 when cancelled).
    bool take_key_capture(UiId id, int& scan_code, int& mouse_btn, bool& cancelled);

    // -------- deferred popup layer --------
    void open_popup(UiId id);
    // In the hit-test pass this also consumes the click, so the click that closed the popup
    // reaches nothing laid out after it.
    void close_popup();
    [[nodiscard]] UiId popup_id() const
    {
        return popup_id_;
    }
    // The popup owner asks this every pass. A hit-test pass that never asks (the owner scrolled
    // out of view, a tab switch) closes the popup, so its modality cannot outlive the widget.
    bool popup_open_for(UiId id);
    void push_overlay(std::function<void()> fn);

    // -------- per-widget scroll offsets --------
    float& scroll_offset(UiId id);

    // -------- toasts --------
    void push_toast(std::string text);
    void draw_toasts(int font);

    // -------- description pane state --------
    UiDescriptionState& description()
    {
        return description_;
    }

    // -------- hold-to-repeat --------
    // True on the press edge and then at the repeat rate while the same (id, dir) is held.
    bool repeat_tick(UiId id, int dir);
    // Repeats fired since the current hold began; 0 on the press edge.
    int repeat_count() const
    {
        return repeat_count_;
    }
    void clear_repeat(UiId id);

private:
    struct PendingKey
    {
        enum class Kind : uint8_t
        {
            Char,
            Backspace,
            Escape,
            Enter,
            Paste,
        };

        Kind kind = Kind::Char;
        char c = 0;
        std::string text;
    };

    struct Focusable
    {
        UiId id = 0;
        UiFocusKind kind = UiFocusKind::Activate;
    };

    struct Toast
    {
        std::string text;
        float age = 0.0f;
    };

    void apply_pending_keys();
    void queue_key(PendingKey key);
    void push_char(char c);
    void insert_char(char c);

    float scale_ = 1.0f;

    int prev_mx_ = 0;
    int prev_my_ = 0;
    bool mouse_moved_ = false;

    UiId hovered_id_ = 0;
    UiId prev_hovered_id_ = 0;
    bool hover_sound_ = false;

    UiId captured_id_ = 0;

    UiId focus_id_ = 0;
    bool focus_visible_ = false;
    UiId activate_id_ = 0;
    UiId adjust_id_ = 0;
    int adjust_delta_ = 0;
    std::vector<Focusable> focusables_;
    std::vector<Focusable> prev_focusables_;

    UiId text_focus_ = 0;
    bool text_seen_this_pass_ = false;
    bool text_changed_ = false;
    std::string text_buffer_;
    std::string* text_target_ = nullptr;
    size_t text_max_len_ = 0;
    UiCharFilter text_filter_ = nullptr;
    std::function<void(bool)> text_commit_;
    std::vector<PendingKey> pending_;

    UiId capture_id_ = 0;
    bool capture_done_ = false;
    bool capture_cancelled_ = false;
    int capture_scan_ = -1;
    int capture_mouse_ = -1;

    UiId popup_id_ = 0;
    bool popup_seen_this_pass_ = false;
    std::vector<std::function<void()>> overlays_;

    std::unordered_map<UiId, float> scroll_offsets_;

    std::vector<Toast> toasts_;

    UiId repeat_id_ = 0;
    int repeat_dir_ = 0;
    int repeat_count_ = 0;
    int64_t repeat_next_ms_ = 0;

    UiDescriptionState description_;

    friend UiTextInputResult ui_text_input(UiContext&, const UiRect&, UiId, std::string&, size_t,
                                           UiCharFilter, const char*, int, bool);
};

// ---------------------------------------------------------------------------
// Geometry / colour helpers
// ---------------------------------------------------------------------------

bool ui_hover(const UiContext& ctx, const UiRect& r, UiId id = 0);
// Absolute rect -> coordinates to draw at under the active clip window.
UiRect ui_draw_rect(const UiContext& ctx, const UiRect& r);
void ui_set_header_color(const UiContext& ctx);
void ui_set_text_color(const UiContext& ctx, bool enabled, bool hovered);

// ---------------------------------------------------------------------------
// Text measurement / wrapping
// ---------------------------------------------------------------------------

std::string ui_fit_middle(std::string_view text, int max_w, int font);
std::vector<std::string> ui_wrap_text(std::string_view text, int max_w, int font);
void ui_draw_wrapped(const UiContext& ctx, const UiRect& r, std::string_view text, int font, int line_h);
int ui_line_block_height(const std::vector<std::string>& lines, int max_w, int font, int line_h, int max_h);
void ui_draw_line_block(const UiRect& r, const std::vector<std::string>& lines, int font, int line_h,
                        int red, int green, int blue);

// ---------------------------------------------------------------------------
// Sounds (stock sounds.tbl ids)
// ---------------------------------------------------------------------------

void ui_play_click();
void ui_play_toggle(bool on);
// Stock "cannot do that" beep, e.g. a scan code a binding capture must reject.
void ui_play_error();

// ---------------------------------------------------------------------------
// Widgets. Every one returns its result in the hit-test pass only.
// ---------------------------------------------------------------------------

void ui_draw_label(const UiContext& ctx, const UiRect& r, const char* text, int font,
                   bool secondary = false);

bool ui_button(UiContext& ctx, const UiRect& r, const char* text, int font, bool enabled = true,
               bool active = false);

bool ui_checkbox(UiContext& ctx, const UiRect& r, bool checked, const char* text, int font,
                 bool enabled = true);

int ui_cycler_arrow_width(const UiContext& ctx);
int ui_cycler_value_width(const UiContext& ctx, const UiRect& r);
// "< value >"; returns -1, 0 or +1.
int ui_cycler(UiContext& ctx, const UiRect& r, const char* value, int font);

int ui_listbox_row_height(int font);
// Returns the clicked index, else -1.
int ui_listbox(UiContext& ctx, const UiRect& r, const std::vector<std::string>& items, int sel,
               float& scroll, int font);

UiTextInputResult ui_text_input(UiContext& ctx, const UiRect& r, UiId id, std::string& text,
                                size_t max_len, UiCharFilter allow_char, const char* hint, int font,
                                bool enabled = true);

// Top-right "X". Returns true on the click; the caller decides what closing means.
bool ui_close_button(UiContext& ctx, const UiRect& r, int font);

// ---------------------------------------------------------------------------
// Scroll regions
// ---------------------------------------------------------------------------

// wheel_step is pixels per notch; draggable = true makes the thumb grabbable.
UiScrollRegion ui_scroll_begin(UiContext& ctx, const UiRect& view, int content_h, float& scroll,
                               int wheel_step, UiId id = 0, bool draggable = false);
void ui_scroll_end(UiContext& ctx, UiScrollRegion& region);

// Where a scroll region's bar sits. ui_scroll_end hit-tests with it, and a skin that paints the
// bar itself draws with it, so the two always agree.
struct UiScrollbarGeometry
{
    UiRect gutter;
    UiRect track; // the gutter less a step box at each end
    int thumb_y = 0;
    int thumb_h = 0;
};
UiScrollbarGeometry ui_scrollbar_geometry(const UiContext& ctx, const UiRect& view, int content_h, float scroll);
