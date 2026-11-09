#include <optional>
#include <string>
#include <vector>
#include "extras_menu.h"
#include "menu_shell.h"
#include "reference_menu.h"
#include "ui_kit.h"
#include "../misc/alpine_options.h"
#include "../multi/demo/demo_browser.h"
#include "../rf/gameseq.h"
#include "../rf/gr/gr.h"
#include "../rf/input.h"
#include "../rf/localize.h"
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
    Reference,
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
// REFERENCE was taken: the column slides out, then the Reference screen slides in over the same chrome.
bool g_reference_pending = false;
// Parallel: what each column button does, and how it reads.
std::vector<ExtrasButton> g_buttons;
std::vector<MenuColumnItem> g_items;
std::string g_back_label;
// Taken at the end of the input pass, so a handler that changes state or plays a video never runs
// with the pass half built.
std::optional<ExtrasAction> g_pending;

void add_button(ExtrasAction action, std::string label, rf::Key hotkey)
{
    g_buttons.push_back({action, hotkey});
    g_items.push_back(MenuColumnItem{std::move(label), false, false});
}

void make_buttons()
{
    g_buttons.clear();
    g_items.clear();
    add_button(ExtrasAction::Credits, menu_stock_label(rf::strings::credits), rf::KEY_C);
    // af_ui.tbl action 3 removes the button.
    if (get_option_or_default<int>(AlpineOptionID::SumTrailerButtonAction, -1) != 3) {
        add_button(ExtrasAction::SummonerTrailer, menu_stock_label(rf::strings::summoner_trailer), rf::KEY_S);
    }
    add_button(ExtrasAction::GlassHouse, menu_stock_label(rf::strings::glass_house), rf::KEY_G);
    add_button(ExtrasAction::Demos, "DEMOS", rf::KEY_D);
    add_button(ExtrasAction::Reference, "REFERENCE", rf::KEY_R);
    g_back_label = menu_stock_label(rf::strings::back);
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
        case ExtrasAction::Reference:
            g_reference_pending = true;
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
    const MenuFrame frame = menu_frame(ctx, g_slide);
    const MenuLayout& lay = frame.lay;
    if (ctx.draw) {
        menu_draw_title_bar(ctx, lay.panel_x + frame.enter_dx, lay.panel_r + frame.enter_dx,
                            lay.rail_right + frame.enter_dx, frame.canvas_y, rf::strings::extras, frame.fonts.title);
    }

    const int clicked = menu_build_column(ctx, lay, lay.column_x + frame.col_dx, frame.top, g_items,
                                          g_back_label.c_str(), frame.fonts.column, "extras");
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

    menu_run_hit_test(g_ui, extras_menu_key_callback, extras_menu_build);

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
    g_reference_pending = false;
    reference_menu_release();
    make_buttons();
    menu_ui_reset(g_ui);
    rf::snd_play(stock_sound_id::menu_slide_in, 0, 0.0f, 1.0f);
}

void extras_menu_do_frame(bool paused)
{
    if (reference_menu_host_frame(g_reference_pending, g_slide, g_closing, paused)) {
        return;
    }

    // paused = 1 means a stock popup owns input this frame; it renders after us and on top.
    if (!paused && !g_closing && !g_reference_pending) {
        extras_menu_input();
    }

    // Nothing under the demo browser lights up for a pointer that is over the browser.
    menu_run_draw(g_ui, extras_menu_build, demo_browser_is_open());
}

bool extras_menu_is_closed()
{
    // A queued state change never runs our frame again, so it cannot wait for the exit animation.
    if (rf::gameseq_get_pending_state() != 0) {
        reference_menu_release();
        return true;
    }
    return g_closing && g_slide <= 0.0f;
}
