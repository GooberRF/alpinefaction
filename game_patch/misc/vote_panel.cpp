#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include "vote_panel.h"
#include "vote_form.h"
#include "../hud/multi_scoreboard.h"
#include "../hud/remote_server_cfg_ui.h"
#include "../input/control_input_filter.h"
#include "../multi/demo/demo.h"
#include "../multi/demo/demo_ui.h"
#include "../os/console.h"
#include "../rf/gameseq.h"
#include "../rf/gr/gr.h"
#include "../rf/input.h"
#include "../rf/multi.h"
#include "../rf/player/control_config.h"
#include "../rf/player/player.h"
#include "../rf/ui.h"
#include "../ui/ui_kit.h"
#include "../ui/vote_menu.h"

namespace
{

// Gameplay overlay needs the cursor back and mouse-look off; both are restored
// on every close, including the forced ones (level change, disconnect).
bool g_gameplay_mouse_overridden = false;
bool g_gameplay_prev_mouse_look = false;

// The click that closes the overlay is still pending for the gameplay controls read later that frame, so attacks stay
// blocked until a later frame sees every button released. Frames rather than a flag, so it cannot stick.
int g_swallow_attack_frames = 0;

// Only attacks are affected; movement is never blocked.
void vote_panel_decay_attack_swallow()
{
    if (g_swallow_attack_frames <= 0) {
        return;
    }
    if (rf::mouse_button_is_down(0) || rf::mouse_button_is_down(1)) {
        return; // still held: keep swallowing
    }
    --g_swallow_attack_frames;
}

// Anything but a live multiplayer gameplay frame closes the overlay: a level change or
// limbo entry, multi_stop, becoming a server.
bool overlay_frame_allowed()
{
    return rf::is_multi && !rf::is_server && rf::gameseq_get_state() == rf::GS_GAMEPLAY;
}

void vote_panel_gameplay_input()
{
    if (!vote_menu_is_over_game()) {
        return;
    }
    if (!overlay_frame_allowed()) {
        vote_panel_close();
        return;
    }

    // Re-assert every frame; respawns and camera changes reset mouse mode.
    gameplay_overlay_apply_mouse(true);

    // A stock popup over gameplay (the console's `connect` confirmation) owns the input; this pump runs outside the
    // state frame that gameseq_process holds back for it.
    if (rf::ui::popup_is_active()) {
        return;
    }
    if (!vote_menu_over_game_input()) {
        vote_panel_close();
    }
}

void vote_panel_gameplay_render()
{
    if (!vote_menu_is_over_game()) {
        return;
    }
    // The input pump runs before gameseq_process, so on the frame a level change
    // lands it still saw gameplay; re-check here so the overlay never draws over
    // limbo or a loading screen even once.
    if (!overlay_frame_allowed()) {
        vote_panel_close();
        return;
    }

    vote_menu_over_game_render();
    rf::gr::reset_clip();

    // gameseq_process re-hides the cursor every gameplay frame (mouse_set_visible
    // (false) at 0x004342E6), which undoes what the input pump sets because the
    // pump runs before it. This hook sits after gameseq_process and before the
    // stock cursor render (game_do_frame -> 0x004355F0 -> 0x00435460), so
    // re-asserting here is what actually makes the cursor appear -- and it draws
    // after us, so it lands on top of the menu for free.
    rf::mouse_set_visible(true);
}

// Runs every frame regardless of game state, right before the state machine
// applies this frame's deferred change and runs the current state's frame.
CallHook<rf::GameState()> gameseq_process_hook{
    0x004B2DB1,
    []() -> rf::GameState {
        // Before the state runs its frame, so the overlay sees keys/clicks first.
        vote_panel_decay_attack_swallow();
        vote_panel_gameplay_input();
        demo_controls_ui_input();
        multi_scoreboard_input();
        const rf::GameState state = gameseq_process_hook.call_target();
        // gameseq_process re-hides the cursor in gameplay and limbo.
        if (multi_scoreboard_cursor_active()) {
            rf::mouse_set_visible(true);
        }
        return state;
    },
};

// A focused text box or a dialog of the menu takes every key; everything else passes
// straight through to the game.
FunHook<void(int)> gameplay_key_callback_hook{
    0x004306F0,
    [](int key) {
        if (key == rf::KEY_NONE || !vote_menu_takes_keys()) {
            gameplay_key_callback_hook.call_target(key);
            return;
        }
        vote_menu_key(key);
    },
};

// ESC during gameplay pushes GS_MAIN_MENU; while the overlay is up that is swallowed
// and handed to the menu, which closes a dropdown or itself. A key the menu takes never
// reaches here. The demo controls panel is no modal (it is expanded by default and
// re-expands once gameplay resumes), so it compacts and the Esc menu still opens.
FunHook<void(rf::GameState, bool, bool)> gameseq_push_state_hook{
    0x00434410,
    [](rf::GameState state, bool transparent, bool pause_beneath) {
        if (state == rf::GS_MAIN_MENU && demo_controls_ui_is_open()) {
            demo_controls_ui_close();
        }
        if (state == rf::GS_MAIN_MENU && vote_menu_is_over_game()) {
            vote_menu_over_game_escape();
            return;
        }
        gameseq_push_state_hook.call_target(state, transparent, pause_beneath);
    },
};

// Clicking in the menu must not also fire the weapon.
bool gameplay_overlay_blocks_action(rf::ControlConfig* ccp, rf::ControlConfigAction action)
{
    const bool open = vote_menu_is_over_game();
    if (!open && g_swallow_attack_frames <= 0) {
        return false;
    }
    if (!rf::local_player || ccp != &rf::local_player->settings.controls) {
        return false;
    }
    if (open && vote_menu_takes_keys()) {
        return true;
    }
    return action == rf::CC_ACTION_PRIMARY_ATTACK || action == rf::CC_ACTION_SECONDARY_ATTACK;
}

// control_input_filter owns the hooks and installs them at startup; the overlay
// only contributes its veto, and until it does the filter has nothing to do.
// Registration must happen once, hence the flag: the overlay can be opened any
// number of times.
bool g_control_veto_registered = false;

void ensure_control_veto_registered()
{
    if (g_control_veto_registered) {
        return;
    }
    control_input_filter_add_veto(&gameplay_overlay_blocks_action);
    g_control_veto_registered = true;
}

// The dispatcher recurses into itself (the self-call at 0x004343E4) to draw the
// states stacked under a transparent one, so the hook below re-enters once per
// state in that chain. Counting entries and exits picks out the outermost
// dispatch, which is the one that must render the overlay -- exactly once a frame.
int g_gameseq_dispatch_depth = 0;

struct GameseqDispatchDepth
{
    GameseqDispatchDepth() { ++g_gameseq_dispatch_depth; }
    ~GameseqDispatchDepth() { --g_gameseq_dispatch_depth; }
    GameseqDispatchDepth(const GameseqDispatchDepth&) = delete;
    GameseqDispatchDepth& operator=(const GameseqDispatchDepth&) = delete;

    [[nodiscard]] bool is_outermost() const { return g_gameseq_dispatch_depth == 1; }
};

// gameseq_process renders an active popup itself, at 0x004342CD, i.e. INSIDE the
// call this hook wraps -- so anything drawn from the after-frame hook
// (0x004B2DC2) lands on top of the popup and hides it. Rendering the overlay
// here instead puts it before the popup render, so a popup is drawn over the
// overlay and stays usable.
FunHook<void(int, int)> gameseq_state_do_frame_hook{
    0x004343C0,
    [](int state_index, int no_input) {
        GameseqDispatchDepth depth;
        gameseq_state_do_frame_hook.call_target(state_index, no_input);
        if (depth.is_outermost()) {
            vote_panel_gameplay_render();
            demo_controls_ui_render();
        }
    },
};

} // namespace

// Shared by every gameplay overlay that needs a free cursor. Overlays can overlap, so
// the saved mouse-look is taken only on the first apply and restored on release.
void gameplay_overlay_apply_mouse(bool active)
{
    rf::Player* const player = rf::local_player;

    if (active) {
        if (player && !g_gameplay_mouse_overridden) {
            g_gameplay_prev_mouse_look = player->settings.controls.mouse_look;
            g_gameplay_mouse_overridden = true;
        }
        if (player) {
            player->settings.controls.mouse_look = false;
        }
        if (rf::keep_mouse_centered) {
            rf::mouse_keep_centered_disable();
        }
        rf::mouse_set_visible(true);
        return;
    }

    // mouse_look is a persisted player setting: only drop the override once the
    // saved value is actually back. If the player object is gone (disconnect,
    // kick, level change) keep it pending so the next apply can still restore it
    // rather than losing the user's setting permanently.
    if (g_gameplay_mouse_overridden) {
        if (!player) {
            return;
        }
        player->settings.controls.mouse_look = g_gameplay_prev_mouse_look;
        g_gameplay_mouse_overridden = false;
    }
    // Only re-grab the cursor if we are actually still in gameplay; on a level
    // change the engine owns the mouse mode.
    if (rf::gameseq_get_state() == rf::GS_GAMEPLAY && !rf::keep_mouse_centered) {
        rf::mouse_keep_centered_enable();
        rf::mouse_set_visible(false);
    }
}

bool gameplay_overlay_mouse_is_overridden()
{
    return g_gameplay_mouse_overridden;
}

void vote_panel_close()
{
    if (vote_menu_is_over_game()) {
        vote_menu_close_over_game();
        gameplay_overlay_release_mouse();
        // Rest of this frame plus at least one whole frame, extended while held.
        g_swallow_attack_frames = 2;
    }
}

bool vote_panel_is_gameplay_overlay_active()
{
    return vote_menu_is_over_game();
}

bool gameplay_overlay_cursor_active()
{
    return vote_menu_is_over_game() || demo_controls_ui_is_open() || multi_scoreboard_cursor_active()
        || console_ui_cursor_active();
}

void gameplay_overlay_release_mouse()
{
    if (!gameplay_overlay_cursor_active()) {
        gameplay_overlay_apply_mouse(false);
    }
}

bool vote_panel_is_capturing_text()
{
    return vote_menu_takes_keys();
}

void vote_panel_reset()
{
    // The form this edit was writing into is gone with the server.
    vote_menu_drop_edits();
    vote_form_reset();
}

bool vote_panel_available()
{
    // A demo is not a server to vote on.
    return rf::is_multi && !rf::is_server && !demo_playback_active();
}

void vote_panel_toggle_gameplay()
{
    if (vote_menu_is_over_game()) {
        vote_panel_close();
        ui_play_click();
        return;
    }
    if (!vote_panel_available() || rf::gameseq_get_state() != rf::GS_GAMEPLAY) {
        return;
    }

    // Mutually exclusive with the remote server config overlay and the demo
    // controls popup: all read the same non-consuming mouse state.
    if (g_remote_server_cfg_popup.is_active()) {
        g_remote_server_cfg_popup.toggle();
    }
    demo_controls_ui_close();

    ensure_control_veto_registered();
    vote_menu_open_over_game();
    gameplay_overlay_apply_mouse(true);
}

void vote_panel_apply_patch()
{
    gameseq_process_hook.install();
    gameseq_push_state_hook.install();
    gameseq_state_do_frame_hook.install();
    gameplay_key_callback_hook.install();
}
