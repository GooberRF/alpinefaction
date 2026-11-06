#include <algorithm>
#include <optional>
#include <string>
#include <vector>
#include <common/utils/string-utils.h>
#include "extras_menu.h"
#include "menu_shell.h"
#include "settings_skin.h"
#include "ui_kit.h"
#include "../misc/alpine_options.h"
#include "../multi/demo/demo_browser.h"
#include "../rf/gameseq.h"
#include "../rf/gr/gr.h"
#include "../rf/input.h"
#include "../rf/localize.h"
#include "../rf/multi.h"
#include "../rf/sound/sound.h"
#include "../rf/ui.h"
#include "../sound/sound.h"

namespace
{

enum class ExtrasAction
{
    Credits,
    SummonerTrailer,
    GlassHouse,
    Demos,
    Back,
};

struct ExtrasButton
{
    ExtrasAction action;
    rf::Key hotkey;
};

UiContext g_ui;
bool g_closing = false;
// 0 = fully off canvas, 1 = seated. Rises on entry, falls again once Back/ESC is taken.
float g_slide = 0.0f;
// Parallel: what each column button does, and how it reads.
std::vector<ExtrasButton> g_buttons;
std::vector<MenuColumnItem> g_items;
std::string g_back_label;
// Taken at the end of the input pass, so a handler that changes state or plays a video never runs
// with the pass half built.
std::optional<ExtrasAction> g_pending;

// Stock labels carry their own line breaks for stock's buttons; the column breaks them itself, at
// single spaces.
std::string stock_label(const char* text)
{
    std::string label;
    for (const char c : string_to_upper(text)) {
        const bool space = c == ' ' || c == '\n' || c == '\r';
        if (!space) {
            label += c;
        }
        else if (!label.empty() && label.back() != ' ') {
            label += ' ';
        }
    }
    if (!label.empty() && label.back() == ' ') {
        label.pop_back();
    }
    return label;
}

void add_button(ExtrasAction action, std::string label, rf::Key hotkey)
{
    g_buttons.push_back({action, hotkey});
    g_items.push_back(MenuColumnItem{std::move(label), false, false});
}

void make_buttons()
{
    g_buttons.clear();
    g_items.clear();
    add_button(ExtrasAction::Credits, stock_label(rf::strings::credits), rf::KEY_C);
    // af_ui.tbl action 3 removes the button.
    if (get_option_or_default<int>(AlpineOptionID::SumTrailerButtonAction, -1) != 3) {
        add_button(ExtrasAction::SummonerTrailer, stock_label(rf::strings::summoner_trailer), rf::KEY_S);
    }
    add_button(ExtrasAction::GlassHouse, stock_label(rf::strings::glass_house), rf::KEY_G);
    add_button(ExtrasAction::Demos, "DEMOS", rf::KEY_D);
    g_back_label = stock_label(rf::strings::back);
}

void request_close()
{
    if (g_closing) {
        return;
    }
    g_closing = true;
    rf::gameseq_pop_state();
}

void run_action(ExtrasAction action)
{
    switch (action) {
        case ExtrasAction::Credits:
            rf::ui::extras_credits_click(0, 0);
            break;
        case ExtrasAction::SummonerTrailer:
            rf::ui::extras_summoner_trailer_click(0, 0);
            break;
        case ExtrasAction::GlassHouse:
            rf::ui::extras_glass_house_click(0, 0);
            break;
        case ExtrasAction::Demos:
            demo_browser_open();
            break;
        case ExtrasAction::Back:
            request_close();
            break;
    }
    // After the handler, as stock's click dispatch does, so a trailer does not pause it mid-play.
    ui_play_click();
}

void extras_menu_build(UiContext& ctx)
{
    const float scale = menu_screen_scale();
    ctx.set_scale(scale);
    const int canvas_y = (rf::gr::clip_height() - static_cast<int>(menu_ref_h * scale)) / 2;
    const MenuLayout lay = menu_layout(ctx);

    const int enter_dx = static_cast<int>((1.0f - g_slide) * static_cast<float>(rf::gr::clip_width() - lay.panel_x));
    const int col_dx = -static_cast<int>((1.0f - g_slide) * static_cast<float>(lay.column_x + lay.column_w));

    if (ctx.draw) {
        menu_draw_title_bar(ctx, lay.panel_x + enter_dx, lay.panel_r + enter_dx, lay.rail_right + enter_dx, canvas_y,
                            rf::strings::extras, menu_font(true, 46.0f, scale));
    }

    const int top = canvas_y + static_cast<int>(menu_column_y * scale);
    const int clicked = menu_build_column(ctx, lay, lay.column_x + col_dx, top, g_items, g_back_label.c_str(),
                                          menu_font(true, 20.0f, scale), "extras");
    if (clicked >= 0 && !ctx.draw) {
        g_pending = clicked < static_cast<int>(g_buttons.size()) ? g_buttons[clicked].action : ExtrasAction::Back;
    }

    // Stock draws its side bars over its menus.
    if (ctx.draw) {
        rf::gr::reset_clip();
        rf::ui::menu_draw_side_bars();
    }
}

void __cdecl extras_menu_key_callback(int key)
{
    const int code = key & rf::KEY_MASK;
    if (code == rf::KEY_ESC) {
        g_pending = ExtrasAction::Back;
        return;
    }
    // Stock's own letters, with no modifier held.
    if (key == code) {
        if (code == rf::KEY_B) {
            g_pending = ExtrasAction::Back;
            return;
        }
        for (const ExtrasButton& button : g_buttons) {
            if (code == button.hotkey) {
                g_pending = button.action;
                return;
            }
        }
    }
    g_ui.handle_nav_key(static_cast<rf::Key>(key));
}

void extras_menu_input()
{
    // The browser sits over the menu and takes all of its input while open.
    if (demo_browser_is_open()) {
        demo_browser_do_input();
        return;
    }

    rf::game_poll(extras_menu_key_callback);

    int x = 0;
    int y = 0;
    int z = 0;
    rf::mouse_get_pos(x, y, z);
    g_ui.begin_frame(x, y, rf::mouse_dz, rf::mouse_button_is_down(0), rf::mouse_was_button_pressed(0) != 0,
                     rf::mouse_was_button_pressed(1) != 0);
    g_ui.begin_pass(UiPass::HitTest);
    extras_menu_build(g_ui);
    g_ui.end_pass();

    if (g_pending) {
        const ExtrasAction action = *g_pending;
        g_pending.reset();
        run_action(action);
    }
}

} // namespace

void extras_menu_enter()
{
    g_closing = false;
    g_slide = 0.0f;
    g_pending.reset();
    make_buttons();
    g_ui.reset();
    g_ui.style = settings_menu_style();
    g_ui.set_hover_sound_enabled(true);
    rf::snd_play(stock_sound_id::menu_slide_in, 0, 0.0f, 1.0f);
}

void extras_menu_do_frame(bool paused)
{
    const float step = menu_slide_step();
    g_slide = g_closing ? std::max(0.0f, g_slide - step) : std::min(1.0f, g_slide + step);

    // paused = 1 means a stock popup owns input this frame; it renders after us and on top.
    if (!paused && !g_closing) {
        extras_menu_input();
    }

    rf::gr::reset_clip();
    rf::ui::menu_draw_background();

    int x = 0;
    int y = 0;
    int z = 0;
    rf::mouse_get_pos(x, y, z);
    // Nothing under the demo browser lights up for a pointer that is over the browser.
    if (demo_browser_is_open()) {
        x = -1;
        y = -1;
    }
    g_ui.set_mouse_pos(x, y);
    g_ui.begin_pass(UiPass::Draw);
    extras_menu_build(g_ui);
    g_ui.end_pass();
    g_ui.draw_overlays();
    rf::gr::reset_clip();
}

bool extras_menu_is_closed()
{
    // A queued state change never runs our frame again, so it cannot wait for the exit animation.
    if (rf::gameseq_get_pending_state() != 0) {
        return true;
    }
    return g_closing && g_slide <= 0.0f;
}
