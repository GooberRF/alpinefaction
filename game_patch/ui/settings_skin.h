#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "ui_kit.h"
#include "../rf/gr/gr.h"

// Painted chrome for the settings menu, in the visual language of the stock menus. Everything is
// authored in the 1920x1080 reference canvas and scaled by UiContext::scale(); the SVG sheets in
// alpinefaction.vpp are rasterised at the exact pixel size the frame draws them at, so every blit
// is 1:1.
//
// These widgets run the same two-pass contract as ui_kit: results come back in the hit-test pass
// only. They drive UiContext's hover / focus / capture / repeat / popup machinery directly instead
// of wrapping the kit widgets, because those own their drawing and the design needs pixel control
// of it.

namespace skin
{
    inline constexpr rf::Color steel_hi{0xE8, 0xE8, 0xEC, 255};
    inline constexpr rf::Color steel_lit{0xD2, 0xD2, 0xD8, 255};
    inline constexpr rf::Color steel{0xB4, 0xB4, 0xBA, 255};
    inline constexpr rf::Color steel_low{0xA2, 0xA2, 0xA9, 255};
    inline constexpr rf::Color steel_mid{0x8E, 0x8E, 0x96, 255};
    inline constexpr rf::Color steel_dark{0x55, 0x55, 0x5C, 255};
    inline constexpr rf::Color steel_edge{0x2E, 0x2E, 0x33, 255};

    inline constexpr rf::Color field{0x00, 0x00, 0x00, 255};
    inline constexpr rf::Color field_edge{0x30, 0x30, 0x30, 255};
    inline constexpr rf::Color bevel_hi{0xC0, 0xC0, 0xC0, 255};
    inline constexpr rf::Color bevel_lo{0x58, 0x58, 0x58, 255};
    inline constexpr rf::Color popup{0x00, 0x00, 0x00, 235};
    inline constexpr rf::Color border{0x78, 0x78, 0x78, 255};
    inline constexpr rf::Color border_dim{0x4A, 0x4A, 0x4A, 255};

    inline constexpr rf::Color txt{0xC0, 0xC0, 0xC0, 255};
    inline constexpr rf::Color txt_hi{0xF0, 0xF0, 0xF0, 255};
    // Disabled text. Hints, placeholders, crumbs and inherited values use txt_hint, which keeps a
    // readable contrast on black.
    inline constexpr rf::Color txt_dim{0x60, 0x60, 0x60, 255};
    inline constexpr rf::Color txt_hint{0x8C, 0x8C, 0x8C, 255};
    // Lettering and marks on steel: the title bar and the panel. Values stay in black wells.
    inline constexpr rf::Color ink_title{0x14, 0x14, 0x18, 255};
    inline constexpr rf::Color ink_status{0x2A, 0x2A, 0x30, 255};
    inline constexpr rf::Color ink_alert{0x8A, 0x1C, 0x10, 255};
    inline constexpr rf::Color ink_hint{0x4A, 0x4A, 0x52, 255};
    inline constexpr rf::Color ink_dim{0x5E, 0x5E, 0x66, 255};
    // The modified marker and reset icons; gold does not show on steel.
    inline constexpr rf::Color amber{0x70, 0x48, 0x00, 255};

    // Teal is reserved for lights: active arrows and a dragged slider nub.
    inline constexpr rf::Color teal_led{0x10, 0xF8, 0xF8, 255};
    inline constexpr rf::Color arrow{0x9A, 0x9A, 0xA0, 255};
    inline constexpr rf::Color gold{0xFF, 0xD7, 0x00, 255};

    inline constexpr rf::Color lamp_red{0xFF, 0x2A, 0x2A, 255};
    inline constexpr rf::Color warn{0xFF, 0x8C, 0x78, 255};
    inline constexpr rf::Color good{0x6C, 0xD8, 0x5A, 255};

    inline constexpr rf::Color team_red{0xFF, 0x5A, 0x5A, 255};
    inline constexpr rf::Color team_blue{0x64, 0x8C, 0xFF, 255};

    inline constexpr rf::Color scroll_box{0x18, 0x20, 0x10, 255};
    inline constexpr rf::Color scroll_box_edge{0x30, 0x48, 0x18, 255};
    inline constexpr rf::Color scroll_arrow{0x80, 0xC0, 0x20, 255};

    inline constexpr rf::Color badge_mp{0x4D, 0xE8, 0xE8, 255};
    inline constexpr rf::Color badge_d3d11{0x9A, 0xB8, 0xD8, 255};
    inline constexpr rf::Color badge_override = gold;
    inline constexpr rf::Color badge_sp{0xD8, 0xB2, 0x4A, 255};
    inline constexpr rf::Color badge_server{0xB4, 0x8A, 0xE0, 255};

    // Canvas metrics the menu and the widgets agree on (reference pixels).
    inline constexpr float row_h = 52.0f;
    // af_menu_cap is authored 78 tall and drawn scaled to the button.
    inline constexpr float menu_button_h = 74.0f;
    inline constexpr float menu_cap_scale = menu_button_h / 78.0f;
    inline constexpr float menu_cap_w = 72.0f * menu_cap_scale;
    // The cap's steel body, without the brackets that stick out past it.
    inline constexpr float menu_cap_body_w = 57.0f * menu_cap_scale;
    // Stock's button height, six to the span of eleven small ones: af_menu_cap_tall is the cap authored 143 tall,
    // drawn at the same scale.
    inline constexpr float menu_button_tall_h = 143.0f * menu_cap_scale;
    // af_menu_plate's width. Only its right end shows, the button's frame between the left bar and its face.
    inline constexpr float menu_plate_w = 100.0f;
    // Every row's control fills this width, so the controls form one column with both edges aligned.
    inline constexpr float control_w = 300.0f;
    inline constexpr float widget_h = 34.0f;
    inline constexpr float cycler_arrow = 24.0f;
    inline constexpr float cycler_gap = 4.0f;
    inline constexpr float dropdown_cap = 30.0f;
    inline constexpr float slider_track_h = 12.0f;
    inline constexpr float slider_thumb_w = 12.0f;
    inline constexpr float slider_thumb_h = 28.0f;
    inline constexpr float readout_w = 72.0f;
    inline constexpr float readout_h = 34.0f;
    inline constexpr float swatch_size = 34.0f;
    inline constexpr float badge_h = 24.0f;
    inline constexpr float bind_row_h = 40.0f;
    inline constexpr float keycap_w = 200.0f;
    inline constexpr float keycap_h = 32.0f;
    inline constexpr float bind_col_w = 220.0f;
    inline constexpr float list_row_h = 40.0f;
    inline constexpr float spray_tile = 96.0f;
    // Image area of a captioned picker tile; the caption line is added on top of it.
    inline constexpr float portrait_tile = 128.0f;
    inline constexpr float icon_size = 18.0f;
    inline constexpr float scrollbar_w = 24.0f;
}

enum class SkinButton
{
    Secondary,
    Danger,
};

UiStyle settings_menu_style();

// ---------------------------------------------------------------------------
// Surfaces. Coordinates are draw space, i.e. already through ui_draw_rect.
// ---------------------------------------------------------------------------

int skin_px(const UiContext& ctx, float reference_px);

// ---------------------------------------------------------------------------
// Text fitting. Every fixed-width plate in the design is authored around English labels; a
// longer translation, a long key name or a small scale would otherwise paint straight over the
// plate edge.
// ---------------------------------------------------------------------------

struct SkinText
{
    std::string text;
    int font;
};

// Fits `text` into `max_w` draw pixels: a smaller size of the same font first, then an ellipsis.
SkinText skin_fit_text(const UiContext& ctx, std::string_view text, int font, int max_w);
// Width a slot button needs to hold `text`, never narrower than `min_ref_w` reference pixels.
int skin_button_width(const UiContext& ctx, const char* text, int font, float min_ref_w);
// Width a cycler needs to show a value `value_w` draw pixels wide at its own size.
int skin_cycler_width(const UiContext& ctx, int value_w);
void skin_draw_text(int x, int y, const std::string& text, int font, rf::Color color);

// Brushed steel for the large plates, the panel frame and the title bar, as stock's are.
// `shaded` gives the darker steel of their end block and channel.
void skin_brushed_plate(const UiContext& ctx, int x, int y, int w, int h, bool shaded);
// The panel's content area: brushed steel recessed into the frame, which the rows and their dark
// lettering sit on.
void skin_steel_field(const UiContext& ctx, int x, int y, int w, int h);
// Stock's ridge between rows of a steel panel: a dark line over a bright one.
void skin_ridge(const UiContext& ctx, int x, int y, int w);
// A stock cross-head screw centred on (cx, cy).
void skin_screw(const UiContext& ctx, int cx, int cy, float size_ref);
// A flat black box with a faint edge.
void skin_slot(const UiContext& ctx, int x, int y, int w, int h);
// A capsule LED centred on (cx, cy); a lit one carries a halo.
void skin_led(const UiContext& ctx, int cx, int cy, float w_ref, float h_ref, bool lit);
// Stock list selection: a red glow fading out to the right, with a lit LED at its left end unless `with_led` is false
// (for rows that draw their own LED there).
void skin_selection_bar(const UiContext& ctx, int x, int y, int w, int h, bool with_led = true);
// Keyboard focus where the stock highlight alone would be ambiguous.
void skin_focus_ring(const UiContext& ctx, int x, int y, int w, int h);
// A whole chrome SVG rasterized at exactly `r`'s draw-space size.
void skin_sized_chrome(const char* name, const UiRect& r);
// A white icon sheet `size` square, tinted.
void skin_icon(const char* name, int x, int y, int size, rf::Color tint);
enum class SkinArrow
{
    Left,
    Right,
    Up,
    Down,
    UpOutline,
    DownOutline,
};
void skin_arrow(SkinArrow arrow, int x, int y, int size, rf::Color tint);
// The stock popup: a near-opaque black panel with a thin grey edge, and a black title band with
// a lit LED beside the title. `dialog` and `band` are draw space.
void skin_popup(const UiContext& ctx, const UiRect& dialog, const UiRect& band, const char* title, int font);

// ---------------------------------------------------------------------------
// Widgets. Every one returns its result in the hit-test pass only. `hot` is the row's highlight
// (hovered or focused), which brightens the value the widget shows.
// ---------------------------------------------------------------------------

// `lit` shows it highlighted without the hover, as a popup's default button is.
bool skin_button(UiContext& ctx, const UiRect& r, const char* text, int font, SkinButton kind,
                 bool enabled, UiId id, bool lit = false);
// One stock menu button. `r` is the hit and layout rect, its face from `r.x`; clicks reach `hidden_w` further left,
// under the left bar, where the plate carries on. `selected` lights it like the
// highlight does; `arrow` marks a button that opens a panel. The label is centered on the face, left
// of the end cap, and takes two lines when one cannot hold it. Disabled, it is dimmed and takes
// nothing. `tall` draws the stock-height cap and plate, for a button skin::menu_button_tall_h high.
bool skin_menu_button(UiContext& ctx, const UiRect& r, int hidden_w, const char* label, int font, bool selected,
                      bool arrow, bool tall, UiId id, bool enabled);
// Widest label line a menu button `w` wide holds.
int skin_menu_button_label_w(const UiContext& ctx, int w);
// Icon-only affordance (per-row reset, search clear).
bool skin_icon_button(UiContext& ctx, const UiRect& r, const char* icon, rf::Color tint,
                      rf::Color tint_hover, UiId id);

// ◀ value ▶. Returns the step taken: -1, 0 or +1; a click on the value and Enter step forward.
int skin_cycler(UiContext& ctx, const UiRect& r, const char* value, int font, bool enabled, UiId id,
                bool hot);
bool skin_dropdown(UiContext& ctx, const UiRect& r, const std::vector<std::string>& items, int& sel,
                   int font, bool enabled, UiId id, bool hot);
// A compact On / Off for the steel: a lamp, lit while `on`, and its label. True when clicked or Enter pressed on it.
bool skin_lamp_toggle(UiContext& ctx, const UiRect& r, const char* label, bool on, int font, UiId id);
int skin_lamp_toggle_width(const UiContext& ctx, const char* label, int font);
struct SkinCyclerRects
{
    UiRect label;
    UiRect toggle;
};
// A labelled cycler right-aligned on `line`, or `centred` on it, its value box `value_w` reference pixels wide.
SkinCyclerRects skin_header_cycler_rects(const UiContext& ctx, const UiRect& line, const char* text, float value_w,
                                         int font, bool centred = false);
// "Label < value >": the label is part of the control, a click on it stepping forward. `lit` / `idle` colour it;
// disabled, it is dimmed. Returns the step taken: -1, 0 or +1.
int skin_labeled_cycler(UiContext& ctx, const UiRect& label, const UiRect& toggle, const char* text, const char* value,
                        int font, bool enabled, UiId id, rf::Color lit, rf::Color idle);
// A closed dropdown's look for a choice made elsewhere (the picture grids). True when pressed.
bool skin_select_box(UiContext& ctx, const UiRect& r, const char* text, int font, bool enabled, UiId id,
                     bool hot);
// Width a slider readout needs to hold any value of its range in `font`, never below
// skin::readout_w. Digits are measured as their widest glyph so the box does not move while dragging.
int skin_slider_readout_w(const UiContext& ctx, float min_value, float max_value, const UiNumberFormat& fmt,
                          int font);
// `readout_w` is the readout box width in pixels; the track takes the rest of `r`.
bool skin_slider(UiContext& ctx, const UiRect& r, float& value, float min_value, float max_value,
                 float step, float nudge, const UiNumberFormat& fmt, int font, int readout_w, bool enabled,
                 UiId id, bool hot);
// ◀ number ▶ in the cycler's shape. The arrows step and repeat while held; the number takes typing.
bool skin_stepper(UiContext& ctx, const UiRect& r, float& value, float min_value, float max_value,
                  float step, const UiNumberFormat& fmt, int font, bool enabled, UiId id,
                  bool integral, bool hot);
// Colour square plus an RRGGBB(AA) box. True when the square is clicked or Enter pressed on it,
// which opens the picker.
bool skin_swatch(UiContext& ctx, const UiRect& r, std::optional<uint32_t>& rgba, int font, bool enabled,
                 UiId id, bool hot);

// Colour picker pieces. Every channel runs 0..1; the hue wraps at 1. Colours are RRGGBBAA.
struct SkinHsva
{
    float h = 0.0f;
    float s = 0.0f;
    float v = 0.0f;
    float a = 1.0f;
};
SkinHsva skin_rgba_to_hsva(uint32_t rgba);
uint32_t skin_hsva_to_rgba(const SkinHsva& c);
// Saturation across, value down. Each area returns true when it changed `c`.
bool skin_sv_square(UiContext& ctx, const UiRect& r, SkinHsva& c, UiId id);
bool skin_hue_strip(UiContext& ctx, const UiRect& r, SkinHsva& c, UiId id);
bool skin_alpha_strip(UiContext& ctx, const UiRect& r, SkinHsva& c, UiId id);
// A colour over grey checks, or Default for an unset optional colour. The chip is the clickable
// form; true when clicked.
void skin_color_preview(const UiContext& ctx, const UiRect& r, std::optional<uint32_t> rgba, int font);
bool skin_color_chip(UiContext& ctx, const UiRect& r, std::optional<uint32_t> rgba, int font, UiId id);
// Text field: hit-tested by the kit, painted here.
// `invalid` paints a lamp-red border plus `warn_text` left of the field in skin::warn.
bool skin_field(UiContext& ctx, const UiRect& r, UiId id, std::string& text, size_t max_len,
                UiCharFilter filter, const char* hint, int font, bool enabled,
                bool invalid = false, const char* warn_text = nullptr);
bool skin_search_box(UiContext& ctx, const UiRect& r, UiId id, std::string& text, int font,
                     const char* hint = "Search options...");
// A text field's look alone, for a box whose typing the engine owns. Draw pass only.
void skin_field_face(const UiContext& ctx, const UiRect& r, std::string_view text, int font, bool caret);

// One key or mouse cap of a binding row. `activated` starts a capture, `cleared` unbinds; the
// small x only appears while the cap is hovered or focused and something is bound. `inherited`
// is the context view's `Global: F` state: still clearable, but drawn in skin::txt_hint.
struct SkinKeyCapResult
{
    bool activated = false;
    bool cleared = false;
};
SkinKeyCapResult skin_key_cap(UiContext& ctx, const UiRect& r, const char* text, bool bound,
                              bool capturing, bool enabled, UiId id, int font,
                              bool inherited = false);

// One row of a reorderable list: 3x2 grip dots, label, stock selection bar. True when clicked.
bool skin_reorder_row(UiContext& ctx, const UiRect& r, const char* label, bool selected, int font,
                      UiId id);

// One row of a stock list, less its text: hover and keyboard focus, a rule under it and the selection bar when
// `selected`. The caller draws the text, clear of the bar's LED.
struct SkinListRow
{
    bool activated = false; // Enter pressed on it, hit-test pass only
    bool clicked = false;   // hit-test pass only
    bool lit = false;       // hovered, selected or showing the keyboard focus
    UiRect draw;            // `r` in draw space
};
SkinListRow skin_list_row(UiContext& ctx, const UiRect& r, bool selected, UiId id);
// A list row tall enough for `font`.
int skin_list_row_h(const UiContext& ctx, int font);

// A scrolling list of rows of one height in the black well `well`, each row's id `ui_id(row_id_name, i)`.
// `empty_text`, when set, follows the rows and scrolls with them, each '\n'-separated line centered in a row of its
// own.
struct SkinScrollList
{
    int count = 0;
    int row_h = 0;
    const char* id_name = "";
    const char* row_id_name = "";
    const char* empty_text = nullptr;
    int font = 0;
};
// What a skin_scroll_list row builder is handed, for every row in order. A row out of view is not to be built; the
// keyboard can still step onto it, and `offscreen_activated` says Enter was pressed on it there.
struct SkinScrollListRow
{
    int index = 0;
    UiRect rect;
    UiId id = 0;
    bool in_view = false;
    bool offscreen_activated = false;
};
void skin_scroll_list(UiContext& ctx, const UiRect& well, const SkinScrollList& list, float& scroll, UiId& scrolled_to,
                      const std::function<void(const SkinScrollListRow&)>& build_row);

// One picker tile: thumbnail (or a placeholder carrying `caption`), hover highlight, selection
// glow and index tag. The bitmap is letterboxed into the image area, so non-square portraits keep
// their aspect. With `caption_line` the tile reserves skin_picker_caption_h at its bottom for the
// caption; without it the whole rect is image area. A caption too long for the tile loses its middle, or with
// `cut_end` its end. A disabled tile is dimmed and takes nothing. True when clicked.
bool skin_picker_tile(UiContext& ctx, const UiRect& r, int bitmap, int index, bool selected,
                      const char* caption, bool caption_line, int font, UiId id, bool cut_end = false,
                      bool enabled = true);
// Height the caption line of a picker tile takes, so the grid layout and the tile agree.
int skin_picker_caption_h(const UiContext& ctx, int font);

// A scrolling grid of picker tiles, centred in `view`. `max_cols` 0 leaves the count to the width.
struct SkinPickerGrid
{
    int count = 0;
    int selected = -1;
    float tile_w = 0.0f;
    float tile_h = 0.0f; // the image area; a captioned tile adds its caption line
    int (*bitmap)(int) = nullptr;
    const char* (*caption)(int) = nullptr;
    bool caption_line = false;
    bool cut_end = false;
    int font = 0;
    int min_cols = 1;
    int max_cols = 0;
    const char* id_name = "";
    bool enabled = true;
};
// Returns the tile picked by click or Enter, else -1.
int skin_picker_grid(UiContext& ctx, const UiRect& view, const SkinPickerGrid& grid, float& scroll, UiId& scrolled_to);

// Draw space: the bitmap letterboxed into `r`.
void skin_draw_fitted_bitmap(int bitmap, const UiRect& r);
// A black well, `image` inside it showing `bitmap` letterboxed, or "No preview" when there is none.
void skin_preview_well(const UiContext& ctx, const UiRect& well, const UiRect& image, int bitmap, int font);
// The same, the image filling the well inside its 1 px edge, as MeshPreview::render_in_well sizes it.
void skin_preview_well(const UiContext& ctx, const UiRect& well, int bitmap, int font);

int skin_badge_width(const UiContext& ctx, const char* text, int font, bool has_marker);
void skin_badge(const UiContext& ctx, const UiRect& r, const char* text, rf::Color color, int font,
                const char* icon, bool lamp_dot);

// Stock scrollbar drawn over the gutter a UiScrollRegion manages: arrow boxes at both ends and a
// steel thumb, placed by ui_scrollbar_geometry exactly where ui_scroll_end hit-tests them.
void skin_scrollbar(const UiContext& ctx, const UiRect& view, int content_h, float scroll,
                    bool dragging);
