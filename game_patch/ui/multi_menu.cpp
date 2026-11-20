#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "multi_menu.h"
#include "column_menu.h"
#include "menu_shell.h"
#include "player_panel.h"
#include "server_browser.h"
#include "ui_kit.h"
#include "../rf/gameseq.h"
#include "../rf/input.h"
#include "../rf/localize.h"
#include "../rf/multi.h"
#include "../rf/ui.h"

namespace
{

enum class MultiAction
{
    Player,
    JoinGame,
    CreateGame,
    Back,
    // The path strip's player.
    OpenPlayer,
};

ColumnMenu g_menu;
PlayerPanel g_player;
ServerBrowserPanel g_browser{g_menu.popup()};
// The column is up, over either state.
bool g_active = false;
// Closed by a jump passing through to a menu below, whose screen plays the slide-out.
bool g_passed_by_jump = false;

int action_id(MultiAction action)
{
    return static_cast<int>(action);
}

std::vector<ColumnButton> make_buttons()
{
    // As stock's, no multiplayer game over the single player one running beneath.
    const bool game_beneath = rf::ui::mainmenu_game_beneath;
    return {
        ColumnButton{action_id(MultiAction::Player), menu_stock_label(rf::strings::player), rf::KEY_P, true},
        ColumnButton{action_id(MultiAction::JoinGame), menu_stock_label(rf::strings::join), rf::KEY_J, true,
                     !game_beneath},
        ColumnButton{action_id(MultiAction::CreateGame), "HOST", rf::KEY_H, false, !game_beneath},
    };
}

void enter_column(bool slide_in_sound)
{
    g_active = true;
    g_passed_by_jump = false;
    g_player.reset();
    const int back = action_id(MultiAction::Back);
    g_menu.enter(menu_stock_label(rf::strings::multiplayer), make_buttons(),
                 ColumnButton{back, menu_stock_label(rf::strings::back), rf::KEY_B}, back, slide_in_sound);
    g_menu.add_panel(&g_player, action_id(MultiAction::Player));
    g_menu.add_panel(&g_browser, action_id(MultiAction::JoinGame));
    g_menu.set_player_action(action_id(MultiAction::OpenPlayer));
}

bool browser_state()
{
    return rf::gameseq_get_state() == rf::GS_MULTI_SERVER_LIST;
}

void run_action(MultiAction action)
{
    switch (action) {
        case MultiAction::Player:
            g_menu.toggle_panel(&g_player);
            break;
        case MultiAction::OpenPlayer:
            // Over the browser's state too, which pops once the browser lets go.
            g_menu.open_panel(&g_player);
            break;
        case MultiAction::JoinGame:
            // The browser lives in the server list's state, which stock's join needs.
            if (browser_state()) {
                g_menu.toggle_panel(&g_browser);
            }
            else if (rf::gameseq_get_pending_state() == rf::GS_NONE) {
                rf::gameseq_push_state(rf::GS_MULTI_SERVER_LIST, false, false);
            }
            break;
        case MultiAction::CreateGame:
            g_menu.request_close();
            rf::gameseq_push_state(rf::GS_MULTI_CREATE_GAME, false, false);
            break;
        case MultiAction::Back:
            if (browser_state()) {
                g_browser.close();
            }
            else if (!g_menu.closing()) {
                g_passed_by_jump = g_menu.jump_action();
                g_menu.request_close();
                rf::gameseq_pop_state();
            }
            break;
    }
    if (!g_menu.jump_action()) {
        ui_play_click();
    }
}

void __cdecl key_callback(int key)
{
    g_menu.key(key);
}

void release()
{
    g_active = false;
    // A forced close drops a half-typed name.
    g_menu.ui().release_text_focus(false);
    g_player.release_preview();
    g_browser.close();
}

} // namespace

void multi_menu_enter(rf::GameState old_state)
{
    // Back from the browser's state: the column stays, and the browser slides away on it.
    if (g_active && old_state == rf::GS_MULTI_SERVER_LIST) {
        return;
    }
    enter_column(true);
    if (player_panel_take_seat_request()) {
        g_menu.seat_panel(&g_player);
    }
}

void multi_menu_enter_server_list()
{
    if (!g_active) {
        enter_column(false);
    }
    // What a join needs of stock's init (0x0044C2D0): no join pending, and multi_do_frame running.
    rf::multi_clear_current_server_addr();
    rf::set_in_mp_flag();
    // Opened along with the column it comes in with, else slid in beside it.
    if (g_menu.seated()) {
        g_menu.open_panel(&g_browser);
    }
    else {
        g_menu.seat_panel(&g_browser);
    }
}

void multi_menu_do_frame(bool paused)
{
    g_browser.frame();
    g_menu.step();
    if (!paused) {
        const std::optional<int> action = g_menu.input(key_callback);
        g_player.note_text_focus(g_menu.ui().focused_text_id());
        if (action) {
            run_action(static_cast<MultiAction>(*action));
        }
    }
    // The browser let go (Esc, BACK, another panel): back to the Multi menu's state.
    if (browser_state() && !g_browser.is_open() && !g_menu.closing()
        && rf::gameseq_get_pending_state() == rf::GS_NONE) {
        rf::gameseq_pop_state();
    }
    // Before any of the frame's 2D: the preview switches the render target.
    g_player.render_preview();
    g_menu.draw();
}

bool multi_menu_is_closed()
{
    // Into the browser's state, and changes the menu did not start, do not wait for the slide.
    if (rf::gameseq_get_pending_state() == rf::GS_MULTI_SERVER_LIST || !g_menu.closing()) {
        return true;
    }
    return g_menu.is_closed();
}

bool multi_menu_passed_by_jump()
{
    return g_passed_by_jump;
}

void multi_menu_on_close(rf::GameState state, rf::GameState new_state)
{
    if (state == rf::GS_MULTI_SERVER_LIST) {
        g_browser.close();
        if (new_state == rf::GS_MULTI_MENU) {
            return;
        }
    }
    else if (new_state == rf::GS_MULTI_SERVER_LIST) {
        return;
    }
    release();
}
