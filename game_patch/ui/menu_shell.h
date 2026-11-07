#pragma once

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
};

struct MenuColumnItem
{
    std::string label;
    bool selected = false;
    // Off for a button that acts rather than opening a panel.
    bool arrow = true;
};

float menu_screen_scale();
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
MenuPanel menu_panel(const UiContext& ctx, const MenuLayout& lay, int dx, int top);
void menu_draw_panel(const UiContext& ctx, const MenuPanel& panel);
// Bridges the gap from the column button whose slot is centred on `mid_y`.
void menu_draw_connector(const UiContext& ctx, const MenuPanel& panel, int mid_y);
// `items` then BACK, one slot each from `y`. Returns the index clicked, `items.size()` for BACK, or -1.
int menu_build_column(UiContext& ctx, const MenuLayout& lay, int x, int y, std::span<const MenuColumnItem> items,
                      const char* back_label, int font, const char* id_name);
