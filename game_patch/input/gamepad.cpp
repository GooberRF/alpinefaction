#include "gamepad.h"
#include "gyro.h"
#include "input.h"
#include "glyph.h"
#include "../hud/multi_spectate.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <xlog/xlog.h>
#include "../os/console.h"
#include "../rf/input.h"
#include "../rf/ui.h"
#include "../rf/player/player.h"
#include "../rf/player/camera.h"
#include "../rf/player/control_config.h"
#include "../rf/player/player_fpgun.h"
#include "../rf/vmesh.h"
#include "../rf/entity.h"
#include "../rf/os/frametime.h"
#include "../rf/gameseq.h"
#include "../misc/alpine_settings.h"
#include "../misc/misc.h"
#include "../main/main.h"
#include <common/utils/os-utils.h>
#include <SDL3/SDL.h>
#include "../rf/os/os.h"

static SDL_Gamepad* g_gamepad = nullptr;
static bool g_motion_sensors_active = false; // true only when the connected gamepad has gyro+accel and is enabled

// Latest sensor samples — latched from SDL events, consumed once per frame.
static float g_gyro_x = 0.0f, g_gyro_y = 0.0f, g_gyro_z = 0.0f;   // deg/s
static float g_accel_x = 0.0f, g_accel_y = 0.0f, g_accel_z = 0.0f; // g-force
static bool g_gyro_fresh = false;

// Button → action mapping: indexed by SDL_GamepadButton, value is rf::ControlConfigAction, -1 = unbound.
static int g_button_map[SDL_GAMEPAD_BUTTON_COUNT];

// Trigger action map: [0] = LT action index, [1] = RT action index, -1 = unbound.
static int g_trigger_action[2] = {rf::CC_ACTION_CROUCH, rf::CC_ACTION_SECONDARY_ATTACK};

static bool g_lt_was_down = false;
static bool g_rt_was_down = false;

// Per-frame action state, indexed by rf::ControlConfigAction. 128 = hard limit.
static constexpr int k_action_count = 128;
static bool g_action_prev[k_action_count] = {};
static bool g_action_curr[k_action_count] = {};

// Gamepad scan code captured during a rebind, or -1 if none pending.
static int g_rebind_pending_sc = -1;

// True when the last meaningful input (button/key press, not mouse movement) came from the gamepad.
static bool g_last_input_was_gamepad = false;
static float g_message_log_close_cooldown = 0.0f; // seconds

// All menu-navigation state in one place.
struct MenuNavState {
    int   deferred_btn_down  = -1;   // SDL button queued from poll for button-down
    int   deferred_btn_up    = -1;   // SDL button queued from poll for button-up
    int   repeat_btn         = -1;   // D-pad button held for auto-repeat, -1 = none
    float repeat_timer       = 0.0f; // seconds until next repeat tick
    float scroll_timer       = 0.0f; // cooldown between right-stick scroll ticks
    bool  lclick_held        = false; // WM_LBUTTONDOWN sent, WM_LBUTTONUP awaiting release
    bool  last_nav_was_dpad  = true;  // true = D-pad drove focus; false = left stick moved cursor
};
static MenuNavState g_menu_nav;

static float g_move_lx = 0.0f, g_move_ly = 0.0f;
static float g_move_mag = 0.0f;

static bool g_flickstick_has_aim = false;
static float g_flickstick_target_yaw = 0.0f;
static float g_flickstick_target_pitch = 0.0f;
static float g_flickstick_prev_stick_angle = 0.0f;
static bool g_flickstick_prev_stick_valid = false;
static float g_flickstick_yaw_delta_filtered = 0.0f;

static bool is_gamepad_input_active()
{
    // Use RF's own window focus flag 
    return g_gamepad && rf::is_main_wnd_active;
}

static bool is_freelook_camera()
{
    return rf::local_player && rf::local_player->cam && rf::local_player->cam->mode == rf::CameraMode::CAMERA_FREELOOK;
}

static bool is_gamepad_menu_state()
{
    if (!rf::gameseq_in_gameplay()) return true;
    if (!rf::keep_mouse_centered) return true;
    if (is_freelook_camera()) return false;
    return !rf::local_player_entity || rf::entity_is_dying(rf::local_player_entity);
}

static void reset_gamepad_input_state()
{
    // Gyro/accel state
    g_gyro_x = g_gyro_y = g_gyro_z = 0.0f;
    g_accel_x = g_accel_y = g_accel_z = 0.0f;
    g_gyro_fresh = false;

    // Action/button state
    memset(g_action_curr, 0, sizeof(g_action_curr));

    // Movement state
    g_move_lx = g_move_ly = 0.0f;
    g_move_mag = 0.0f;

    // Menu navigation state
    g_menu_nav = {};

    // Rebind state
    g_rebind_pending_sc = -1;

    // Flick stick state
    g_flickstick_has_aim = false;
    g_flickstick_target_yaw = 0.0f;
    g_flickstick_target_pitch = 0.0f;
    g_flickstick_prev_stick_valid = false;
    g_flickstick_yaw_delta_filtered = 0.0f;

    // Misc input tracking
    g_lt_was_down = false;
    g_rt_was_down = false;
    g_last_input_was_gamepad = false;
}

// Normalize an axis value, strip the deadzone band, and rescale the remainder to [-1, 1].
static float get_axis(SDL_GamepadAxis axis, float deadzone)
{
    if (!g_gamepad) return 0.0f;
    float v = SDL_GetGamepadAxis(g_gamepad, axis) / (float)SDL_MAX_SINT16;
    if (v >  deadzone) return (v - deadzone) / (1.0f - deadzone);
    if (v < -deadzone) return (v + deadzone) / (1.0f - deadzone);
    return 0.0f;
}

static float wrap_angle_pi(float a)
{
    while (a > 3.14159265f) a -= 2.0f * 3.14159265f;
    while (a <= -3.14159265f) a += 2.0f * 3.14159265f;
    return a;
}

static float angle_diff(float target, float current)
{
    return wrap_angle_pi(target - current);
}

static void gamepad_apply_flickstick(float rx, float ry, float current_yaw, float current_pitch,
                                    float& yaw_delta, float& pitch_delta);

static rf::VMesh* g_local_player_body_vmesh = nullptr;
static bool g_scaling_fpgun_vmesh = false;

static bool action_is_down(rf::ControlConfigAction action)
{
    int i = static_cast<int>(action);
    return i >= 0 && i < k_action_count && g_action_curr[i];
}

static void try_open_gamepad(SDL_JoystickID id)
{
    g_gamepad = SDL_OpenGamepad(id);
    if (!g_gamepad) {
        xlog::warn("Failed to open gamepad: {}", SDL_GetError());
        return;
    }

    xlog::info("Gamepad connected: {}", SDL_GetGamepadName(g_gamepad));

    if (!SDL_GamepadHasSensor(g_gamepad, SDL_SENSOR_GYRO) ||
        !SDL_GamepadHasSensor(g_gamepad, SDL_SENSOR_ACCEL)) {
        xlog::info("Gamepad does not support motion sensor");
        return;
    }

    if (!SDL_SetGamepadSensorEnabled(g_gamepad, SDL_SENSOR_GYRO,  true) ||
        !SDL_SetGamepadSensorEnabled(g_gamepad, SDL_SENSOR_ACCEL, true)) {
        xlog::warn("Failed to enable motion sensors: {}", SDL_GetError());
        return;
    }

    xlog::info("Motion sensors enabled");
    g_motion_sensors_active = true;
    gyro_reset();
}

// Injects the keyboard scan code bound to `action` — gameplay only, never into menus.
static void inject_action_key(int action, bool down)
{
    if (!rf::gameseq_in_gameplay()) return;
    if (!rf::local_player || action < 0 || action >= rf::local_player->settings.controls.num_bindings)
        return;
    int16_t sc = rf::local_player->settings.controls.bindings[action].scan_codes[0];
    if (sc > 0)
        rf::key_process_event(sc, down ? 1 : 0, 0);
}

// Injects a raw key scan code down+up pair directly into the RF key queue.
static void inject_menu_key(int key)
{
    rf::key_process_event(key, 1, 0);
    rf::key_process_event(key, 0, 0);
}

// Moves the OS cursor by (dx, dy) screen pixels, clamped to the RF window client area,
// and delivers a synchronous WM_MOUSEMOVE so RF's UI updates hover state this frame.
static void menu_nav_move_cursor(int dx, int dy)
{
    POINT pt;
    GetCursorPos(&pt);

    RECT rc;
    GetClientRect(rf::main_wnd, &rc);
    POINT tl{rc.left, rc.top}, br{rc.right - 1, rc.bottom - 1};
    ClientToScreen(rf::main_wnd, &tl);
    ClientToScreen(rf::main_wnd, &br);
    pt.x = std::clamp(pt.x + dx, tl.x, br.x);
    pt.y = std::clamp(pt.y + dy, tl.y, br.y);
    SetCursorPos(pt.x, pt.y);

    POINT client = pt;
    ScreenToClient(rf::main_wnd, &client);
    SendMessage(rf::main_wnd, WM_MOUSEMOVE, 0, MAKELPARAM(client.x, client.y));
}

// Maps a D-pad button to the RF keyboard nav scan code it should inject.
static int dpad_btn_to_navkey(int btn)
{
    switch (btn) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP:    return rf::KEY_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:  return rf::KEY_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:  return rf::KEY_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return rf::KEY_RIGHT;
    default: return 0;
    }
}

// Syncs g_action_curr for any binding whose scan_codes contain `sc`,
static void sync_extra_actions_for_scancode(int16_t sc, bool down, int primary_action)
{
    if (!rf::local_player) return;
    auto& cc = rf::local_player->settings.controls;
    for (int i = 0; i < cc.num_bindings && i < k_action_count; ++i) {
        if (i == primary_action) continue;
        if (cc.bindings[i].scan_codes[0] == sc || cc.bindings[i].scan_codes[1] == sc)
            g_action_curr[i] = down;
    }
}

static void update_trigger_actions()
{
    float rt = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / (float)SDL_MAX_SINT16;
    float lt = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)  / (float)SDL_MAX_SINT16;
    bool lt_down = lt > 0.5f;
    bool rt_down = rt > 0.5f;

    if (lt_down != g_lt_was_down) {
        inject_action_key(g_trigger_action[0], lt_down);
        if (g_trigger_action[0] >= 0 && g_trigger_action[0] < k_action_count)
            g_action_curr[g_trigger_action[0]] = lt_down;
        sync_extra_actions_for_scancode(static_cast<int16_t>(CTRL_GAMEPAD_LEFT_TRGGER), lt_down, g_trigger_action[0]);
    }
    if (rt_down != g_rt_was_down) {
        inject_action_key(g_trigger_action[1], rt_down);
        if (g_trigger_action[1] >= 0 && g_trigger_action[1] < k_action_count)
            g_action_curr[g_trigger_action[1]] = rt_down;
        sync_extra_actions_for_scancode(static_cast<int16_t>(CTRL_GAMEPAD_RIGHT_TRGGER), rt_down, g_trigger_action[1]);
    }

    g_lt_was_down = lt_down;
    g_rt_was_down = rt_down;
}

// Returns true if a gamepad button or trigger currently held maps to `action_idx`.
static bool is_action_held_by_button(int action_idx)
{
    if (!g_gamepad) return false;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
        if (g_button_map[b] == action_idx && SDL_GetGamepadButton(g_gamepad, static_cast<SDL_GamepadButton>(b)))
            return true;
    if (g_trigger_action[0] == action_idx && g_lt_was_down) return true;
    if (g_trigger_action[1] == action_idx && g_rt_was_down) return true;
    return false;
}

static void set_movement_key(rf::ControlConfigAction action, bool down)
{
    int idx = static_cast<int>(action);
    // A digital button binding takes priority: don't release the key while a button holds it.
    // Only applies during gameplay — outside of it no scan codes should be injected at all.
    bool in_gameplay = rf::gameseq_in_gameplay();
    if (in_gameplay)
        down = down || is_action_held_by_button(idx);
    if (g_action_curr[idx] == down) return;
    if (in_gameplay && rf::local_player && !rf::console::console_is_visible()) {
        int16_t sc = rf::local_player->settings.controls.bindings[idx].scan_codes[0];
        if (sc >= 0)
            rf::key_process_event(sc, down ? 1 : 0, 0);
    }
    g_action_curr[idx] = down;
}

static void release_movement_keys()
{
    g_move_lx = g_move_ly = 0.0f;
    g_move_mag = 0.0f;
    set_movement_key(rf::CC_ACTION_FORWARD,     false);
    set_movement_key(rf::CC_ACTION_BACKWARD,    false);
    set_movement_key(rf::CC_ACTION_SLIDE_LEFT,  false);
    set_movement_key(rf::CC_ACTION_SLIDE_RIGHT, false);
}

static void update_stick_movement()
{
    if (!rf::local_player)
        return;

    if (!rf::gameseq_in_gameplay() || is_gamepad_menu_state()) {
        if (!is_freelook_camera()) {
            release_movement_keys();
            return;
        }
    }

    if (rf::local_player_entity && rf::entity_is_dying(rf::local_player_entity)) {
        release_movement_keys();
        reset_gamepad_input_state();
        return;
    }

    SDL_GamepadAxis mov_x = g_alpine_game_config.gamepad_swap_sticks ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX;
    SDL_GamepadAxis mov_y = g_alpine_game_config.gamepad_swap_sticks ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY;
    float mov_dz          = g_alpine_game_config.gamepad_swap_sticks ? g_alpine_game_config.gamepad_look_deadzone
                                                                      : g_alpine_game_config.gamepad_move_deadzone;
    float lx = get_axis(mov_x, mov_dz);
    float ly = get_axis(mov_y, mov_dz);

    g_move_lx = lx;
    g_move_ly = ly;
    g_move_mag = std::min(1.0f, std::sqrt(lx * lx + ly * ly));

    set_movement_key(rf::CC_ACTION_FORWARD,     ly < 0.0f);
    set_movement_key(rf::CC_ACTION_BACKWARD,    ly > 0.0f);
    set_movement_key(rf::CC_ACTION_SLIDE_LEFT,  lx < 0.0f);
    set_movement_key(rf::CC_ACTION_SLIDE_RIGHT, lx > 0.0f);
}

static SDL_GamepadButton get_menu_confirm_button()
{
    if (g_gamepad && SDL_GetGamepadButtonLabel(g_gamepad, SDL_GAMEPAD_BUTTON_EAST) == SDL_GAMEPAD_BUTTON_LABEL_A)
        return SDL_GAMEPAD_BUTTON_EAST;
    return SDL_GAMEPAD_BUTTON_SOUTH;
}

static SDL_GamepadButton get_menu_cancel_button()
{
    if (g_gamepad && SDL_GetGamepadButtonLabel(g_gamepad, SDL_GAMEPAD_BUTTON_SOUTH) == SDL_GAMEPAD_BUTTON_LABEL_B)
        return SDL_GAMEPAD_BUTTON_SOUTH;
    return SDL_GAMEPAD_BUTTON_EAST;
}

// Returns true if `state` is a UI overlay where Cancel should be handled by the gamepad menu
// system (close/escape the state) rather than injecting a raw ESC into gameplay.
static bool is_gamepad_cancellable_menu_state(rf::GameState state)
{
    return state == rf::GS_MESSAGE_LOG
        || state == rf::GS_OPTIONS_MENU
        || state == rf::GS_MULTI_MENU
        || state == rf::GS_HELP
        || state == rf::GS_MULTI_SERVER_LIST
        || state == rf::GS_SAVE_GAME_MENU
        || state == rf::GS_LOAD_GAME_MENU
        || state == rf::GS_MAIN_MENU
        || state == rf::GS_MULTI_LIMBO
        || state == rf::GS_FRAMERATE_TEST_END
        || state == rf::GS_CREDITS
        || state == rf::GS_BOMB_DEFUSE
        || state == rf::GS_INTRO_VIDEO;
}

// Handles a menu button press.
// D-pad injects keyboard navigation keys.
// Left-stick cursor movement is handled in gamepad_do_menu_frame(); confirm there uses WM_LBUTTONDOWN.
// Returns true if the event was consumed and should mark gamepad as last input.
static bool menu_nav_on_button_down(int btn)
{
    const SDL_GamepadButton confirm_btn = get_menu_confirm_button();
    const SDL_GamepadButton cancel_btn  = get_menu_cancel_button();

    if (btn == static_cast<int>(confirm_btn)) {
        if (!rf::ui::options_controls_waiting_for_key) {
            if (g_menu_nav.last_nav_was_dpad) {
                // D-pad drove focus — confirm via keyboard so any screen responds.
                inject_menu_key(rf::KEY_ENTER);
            } else {
                // Left stick moved cursor — click whatever is under it.
                POINT pt;
                GetCursorPos(&pt);
                ScreenToClient(rf::main_wnd, &pt);
                SendMessage(rf::main_wnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(pt.x, pt.y));
                g_menu_nav.lclick_held = true;
            }
        }
        return true;
    }
    if (btn == static_cast<int>(cancel_btn)) {
        // Close message log / menu-related states first and consume input.
        rf::GameState current_state = rf::gameseq_get_state();
        if (is_gamepad_cancellable_menu_state(current_state)) {
            if (current_state == rf::GS_MESSAGE_LOG) {
                rf::gameseq_set_state(rf::GS_GAMEPLAY, false);
                g_message_log_close_cooldown = 0.2f; // lock input for a short interval
            } else {
                // For other menu states, send ESC as usual to close them.
                inject_menu_key(rf::KEY_ESC);
            }
            return true;
        }

        // In spectate modes and after death, also consume Cancel so no menu opens.
        if ((rf::local_player && rf::player_is_dead(rf::local_player)) || multi_spectate_is_spectating()) {
            return true;
        }

        inject_menu_key(rf::KEY_ESC);
        return true;
    }
    // D-pad: inject keyboard nav key and arm auto-repeat.
    switch (btn) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        if (!rf::ui::options_controls_waiting_for_key) {
            inject_menu_key(dpad_btn_to_navkey(btn));
            g_menu_nav.last_nav_was_dpad = true;
            g_menu_nav.repeat_btn        = btn;
            g_menu_nav.repeat_timer      = 0.4f;
        }
        return true;
    default:
        return false;
    }
}

// Clears D-pad auto-repeat and sends WM_LBUTTONUP if a click is being held.
static void menu_nav_on_button_up(int btn)
{
    if (btn == g_menu_nav.repeat_btn)
        g_menu_nav.repeat_btn = -1;
    if (btn == static_cast<int>(get_menu_confirm_button()) && g_menu_nav.lclick_held) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(rf::main_wnd, &pt);
        SendMessage(rf::main_wnd, WM_LBUTTONUP, 0, MAKELPARAM(pt.x, pt.y));
        g_menu_nav.lclick_held = false;
    }
}

void gamepad_sdl_poll()
{
    memcpy(g_action_prev, g_action_curr, sizeof(g_action_curr));
    SDL_UpdateGamepads();
    // Flush SDL Gamepad events outside the gamepad range to prevent queue buildup.
    // Keyboard and mouse input is handled via Win32 message dispatch and doesn't need flushing.
    SDL_FlushEvents(SDL_EVENT_FIRST,
        static_cast<SDL_EventType>(static_cast<Uint32>(SDL_EVENT_GAMEPAD_AXIS_MOTION) - 1u));
    SDL_FlushEvents(
        static_cast<SDL_EventType>(static_cast<Uint32>(SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED) + 1u),
        SDL_EVENT_LAST);
    SDL_Event events[16];
    int n;
    while ((n = SDL_PeepEvents(events, static_cast<int>(std::size(events)),
                               SDL_GETEVENT, SDL_EVENT_GAMEPAD_AXIS_MOTION,
                               SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED)) > 0) {
        for (int i = 0; i < n; ++i) {
            const SDL_Event& ev = events[i];
            switch (ev.type) {
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!g_gamepad)
                    try_open_gamepad(ev.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (g_gamepad && SDL_GetGamepadID(g_gamepad) == ev.gdevice.which) {
                    xlog::info("Gamepad disconnected");
                    release_movement_keys();
                    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
                        inject_action_key(g_button_map[b], false);
                    inject_action_key(g_trigger_action[0], false);
                    inject_action_key(g_trigger_action[1], false);
                    if (g_menu_nav.lclick_held) {
                        POINT pt;
                        GetCursorPos(&pt);
                        ScreenToClient(rf::main_wnd, &pt);
                        SendMessage(rf::main_wnd, WM_LBUTTONUP, 0, MAKELPARAM(pt.x, pt.y));
                        g_menu_nav.lclick_held = false;
                    }
                    SDL_CloseGamepad(g_gamepad);
                    g_gamepad               = nullptr;
                    g_motion_sensors_active = false;
                    reset_gamepad_input_state();
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (g_message_log_close_cooldown > 0.0f) break;
                if (!is_gamepad_input_active() || SDL_GetGamepadID(g_gamepad) != ev.gbutton.which) break;
                g_last_input_was_gamepad = true;
                if (ui_ctrl_bindings_view_active() && rf::ui::options_controls_waiting_for_key) {
                    if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_START) {
                        // Start is reserved as "cancel rebind", same role as keyboard ESC.
                        // TODO: replace baked-in ESC with menu game action
                        rf::key_process_event(rf::KEY_ESC, 1, 0);
                        rf::key_process_event(rf::KEY_ESC, 0, 0);
                    } else {
                        g_rebind_pending_sc = CTRL_GAMEPAD_SCAN_BASE + ev.gbutton.button;
                        rf::key_process_event(static_cast<int>(CTRL_REBIND_SENTINEL), 1, 0);
                    }
                    break; // skip normal gameplay dispatch
                }
                if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_START) {
                    rf::key_process_event(rf::KEY_ESC, 1, 0);
                    rf::key_process_event(rf::KEY_ESC, 0, 0);
                }
                // Always queue for menu nav; gameplay dispatch runs below (inject_action_key
                // has its own gameseq_in_gameplay() guard so it is a no-op outside gameplay).
                g_menu_nav.deferred_btn_down = ev.gbutton.button;
                if (ev.gbutton.button < SDL_GAMEPAD_BUTTON_COUNT) {
                    int mapped = g_button_map[ev.gbutton.button];
                    if (mapped >= 0) {
                        inject_action_key(mapped, true);
                        g_action_curr[mapped] = true;
                    }
                    int16_t gp_sc = static_cast<int16_t>(CTRL_GAMEPAD_SCAN_BASE + ev.gbutton.button);
                    sync_extra_actions_for_scancode(gp_sc, true, mapped);
                }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                if (!is_gamepad_input_active() || SDL_GetGamepadID(g_gamepad) != ev.gbutton.which) break;
                // Always queue for menu nav (button-up clears repeat and releases held click).
                g_menu_nav.deferred_btn_up = ev.gbutton.button;
                if (ev.gbutton.button < SDL_GAMEPAD_BUTTON_COUNT) {
                    int mapped = g_button_map[ev.gbutton.button];
                    if (mapped >= 0) {
                        inject_action_key(mapped, false);
                        g_action_curr[mapped] = false;
                    }
                    int16_t gp_sc = static_cast<int16_t>(CTRL_GAMEPAD_SCAN_BASE + ev.gbutton.button);
                    sync_extra_actions_for_scancode(gp_sc, false, mapped);
                }
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
                if (g_message_log_close_cooldown > 0.0f) break;
                if (!is_gamepad_input_active() || SDL_GetGamepadID(g_gamepad) != ev.gaxis.which) break;
                float v = ev.gaxis.value / static_cast<float>(SDL_MAX_SINT16);
                float deadzone = 0.0f;
                switch (static_cast<SDL_GamepadAxis>(ev.gaxis.axis)) {
                case SDL_GAMEPAD_AXIS_LEFTX:
                case SDL_GAMEPAD_AXIS_LEFTY:
                    deadzone = g_alpine_game_config.gamepad_move_deadzone;
                    break;
                case SDL_GAMEPAD_AXIS_RIGHTX:
                case SDL_GAMEPAD_AXIS_RIGHTY:
                    deadzone = g_alpine_game_config.gamepad_look_deadzone;
                    break;
                case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
                case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
                    deadzone = 0.5f;
                    break;
                default:
                    break;
                }
                if (std::abs(v) > deadzone)
                    g_last_input_was_gamepad = true;
                break;
            }
            case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
                if (!g_motion_sensors_active || !is_gamepad_input_active() || SDL_GetGamepadID(g_gamepad) != ev.gsensor.which) break; // discard gyro events while app is not focused
                if (ev.gsensor.sensor == SDL_SENSOR_GYRO) {
                    constexpr float rad2deg = 180.0f / 3.14159265f;
                    g_gyro_x = ev.gsensor.data[0] * rad2deg;
                    g_gyro_y = ev.gsensor.data[1] * rad2deg;
                    g_gyro_z = ev.gsensor.data[2] * rad2deg;
                    g_gyro_fresh = true;
                } else if (ev.gsensor.sensor == SDL_SENSOR_ACCEL) {
                    g_accel_x = ev.gsensor.data[0] / SDL_STANDARD_GRAVITY;
                    g_accel_y = ev.gsensor.data[1] / SDL_STANDARD_GRAVITY;
                    g_accel_z = ev.gsensor.data[2] / SDL_STANDARD_GRAVITY;
                }
                break;
            default:
                break;
            }
        }
    }
}

static void gamepad_do_menu_frame()
{
    if (g_menu_nav.deferred_btn_down != -1) {
        if (menu_nav_on_button_down(g_menu_nav.deferred_btn_down))
            g_last_input_was_gamepad = true;
        g_menu_nav.deferred_btn_down = -1;
    }
    if (g_menu_nav.deferred_btn_up != -1) {
        menu_nav_on_button_up(g_menu_nav.deferred_btn_up);
        g_menu_nav.deferred_btn_up = -1;
    }

    constexpr float k_menu_stick_deadzone = 0.24f;
    constexpr float k_base_speed          = 1000.0f;
    float sx = get_axis(SDL_GAMEPAD_AXIS_LEFTX, k_menu_stick_deadzone);
    float sy = get_axis(SDL_GAMEPAD_AXIS_LEFTY, k_menu_stick_deadzone);
    if (sx != 0.0f || sy != 0.0f) {
        float speed = k_base_speed * (static_cast<float>(rf::gr::screen_height()) / 600.0f);
        int dx = static_cast<int>(sx * speed * rf::frametime);
        int dy = static_cast<int>(sy * speed * rf::frametime);
        if (dx != 0 || dy != 0) {
            menu_nav_move_cursor(dx, dy);
            g_menu_nav.last_nav_was_dpad = false;
            g_last_input_was_gamepad = true;
        }
    }

    if (g_menu_nav.repeat_btn >= 0 && !rf::ui::options_controls_waiting_for_key) {
        g_menu_nav.repeat_timer -= rf::frametime;
        if (g_menu_nav.repeat_timer <= 0.0f) {
            inject_menu_key(dpad_btn_to_navkey(g_menu_nav.repeat_btn));
            g_menu_nav.repeat_timer = 0.12f;
        }
    }

    constexpr float k_scroll_deadzone = 0.24f;
    float ry = get_axis(SDL_GAMEPAD_AXIS_RIGHTY, k_scroll_deadzone);
    if (ry != 0.0f) {
        g_menu_nav.scroll_timer -= rf::frametime;
        if (g_menu_nav.scroll_timer <= 0.0f) {
            rf::mouse_dz = (ry < 0.0f) ? 1 : -1;
            g_menu_nav.scroll_timer = 0.12f;
        }
    } else {
        g_menu_nav.scroll_timer = 0.0f;
    }
}

void gamepad_do_frame()
{
    gamepad_sdl_poll();

    if (g_message_log_close_cooldown > 0.0f) {
        g_message_log_close_cooldown -= rf::frametime;
        if (g_message_log_close_cooldown < 0.0f)
            g_message_log_close_cooldown = 0.0f;
        return;
    }

    gyro_update_calibration_mode();

    if (!is_gamepad_input_active())
        return;

    if (ui_ctrl_bindings_view_active() && rf::ui::options_controls_waiting_for_key) {
        float lt = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)  / static_cast<float>(SDL_MAX_SINT16);
        float rt = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / static_cast<float>(SDL_MAX_SINT16);
        if (lt > 0.5f && !g_lt_was_down) {
            g_rebind_pending_sc = CTRL_GAMEPAD_LEFT_TRGGER;
            rf::key_process_event(static_cast<int>(CTRL_REBIND_SENTINEL), 1, 0);
        }
        if (rt > 0.5f && !g_rt_was_down) {
            g_rebind_pending_sc = CTRL_GAMEPAD_RIGHT_TRGGER;
            rf::key_process_event(static_cast<int>(CTRL_REBIND_SENTINEL), 1, 0);
        }
    }

    update_trigger_actions();
    update_stick_movement();

    if (is_gamepad_menu_state())
        gamepad_do_menu_frame();

    g_local_player_body_vmesh = rf::local_player ? rf::get_player_entity_parent_vmesh(rf::local_player) : nullptr;

    if (g_motion_sensors_active) {
        gyro_process_motion(g_gyro_x, g_gyro_y, g_gyro_z,
                            g_accel_x, g_accel_y, g_accel_z, rf::frametime);
        g_gyro_fresh = false;
    }
}

static bool is_gamepad_controls_rebind_active()
{
    return ui_ctrl_bindings_view_active() && rf::ui::options_controls_waiting_for_key;
}

static bool is_key_allowed_during_rebind(int scan_code)
{
    if (scan_code == CTRL_REBIND_SENTINEL)
        return true;
    if ((scan_code & rf::KEY_MASK) == rf::KEY_ESC)
        return true;
    return false;
}

FunHook<void(int,int,int)> key_process_event_hook{
    0x0051E6C0,
    [](int scan_code, int key_down, int delta_time) {
        if (is_gamepad_controls_rebind_active() && !is_key_allowed_during_rebind(scan_code))
            return;
        key_process_event_hook.call_target(scan_code, key_down, delta_time);
    }
};

FunHook<int(int)> mouse_was_button_pressed_hook{
    0x0051E5D0,
    [](int btn_idx) -> int {
        if (is_gamepad_controls_rebind_active())
            return 0;
        return mouse_was_button_pressed_hook.call_target(btn_idx);
    }
};

void gamepad_get_camera(float& pitch_delta, float& yaw_delta)
{
    pitch_delta = 0.0f;
    yaw_delta   = 0.0f;

    if (g_message_log_close_cooldown > 0.0f) {
        return;
    }

    const bool has_player_entity = rf::local_player_entity && !rf::entity_is_dying(rf::local_player_entity);
    const bool is_freelook = !has_player_entity && is_freelook_camera();
    if (!is_gamepad_input_active() || !rf::keep_mouse_centered) {
        reset_gamepad_input_state();
        return;
    }
    if (!has_player_entity && !is_freelook) {
        reset_gamepad_input_state();
        return;
    }

    SDL_GamepadAxis cam_x = g_alpine_game_config.gamepad_swap_sticks ? SDL_GAMEPAD_AXIS_LEFTX  : SDL_GAMEPAD_AXIS_RIGHTX;
    SDL_GamepadAxis cam_y = g_alpine_game_config.gamepad_swap_sticks ? SDL_GAMEPAD_AXIS_LEFTY  : SDL_GAMEPAD_AXIS_RIGHTY;
    float cam_dz          = g_alpine_game_config.gamepad_swap_sticks ? g_alpine_game_config.gamepad_move_deadzone
                                                                       : g_alpine_game_config.gamepad_look_deadzone;

    // Raw axes (no deadzone remapping) for flickstick rotation math to avoid quadrant snapping.
    float raw_rx = SDL_GetGamepadAxis(g_gamepad, cam_x) / static_cast<float>(SDL_MAX_SINT16);
    float raw_ry = SDL_GetGamepadAxis(g_gamepad, cam_y) / static_cast<float>(SDL_MAX_SINT16);

    float rx = get_axis(cam_x, cam_dz);
    float ry = get_axis(cam_y, cam_dz);

    float joy_pitch_sign = g_alpine_game_config.gamepad_joy_invert_y ? 1.0f : -1.0f;

    float current_yaw = rf::local_player_entity ? rf::local_player_entity->control_data.phb.y : 0.0f;
    float current_pitch = rf::local_player_entity ? rf::local_player_entity->control_data.eye_phb.x : 0.0f;
    float stick_mag = std::hypot(rx, ry);

    // Flickstick and gyro rely on entity orientation math that is only valid for the
    // local player entity. During spectator/freelook camera mode there is no player
    // entity, so fall back to plain joystick camera for both look and gyro.
    const bool is_spectator_camera = is_freelook;

    if (g_alpine_game_config.gamepad_flickstick && !is_spectator_camera) {
        gamepad_apply_flickstick(raw_rx, raw_ry, current_yaw, current_pitch, yaw_delta, pitch_delta);
    } else {
        g_flickstick_yaw_delta_filtered = 0.0f;
        yaw_delta   =              rf::frametime * g_alpine_game_config.gamepad_joy_sensitivity * rx;
        pitch_delta = joy_pitch_sign * rf::frametime * g_alpine_game_config.gamepad_joy_sensitivity * ry;
    }

    bool allow_gyro = !is_spectator_camera
        && g_motion_sensors_active
        && g_alpine_game_config.gamepad_gyro_enabled
        && g_alpine_game_config.gamepad_gyro_sensitivity > 0.0f
        && gyro_modifier_is_active();

    if (allow_gyro) {
        float gyro_pitch, gyro_yaw;
        gyro_get_axis_orientation(gyro_pitch, gyro_yaw);
        gyro_apply_tightening(gyro_pitch, gyro_yaw);
        gyro_apply_smoothing(gyro_pitch, gyro_yaw);

        constexpr float deg2rad = 3.14159265f / 180.0f;
        float pitch_sign = g_alpine_game_config.gamepad_gyro_invert_y ? -1.0f : 1.0f;
        yaw_delta   -= gyro_yaw   * deg2rad * g_alpine_game_config.gamepad_gyro_sensitivity * rf::frametime;
        pitch_delta += pitch_sign * gyro_pitch * deg2rad * g_alpine_game_config.gamepad_gyro_sensitivity * rf::frametime;
    }
}

// Flick stick is based on GyroWiki documents
// http://gyrowiki.jibbsmart.com/blog:good-gyro-controls-part-2:the-flick-stick
static void gamepad_apply_flickstick(float rx, float ry, float current_yaw, float current_pitch,
                                    float& yaw_delta, float& pitch_delta)
{
    yaw_delta = 0.0f;
    pitch_delta = 0.0f;

    float stick_mag = std::hypot(rx, ry);
    bool start_flick = stick_mag > g_alpine_game_config.gamepad_flickstick_deadzone;
    bool end_flick   = stick_mag <= g_alpine_game_config.gamepad_flickstick_release_deadzone;

    float flick_angle     = std::atan2(rx, -ry);
    float flick_turn_delta = 0.0f;
    float flick_angle_change = 0.0f;

    if (g_flickstick_prev_stick_valid) {
        flick_turn_delta = angle_diff(flick_angle, g_flickstick_prev_stick_angle);
        flick_angle_change = std::abs(flick_turn_delta);
    }
    g_flickstick_prev_stick_angle = flick_angle;
    g_flickstick_prev_stick_valid = true;

    static constexpr float k_flickstick_retrigger_angle = 1.04719755f; // 60 degrees

    if (start_flick && (!g_flickstick_has_aim || flick_angle_change > k_flickstick_retrigger_angle)) {
        // New flick event (either initial or quick direction change).
        g_flickstick_has_aim = true;
        g_flickstick_target_yaw = wrap_angle_pi(current_yaw + flick_angle);
        g_flickstick_target_pitch = current_pitch;
        yaw_delta = angle_diff(g_flickstick_target_yaw, current_yaw);
    } else if (g_flickstick_has_aim && start_flick) {
        // Continue current flick with relative delta movement.
        yaw_delta = flick_turn_delta;
        g_flickstick_target_yaw = wrap_angle_pi(g_flickstick_target_yaw + flick_turn_delta);
    } else if (end_flick) {
        g_flickstick_has_aim = false;
        g_flickstick_prev_stick_valid = false;
    } else {
        yaw_delta = 0.0f;
        pitch_delta = 0.0f;
    }

    // Apply sweep sensitivity multiplier.
    yaw_delta *= g_alpine_game_config.gamepad_flickstick_sweep;

    // Flick-stick smoothing (optional, keep turn smooth while still responsive)
    float k_flickstick_smooth = std::clamp(g_alpine_game_config.gamepad_flickstick_smoothing, 0.0f, 1.0f);
    g_flickstick_yaw_delta_filtered = g_flickstick_yaw_delta_filtered * k_flickstick_smooth
        + yaw_delta * (1.0f - k_flickstick_smooth);
    yaw_delta = g_flickstick_yaw_delta_filtered;
    pitch_delta = 0.0f;
}


FunHook<bool(rf::ControlConfig*, rf::ControlConfigAction)> control_is_control_down_hook{
    0x00430F40,
    [](rf::ControlConfig* ccp, rf::ControlConfigAction action) -> bool {
        return control_is_control_down_hook.call_target(ccp, action) || action_is_down(action);
    },
};

FunHook<bool(rf::ControlConfig*, rf::ControlConfigAction, bool*)> control_config_check_pressed_hook{
    0x0043D4F0,
    [](rf::ControlConfig* ccp, rf::ControlConfigAction action, bool* just_pressed) -> bool {
        bool result = control_config_check_pressed_hook.call_target(ccp, action, just_pressed);
        if (result) return true;

        int idx = static_cast<int>(action);
        if (idx < 0 || idx >= k_action_count || !g_action_curr[idx])
            return false;

        bool is_just_pressed = !g_action_prev[idx];
        if (ccp->bindings[idx].press_mode != 0 || is_just_pressed) {
            if (just_pressed) *just_pressed = is_just_pressed;
            return true;
        }
        return false;
    },
};

// Returns true if `entity` is the vehicle that the local player is currently riding.
static bool is_local_player_vehicle(rf::Entity* entity)
{
    if (!rf::local_player_entity || !rf::entity_in_vehicle(rf::local_player_entity))
        return false;
    rf::Entity* vehicle = rf::entity_from_handle(rf::local_player_entity->host_handle);
    return vehicle == entity;
}

FunHook<void(rf::Entity*)> physics_simulate_entity_hook{
    0x0049F3C0,
    [](rf::Entity* entity) {
        if (entity == rf::local_player_entity && rf::entity_is_dying(entity)) {
            entity->ai.ci.move.x = 0.0f;
            entity->ai.ci.move.z = 0.0f;
        } else if (is_gamepad_input_active() && entity == rf::local_player_entity && g_move_mag > 0.001f) {
            if (rf::is_multi) {
                float inv_mag = 1.0f / g_move_mag;
                entity->ai.ci.move.x = g_move_lx * inv_mag;
                entity->ai.ci.move.z = -g_move_ly * inv_mag;
            } else {
                entity->ai.ci.move.x = g_move_lx;
                entity->ai.ci.move.z = -g_move_ly;
            }
        }

        // Pre-sim: inject gamepad stick + gyro into vehicle rotation input (ci.rot)
        // ci.rot expects values in the range of keyboard input (±1.0).
        // The vehicle's own turn rate and frametime scaling apply on top.
        if (is_gamepad_input_active() && is_local_player_vehicle(entity)) {
            // Stick: raw axis (-1 to 1) maps directly to keyboard-like input
            SDL_GamepadAxis rot_x = g_alpine_game_config.gamepad_swap_sticks ? SDL_GAMEPAD_AXIS_LEFTX  : SDL_GAMEPAD_AXIS_RIGHTX;
            SDL_GamepadAxis rot_y = g_alpine_game_config.gamepad_swap_sticks ? SDL_GAMEPAD_AXIS_LEFTY  : SDL_GAMEPAD_AXIS_RIGHTY;
            float rot_dz          = g_alpine_game_config.gamepad_swap_sticks ? g_alpine_game_config.gamepad_move_deadzone
                                                                              : g_alpine_game_config.gamepad_look_deadzone;
            float rx = get_axis(rot_x, rot_dz);
            float ry = get_axis(rot_y, rot_dz);
            float joy_pitch_sign = g_alpine_game_config.gamepad_joy_invert_y ? 1.0f : -1.0f;
            entity->ai.ci.rot.y += rx;
            entity->ai.ci.rot.x += joy_pitch_sign * ry;

            // Gyro: convert deg/s to ci.rot range (±1.0) with a fixed scale factor.
            // 1/90 means 90 deg/s of gyro rotation equals full keyboard deflection.
            if (g_motion_sensors_active && g_alpine_game_config.gamepad_gyro_enabled
                && g_alpine_game_config.gamepad_gyro_vehicle_camera
                && g_alpine_game_config.gamepad_gyro_sensitivity > 0.0f
                && gyro_modifier_is_active()) {
                float gyro_pitch, gyro_yaw;
                gyro_get_axis_orientation(gyro_pitch, gyro_yaw);
                gyro_apply_tightening(gyro_pitch, gyro_yaw);
                gyro_apply_smoothing(gyro_pitch, gyro_yaw);

                constexpr float gyro_to_rot = 1.0f / 90.0f;
                float sens = g_alpine_game_config.gamepad_gyro_sensitivity;
                float pitch_sign = g_alpine_game_config.gamepad_gyro_invert_y ? -1.0f : 1.0f;
                entity->ai.ci.rot.y += -gyro_yaw * gyro_to_rot * sens;
                entity->ai.ci.rot.x += pitch_sign * gyro_pitch * gyro_to_rot * sens;
            }
        }

        physics_simulate_entity_hook.call_target(entity);
    },
};

static FunHook<void(rf::Player*)> player_fpgun_process_hook{
    0x004AA6D0,
    [](rf::Player* player) {
        bool scale = player == rf::local_player && !rf::is_multi
            && g_move_mag > 0.001f && g_move_mag < 0.999f;
        if (scale) g_scaling_fpgun_vmesh = true;
        player_fpgun_process_hook.call_target(player);
        if (scale) g_scaling_fpgun_vmesh = false;
    },
};

static FunHook<void(rf::VMesh*, float, int, rf::Vector3*, rf::Matrix3*, int)> vmesh_process_hook{
    0x00503360,
    [](rf::VMesh* vmesh, float frametime, int increment_only, rf::Vector3* pos, rf::Matrix3* orient, int lod_level) {
        bool is_player_body = vmesh == g_local_player_body_vmesh;
        if (!rf::is_multi && g_move_mag > 0.001f && g_move_mag < 0.999f
                && (is_player_body || g_scaling_fpgun_vmesh))
            frametime *= g_move_mag;
        vmesh_process_hook.call_target(vmesh, frametime, increment_only, pos, orient, lod_level);
    },
};

ConsoleCommand2 joy_sens_cmd{
    "joy_sens",
    [](std::optional<float> val) {
        if (val) g_alpine_game_config.gamepad_joy_sensitivity = std::max(0.0f, val.value());
        rf::console::print("Gamepad sensitivity: {:.4f}", g_alpine_game_config.gamepad_joy_sensitivity);
    },
    "Set gamepad look sensitivity (default 5.0)",
    "joy_sens [value]",
};

ConsoleCommand2 joy_move_deadzone_cmd{
    "joy_move_deadzone",
    [](std::optional<float> val) {
        if (val) g_alpine_game_config.gamepad_move_deadzone = std::clamp(val.value(), 0.0f, 0.9f);
        rf::console::print("Gamepad move (left stick) deadzone: {:.2f}", g_alpine_game_config.gamepad_move_deadzone);
    },
    "Set left stick deadzone 0.0-0.9 (default 0.25)",
    "joy_move_deadzone [value]",
};

ConsoleCommand2 joy_look_deadzone_cmd{
    "joy_look_deadzone",
    [](std::optional<float> val) {
        if (val) g_alpine_game_config.gamepad_look_deadzone = std::clamp(val.value(), 0.0f, 0.9f);
        rf::console::print("Gamepad look (right stick) deadzone: {:.2f}", g_alpine_game_config.gamepad_look_deadzone);
    },
    "Set right stick deadzone 0.0-0.9 (default 0.15)",
    "joy_look_deadzone [value]",
};

ConsoleCommand2 joy_flickstick_cmd{
    "joy_flickstick",
    [](std::optional<int> val) {
        if (val) g_alpine_game_config.gamepad_flickstick = val.value() != 0;
        rf::console::print("Joy flick-stick: {}", g_alpine_game_config.gamepad_flickstick ? "enabled" : "disabled");
    },
    "Enable/disable flick-stick mode (default 0)",
    "joy_flickstick [0|1]",
};

ConsoleCommand2 joy_flickstick_sweep_cmd{
    "joy_flickstick_sweep",
    [](std::optional<float> val) {
        if (val) g_alpine_game_config.gamepad_flickstick_sweep = std::clamp(val.value(), 0.01f, 6.0f);
        rf::console::print("Gamepad flickstick sweep: {:.2f}", g_alpine_game_config.gamepad_flickstick_sweep);
    },
    "Set flick-stick sweep sensitivity 0.01-6.0 (default 1.00)",
    "joy_flickstick_sweep [value]",
};

ConsoleCommand2 joy_flickstick_smoothing_cmd{
    "joy_flickstick_smoothing",
    [](std::optional<float> val) {
        if (val) g_alpine_game_config.gamepad_flickstick_smoothing = std::clamp(val.value(), 0.0f, 1.0f);
        rf::console::print("Gamepad flickstick smoothing: {:.2f}", g_alpine_game_config.gamepad_flickstick_smoothing);
    },
    "Set flick-stick smoothing factor 0.0-1.0 (default 0.75)",
    "joy_flickstick_smoothing [value]",
};

ConsoleCommand2 joy_flickstick_deadzone_cmd{
    "joy_flickstick_deadzone",
    [](std::optional<float> val) {
        if (val) g_alpine_game_config.gamepad_flickstick_deadzone = std::clamp(val.value(), 0.0f, 0.9f);
        rf::console::print("Gamepad flickstick deadzone: {:.2f}", g_alpine_game_config.gamepad_flickstick_deadzone);
    },
    "Set flick-stick activation deadzone 0.0-0.9 (default 0.80)",
    "joy_flickstick_deadzone [value]",
};

ConsoleCommand2 joy_flickstick_release_deadzone_cmd{
    "joy_flickstick_release_deadzone",
    [](std::optional<float> val) {
        if (val) g_alpine_game_config.gamepad_flickstick_release_deadzone = std::clamp(val.value(), 0.0f, 0.9f);
        rf::console::print("Gamepad flickstick release deadzone: {:.2f}", g_alpine_game_config.gamepad_flickstick_release_deadzone);
    },
    "Set flick-stick release deadzone 0.0-0.9 (default 0.70)",
    "joy_flickstick_release_deadzone [value]",
};

ConsoleCommand2 gyro_camera_cmd{
    "gyro_camera",
    [](std::optional<int> val) {
        if (val) g_alpine_game_config.gamepad_gyro_enabled = val.value() != 0;
        rf::console::print("Gyro camera: {}", g_alpine_game_config.gamepad_gyro_enabled ? "enabled" : "disabled");
    },
    "Enable/disable gyro camera (default 0)",
    "gyro_camera [0|1]",
};

ConsoleCommand2 gyro_vehicle_camera_cmd{
    "gyro_vehicle_camera",
    [](std::optional<int> val) {
        if (val) g_alpine_game_config.gamepad_gyro_vehicle_camera = val.value() != 0;
        rf::console::print("Gyro camera for vehicles: {}", g_alpine_game_config.gamepad_gyro_vehicle_camera ? "enabled" : "disabled");
    },
    "Enable/disable gyro camera while in vehicles (default 0)",
    "gyro_vehicle_camera [0|1]",
};

ConsoleCommand2 gyro_sens_cmd{
    "gyro_sens",
    [](std::optional<float> val) {
        if (val) g_alpine_game_config.gamepad_gyro_sensitivity = std::clamp(val.value(), 0.0f, 30.0f);
        rf::console::print("Gyro sensitivity: {:.4f}", g_alpine_game_config.gamepad_gyro_sensitivity);
    },
    "Set gyro sensitivity 0-30 (default 2.5)",
    "gyro_sens [value]",
};

ConsoleCommand2 input_prompts_cmd{
    "input_prompts",
    [](std::optional<int> val) {
        if (val) {
            g_alpine_game_config.input_prompt_override = std::clamp(*val, 0, 2);
        }
        static const char* modes[] = {"Auto", "Controller", "Keyboard/Mouse"};
        rf::console::print("Input prompts: {} ({})", modes[g_alpine_game_config.input_prompt_override], g_alpine_game_config.input_prompt_override);
    },
    "Set input prompt display: 0=Auto, 1=Controller, 2=Keyboard/Mouse",
    "input_prompts [0|1|2]",
};

ConsoleCommand2 gamepad_prompts_cmd{
    "gamepad_prompts",
    [](std::optional<int> val) {
        static const char* icon_names[] = {
            "Auto", "Generic", "Xbox 360 Controller", "Xbox Wireless Controller",
            "DualShock 3", "DualShock 4", "DualSense", "Nintendo Switch Controller", "Nintendo GameCube Controller",
            "Steam Controller (2015)", "Steam Deck",
        };
        if (val) g_alpine_game_config.gamepad_icon_override = std::clamp(val.value(), 0, 10);
        rf::console::print("Gamepad icons: {} ({})",
            icon_names[g_alpine_game_config.gamepad_icon_override],
            g_alpine_game_config.gamepad_icon_override);
    },
    "Set gamepad button icon style: 0=Auto, 1=Generic, 2=Xbox 360 Controller, 3=Xbox Wireless Controller, 4=DualShock 3, 5=DualShock 4, 6=DualSense, 7=Nintendo Switch Controller, 8=Nintendo GameCube Controller, 9=Steam Controller (2015), 10=Steam Deck",
    "gamepad_prompts [0-10]",
};

bool gamepad_is_motionsensors_supported()
{
    return g_motion_sensors_active;
}

bool gamepad_is_last_input_gamepad()
{
    if (g_alpine_game_config.input_prompt_override == 1) return true;
    if (g_alpine_game_config.input_prompt_override == 2) return false;
    return g_last_input_was_gamepad;
}

void gamepad_set_last_input_keyboard()
{
    g_last_input_was_gamepad = false;
}

int gamepad_get_button_for_action(int action_idx)
{
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
        if (g_button_map[b] == action_idx)
            return b;
    return -1;
}

int gamepad_get_trigger_for_action(int action_idx)
{
    if (g_trigger_action[0] == action_idx) return 0;
    if (g_trigger_action[1] == action_idx) return 1;
    return -1;
}

int gamepad_get_button_count()
{
    return SDL_GAMEPAD_BUTTON_COUNT;
}

const char* gamepad_get_scan_code_name(int scan_code)
{
    int offset = scan_code - CTRL_GAMEPAD_SCAN_BASE;
    if (offset >= 0 && offset < SDL_GAMEPAD_BUTTON_COUNT + 2) {
        auto icon_pref = static_cast<ControllerIconType>(g_alpine_game_config.gamepad_icon_override);
        return gamepad_get_effective_display_name(icon_pref, g_gamepad, offset);
    }
    return "<none>";
}

void gamepad_clear_all_bindings()
{
    memset(g_button_map, -1, sizeof(g_button_map));
    g_trigger_action[0] = g_trigger_action[1] = -1;
}

void gamepad_sync_bindings_from_scan_codes()
{
    if (!rf::local_player) return;
    gamepad_clear_all_bindings();
    auto& cc = rf::local_player->settings.controls;
    for (int i = 0; i < cc.num_bindings; ++i) {
        int16_t sc = cc.bindings[i].scan_codes[0];
        int offset = static_cast<int>(sc) - CTRL_GAMEPAD_SCAN_BASE;
        if (offset >= 0 && offset < SDL_GAMEPAD_BUTTON_COUNT) {
            if (offset != SDL_GAMEPAD_BUTTON_START) // Start is reserved, never rebindable
                g_button_map[offset] = i;
        }
        else if (sc == static_cast<int16_t>(CTRL_GAMEPAD_LEFT_TRGGER))
            g_trigger_action[0] = i;
        else if (sc == static_cast<int16_t>(CTRL_GAMEPAD_RIGHT_TRGGER))
            g_trigger_action[1] = i;
    }
}

bool gamepad_has_pending_rebind()
{
    return g_rebind_pending_sc >= 0;
}

void gamepad_apply_rebind()
{
    rf::key_process_event(static_cast<int>(CTRL_REBIND_SENTINEL), 0, 0);

    if (!rf::local_player) {
        g_rebind_pending_sc = -1;
        return;
    }

    auto& cc = rf::local_player->settings.controls;

    auto new_code = g_rebind_pending_sc >= 0 ? static_cast<int16_t>(g_rebind_pending_sc) : int16_t{0};
    g_rebind_pending_sc = -1;

    for (int i = 0; i < cc.num_bindings; ++i) {
        if (cc.bindings[i].scan_codes[0] != CTRL_REBIND_SENTINEL)
            continue;

        if (new_code != 0) {
            for (int j = 0; j < cc.num_bindings; ++j) {
                if (j != i && cc.bindings[j].scan_codes[0] == new_code)
                    cc.bindings[j].scan_codes[0] = 0;
            }
        }

        cc.bindings[i].scan_codes[0] = new_code;

        // Gyro modifier actions are mutually exclusive, so if one of them is bound, it unbinds the other two.
        if (new_code != 0) {
            const int hold_idx     = static_cast<int>(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_GYRO_MODIFIER_HOLD));
            const int hold_inv_idx = static_cast<int>(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_GYRO_MODIFIER_HOLD_INVERT));
            const int toggle_idx   = static_cast<int>(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_GYRO_MODIFIER_TOGGLE));
            if (i == hold_idx || i == hold_inv_idx || i == toggle_idx) {
                for (int k = 0; k < cc.num_bindings; ++k) {
                    if (k != i && (k == hold_idx || k == hold_inv_idx || k == toggle_idx))
                        cc.bindings[k].scan_codes[0] = 0;
                }
            }
        }

        break;
    }
}

int gamepad_get_button_binding(int button_idx)
{
    if (button_idx < 0 || button_idx >= SDL_GAMEPAD_BUTTON_COUNT) return -1;
    return g_button_map[button_idx];
}

void gamepad_set_button_binding(int button_idx, int action_idx)
{
    if (button_idx < 0 || button_idx >= SDL_GAMEPAD_BUTTON_COUNT) return;
    g_button_map[button_idx] = action_idx;
}

int gamepad_get_trigger_action(int trigger_idx)
{
    if (trigger_idx < 0 || trigger_idx > 1) return -1;
    return g_trigger_action[trigger_idx];
}

void gamepad_set_trigger_action(int trigger_idx, int action_idx)
{
    if (trigger_idx < 0 || trigger_idx > 1) return;
    g_trigger_action[trigger_idx] = action_idx;
}

void gamepad_reset_to_defaults()
{
    memset(g_button_map, -1, sizeof(g_button_map));
    g_button_map[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = rf::CC_ACTION_PRIMARY_ATTACK;
    g_button_map[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER]  = rf::CC_ACTION_JUMP;
    g_button_map[SDL_GAMEPAD_BUTTON_SOUTH]          = rf::CC_ACTION_USE;
    g_button_map[SDL_GAMEPAD_BUTTON_NORTH]          = rf::CC_ACTION_RELOAD;
    g_button_map[SDL_GAMEPAD_BUTTON_EAST]           = rf::CC_ACTION_NEXT_WEAPON;
    g_button_map[SDL_GAMEPAD_BUTTON_WEST]           = rf::CC_ACTION_PREV_WEAPON;
    g_button_map[SDL_GAMEPAD_BUTTON_DPAD_LEFT]      = rf::CC_ACTION_HIDE_WEAPON;
    g_button_map[SDL_GAMEPAD_BUTTON_DPAD_RIGHT]     = rf::CC_ACTION_MESSAGES;
    g_button_map[SDL_GAMEPAD_BUTTON_DPAD_DOWN]      = static_cast<int>(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_CENTER_VIEW));
    g_trigger_action[0] = rf::CC_ACTION_CROUCH;
    g_trigger_action[1] = rf::CC_ACTION_SECONDARY_ATTACK;
}

void gamepad_apply_patch()
{
    gamepad_reset_to_defaults();

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3_SIXAXIS_DRIVER, "1");

    // Load SDL_GameControllerDB
    std::string mappings_path = get_module_dir(g_hmodule) + "gamecontrollerdb.txt";
    if (SDL_AddGamepadMappingsFromFile(mappings_path.c_str()) < 0)
        xlog::warn("SDL_GameControllerDB: failed to load mappings: {}", SDL_GetError());

    control_is_control_down_hook.install();
    control_config_check_pressed_hook.install();
    physics_simulate_entity_hook.install();
    player_fpgun_process_hook.install();
    vmesh_process_hook.install();
    key_process_event_hook.install();
    mouse_was_button_pressed_hook.install();
    joy_sens_cmd.register_cmd();
    joy_move_deadzone_cmd.register_cmd();
    joy_look_deadzone_cmd.register_cmd();
    joy_flickstick_cmd.register_cmd();
    joy_flickstick_sweep_cmd.register_cmd();
    joy_flickstick_smoothing_cmd.register_cmd();
    joy_flickstick_deadzone_cmd.register_cmd();
    joy_flickstick_release_deadzone_cmd.register_cmd();
    gyro_sens_cmd.register_cmd();
    gyro_camera_cmd.register_cmd();
    gyro_vehicle_camera_cmd.register_cmd();
    input_prompts_cmd.register_cmd();
    gamepad_prompts_cmd.register_cmd();
    gyro_apply_patch();
}

static void gamepad_msg_handler(UINT msg, WPARAM w_param, LPARAM)
{
    if (msg != WM_ACTIVATEAPP || w_param)
        return;
    // Focus lost: release all gamepad input so nothing stays held while unfocused.
    if (g_gamepad) {
        release_movement_keys();
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
            inject_action_key(g_button_map[b], false);
        inject_action_key(g_trigger_action[0], false);
        inject_action_key(g_trigger_action[1], false);
        if (g_menu_nav.lclick_held) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(rf::main_wnd, &pt);
            SendMessage(rf::main_wnd, WM_LBUTTONUP, 0, MAKELPARAM(pt.x, pt.y));
            g_menu_nav.lclick_held = false;
        }
    }
    reset_gamepad_input_state();
}

void gamepad_sdl_init()
{
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        xlog::error("Failed to initialize SDL gamepad subsystem: {}", SDL_GetError());
        return;
    }

    if (SDL_HasGamepad()) {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (ids) {
            for (int i = 0; i < count; ++i)
                xlog::info("Gamepad found: {}", SDL_GetGamepadNameForID(ids[i]));
            if (count > 0)
                try_open_gamepad(ids[0]);
            SDL_free(ids);
        }
    }

    rf::os_add_msg_handler(gamepad_msg_handler);
    xlog::info("Gamepad support initialized");
}
