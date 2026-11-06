#pragma once

#include <cstdint>
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
// Stock list selection: a red glow fading out to the right, with a lit LED at its left end.
void skin_selection_bar(const UiContext& ctx, int x, int y, int w, int h);
// Keyboard focus where the stock highlight alone would be ambiguous.
void skin_focus_ring(const UiContext& ctx, int x, int y, int w, int h);
// A whole chrome SVG rasterised at exactly `r`'s draw-space size.
void skin_sized_chrome(const char* name, const UiRect& r);
// The stock popup: a near-opaque black panel with a thin grey edge, and a black title band with
// a lit LED beside the title. `dialog` and `band` are draw space.
void skin_popup(const UiContext& ctx, const UiRect& dialog, const UiRect& band, const char* title, int font);

// ---------------------------------------------------------------------------
// Widgets. Every one returns its result in the hit-test pass only. `hot` is the row's highlight
// (hovered or focused), which brightens the value the widget shows.
// ---------------------------------------------------------------------------

bool skin_button(UiContext& ctx, const UiRect& r, const char* text, int font, SkinButton kind,
                 bool enabled, UiId id);
// One stock menu button. `r` is the part left visible, which is the hit area; the well carries
// on `hidden_w` further left, under the side bar. `selected` lights it like the highlight does;
// `arrow` marks a button that opens a panel. The label is centred between the end cap and
// `label_inset` (<= 0) left of `r`, where the bar's narrower body leaves the well showing, and takes
// two lines when one cannot hold it.
bool skin_menu_button(UiContext& ctx, const UiRect& r, int hidden_w, int label_inset, const char* label, int font,
                      bool selected, bool arrow, UiId id);
// Widest label line a menu button `w` wide holds.
int skin_menu_button_label_w(const UiContext& ctx, int w, int label_inset);
// Icon-only affordance (per-row reset, search clear).
bool skin_icon_button(UiContext& ctx, const UiRect& r, const char* icon, rf::Color tint,
                      rf::Color tint_hover, UiId id);

// ◀ value ▶. Returns the step taken: -1, 0 or +1; a click on the value and Enter step forward.
int skin_cycler(UiContext& ctx, const UiRect& r, const char* value, int font, bool enabled, UiId id,
                bool hot);
bool skin_dropdown(UiContext& ctx, const UiRect& r, const std::vector<std::string>& items, int& sel,
                   int font, bool enabled, UiId id, bool hot);
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
bool skin_search_box(UiContext& ctx, const UiRect& r, UiId id, std::string& text, int font);

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

// One picker tile: thumbnail (or a placeholder carrying `caption`), hover highlight, selection
// glow and index tag. The bitmap is letterboxed into the image area, so non-square portraits keep
// their aspect. With `caption_line` the tile reserves skin_picker_caption_h at its bottom for the
// caption; without it the whole rect is image area. True when clicked.
bool skin_picker_tile(UiContext& ctx, const UiRect& r, int bitmap, int index, bool selected,
                      const char* caption, bool caption_line, int font, UiId id);
// Height the caption line of a picker tile takes, so the grid layout and the tile agree.
int skin_picker_caption_h(const UiContext& ctx, int font);

int skin_badge_width(const UiContext& ctx, const char* text, int font, bool has_marker);
void skin_badge(const UiContext& ctx, const UiRect& r, const char* text, rf::Color color, int font,
                const char* icon, bool lamp_dot);

// Stock scrollbar drawn over the gutter a UiScrollRegion manages: arrow boxes at both ends and a
// steel thumb, placed by ui_scrollbar_geometry exactly where ui_scroll_end hit-tests them.
void skin_scrollbar(const UiContext& ctx, const UiRect& view, int content_h, float scroll,
                    bool dragging);
