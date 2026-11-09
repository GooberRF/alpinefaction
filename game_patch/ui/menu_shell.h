#pragma once

#include <functional>
#include <span>
#include <string>
#include "ui_kit.h"

// The frame Alpine's menu screens share: a layout hung off the stock side bars, the button column
// down the left and the title bar, all sized against a 1920x1080 reference.

inline constexpr float menu_ref_w = 1920.0f;
inline constexpr float menu_ref_h = 1080.0f;
inline constexpr float menu_column_y = 130.0f;
// The gap between the column and a panel, which a connector bridges.
inline constexpr float menu_connector_w = 48.0f;
// Button slots down the column, which a panel spans. The buttons then BACK fill them from the top
// and the rest stay empty, as on stock.
inline constexpr int menu_panel_slots = 11;
// A panel's header line, and where its body starts below the header's ridge.
inline constexpr float menu_panel_header_y = 158.0f;
inline constexpr float menu_panel_header_h = 54.0f;
inline constexpr float menu_panel_body_y = 218.0f;
// A panel's footer line, measured up from the bottom of its steel field.
inline constexpr float menu_panel_footer_inset = 52.0f;
inline constexpr float menu_panel_footer_h = 40.0f;

// Horizontal anchors in screen pixels, taken from the side bars.
struct MenuLayout
{
    int rail_left = 0;     // inner edges of the side bar pillars
    int rail_right = 0;
    int column_x = 0;      // where the buttons come out from under the left bar
    int column_hidden = 0; // how much of each well stays under it
    int column_w = 0;      // visible button width, end cap included
    int label_inset = 0;   // <= 0: offset from column_x to where the bar's narrower body leaves the wells showing
    int panel_x = 0;       // seated
    int panel_r = 0;       // under the right bar
};

// The panel beside the column: its steel frame and the recessed field inside it.
struct MenuPanel
{
    UiRect frame;
    UiRect inset;
    // Where content goes, clear of the frame's channel and the right pillar.
    int content_x = 0;
    int content_r = 0;
    // The header line, and the body below its ridge down to the field's bottom margin.
    UiRect header;
    UiRect body;
};

struct MenuFonts
{
    int title;
    int column;
    int heading;
    int name;
    int body;
    int caption;
};

// Where a menu screen's chrome sits this frame, for `slide` (0 = off canvas, 1 = seated) of its entry.
struct MenuFrame
{
    int canvas_y = 0;
    MenuLayout lay;
    int slide_w = 0;  // how far a panel travels off to the right
    int enter_dx = 0; // the title bar and panel, coming in from the right
    int col_dx = 0;   // the column, coming in from the left
    int top = 0;      // the column's first slot
    MenuFonts fonts{};
};

struct MenuColumnItem
{
    std::string label;
    bool selected = false;
    // Off for a button that acts rather than opening a panel.
    bool arrow = true;
};

// A stock string as a column label: upper case, its line breaks folded to single spaces for the column to
// break itself.
std::string menu_stock_label(const char* text);
float menu_screen_scale();
// Also sets the context's scale.
MenuFrame menu_frame(UiContext& ctx, float slide);
int menu_font(bool bold, float ref_px, float scale);
MenuLayout menu_layout(const UiContext& ctx);
int menu_button_h(const UiContext& ctx);
// This frame's share of a 0..1 slide. Every slide runs at constant speed for as long as the stock
// menus' do.
float menu_slide_step();
// `clear_r` is the right pillar's inner edge. Returns the free stretch of the bar after the title.
UiRect menu_draw_title_bar(const UiContext& ctx, int x, int right, int clear_r, int canvas_y, const char* title,
                           int title_font);
// `dx` slides the panel off to the right; `top` is the column's first slot.
MenuPanel menu_panel(const UiContext& ctx, const MenuLayout& lay, int dx, int top, int canvas_y);
void menu_draw_panel(const UiContext& ctx, const MenuPanel& panel);
// The ridge under the panel's header line.
void menu_draw_panel_ridge(const UiContext& ctx, const MenuPanel& panel);
// The panel's heading on its header line, and the ridge under it.
void menu_draw_panel_heading(const UiContext& ctx, const MenuPanel& panel, const char* text, int font);
// Bridges the gap from the column button whose slot is centred on `mid_y`.
void menu_draw_connector(const UiContext& ctx, const MenuPanel& panel, int mid_y);
// `items` then BACK, one slot each from `y`. Returns the index clicked, `items.size()` for BACK, or -1.
int menu_build_column(UiContext& ctx, const MenuLayout& lay, int x, int y, std::span<const MenuColumnItem> items,
                      const char* back_label, int font, const char* id_name);

// Forgets the context's transients and gives it the menus' style.
void menu_ui_reset(UiContext& ctx);
// A frame's hit-test pass: the keys go to `key_callback`, the mouse to the context, then `build` runs. A text box
// lets go once the keys have opened the console, before they reach it.
void menu_run_hit_test(UiContext& ctx, void (*key_callback)(int), const std::function<void(UiContext&)>& build);
// A frame's draw pass over the stock menu background, popups on top.
void menu_run_draw(UiContext& ctx, const std::function<void(UiContext&)>& build);
// The console reads the key queue a focused text box drains, so the box lets go while the console is open.
void menu_drop_text_focus_under_console(UiContext& ctx);
