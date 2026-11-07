#include <algorithm>
#include <cmath>
#include <string_view>
#include "menu_shell.h"
#include "settings_skin.h"
#include "../graphics/gr.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/os/frametime.h"
#include "../rf/ui.h"

namespace
{

// Horizontally the layout hangs off the stock side bars, which are drawn in 640-wide units. The
// inner edge is the bars' widest point, the bracket above the column; beside the buttons their body
// ends further out, with only smaller brackets reaching in lower down.
constexpr float rail_inner_units = 57.0f;
constexpr float rail_body_units = 40.7f;
constexpr float rail_tuck_units = 11.0f;
// The visible part of a column button's well; the end cap follows it.
constexpr float well_visible_w = 220.0f;

constexpr float slide_seconds = 0.25f;
constexpr float min_animation_step = 0.002f;

// A column label breaks at its spaces, so its widest word is what has to fit.
void note_widest_word(std::string_view text, int font, std::string_view& widest, int& widest_w)
{
    while (!text.empty()) {
        const std::size_t end = text.find(' ');
        const std::string_view word = text.substr(0, end);
        const int w = rf::gr::get_string_size(word, font).first;
        if (w > widest_w) {
            widest = word;
            widest_w = w;
        }
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    }
}

} // namespace

float menu_screen_scale()
{
    return std::min(rf::gr::clip_width() / menu_ref_w, rf::gr::clip_height() / menu_ref_h);
}

int menu_font(bool bold, float ref_px, float scale)
{
    // TTF sizes for a window under 480 px are unreadable, so stock keeps its bitmap fonts there.
    if (rf::gr::clip_height() >= 480) {
        const int px = std::max(8, static_cast<int>(std::lround(ref_px * scale)));
        const int font = ui_ttf_font(bold, px);
        if (font >= 0) {
            return font;
        }
    }
    return bold ? rf::ui::large_font : rf::ui::medium_font_0;
}

MenuLayout menu_layout(const UiContext& ctx)
{
    const int w = rf::gr::clip_width();
    // The side bars are drawn at stock's own menu scale; fall back to the screen's ratio if stock
    // has not set it.
    const float sx = rf::ui::scale_x > 0.0f ? rf::ui::scale_x : static_cast<float>(w) / 640.0f;
    const int tuck = static_cast<int>(std::lround(rail_tuck_units * sx));
    MenuLayout lay;
    lay.rail_left = static_cast<int>(std::lround(rail_inner_units * sx));
    lay.rail_right = w - lay.rail_left;
    lay.column_x = lay.rail_left;
    lay.column_hidden = lay.rail_left - tuck;
    lay.column_w = skin_px(ctx, well_visible_w) + skin_px(ctx, skin::menu_cap_w);
    lay.label_inset = static_cast<int>(std::lround(rail_body_units * sx)) - lay.column_x;
    lay.panel_x = lay.column_x + lay.column_w + skin_px(ctx, menu_connector_w);
    lay.panel_r = w - tuck;
    return lay;
}

int menu_button_h(const UiContext& ctx)
{
    // Every slot uses the same rounded height, so anything lined up with a button lines up exactly.
    return skin_px(ctx, skin::menu_button_h);
}

float menu_slide_step()
{
    // Floored so a stalled frametime can never park a slide short of its end.
    return std::max(rf::frametime, min_animation_step) / slide_seconds;
}

UiRect menu_draw_title_bar(const UiContext& ctx, int x, int right, int clear_r, int canvas_y, const char* title,
                           int title_font)
{
    const auto s = [&](float v) { return ctx.scaled(v); };
    const UiRect bar{x, canvas_y + s(20.0f), right - x, s(84.0f)};
    skin_brushed_plate(ctx, bar.x, bar.y, bar.w, bar.h, false);
    const int block_w = s(84.0f);
    skin_brushed_plate(ctx, bar.x, bar.y, block_w, bar.h, true);
    const int mid_y = bar.y + bar.h / 2;
    skin_screw(ctx, bar.x + block_w / 2, mid_y, 20.0f);
    skin_screw(ctx, clear_r - s(30.0f), mid_y, 20.0f);

    const int title_x = bar.x + block_w + s(24.0f);
    rf::gr::set_color(skin::ink_title);
    rf::gr::string(title_x, mid_y - rf::gr::get_font_height(title_font) / 2, title, title_font);

    const int free_l = title_x + rf::gr::get_string_size(title, title_font).first + s(40.0f);
    const int free_r = clear_r - s(60.0f);
    return UiRect{free_l, bar.y, std::max(0, free_r - free_l), bar.h};
}

int menu_build_column(UiContext& ctx, const MenuLayout& lay, int x, int y, std::span<const MenuColumnItem> items,
                      const char* back_label, int font, const char* id_name)
{
    // Every label takes the size the widest word fits at, so the column reads as one set; a label
    // too long for one line breaks onto two rather than shrinking the rest.
    std::string_view widest = back_label;
    int widest_w = 0;
    note_widest_word(back_label, font, widest, widest_w);
    for (const MenuColumnItem& item : items) {
        note_widest_word(item.label, font, widest, widest_w);
    }
    font = skin_fit_text(ctx, widest, font, skin_menu_button_label_w(ctx, lay.column_w, lay.label_inset)).font;

    const int button_h = menu_button_h(ctx);
    int clicked = -1;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const UiRect r{x, y + static_cast<int>(i) * button_h, lay.column_w, button_h};
        if (skin_menu_button(ctx, r, lay.column_hidden, lay.label_inset, items[i].label.c_str(), font,
                             items[i].selected, items[i].arrow, ui_id(id_name, static_cast<int>(i)))) {
            clicked = static_cast<int>(i);
        }
    }
    const UiRect back{x, y + static_cast<int>(items.size()) * button_h, lay.column_w, button_h};
    if (skin_menu_button(ctx, back, lay.column_hidden, lay.label_inset, back_label, font, false, false,
                         ui_id("back"))) {
        clicked = static_cast<int>(items.size());
    }
    return clicked;
}
