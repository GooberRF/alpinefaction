#include <optional>
#include <string>
#include <vector>
#include "game_menu.h"
#include "column_menu.h"
#include "menu_popup.h"
#include "menu_shell.h"
#include "multi_pause_menu.h"
#include "player_panel.h"
#include "reference_menu.h"
#include "ui_kit.h"
#include "../misc/misc.h"
#include "../rf/gameseq.h"
#include "../rf/input.h"
#include "../rf/localize.h"
#include "../rf/multi.h"
#include "../rf/player/control_config.h"
#include "../rf/player/player.h"
#include "../rf/save_restore.h"
#include "../rf/ui.h"

namespace
{

enum class GameMenuAction
{
    NewGame,
    Save,
    Load,
    Multi,
    Options,
    Reference,
    Return,
    Extras,
    Quit,
    // Esc on the title menu, which stock ignores.
    None,
    // The path strip's player: Multi with its PLAYER panel seated.
    Player,
};

enum class GameMenuMode
{
    Title,
    SinglePause,
    MultiPause,
};

ColumnMenu g_menu;
bool g_active = false;
GameMenuMode g_mode = GameMenuMode::Title;

int action_id(GameMenuAction action)
{
    return static_cast<int>(action);
}

ColumnButton button(GameMenuAction action, const char* label, rf::Key hotkey)
{
    return ColumnButton{action_id(action), menu_stock_label(label), hotkey};
}

// Stock's buttons for each mode, in its order and with its letters. Quit takes the column's Back slot.
std::vector<ColumnButton> make_buttons(GameMenuMode mode)
{
    switch (mode) {
        case GameMenuMode::Title:
            return {
                button(GameMenuAction::NewGame, rf::strings::main_menu_new, rf::KEY_N),
                button(GameMenuAction::Load, rf::strings::main_menu_load, rf::KEY_L),
                button(GameMenuAction::Multi, rf::strings::multi, rf::KEY_M),
                button(GameMenuAction::Options, rf::strings::options, rf::KEY_O),
                button(GameMenuAction::Extras, rf::strings::main_menu_extras, rf::KEY_E),
            };
        case GameMenuMode::SinglePause:
            return {
                button(GameMenuAction::Save, rf::strings::main_menu_save, rf::KEY_S),
                button(GameMenuAction::Load, rf::strings::main_menu_load, rf::KEY_L),
                button(GameMenuAction::Options, rf::strings::options, rf::KEY_O),
                button(GameMenuAction::Return, rf::strings::main_menu_return, rf::KEY_R),
            };
        case GameMenuMode::MultiPause:
            return {
                button(GameMenuAction::Multi, rf::strings::multi, rf::KEY_M),
                button(GameMenuAction::Options, rf::strings::options, rf::KEY_O),
                ColumnButton{action_id(GameMenuAction::Reference), "REFERENCE"},
                button(GameMenuAction::Return, rf::strings::main_menu_return, rf::KEY_R),
            };
    }
    return {};
}

void open_new_game()
{
    g_menu.popup().open(
        menu_stock_label(rf::strings::new_game), rf::strings::new_game_training_advice,
        {
            {menu_stock_label(rf::strings::cancel)},
            {menu_stock_label(rf::strings::skip_training), rf::KEY_S},
            {menu_stock_label(rf::strings::go_to_training), rf::KEY_G},
        },
        // Esc cancels and Enter goes to training, as stock's keys and default do.
        0, 2,
        [](int pressed, const std::string&) {
            if (pressed == 1) {
                rf::ui::mainmenu_new_game_skip_training();
            }
            else if (pressed == 2) {
                rf::ui::mainmenu_new_game_training();
            }
        },
        MenuPopupStyle::Neutral);
}

void run_action(GameMenuAction action)
{
    switch (action) {
        case GameMenuAction::NewGame:
            open_new_game();
            ui_play_click();
            break;
        case GameMenuAction::Save:
            rf::ui::mainmenu_button_action(rf::ui::MAIN_MENU_BUTTON_SAVE);
            g_menu.request_close();
            break;
        case GameMenuAction::Load:
            rf::ui::mainmenu_button_action(rf::ui::MAIN_MENU_BUTTON_LOAD);
            g_menu.request_close();
            break;
        case GameMenuAction::Multi:
            player_panel_request_seat(false);
            if (g_mode == GameMenuMode::MultiPause) {
                g_menu.hand_off({multi_pause_menu_open, multi_pause_menu_is_active, multi_pause_menu_do_frame});
                if (!g_menu.jump_action()) {
                    ui_play_click();
                }
            }
            else {
                // Stock's network checks may put up a popup instead of opening the Multi menu.
                if (g_menu.jump_action()) {
                    rf::ui::mainmenu_multi_click();
                }
                else {
                    rf::ui::mainmenu_button_action(rf::ui::MAIN_MENU_BUTTON_MULTI);
                }
                if (rf::gameseq_get_pending_state() == rf::GS_MULTI_MENU) {
                    g_menu.request_close();
                }
            }
            break;
        case GameMenuAction::Options:
            rf::ui::mainmenu_button_action(rf::ui::MAIN_MENU_BUTTON_OPTIONS);
            g_menu.request_close();
            break;
        case GameMenuAction::Reference:
            g_menu.hand_off({reference_menu_open, reference_menu_is_active, reference_menu_do_frame});
            ui_play_click();
            break;
        case GameMenuAction::Return:
            rf::ui::mainmenu_button_action(rf::ui::MAIN_MENU_BUTTON_RETURN);
            g_menu.request_close();
            break;
        case GameMenuAction::Extras:
            rf::ui::mainmenu_button_action(rf::ui::MAIN_MENU_BUTTON_EXTRAS);
            g_menu.request_close();
            break;
        case GameMenuAction::Quit:
            // Stock's confirmation; its Yes changes the state, which closes this one without waiting.
            rf::ui::mainmenu_button_action(rf::ui::MAIN_MENU_BUTTON_QUIT);
            break;
        case GameMenuAction::None:
            break;
        case GameMenuAction::Player:
            run_action(GameMenuAction::Multi);
            // Also for the Multi screen that the continue button of stock's network popup opens.
            player_panel_request_seat(true);
            break;
    }
}

// Stock's main menu takes the Quick Load control.
void poll_quick_load()
{
    bool just_pressed = false;
    if (rf::local_player
        && rf::control_config_check_pressed(&rf::local_player->settings.controls, rf::CC_ACTION_QUICK_LOAD,
                                            &just_pressed)
        && just_pressed) {
        rf::sr::quick_load();
    }
}

void __cdecl key_callback(int key)
{
    g_menu.key(key);
}

} // namespace

void game_menu_on_main_menu_init(bool passing_through)
{
    // Set only on the way out to the Multi screen, which takes it, so one still here is stale: its network popup was
    // canceled.
    player_panel_request_seat(false);
    if (legacy_menus_enabled() || passing_through) {
        if (g_active) {
            game_menu_release();
        }
        return;
    }
    g_active = true;
    multi_pause_menu_release();
    reference_menu_release();
    g_mode = !rf::ui::mainmenu_game_beneath ? GameMenuMode::Title
             : rf::is_multi                 ? GameMenuMode::MultiPause
                                            : GameMenuMode::SinglePause;
    // Stock's init has played the slide-in. Esc returns to a game, as stock's does, and does nothing on the title.
    const GameMenuAction esc = g_mode == GameMenuMode::Title ? GameMenuAction::None : GameMenuAction::Return;
    const bool title = g_mode == GameMenuMode::Title;
    g_menu.enter(title ? "RED FACTION" : "", make_buttons(g_mode),
                 button(GameMenuAction::Quit, rf::strings::quit, rf::KEY_Q), action_id(esc), false);
    g_menu.set_crumb(menu_root_crumb());
    g_menu.set_esc_hint(title ? "" : "Return");
    if (g_mode != GameMenuMode::SinglePause) {
        g_menu.set_player_action(action_id(GameMenuAction::Player));
    }
}

bool game_menu_is_active()
{
    return g_active;
}

void game_menu_do_frame(bool paused)
{
    if (g_menu.host_frame(paused)) {
        return;
    }

    // paused = 1 means a stock popup owns input this frame; it renders after us and on top.
    if (!paused) {
        // On the title screen a click on the hint bar's version counts toward the egg, as stock's label does, unless
        // the pass ran under a popup; asked first, as the pass's commit may close it.
        const bool egg = g_mode == GameMenuMode::Title && !g_menu.popup().active() && !g_menu.closing();
        const std::optional<int> action = g_menu.input(key_callback);
        const UiContext& ui = g_menu.ui();
        if (egg && ui.click && ui_hover(ui, menu_strip_context_rect(ui))) {
            main_menu_version_clicked();
        }
        if (action) {
            run_action(static_cast<GameMenuAction>(*action));
        }
        if (!g_menu.popup().active() && !g_menu.closing()) {
            poll_quick_load();
        }
    }

    g_menu.draw();
    if (g_mode != GameMenuMode::MultiPause) {
        main_menu_render_egg();
    }
}

bool game_menu_is_closed()
{
    // A change the menu did not start (a level change, a kick, a confirmed popup) does not wait for the slide.
    if (g_menu.closing() && !g_menu.is_closed()) {
        return false;
    }
    game_menu_release();
    return true;
}

void game_menu_release()
{
    g_active = false;
    // No jump passes the main menu, so one left as it closes is stale, as after a level change.
    column_menu_cancel_jump();
    multi_pause_menu_release();
    reference_menu_release();
}
