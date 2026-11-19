#include <optional>
#include <vector>
#include "multi_pause_menu.h"
#include "column_menu.h"
#include "menu_shell.h"
#include "player_panel.h"
#include "server_browser.h"
#include "ui_kit.h"
#include "vote_menu.h"
#include "../misc/vote_panel.h"
#include "../multi/demo/demo.h"
#include "../rf/input.h"
#include "../rf/localize.h"
#include "../rf/multi.h"

namespace
{

enum class MultiAction
{
    Player,
    JoinGame,
    CallVote,
    Back,
    // The path strip's player.
    OpenPlayer,
};

ColumnMenu g_multi;
bool g_multi_active = false;
PlayerPanel g_player;
ServerBrowserPanel g_browser{g_multi.popup()};

int action_id(MultiAction action)
{
    return static_cast<int>(action);
}

std::vector<ColumnButton> make_multi_buttons()
{
    std::vector<ColumnButton> buttons{
        ColumnButton{action_id(MultiAction::Player), menu_stock_label(rf::strings::player), rf::KEY_P, true},
    };
    // A demo is left through its own menu.
    if (!demo_playback_active()) {
        buttons.push_back(
            ColumnButton{action_id(MultiAction::JoinGame), menu_stock_label(rf::strings::join), rf::KEY_J, true});
    }
    // Hidden where F4 is unavailable: on a listen host or in a demo.
    if (vote_panel_available()) {
        buttons.push_back(ColumnButton{action_id(MultiAction::CallVote), "CALL VOTE", rf::KEY_V});
    }
    return buttons;
}

void run_multi_action(MultiAction action)
{
    switch (action) {
        case MultiAction::Player:
            g_multi.toggle_panel(&g_player);
            break;
        case MultiAction::OpenPlayer:
            g_multi.open_panel(&g_player);
            break;
        case MultiAction::JoinGame:
            g_multi.toggle_panel(&g_browser);
            break;
        case MultiAction::CallVote:
            g_multi.hand_off({vote_menu_open, vote_menu_is_active, vote_menu_do_frame});
            break;
        case MultiAction::Back:
            g_multi.request_close();
            break;
    }
    if (!g_multi.jump_action()) {
        ui_play_click();
    }
}

void __cdecl multi_key_callback(int key)
{
    g_multi.key(key);
}

} // namespace

void multi_pause_menu_open()
{
    g_multi_active = true;
    g_player.reset();
    g_browser.close();
    const int back = action_id(MultiAction::Back);
    g_multi.enter(menu_stock_label(rf::strings::multiplayer), make_multi_buttons(),
                  ColumnButton{back, menu_stock_label(rf::strings::back), rf::KEY_B}, back);
    g_multi.add_panel(&g_player, action_id(MultiAction::Player));
    g_multi.add_panel(&g_browser, action_id(MultiAction::JoinGame));
    g_multi.set_player_action(action_id(MultiAction::OpenPlayer));
    if (player_panel_take_seat_request()) {
        g_multi.seat_panel(&g_player);
    }
}

bool multi_pause_menu_is_active()
{
    return g_multi_active;
}

void multi_pause_menu_do_frame(bool paused)
{
    if (g_multi.host_frame(paused)) {
        // One turning preview at a time.
        g_player.release_preview();
        return;
    }
    g_browser.frame();

    // paused = 1 means a stock popup owns input this frame; it renders after us and on top.
    if (!paused) {
        const std::optional<int> action = g_multi.input(multi_key_callback);
        // A server switch has left the game: nothing of it is drawn again.
        if (!rf::is_multi) {
            return;
        }
        g_player.note_text_focus(g_multi.ui().focused_text_id());
        if (action) {
            run_multi_action(static_cast<MultiAction>(*action));
        }
    }

    g_player.render_preview();
    g_multi.draw();
    if (g_multi.is_closed()) {
        multi_pause_menu_release();
    }
}

void multi_pause_menu_release()
{
    g_multi_active = false;
    // A forced close drops a half-typed name.
    g_multi.ui().release_text_focus(false);
    g_player.release_preview();
    g_browser.close();
    vote_menu_release();
}
