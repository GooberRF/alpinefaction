#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "extras_menu.h"
#include "column_menu.h"
#include "menu_shell.h"
#include "reference_menu.h"
#include "ui_kit.h"
#include "../misc/alpine_options.h"
#include "../multi/demo/demo_browser.h"
#include "../rf/gameseq.h"
#include "../rf/input.h"
#include "../rf/localize.h"
#include "../rf/ui.h"

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

class DemosPanel final : public ColumnPanel
{
public:
    [[nodiscard]] bool is_open() const override
    {
        return demo_browser_is_open();
    }
    void open() override
    {
        demo_browser_open();
    }
    // Extras has no other panel to switch to.
    void close() override {}
    bool back() override
    {
        demo_browser_back();
        return true;
    }
    void build(UiContext& ctx, const MenuPanel& panel, const MenuFonts& fonts) override
    {
        demo_browser_build_panel(ctx, panel, fonts);
    }
    void commit() override
    {
        demo_browser_commit_panel();
    }
    void hints(const UiContext& ctx, std::vector<MenuHint>& hints) const override
    {
        // As back(), unless a box or a dropdown takes Esc.
        if (ctx.focused_text_id() != 0 || ctx.popup_id() != 0 || !demo_browser_in_subfolder()) {
            return;
        }
        for (MenuHint& hint : hints) {
            if (hint.key == "ESC") {
                hint.label = "Up";
            }
        }
    }
};

ColumnMenu g_menu;
DemosPanel g_demos;

ColumnButton button(ExtrasAction action, std::string label, rf::Key hotkey, bool opens_panel = false)
{
    return ColumnButton{static_cast<int>(action), std::move(label), hotkey, opens_panel};
}

std::vector<ColumnButton> make_buttons()
{
    std::vector<ColumnButton> buttons;
    buttons.push_back(button(ExtrasAction::Credits, menu_stock_label(rf::strings::credits), rf::KEY_C));
    // af_ui.tbl action 3 removes the button.
    if (get_option_or_default<int>(AlpineOptionID::SumTrailerButtonAction, -1) != 3) {
        buttons.push_back(
            button(ExtrasAction::SummonerTrailer, menu_stock_label(rf::strings::summoner_trailer), rf::KEY_S));
    }
    buttons.push_back(button(ExtrasAction::Demos, "DEMOS", rf::KEY_D, true));
    buttons.push_back(button(ExtrasAction::Reference, "REFERENCE", rf::KEY_R));
    buttons.push_back(button(ExtrasAction::GlassHouse, menu_stock_label(rf::strings::glass_house), rf::KEY_G));
    return buttons;
}

void request_close()
{
    if (g_menu.closing()) {
        return;
    }
    g_menu.request_close();
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
            // Picking the open panel's button again leaves it as it is, as Options does.
            g_menu.open_panel(&g_demos);
            break;
        case ExtrasAction::Reference:
            g_menu.hand_off({reference_menu_open, reference_menu_is_active, reference_menu_do_frame});
            break;
        case ExtrasAction::Back:
            request_close();
            break;
    }
    // After the handler, as stock's click dispatch does, so a trailer does not pause it mid-play.
    if (!g_menu.jump_action()) {
        ui_play_click();
    }
}

void __cdecl key_callback(int key)
{
    g_menu.key(key);
}

} // namespace

void extras_menu_enter()
{
    reference_menu_release();
    const int back = static_cast<int>(ExtrasAction::Back);
    g_menu.enter(menu_stock_label(rf::strings::extras), make_buttons(),
                 ColumnButton{back, menu_stock_label(rf::strings::back), rf::KEY_B}, back);
    g_menu.add_panel(&g_demos, static_cast<int>(ExtrasAction::Demos));
}

void extras_menu_do_frame(bool paused)
{
    if (g_menu.host_frame(paused)) {
        return;
    }
    // paused = 1 means a stock popup owns input this frame; it renders after us and on top.
    if (!paused) {
        if (const std::optional<int> action = g_menu.input(key_callback)) {
            run_action(static_cast<ExtrasAction>(*action));
        }
    }
    g_menu.draw();
}

bool extras_menu_is_closed()
{
    // A queued state change never runs our frame again, so it cannot wait for the exit animation.
    if (rf::gameseq_get_pending_state() != 0) {
        reference_menu_release();
        return true;
    }
    return g_menu.is_closed();
}
