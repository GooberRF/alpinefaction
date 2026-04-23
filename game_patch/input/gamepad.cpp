#include "gamepad.h"
#include "gyro.h"
#include "rumble.h"
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
#include "../rf/weapon.h"
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
static bool g_motion_sensors_supported = false;
static bool g_rumble_supported         = false;
static bool g_trigger_rumble_supported = false;

static float g_camera_gamepad_dx = 0.0f;
static float g_camera_gamepad_dy = 0.0f;

static float g_gamepad_scope_sensitivity_value = 0.25f;
static float g_gamepad_scanner_sensitivity_value = 0.25f;
static float g_gamepad_scope_gyro_sensitivity_value = 0.25f;
static float g_gamepad_scanner_gyro_sensitivity_value = 0.25f;
static float g_gamepad_scope_applied_dynamic_sensitivity_value = 1.0f;
static float g_gamepad_scope_gyro_applied_dynamic_sensitivity_value = 1.0f;

static int g_button_map[SDL_GAMEPAD_BUTTON_COUNT];
static int g_button_map_alt[SDL_GAMEPAD_BUTTON_COUNT];
static int g_trigger_action[2] = {rf::CC_ACTION_CROUCH, rf::CC_ACTION_SECONDARY_ATTACK}; // [0] = LT, [1] = RT

// Menu-only maps for context-sensitive AF actions (spectate, vote, menus) that share buttons with gameplay.
static int g_menu_button_map[SDL_GAMEPAD_BUTTON_COUNT];
static int g_menu_trigger_action[2] = {-1, -1};

static bool g_lt_was_down = false;
static bool g_rt_was_down = false;

static constexpr int k_action_count = 128;
static bool g_action_prev[k_action_count] = {};
static bool g_action_curr[k_action_count] = {};

static int g_rebind_pending_sc = -1; // scan code captured during rebind, -1 = none pending
static bool g_last_input_was_gamepad = false;
static float g_message_log_close_cooldown = 0.0f;
static int g_pending_scroll_delta = 0;
static float g_menu_cursor_accum_x = 0.0f;
static float g_menu_cursor_accum_y = 0.0f;
static bool g_gyro_menu_cursor_active = false;

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

struct TouchpadState {
    bool  active = false;
    float last_x = 0.0f;
    float last_y = 0.0f;
};
static TouchpadState g_touchpad;

static float g_move_lx = 0.0f, g_move_ly = 0.0f;
static float g_move_mag = 0.0f;

static bool  g_flickstick_was_in_flick_zone = false; // stick was past the flick deadzone last frame
static float g_flickstick_flick_progress    = 0.0f;  // seconds into the current flick animation
static float g_flickstick_flick_size        = 0.0f;  // yaw to output over the flick animation (rad)
static float g_flickstick_prev_stick_angle  = 0.0f;  // stick angle from the previous frame
static constexpr int k_turn_smooth_buf_size = 5;     // ring buffer size for turn smoothing
static float g_flickstick_turn_smooth_buf[k_turn_smooth_buf_size] = {};
static int   g_flickstick_turn_smooth_idx   = 0;

static rf::VMesh* g_local_player_body_vmesh = nullptr;
static bool g_scaling_fpgun_vmesh = false;

static Uint64 g_sensor_last_gyro_ts  = 0;
static Uint64 g_sensor_last_accel_ts = 0;
static float  g_sensor_accel[3]      = {};
static float  g_sensor_gyro[3]       = {};

static bool is_gamepad_input_active()
{
    return g_gamepad && rf::is_main_wnd_active;
}

static bool is_freelook_camera()
{
    return rf::local_player && rf::local_player->cam
        && rf::local_player->cam->mode == rf::CameraMode::CAMERA_FREELOOK;
}

static void update_gamepad_scoped_sensitivities()
{
    g_gamepad_scope_sensitivity_value = g_alpine_game_config.gamepad_scope_sensitivity_modifier;
    g_gamepad_scanner_sensitivity_value = g_alpine_game_config.gamepad_scanner_sensitivity_modifier;
    g_gamepad_scope_gyro_sensitivity_value = g_alpine_game_config.gamepad_scope_gyro_sensitivity_modifier;
    g_gamepad_scanner_gyro_sensitivity_value = g_alpine_game_config.gamepad_scanner_gyro_sensitivity_modifier;
    g_gamepad_scope_applied_dynamic_sensitivity_value =
        (1.0f / (4.0f * g_alpine_game_config.gamepad_scope_sensitivity_modifier)) * rf::scope_sensitivity_constant;
    g_gamepad_scope_gyro_applied_dynamic_sensitivity_value =
        (1.0f / (4.0f * g_alpine_game_config.gamepad_scope_gyro_sensitivity_modifier)) * rf::scope_sensitivity_constant;
}

static bool is_menu_only_action(int action_idx)
{
    if (action_idx < 0) return false;
    if (action_idx == static_cast<int>(rf::CC_ACTION_CHAT)
     || action_idx == static_cast<int>(rf::CC_ACTION_TEAM_CHAT)
     || action_idx == static_cast<int>(rf::CC_ACTION_MP_STATS))
        return true;
    using rf::AlpineControlConfigAction;
    return action_idx == static_cast<int>(get_af_control(AlpineControlConfigAction::AF_ACTION_VOTE_YES))
        || action_idx == static_cast<int>(get_af_control(AlpineControlConfigAction::AF_ACTION_VOTE_NO))
        || action_idx == static_cast<int>(get_af_control(AlpineControlConfigAction::AF_ACTION_READY))
        || action_idx == static_cast<int>(get_af_control(AlpineControlConfigAction::AF_ACTION_DROP_FLAG))
        || action_idx == static_cast<int>(get_af_control(AlpineControlConfigAction::AF_ACTION_CHAT_MENU))
        || action_idx == static_cast<int>(get_af_control(AlpineControlConfigAction::AF_ACTION_TAUNT_MENU))
        || action_idx == static_cast<int>(get_af_control(AlpineControlConfigAction::AF_ACTION_COMMAND_MENU))
        || action_idx == static_cast<int>(get_af_control(AlpineControlConfigAction::AF_ACTION_PING_LOCATION))
        || action_idx == static_cast<int>(get_af_control(AlpineControlConfigAction::AF_ACTION_SPECTATE_MENU))
        || action_idx == static_cast<int>(get_af_control(AlpineControlConfigAction::AF_ACTION_REMOTE_SERVER_CFG))
        || action_idx == static_cast<int>(get_af_control(AlpineControlConfigAction::AF_ACTION_SPECTATE_TOGGLE_FREELOOK))
        || action_idx == static_cast<int>(get_af_control(AlpineControlConfigAction::AF_ACTION_SPECTATE_TOGGLE));
}

static bool is_gamepad_menu_state()
{
    if (!rf::gameseq_in_gameplay()) return true;
    if (!rf::keep_mouse_centered) return true;
    if (is_freelook_camera()) return false;
    return !rf::local_player_entity || rf::entity_is_dying(rf::local_player_entity);
}

static bool is_gamepad_menu_navigation_state()
{
    const rf::GameState state = rf::gameseq_get_state();
    if (state == rf::GS_MULTI_LIMBO || state == rf::GS_LEVEL_TRANSITION || state == rf::GS_NEW_LEVEL)
        return false;
    return is_gamepad_menu_state();
}

static void reset_gamepad_input_state()
{
    g_camera_gamepad_dx = 0.0f;
    g_camera_gamepad_dy = 0.0f;
    memset(g_action_curr, 0, sizeof(g_action_curr));
    g_move_lx = g_move_ly = 0.0f;
    g_move_mag = 0.0f;
    g_menu_nav = {};
    g_rebind_pending_sc = -1;
    g_flickstick_was_in_flick_zone = false;
    g_flickstick_flick_progress    = 0.0f;
    g_flickstick_flick_size        = 0.0f;
    g_flickstick_prev_stick_angle  = 0.0f;
    memset(g_flickstick_turn_smooth_buf, 0, sizeof(g_flickstick_turn_smooth_buf));
    g_flickstick_turn_smooth_idx   = 0;
    g_touchpad = {};
    g_menu_cursor_accum_x = 0.0f;
    g_menu_cursor_accum_y = 0.0f;
    g_gyro_menu_cursor_active = false;
    g_lt_was_down = false;
    g_rt_was_down = false;
    g_last_input_was_gamepad = false;
    g_sensor_last_gyro_ts  = 0;
    g_sensor_last_accel_ts = 0;
    memset(g_sensor_accel, 0, sizeof(g_sensor_accel));
    memset(g_sensor_gyro,  0, sizeof(g_sensor_gyro));
    g_pending_scroll_delta = 0;
}

// Normalize an axis value, strip the deadzone band, and rescale the remainder to [-1, 1].
// Per-axis (cross-shaped) deadzone: each axis is independently deadzoned and rescaled.
static float get_axis(SDL_GamepadAxis axis, float deadzone)
{
    if (!g_gamepad) return 0.0f;
    float v = SDL_GetGamepadAxis(g_gamepad, axis) / static_cast<float>(SDL_JOYSTICK_AXIS_MAX);
    if (v >  deadzone) return (v - deadzone) / (1.0f - deadzone);
    if (v < -deadzone) return (v + deadzone) / (1.0f - deadzone);
    return 0.0f;
}

// Radial (circular) deadzone: deadzone applied to stick magnitude; preserves direction.
static void get_axis_circular(SDL_GamepadAxis axis_x, SDL_GamepadAxis axis_y, float deadzone,
                              float& out_x, float& out_y)
{
    if (!g_gamepad) { out_x = out_y = 0.0f; return; }
    float raw_x = SDL_GetGamepadAxis(g_gamepad, axis_x) / static_cast<float>(SDL_JOYSTICK_AXIS_MAX);
    float raw_y = SDL_GetGamepadAxis(g_gamepad, axis_y) / static_cast<float>(SDL_JOYSTICK_AXIS_MAX);
    float mag = std::hypot(raw_x, raw_y);
    float remapped = (mag > deadzone) ? (mag - deadzone) / (1.0f - deadzone) : 0.0f;
    float scale = mag > 0.0f ? remapped / mag : 0.0f;
    out_x = raw_x * scale;
    out_y = raw_y * scale;
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


static bool action_is_down(rf::ControlConfigAction action)
{
    int i = static_cast<int>(action);
    return i >= 0 && i < k_action_count && g_action_curr[i];
}

static bool try_enable_gamepad_sensors()
{
    if (!g_gamepad) return false;

    if (!SDL_GamepadHasSensor(g_gamepad, SDL_SENSOR_GYRO) ||
        !SDL_GamepadHasSensor(g_gamepad, SDL_SENSOR_ACCEL)) {
        xlog::info("Motion sensors are not supported");
        return false;
    }

    if (!SDL_SetGamepadSensorEnabled(g_gamepad, SDL_SENSOR_GYRO,  true) ||
        !SDL_SetGamepadSensorEnabled(g_gamepad, SDL_SENSOR_ACCEL, true)) {
        xlog::warn("Failed to enable motion sensors: {}", SDL_GetError());
        return false;
    }

    xlog::info("Motion sensors are supported");
    g_motion_sensors_supported = true;
    gyro_reset_full();
    return true;
}

static bool try_enable_gamepad_rumble()
{
    if (!g_gamepad) return false;

    if (!SDL_GetBooleanProperty(SDL_GetGamepadProperties(g_gamepad), SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false)) {
        xlog::info("Rumble is not supported");
        return false;
    }

    xlog::info("Rumble is supported");
    g_rumble_supported = true;
    return true;
}

static bool try_enable_gamepad_trigger_rumble()
{
    if (!g_gamepad) return false;

    if (!SDL_GetBooleanProperty(SDL_GetGamepadProperties(g_gamepad), SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false)) {
        xlog::info("Trigger rumble is not supported");
        return false;
    }

    xlog::info("Trigger rumble is supported");
    g_trigger_rumble_supported = true;
    return true;
}

static void try_open_gamepad(SDL_JoystickID id)
{
    g_gamepad = SDL_OpenGamepad(id);
    if (!g_gamepad) {
        xlog::warn("Failed to open gamepad: {}", SDL_GetError());
        return;
    }

    xlog::info("Gamepad connected: {}", SDL_GetGamepadName(g_gamepad));
    try_enable_gamepad_rumble();
    try_enable_gamepad_trigger_rumble();
    try_enable_gamepad_sensors();
}

static void inject_action_key(int action, bool down)
{
    if (!rf::gameseq_in_gameplay()) return;
    if (rf::console::console_is_visible()) return;
    if (!rf::local_player || action < 0 || action >= rf::local_player->settings.controls.num_bindings)
        return;
    int16_t sc = rf::local_player->settings.controls.bindings[action].scan_codes[0];
    if (sc > 0)
        rf::key_process_event(sc, down ? 1 : 0, 0);
}

static void force_release_action_key(int action)
{
    if (rf::console::console_is_visible()) return;
    if (!rf::local_player || action < 0 || action >= rf::local_player->settings.controls.num_bindings)
        return;
    int16_t sc = rf::local_player->settings.controls.bindings[action].scan_codes[0];
    if (sc > 0)
        rf::key_process_event(sc, 0, 0);
}

static void menu_nav_inject_key(int key)
{
    if (rf::console::console_is_visible()) return;
    rf::key_process_event(key, 1, 0);
    rf::key_process_event(key, 0, 0);
}

// Returns true if `state` is a UI overlay where Cancel should be handled by the gamepad menu
// system (close/escape) rather than injecting a raw ESC into gameplay.
static bool is_gamepad_cancellable_menu_state(rf::GameState state)
{
    return state == rf::GS_MESSAGE_LOG
        || state == rf::GS_OPTIONS_MENU
        || state == rf::GS_MULTI_MENU
        || state == rf::GS_HELP
        || state == rf::GS_EXTRAS_MENU
        || state == rf::GS_MULTI_SERVER_LIST
        || state == rf::GS_SAVE_GAME_MENU
        || state == rf::GS_LOAD_GAME_MENU
        || state == rf::GS_MAIN_MENU
        || state == rf::GS_LEVEL_TRANSITION
        || state == rf::GS_MULTI_LIMBO
        || state == rf::GS_FRAMERATE_TEST_END
        || state == rf::GS_CREDITS
        || state == rf::GS_BOMB_DEFUSE;
}

static void menu_nav_handle_confirm()
{
    if (rf::ui::options_controls_waiting_for_key) return;

    if (g_menu_nav.last_nav_was_dpad) {
        menu_nav_inject_key(rf::KEY_ENTER);
    } else {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(rf::main_wnd, &pt);
        SendMessage(rf::main_wnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(pt.x, pt.y));
        g_menu_nav.lclick_held = true;
    }
}

static void menu_nav_handle_cancel()
{
    rf::GameState current_state = rf::gameseq_get_state();
    if (is_gamepad_cancellable_menu_state(current_state)) {
        if (current_state == rf::GS_MESSAGE_LOG) {
            rf::gameseq_set_state(rf::GS_GAMEPLAY, false);
            g_message_log_close_cooldown = 0.2f;
        } else if (current_state == rf::GS_MULTI_LIMBO
                || current_state == rf::GS_LEVEL_TRANSITION
                || current_state == rf::GS_NEW_LEVEL) {
        } else {
            menu_nav_inject_key(rf::KEY_ESC);
        }
        return;
    }

    if ((rf::local_player_entity && rf::entity_is_dying(rf::local_player_entity))
        || (rf::local_player && rf::player_is_dead(rf::local_player))) {
        return;
    }

    menu_nav_inject_key(rf::KEY_ESC);
}

static void menu_nav_release_click()
{
    if (!g_menu_nav.lclick_held) return;
    POINT pt;
    GetCursorPos(&pt);
    ScreenToClient(rf::main_wnd, &pt);
    SendMessage(rf::main_wnd, WM_LBUTTONUP, 0, MAKELPARAM(pt.x, pt.y));
    g_menu_nav.lclick_held = false;
}

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

static void menu_nav_apply_cursor_delta(float dx, float dy)
{
    if (dx == 0.0f && dy == 0.0f) return;
    g_menu_cursor_accum_x += dx;
    g_menu_cursor_accum_y += dy;
    int ix = static_cast<int>(g_menu_cursor_accum_x);
    int iy = static_cast<int>(g_menu_cursor_accum_y);
    g_menu_cursor_accum_x -= static_cast<float>(ix);
    g_menu_cursor_accum_y -= static_cast<float>(iy);
    if (ix == 0 && iy == 0) return;
    menu_nav_move_cursor(ix, iy);
    g_menu_nav.last_nav_was_dpad = false;
    g_last_input_was_gamepad = true;
}

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

static SDL_GamepadButton get_gyro_toggle_button()
{
    if (g_gamepad && SDL_GetGamepadButtonLabel(g_gamepad, SDL_GAMEPAD_BUTTON_WEST) == SDL_GAMEPAD_BUTTON_LABEL_Y)
        return SDL_GAMEPAD_BUTTON_WEST;
    return SDL_GAMEPAD_BUTTON_NORTH;
}

static bool menu_nav_on_button_down(int btn)
{
    const SDL_GamepadButton confirm_btn      = get_menu_confirm_button();
    const SDL_GamepadButton cancel_btn       = get_menu_cancel_button();
    const SDL_GamepadButton gyro_toggle_btn  = get_gyro_toggle_button();

    if (btn == static_cast<int>(confirm_btn)) {
        menu_nav_handle_confirm();
        return true;
    }
    if (btn == static_cast<int>(cancel_btn)) {
        menu_nav_handle_cancel();
        return true;
    }
    if (btn == static_cast<int>(gyro_toggle_btn)) {
        if (g_motion_sensors_supported && g_alpine_game_config.gamepad_gyro_menu_cursor_sensitivity > 0.0f)
            g_gyro_menu_cursor_active = !g_gyro_menu_cursor_active;
        return true;
    }
    switch (btn) {
    case SDL_GAMEPAD_BUTTON_TOUCHPAD:
        menu_nav_handle_confirm();
        return true;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        if (!rf::ui::options_controls_waiting_for_key) {
            menu_nav_inject_key(dpad_btn_to_navkey(btn));
            g_menu_nav.last_nav_was_dpad = true;
            g_menu_nav.repeat_btn        = btn;
            g_menu_nav.repeat_timer      = 0.4f;
        }
        return true;
    default:
        return false;
    }
}

static void menu_nav_on_button_up(int btn)
{
    if (btn == g_menu_nav.repeat_btn)
        g_menu_nav.repeat_btn = -1;
    if (btn == static_cast<int>(get_menu_confirm_button()) || btn == SDL_GAMEPAD_BUTTON_TOUCHPAD)
        menu_nav_release_click();
}

static void update_trigger_actions()
{
    float rt = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / static_cast<float>(SDL_JOYSTICK_AXIS_MAX);
    float lt = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)  / static_cast<float>(SDL_JOYSTICK_AXIS_MAX);
    bool lt_down = lt > 0.5f;
    bool rt_down = rt > 0.5f;

    if (lt_down != g_lt_was_down) {
        if (lt_down)
            inject_action_key(g_trigger_action[0], true);
        else
            force_release_action_key(g_trigger_action[0]);
        if (g_trigger_action[0] >= 0 && g_trigger_action[0] < k_action_count)
            g_action_curr[g_trigger_action[0]] = lt_down;
        if (g_menu_trigger_action[0] >= 0 && g_menu_trigger_action[0] < k_action_count)
            g_action_curr[g_menu_trigger_action[0]] = lt_down;
        sync_extra_actions_for_scancode(static_cast<int16_t>(CTRL_GAMEPAD_LEFT_TRIGGER), lt_down, g_trigger_action[0]);
    }
    if (rt_down != g_rt_was_down) {
        if (rt_down)
            inject_action_key(g_trigger_action[1], true);
        else
            force_release_action_key(g_trigger_action[1]);
        if (g_trigger_action[1] >= 0 && g_trigger_action[1] < k_action_count)
            g_action_curr[g_trigger_action[1]] = rt_down;
        if (g_menu_trigger_action[1] >= 0 && g_menu_trigger_action[1] < k_action_count)
            g_action_curr[g_menu_trigger_action[1]] = rt_down;
        sync_extra_actions_for_scancode(static_cast<int16_t>(CTRL_GAMEPAD_RIGHT_TRIGGER), rt_down, g_trigger_action[1]);
    }

    g_lt_was_down = lt_down;
    g_rt_was_down = rt_down;
}


static bool is_action_held_by_button(int action_idx)
{
    if (!g_gamepad) return false;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
        if ((g_button_map[b] == action_idx || g_button_map_alt[b] == action_idx)
            && SDL_GetGamepadButton(g_gamepad, static_cast<SDL_GamepadButton>(b)))
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

    static constexpr rf::ControlConfigAction k_move_actions[] = {
        rf::CC_ACTION_FORWARD,
        rf::CC_ACTION_BACKWARD,
        rf::CC_ACTION_SLIDE_LEFT,
        rf::CC_ACTION_SLIDE_RIGHT,
    };
    for (rf::ControlConfigAction action : k_move_actions) {
        int idx = static_cast<int>(action);
        if (g_action_curr[idx] && rf::local_player && !rf::console::console_is_visible()) {
            int16_t sc = rf::local_player->settings.controls.bindings[idx].scan_codes[0];
            if (sc >= 0)
                rf::key_process_event(sc, 0, 0);
        }
        g_action_curr[idx] = false;
    }
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

    // Suppress movement input while viewing a security camera
    if (rf::local_player && rf::local_player->view_from_handle != -1) {
        release_movement_keys();
        return;
    }

    SDL_GamepadAxis mov_x = g_alpine_game_config.gamepad_swap_sticks ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX;
    SDL_GamepadAxis mov_y = g_alpine_game_config.gamepad_swap_sticks ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY;
    float mov_dz          = g_alpine_game_config.gamepad_swap_sticks ? g_alpine_game_config.gamepad_look_deadzone
                                                                      : g_alpine_game_config.gamepad_move_deadzone;

    // Cross-shaped deadzone for movement; slightly enlarged to tighten the neutral zone.
    constexpr float k_movement_dz_multiplier = 1.1f;
    float lx = get_axis(mov_x, mov_dz * k_movement_dz_multiplier);
    float ly = get_axis(mov_y, mov_dz * k_movement_dz_multiplier);

    g_move_lx = lx;
    g_move_ly = ly;
    g_move_mag = std::min(1.0f, std::sqrt(lx * lx + ly * ly));

    set_movement_key(rf::CC_ACTION_FORWARD,     ly < 0.0f);
    set_movement_key(rf::CC_ACTION_BACKWARD,    ly > 0.0f);
    set_movement_key(rf::CC_ACTION_SLIDE_LEFT,  lx < 0.0f);
    set_movement_key(rf::CC_ACTION_SLIDE_RIGHT, lx > 0.0f);
}

static void disconnect_active_gamepad()
{
    SDL_CloseGamepad(g_gamepad);
    g_gamepad                  = nullptr;
    g_motion_sensors_supported = false;
    g_rumble_supported         = false;
    g_trigger_rumble_supported = false;
    release_movement_keys();
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
        inject_action_key(g_button_map[b], false);
        inject_action_key(g_button_map_alt[b], false);
    }
    inject_action_key(g_trigger_action[0], false);
    inject_action_key(g_trigger_action[1], false);
    menu_nav_release_click();
    reset_gamepad_input_state();
}

static void handle_gamepad_added(const SDL_GamepadDeviceEvent& ev)
{
    if (!g_gamepad) {
        try_open_gamepad(ev.which);
        return;
    }
    if (SDL_GetGamepadID(g_gamepad) == ev.which)
        return;
    xlog::info("New gamepad connected, hotswapping from '{}' to '{}'",
        SDL_GetGamepadName(g_gamepad), SDL_GetGamepadNameForID(ev.which));
    disconnect_active_gamepad();
    try_open_gamepad(ev.which);
}

static void handle_gamepad_removed(const SDL_GamepadDeviceEvent& ev)
{
    if (!(g_gamepad && SDL_GetGamepadID(g_gamepad) == ev.which))
        return;

    xlog::info("Gamepad disconnected");
    disconnect_active_gamepad();

    // Fall back to any remaining connected gamepad
    int fallback_count = 0;
    SDL_JoystickID* fallback_ids = SDL_GetGamepads(&fallback_count);
    if (fallback_ids) {
        for (int i = 0; i < fallback_count; ++i) {
            if (fallback_ids[i] != ev.which) {
                try_open_gamepad(fallback_ids[i]);
                break;
            }
        }
        SDL_free(fallback_ids);
    }
    if (g_gamepad)
        xlog::info("Fell back to gamepad: '{}'", SDL_GetGamepadName(g_gamepad));
}

static void handle_gamepad_button_down(const SDL_GamepadButtonEvent& ev)
{
    if (g_message_log_close_cooldown > 0.0f) return;
    if (!is_gamepad_input_active() || SDL_GetGamepadID(g_gamepad) != ev.which) return;

    g_last_input_was_gamepad = true;

    if (ui_ctrl_bindings_view_active() && rf::ui::options_controls_waiting_for_key) {
        if (ev.button == SDL_GAMEPAD_BUTTON_START) {
            menu_nav_inject_key(rf::KEY_ESC);
        } else {
            g_rebind_pending_sc = CTRL_GAMEPAD_SCAN_BASE + ev.button;
            rf::key_process_event(static_cast<int>(CTRL_REBIND_SENTINEL), 1, 0);
        }
        return;
    }

    if (ev.button == SDL_GAMEPAD_BUTTON_START) {
        // START always behaves exactly like ESC — unconditionally, in any state.
        // The Cancel face button has its own restricted logic via menu_nav_handle_cancel().
        menu_nav_inject_key(rf::KEY_ESC);
    }

    bool in_menu_nav_state = is_gamepad_menu_navigation_state();
    bool in_spectate_state = multi_spectate_is_spectating();
    if (in_menu_nav_state)
        g_menu_nav.deferred_btn_down = ev.button;

    bool is_menu_nav_button = in_menu_nav_state && !in_spectate_state
        && (ev.button == static_cast<int>(get_menu_confirm_button())
            || ev.button == static_cast<int>(get_menu_cancel_button())
            || ev.button == SDL_GAMEPAD_BUTTON_TOUCHPAD);

    if (!is_menu_nav_button && ev.button < SDL_GAMEPAD_BUTTON_COUNT) {
        int mapped = g_button_map[ev.button];
        if (mapped >= 0) {
            inject_action_key(mapped, true);
            g_action_curr[mapped] = true;
        }
        int alt_mapped = g_button_map_alt[ev.button];
        if (alt_mapped >= 0) {
            inject_action_key(alt_mapped, true);
            g_action_curr[alt_mapped] = true;
        }
        int menu_mapped = g_menu_button_map[ev.button];
        if (menu_mapped >= 0)
            g_action_curr[menu_mapped] = true;
        int16_t gp_sc = static_cast<int16_t>(CTRL_GAMEPAD_SCAN_BASE + ev.button);
        sync_extra_actions_for_scancode(gp_sc, true, mapped);
    }
}

static void handle_gamepad_button_up(const SDL_GamepadButtonEvent& ev)
{
    if (!is_gamepad_input_active() || SDL_GetGamepadID(g_gamepad) != ev.which) return;

    if (is_gamepad_menu_navigation_state())
        g_menu_nav.deferred_btn_up = ev.button;

    if (ev.button < SDL_GAMEPAD_BUTTON_COUNT) {
        int mapped = g_button_map[ev.button];
        if (mapped >= 0) {
            force_release_action_key(mapped);
            g_action_curr[mapped] = false;
        }
        int alt_mapped = g_button_map_alt[ev.button];
        if (alt_mapped >= 0) {
            force_release_action_key(alt_mapped);
            g_action_curr[alt_mapped] = false;
        }
        int menu_mapped = g_menu_button_map[ev.button];
        if (menu_mapped >= 0)
            g_action_curr[menu_mapped] = false;
        int16_t gp_sc = static_cast<int16_t>(CTRL_GAMEPAD_SCAN_BASE + ev.button);
        sync_extra_actions_for_scancode(gp_sc, false, mapped);
    }
}

static void handle_gamepad_axis_motion(const SDL_GamepadAxisEvent& ev)
{
    if (g_message_log_close_cooldown > 0.0f) return;
    if (!is_gamepad_input_active() || SDL_GetGamepadID(g_gamepad) != ev.which) return;

    float v = ev.value / static_cast<float>(SDL_JOYSTICK_AXIS_MAX);
    switch (static_cast<SDL_GamepadAxis>(ev.axis)) {
    case SDL_GAMEPAD_AXIS_LEFTX:
    case SDL_GAMEPAD_AXIS_LEFTY:
        if (std::abs(v) > g_alpine_game_config.gamepad_move_deadzone)
            g_last_input_was_gamepad = true;
        break;
    case SDL_GAMEPAD_AXIS_RIGHTX:
    case SDL_GAMEPAD_AXIS_RIGHTY:
        if (std::abs(v) > g_alpine_game_config.gamepad_look_deadzone)
            g_last_input_was_gamepad = true;
        break;
    case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
    case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
        if (v > 0.5f)
            g_last_input_was_gamepad = true;
        break;
    default:
        break;
    }
}

static void handle_gamepad_touchpad_down(const SDL_GamepadTouchpadEvent& ev)
{
    if (!is_gamepad_input_active() || SDL_GetGamepadID(g_gamepad) != ev.which) return;
    if (ev.touchpad != 0 || ev.finger != 0) return; // track primary finger on first touchpad only
    if (g_message_log_close_cooldown > 0.0f) return;
    g_touchpad.active = true;
    g_touchpad.last_x = ev.x;
    g_touchpad.last_y = ev.y;
    // Reset accumulator so the first motion event of a new touch starts clean.
    g_menu_cursor_accum_x = 0.0f;
    g_menu_cursor_accum_y = 0.0f;
    g_last_input_was_gamepad = true;
}

static void handle_gamepad_touchpad_motion(const SDL_GamepadTouchpadEvent& ev)
{
    if (!is_gamepad_input_active() || SDL_GetGamepadID(g_gamepad) != ev.which) return;
    if (ev.touchpad != 0 || ev.finger != 0) return;
    if (!g_touchpad.active) return;
    // Always update position so delta is fresh when we next enter menu state.
    float dx = ev.x - g_touchpad.last_x;
    float dy = ev.y - g_touchpad.last_y;
    g_touchpad.last_x = ev.x;
    g_touchpad.last_y = ev.y;
    if (g_message_log_close_cooldown > 0.0f) return;
    if (!is_gamepad_menu_navigation_state()) return;
    // Scale: one full touchpad swipe maps to traversing the full screen dimension.
    float fdx = dx * static_cast<float>(rf::gr::screen_width());
    float fdy = dy * static_cast<float>(rf::gr::screen_height());
    menu_nav_apply_cursor_delta(fdx, fdy);
}

static void handle_gamepad_touchpad_up(const SDL_GamepadTouchpadEvent& ev)
{
    if (!is_gamepad_input_active() || SDL_GetGamepadID(g_gamepad) != ev.which) return;
    if (ev.touchpad != 0 || ev.finger != 0) return;
    g_touchpad.active = false;
}

static void handle_gamepad_sensor_update(const SDL_GamepadSensorEvent& ev)
{
    if (!g_motion_sensors_supported) return;
    if (!g_gamepad || SDL_GetGamepadID(g_gamepad) != ev.which) return;

    constexpr float rad2deg = 180.0f / 3.14159265f;

    switch (ev.sensor) {
    case SDL_SENSOR_GYRO:
        g_sensor_gyro[0] = ev.data[0] * rad2deg;
        g_sensor_gyro[1] = ev.data[1] * rad2deg;
        g_sensor_gyro[2] = ev.data[2] * rad2deg;
        break;
    case SDL_SENSOR_ACCEL:
        g_sensor_accel[0] = ev.data[0] / SDL_STANDARD_GRAVITY;
        g_sensor_accel[1] = ev.data[1] / SDL_STANDARD_GRAVITY;
        g_sensor_accel[2] = ev.data[2] / SDL_STANDARD_GRAVITY;
        g_sensor_last_accel_ts = ev.sensor_timestamp;
        break;
    default:
        break;
    }

    if (ev.sensor == SDL_SENSOR_GYRO && g_sensor_last_gyro_ts && g_sensor_last_accel_ts) {
        float dt = static_cast<float>(ev.sensor_timestamp - g_sensor_last_gyro_ts) * 1e-9f;
        if (dt > 0.0f && dt < 0.1f) {
            gyro_process_motion(
                g_sensor_gyro[0], g_sensor_gyro[1], g_sensor_gyro[2],
                g_sensor_accel[0], g_sensor_accel[1], g_sensor_accel[2],
                dt);
        }
    }

    if (ev.sensor == SDL_SENSOR_GYRO)
        g_sensor_last_gyro_ts = ev.sensor_timestamp;
}

void gamepad_sdl_poll()
{
    if (SDL_IsMainThread())
        SDL_PumpEvents();

    memcpy(g_action_prev, g_action_curr, sizeof(g_action_curr));

    SDL_Event events[64];
    int n;
    while ((n = SDL_PeepEvents(events, static_cast<int>(std::size(events)),
                               SDL_GETEVENT, SDL_EVENT_GAMEPAD_AXIS_MOTION,
                               SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED)) > 0) {
        for (int i = 0; i < n; ++i) {
            const SDL_Event& ev = events[i];
            switch (ev.type) {
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                handle_gamepad_axis_motion(ev.gaxis);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                handle_gamepad_button_down(ev.gbutton);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                handle_gamepad_button_up(ev.gbutton);
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                handle_gamepad_added(ev.gdevice);
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                handle_gamepad_removed(ev.gdevice);
                break;
            case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
                handle_gamepad_touchpad_down(ev.gtouchpad);
                break;
            case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
                handle_gamepad_touchpad_motion(ev.gtouchpad);
                break;
            case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
                handle_gamepad_touchpad_up(ev.gtouchpad);
                break;
            case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
                handle_gamepad_sensor_update(ev.gsensor);
                break;
            default:
                break;
            }
        }
    }
    if (n < 0)
        xlog::warn("SDL Events error: {}", SDL_GetError());

    // Discard non-gamepad SDL events that accumulated in the queue.
    SDL_FlushEvents(SDL_EVENT_FIRST,
        static_cast<SDL_EventType>(SDL_EVENT_GAMEPAD_AXIS_MOTION - 1));
    SDL_FlushEvents(
        static_cast<SDL_EventType>(SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED + 1),
        SDL_EVENT_LAST);
}

static void menu_nav_handle_gyro_cursor_frame()
{
    if (!g_gyro_menu_cursor_active) return;
    if (!g_motion_sensors_supported) return;
    float sensitivity = g_alpine_game_config.gamepad_gyro_menu_cursor_sensitivity;
    if (sensitivity <= 0.0f) return;

    float pitch_dps, yaw_dps;
    gyro_get_calibrated_rates(pitch_dps, yaw_dps);
    if (pitch_dps == 0.0f && yaw_dps == 0.0f) return;

    float scale = sensitivity * (static_cast<float>(rf::gr::screen_height()) / 600.0f) * rf::frametime;
    float dx = -yaw_dps   * scale;
    float dy = -pitch_dps * scale;
    menu_nav_apply_cursor_delta(dx, dy);
}

static void menu_nav_handle_cursor_frame()
{
    constexpr float k_menu_stick_deadzone = 0.24f;
    constexpr float k_base_speed          = 1000.0f;
    float sx, sy;
    get_axis_circular(SDL_GAMEPAD_AXIS_LEFTX, SDL_GAMEPAD_AXIS_LEFTY, k_menu_stick_deadzone, sx, sy);
    if (sx == 0.0f && sy == 0.0f) return;
    float speed = k_base_speed * (static_cast<float>(rf::gr::screen_height()) / 600.0f);
    float dx = sx * speed * rf::frametime;
    float dy = sy * speed * rf::frametime;
    menu_nav_apply_cursor_delta(dx, dy);
}

static void menu_nav_tick_dpad_repeat()
{
    if (g_menu_nav.repeat_btn < 0 || rf::ui::options_controls_waiting_for_key) return;
    g_menu_nav.repeat_timer -= rf::frametime;
    if (g_menu_nav.repeat_timer <= 0.0f) {
        menu_nav_inject_key(dpad_btn_to_navkey(g_menu_nav.repeat_btn));
        g_menu_nav.repeat_timer = 0.12f;
    }
}

static void menu_nav_tick_scroll()
{
    constexpr float k_scroll_deadzone = 0.24f;
    float ry = get_axis(SDL_GAMEPAD_AXIS_RIGHTY, k_scroll_deadzone);
    if (ry == 0.0f) {
        g_menu_nav.scroll_timer = 0.0f;
        return;
    }
    g_menu_nav.scroll_timer -= rf::frametime;
    if (g_menu_nav.scroll_timer > 0.0f) return;
    rf::mouse_dz = (ry < 0.0f) ? 1 : -1;
    g_pending_scroll_delta = rf::mouse_dz;
    if (rf::gameseq_get_state() == rf::GS_MESSAGE_LOG) {
        if (ry < 0.0f)
            rf::ui::message_log_up_on_click(-1, -1);
        else
            rf::ui::message_log_down_on_click(-1, -1);
    }
    g_menu_nav.scroll_timer = 0.12f;
}

int gamepad_consume_menu_scroll()
{
    int v = g_pending_scroll_delta;
    g_pending_scroll_delta = 0;
    return v;
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

    menu_nav_handle_gyro_cursor_frame();
    menu_nav_handle_cursor_frame();
    menu_nav_tick_dpad_repeat();
    menu_nav_tick_scroll();
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
        float lt = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)  / static_cast<float>(SDL_JOYSTICK_AXIS_MAX);
        float rt = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / static_cast<float>(SDL_JOYSTICK_AXIS_MAX);
        if (lt > 0.5f && !g_lt_was_down) {
            g_rebind_pending_sc = CTRL_GAMEPAD_LEFT_TRIGGER;
            rf::key_process_event(static_cast<int>(CTRL_REBIND_SENTINEL), 1, 0);
        }
        if (rt > 0.5f && !g_rt_was_down) {
            g_rebind_pending_sc = CTRL_GAMEPAD_RIGHT_TRIGGER;
            rf::key_process_event(static_cast<int>(CTRL_REBIND_SENTINEL), 1, 0);
        }
    }

    update_trigger_actions();
    update_stick_movement();

    if (is_gamepad_menu_navigation_state())
        gamepad_do_menu_frame();

    g_local_player_body_vmesh = rf::local_player ? rf::get_player_entity_parent_vmesh(rf::local_player) : nullptr;

    if (g_gamepad)
        rumble_do_frame();
}

// Flick stick is based on GyroWiki documents
// http://gyrowiki.jibbsmart.com/blog:good-gyro-controls-part-2:the-flick-stick
static void gamepad_apply_flickstick(SDL_GamepadAxis cam_x, SDL_GamepadAxis cam_y,
                                    float& yaw_delta, float& pitch_delta)
{
    yaw_delta = 0.0f;
    pitch_delta = 0.0f;

    // Raw axes — no deadzone remapping to avoid quadrant snapping in the angle math.
    float rx = g_gamepad ? SDL_GetGamepadAxis(g_gamepad, cam_x) / static_cast<float>(SDL_JOYSTICK_AXIS_MAX) : 0.0f;
    float ry = g_gamepad ? SDL_GetGamepadAxis(g_gamepad, cam_y) / static_cast<float>(SDL_JOYSTICK_AXIS_MAX) : 0.0f;

    float stick_mag      = std::hypot(rx, ry);
    bool  in_flick_zone  = stick_mag >  g_alpine_game_config.gamepad_flickstick_deadzone;
    bool  fully_released = stick_mag <= g_alpine_game_config.gamepad_flickstick_release_deadzone;
    float smooth         = g_alpine_game_config.gamepad_flickstick_smoothing;
    float sweep          = g_alpine_game_config.gamepad_flickstick_sweep;

    float flick_angle = std::atan2(rx, -ry);

    if (in_flick_zone) {
        if (!g_flickstick_was_in_flick_zone) {
            g_flickstick_flick_progress = 0.0f;
            g_flickstick_flick_size     = flick_angle * sweep;
        } else {
            float turn_delta = angle_diff(flick_angle, g_flickstick_prev_stick_angle) * sweep;

            if (smooth > 0.0f) {
                constexpr float k_max_threshold = 0.3f;
                float threshold2 = smooth * k_max_threshold;
                float threshold1 = threshold2 * 0.5f;
                float direct_weight = std::clamp((std::abs(turn_delta) - threshold1) / (threshold2 - threshold1), 0.0f, 1.0f);
                g_flickstick_turn_smooth_idx = (g_flickstick_turn_smooth_idx + 1) % k_turn_smooth_buf_size;
                g_flickstick_turn_smooth_buf[g_flickstick_turn_smooth_idx] = turn_delta * (1.0f - direct_weight);
                float avg = 0.0f;
                for (int i = 0; i < k_turn_smooth_buf_size; ++i) avg += g_flickstick_turn_smooth_buf[i];
                turn_delta = turn_delta * direct_weight + avg / k_turn_smooth_buf_size;
            }

            yaw_delta += turn_delta;
        }
    } else if (fully_released && g_flickstick_was_in_flick_zone) {
        memset(g_flickstick_turn_smooth_buf, 0, sizeof(g_flickstick_turn_smooth_buf));
        g_flickstick_turn_smooth_idx = 0;
    }

    g_flickstick_prev_stick_angle  = flick_angle;
    g_flickstick_was_in_flick_zone = in_flick_zone || (g_flickstick_was_in_flick_zone && !fully_released);

    constexpr float k_flick_time = 0.1f;
    if (g_flickstick_flick_progress < k_flick_time) {
        float last_t = g_flickstick_flick_progress / k_flick_time;
        g_flickstick_flick_progress = std::min(g_flickstick_flick_progress + rf::frametime, k_flick_time);
        float this_t = g_flickstick_flick_progress / k_flick_time;
        auto warp_ease_out = [](float t) -> float { float f = 1.0f - t; return 1.0f - f * f; };
        yaw_delta += (warp_ease_out(this_t) - warp_ease_out(last_t)) * g_flickstick_flick_size;
    }
}

static void gamepad_apply_joystick(SDL_GamepadAxis cam_x, SDL_GamepadAxis cam_y, float cam_dz,
                                   float zoom_sens, float& yaw_delta, float& pitch_delta)
{
    float rx, ry;
    get_axis_circular(cam_x, cam_y, cam_dz, rx, ry);

    float joy_pitch_sign = g_alpine_game_config.gamepad_joy_invert_y ? 1.0f : -1.0f;
    // Reset flickstick state so switching back to flickstick always starts a fresh flick.
    g_flickstick_was_in_flick_zone = false;
    g_flickstick_flick_size        = 0.0f;
    memset(g_flickstick_turn_smooth_buf, 0, sizeof(g_flickstick_turn_smooth_buf));
    g_flickstick_turn_smooth_idx   = 0;
    yaw_delta   =              rf::frametime * g_alpine_game_config.gamepad_joy_sensitivity * rx * zoom_sens;
    pitch_delta = joy_pitch_sign * rf::frametime * g_alpine_game_config.gamepad_joy_sensitivity * ry * zoom_sens;
}

static void gamepad_apply_gyro(bool has_player_entity, float zoom_sens, float& yaw_delta, float& pitch_delta)
{
    float gyro_pitch, gyro_yaw;
    gyro_get_axis_orientation(gyro_pitch, gyro_yaw);
    gyro_apply_smoothing(gyro_pitch, gyro_yaw);
    gyro_apply_tightening(gyro_pitch, gyro_yaw);

    constexpr float deg2rad = 3.14159265f / 180.0f;
    float sens = g_alpine_game_config.gamepad_gyro_sensitivity * deg2rad * rf::frametime;

    float gyro_zoom_sens = 1.0f;
    if (has_player_entity) {
        if (rf::local_player->fpgun_data.scanning_for_target) {
            gyro_zoom_sens *= g_gamepad_scanner_gyro_sensitivity_value;
        } else {
            float zoom = rf::local_player->fpgun_data.zoom_factor;
            if (zoom > 1.0f) {
                if (g_alpine_game_config.scope_static_sensitivity) {
                    gyro_zoom_sens *= g_gamepad_scope_gyro_sensitivity_value;
                } else {
                    constexpr float zoom_scale = 30.0f;
                    float divisor = (zoom - 1.0f) * g_gamepad_scope_gyro_applied_dynamic_sensitivity_value * zoom_scale;
                    if (divisor > 1.0f) {
                        gyro_zoom_sens /= divisor;
                    }
                }
            }
        }
    }

    float out_yaw   = -gyro_yaw   * sens * gyro_zoom_sens;
    float out_pitch =  gyro_pitch * sens * gyro_zoom_sens;

    if (g_alpine_game_config.gamepad_gyro_invert_y)
        out_pitch = -out_pitch;
    gyro_apply_vh_mixer(out_pitch, out_yaw);

    yaw_delta   += out_yaw;
    pitch_delta += out_pitch;
}

void consume_raw_gamepad_deltas(float& pitch_delta, float& yaw_delta)
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

    // Suppress look input while viewing a security camera
    if (rf::local_player && rf::local_player->view_from_handle != -1) {
        release_movement_keys();
        reset_gamepad_input_state();
        return;
    }

    SDL_GamepadAxis cam_x = g_alpine_game_config.gamepad_swap_sticks ? SDL_GAMEPAD_AXIS_LEFTX  : SDL_GAMEPAD_AXIS_RIGHTX;
    SDL_GamepadAxis cam_y = g_alpine_game_config.gamepad_swap_sticks ? SDL_GAMEPAD_AXIS_LEFTY  : SDL_GAMEPAD_AXIS_RIGHTY;
    float cam_dz          = g_alpine_game_config.gamepad_swap_sticks ? g_alpine_game_config.gamepad_move_deadzone
                                                                       : g_alpine_game_config.gamepad_look_deadzone;

    bool is_scoped_or_scanning = has_player_entity
        && (rf::player_fpgun_is_zoomed(rf::local_player) || rf::local_player->fpgun_data.scanning_for_target);

    update_gamepad_scoped_sensitivities();

    float gamepad_zoom_sens = 1.0f;
    if (has_player_entity) {
        if (rf::local_player->fpgun_data.scanning_for_target) {
            gamepad_zoom_sens *= g_gamepad_scanner_sensitivity_value;
        } else {
            float zoom = rf::local_player->fpgun_data.zoom_factor;
            if (zoom > 1.0f) {
                if (g_alpine_game_config.scope_static_sensitivity) {
                    gamepad_zoom_sens *= g_gamepad_scope_sensitivity_value;
                } else {
                    constexpr float zoom_scale = 30.0f;
                    float divisor = (zoom - 1.0f) * g_gamepad_scope_applied_dynamic_sensitivity_value * zoom_scale;
                    if (divisor > 1.0f) {
                        gamepad_zoom_sens /= divisor;
                    }
                }
            }
        }
    }

    // Use flickstick when not scoped/scanning; joystick while scoped/scanning for consistent aim.
    if (g_alpine_game_config.gamepad_joy_camera && !is_freelook && !is_scoped_or_scanning) {
        gamepad_apply_flickstick(cam_x, cam_y, yaw_delta, pitch_delta);
        yaw_delta   *= gamepad_zoom_sens;
        pitch_delta *= gamepad_zoom_sens;
    } else {
        gamepad_apply_joystick(cam_x, cam_y, cam_dz, gamepad_zoom_sens, yaw_delta, pitch_delta);
    }

    bool allow_gyro = !is_freelook
        && g_motion_sensors_supported
        && g_alpine_game_config.gamepad_gyro_enabled
        && g_alpine_game_config.gamepad_gyro_sensitivity > 0.0f
        && gyro_modifier_is_active();

    if (allow_gyro)
        gamepad_apply_gyro(has_player_entity, gamepad_zoom_sens, yaw_delta, pitch_delta);

    g_camera_gamepad_dx += pitch_delta;
    g_camera_gamepad_dy += yaw_delta;
    pitch_delta = g_camera_gamepad_dx;
    yaw_delta = g_camera_gamepad_dy;
    g_camera_gamepad_dx = 0.0f;
    g_camera_gamepad_dy = 0.0f;
}

void flush_freelook_gamepad_deltas()
{
    if (!is_freelook_camera() || !rf::local_player || !rf::local_player->cam)
        return;
    rf::Entity* cam_entity = rf::local_player->cam->camera_entity;
    if (!cam_entity)
        return;

    float gamepad_pitch = 0.0f, gamepad_yaw = 0.0f;
    consume_raw_gamepad_deltas(gamepad_pitch, gamepad_yaw);
    if (gamepad_pitch == 0.0f && gamepad_yaw == 0.0f)
        return;

    cam_entity->control_data.eye_phb.x += gamepad_pitch;
    cam_entity->control_data.phb.y += gamepad_yaw;
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

        // Inject stick + gyro into vehicle rotation (ci.rot, range ±1.0 like keyboard input).
        if (is_gamepad_input_active() && is_local_player_vehicle(entity)) {
            SDL_GamepadAxis rot_x = g_alpine_game_config.gamepad_swap_sticks ? SDL_GAMEPAD_AXIS_LEFTX  : SDL_GAMEPAD_AXIS_RIGHTX;
            SDL_GamepadAxis rot_y = g_alpine_game_config.gamepad_swap_sticks ? SDL_GAMEPAD_AXIS_LEFTY  : SDL_GAMEPAD_AXIS_RIGHTY;
            float rot_dz          = g_alpine_game_config.gamepad_swap_sticks ? g_alpine_game_config.gamepad_move_deadzone
                                                                              : g_alpine_game_config.gamepad_look_deadzone;
            constexpr float k_vehicle_dz_multiplier = 1.2f;
            float rx = get_axis(rot_x, rot_dz * k_vehicle_dz_multiplier);
            float ry = get_axis(rot_y, rot_dz * k_vehicle_dz_multiplier);
            float joy_pitch_sign = g_alpine_game_config.gamepad_joy_invert_y ? 1.0f : -1.0f;
            // Normalize so that the default sensitivity (2.5) produces 1:1 vehicle scale.
            constexpr float k_default_sens = 2.5f;
            float joy_sens = g_alpine_game_config.gamepad_joy_sensitivity / k_default_sens;
            entity->ai.ci.rot.y += std::clamp(rx * joy_sens, -1.0f, 1.0f);
            entity->ai.ci.rot.x += std::clamp(joy_pitch_sign * ry * joy_sens, -1.0f, 1.0f);

            // 1/90 scale: 90 deg/s gyro = full keyboard deflection at default sensitivity.
            // Normalized by k_default_sens so gyro and joystick sensitivity values are equivalent.
            if (g_motion_sensors_supported && g_alpine_game_config.gamepad_gyro_enabled
                && g_alpine_game_config.gamepad_gyro_vehicle_camera
                && g_alpine_game_config.gamepad_gyro_sensitivity > 0.0f
                && gyro_modifier_is_active()) {
                float gyro_pitch, gyro_yaw;
                gyro_get_axis_orientation(gyro_pitch, gyro_yaw);
                gyro_apply_smoothing(gyro_pitch, gyro_yaw);
                gyro_apply_tightening(gyro_pitch, gyro_yaw);
                gyro_apply_vh_mixer(gyro_pitch, gyro_yaw);

                constexpr float gyro_to_rot = 1.0f / 90.0f;
                float sens = g_alpine_game_config.gamepad_gyro_sensitivity / k_default_sens;
                float pitch_sign = g_alpine_game_config.gamepad_gyro_invert_y ? -1.0f : 1.0f;
                entity->ai.ci.rot.y += std::clamp(-gyro_yaw * gyro_to_rot * sens, -1.0f, 1.0f);
                entity->ai.ci.rot.x += std::clamp(pitch_sign * gyro_pitch * gyro_to_rot * sens, -1.0f, 1.0f);
            }
        }

        physics_simulate_entity_hook.call_target(entity);
    },
};

static bool fpgun_should_scale(rf::Player* player)
{
    if (player != rf::local_player || rf::is_multi)
        return false;
    if (!(g_move_mag > 0.001f && g_move_mag < 0.999f))
        return false;
    if (!(rf::player_fpgun_is_in_state_anim(player, rf::WS_IDLE)
          || rf::player_fpgun_is_in_state_anim(player, rf::WS_RUN)))
        return false;

    if (rf::player_fpgun_action_anim_is_playing(player, rf::WA_FIRE)
        || rf::player_fpgun_action_anim_is_playing(player, rf::WA_ALT_FIRE)
        || rf::player_fpgun_action_anim_is_playing(player, rf::WA_FIRE_FAIL)
        || rf::player_fpgun_action_anim_is_playing(player, rf::WA_DRAW)
        || rf::player_fpgun_action_anim_is_playing(player, rf::WA_HOLSTER)
        || rf::player_fpgun_action_anim_is_playing(player, rf::WA_RELOAD)
        || rf::player_fpgun_action_anim_is_playing(player, rf::WA_JUMP)
        || rf::player_fpgun_action_anim_is_playing(player, rf::WA_CUSTOM_START)
        || rf::player_fpgun_action_anim_is_playing(player, rf::WA_CUSTOM_LEAVE))
    {
        return false;
    }

    return true;
}

static FunHook<void(rf::Player*)> player_fpgun_process_hook{
    0x004AA6D0,
    [](rf::Player* player) {
        bool scale = fpgun_should_scale(player);
        if (scale)
            g_scaling_fpgun_vmesh = true;

        player_fpgun_process_hook.call_target(player);

        if (scale)
            g_scaling_fpgun_vmesh = false;
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

ConsoleCommand2 joy_scope_sens_cmd{
    "joy_scope_sens",
    [](std::optional<float> val) {
        if (val) g_alpine_game_config.set_gamepad_scope_sens_mod(val.value());
        rf::console::print("Gamepad scope sensitivity modifier: {:.4f}", g_alpine_game_config.gamepad_scope_sensitivity_modifier);
    },
    "Set gamepad scope sensitivity modifier (default 0.25)",
    "joy_scope_sens [value]",
};

ConsoleCommand2 joy_scanner_sens_cmd{
    "joy_scanner_sens",
    [](std::optional<float> val) {
        if (val) g_alpine_game_config.set_gamepad_scanner_sens_mod(val.value());
        rf::console::print("Gamepad scanner sensitivity modifier: {:.4f}", g_alpine_game_config.gamepad_scanner_sensitivity_modifier);
    },
    "Set gamepad scanner sensitivity modifier (default 0.25)",
    "joy_scanner_sens [value]",
};

ConsoleCommand2 joy_flickstick_cmd{
    "joy_flickstick",
    [](std::optional<int> val) {
        if (val) g_alpine_game_config.gamepad_joy_camera = val.value() != 0;
        rf::console::print("Joy flick-stick: {}", g_alpine_game_config.gamepad_joy_camera ? "enabled" : "disabled");
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

ConsoleCommand2 joy_rumble_cmd{
    "joy_rumble",
    [](std::optional<float> val) {
        if (val) g_alpine_game_config.gamepad_rumble_intensity = std::clamp(val.value(), 0.0f, 1.0f);
        rf::console::print("Gamepad rumble intensity: {:.2f}", g_alpine_game_config.gamepad_rumble_intensity);
    },
    "Set gamepad rumble intensity 0.0-1.0 (default 1.0)",
    "joy_rumble [value]",
};

ConsoleCommand2 joy_rumble_triggers_cmd{
    "joy_rumble_triggers",
    [](std::optional<float> val) {
        if (!g_trigger_rumble_supported) {
            rf::console::print("Value blocked, gamepad does not support Trigger Rumbles");
            return;
        }
        if (val)
            g_alpine_game_config.gamepad_trigger_rumble_intensity = std::clamp(val.value(), 0.0f, 1.0f);
        rf::console::print("Trigger rumble intensity: {:.2f}", g_alpine_game_config.gamepad_trigger_rumble_intensity);
    },
    "Set gamepad trigger rumble intensity 0.0-1.0 (default 1.0, if supported by controller)",
    "joy_rumble_triggers [value]",
};

ConsoleCommand2 joy_rumble_weapon_cmd{
    "joy_rumble_weapon",
    [](std::optional<int> val) {
        if (val) g_alpine_game_config.gamepad_weapon_rumble_enabled = val.value() != 0;
        rf::console::print("Weapon rumble: {}", g_alpine_game_config.gamepad_weapon_rumble_enabled ? "enabled" : "disabled");
    },
    "Enable/disable weapon rumble (default 1)",
    "joy_rumble_weapon [0|1]",
};

ConsoleCommand2 joy_rumble_environmental_cmd{
    "joy_rumble_environmental",
    [](std::optional<int> val) {
        if (val) g_alpine_game_config.gamepad_environmental_rumble_enabled = val.value() != 0;
        rf::console::print("Environmental rumble: {}", g_alpine_game_config.gamepad_environmental_rumble_enabled ? "enabled" : "disabled");
    },
    "Enable/disable environmental rumble (default 1)",
    "joy_rumble_environmental [0|1]",
};

ConsoleCommand2 joy_rumble_when_primary_cmd{
    "joy_rumble_when_primary",
    [](std::optional<int> val) {
        if (val) g_alpine_game_config.gamepad_rumble_when_primary = val.value() != 0;
        rf::console::print("Gamepad rumble only when gamepad is primary input: {}", g_alpine_game_config.gamepad_rumble_when_primary ? "enabled" : "disabled");
    },
    "Enable/disable rumble only when gamepad is the primary input device (default 1)",
    "joy_rumble_when_primary [0|1]",
};

ConsoleCommand2 joy_rumble_vibration_filter_cmd{
    "joy_rumble_vibration_filter",
    [](std::optional<int> val) {
        if (val) g_alpine_game_config.gamepad_rumble_vibration_filter = std::clamp(val.value(), 0, 2);
        auto mode_name = g_alpine_game_config.gamepad_rumble_vibration_filter == 0 ? "Off" : 
                        g_alpine_game_config.gamepad_rumble_vibration_filter == 1 ? "Auto (reduces low-freq motor while gyro is active)" : "On (always reduce)";
        rf::console::print("Gamepad rumble vibration filter: {} ({})", g_alpine_game_config.gamepad_rumble_vibration_filter, mode_name);
    },
    "Set vibration filter mode 0=Off, 1=Auto (default, low-freq motor while gyro is active), 2=On (reduces low-freq motor)",
    "joy_rumble_vibration_filter [0|1|2]",
};

ConsoleCommand2 gyro_menu_cursor_sens_cmd{
    "gyro_menu_cursor_sens",
    [](std::optional<float> val) {
        if (val) g_alpine_game_config.gamepad_gyro_menu_cursor_sensitivity = std::clamp(val.value(), 0.0f, 30.0f);
        rf::console::print("Gyro menu cursor sensitivity: {:.4f}", g_alpine_game_config.gamepad_gyro_menu_cursor_sensitivity);
    },
    "Set gyro cursor sensitivity for menus (0 = disabled, default 1.0)",
    "gyro_menu_cursor_sens [value]",
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

ConsoleCommand2 gyro_scope_sens_cmd{
    "gyro_scope_sens",
    [](std::optional<float> val) {
        if (val) g_alpine_game_config.set_gamepad_scope_gyro_sens_mod(val.value());
        rf::console::print("Gamepad scope gyro sensitivity modifier: {:.4f}", g_alpine_game_config.gamepad_scope_gyro_sensitivity_modifier);
    },
    "Set gamepad scope gyro sensitivity modifier (default 0.25)",
    "gyro_scope_sens [value]",
};

ConsoleCommand2 gyro_scanner_sens_cmd{
    "gyro_scanner_sens",
    [](std::optional<float> val) {
        if (val) g_alpine_game_config.set_gamepad_scanner_gyro_sens_mod(val.value());
        rf::console::print("Gamepad scanner gyro sensitivity modifier: {:.4f}", g_alpine_game_config.gamepad_scanner_gyro_sensitivity_modifier);
    },
    "Set gamepad scanner gyro sensitivity modifier (default 0.25)",
    "gyro_scanner_sens [value]",
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

ConsoleCommand2 joy_reconnect_cmd{
    "joy_reset",
    [](std::optional<int>) {
        if (!g_gamepad) {
            // No gamepad open — try to pick up any connected one.
            if (SDL_HasGamepad()) {
                int count = 0;
                SDL_JoystickID* ids = SDL_GetGamepads(&count);
                if (ids) {
                    if (count > 0)
                        try_open_gamepad(ids[0]);
                    SDL_free(ids);
                }
            }
            if (g_gamepad)
                rf::console::print("Gamepad reset: opened {}", SDL_GetGamepadName(g_gamepad));
            else
                rf::console::print("Gamepad reset: no gamepad found");
            return;
        }

        SDL_JoystickID prev_id = SDL_GetGamepadID(g_gamepad);
        disconnect_active_gamepad();
        try_open_gamepad(prev_id);

        if (g_gamepad)
            rf::console::print("Gamepad reset: reopened {}", SDL_GetGamepadName(g_gamepad));
        else
            rf::console::print("Gamepad reset: failed to reopen gamepad");
    },
    "Close and reopen the SDL gamepad (re-enables sensors, resets gyro state)",
};

// Returns the secondary (alt) scan code for the action bound to the given primary
// scan code, or -1 if there is no secondary. Used by the binding list renderer.
int gamepad_get_alt_sc_for_primary_sc(int primary_sc)
{
    // Menu-only actions use CTRL_GAMEPAD_MENU_BASE codes and never carry a secondary binding.
    if (primary_sc >= CTRL_GAMEPAD_MENU_BASE && primary_sc < CTRL_GAMEPAD_MENU_BASE + SDL_GAMEPAD_BUTTON_COUNT)
        return -1;

    // Resolve which gameplay action index owns this primary scan code.
    int action = -1;
    int offset = primary_sc - CTRL_GAMEPAD_SCAN_BASE;
    if (offset >= 0 && offset < SDL_GAMEPAD_BUTTON_COUNT)
        action = g_button_map[offset];
    else if (primary_sc == static_cast<int>(CTRL_GAMEPAD_LEFT_TRIGGER))
        action = g_trigger_action[0];
    else if (primary_sc == static_cast<int>(CTRL_GAMEPAD_RIGHT_TRIGGER))
        action = g_trigger_action[1];

    if (action < 0) return -1;

    // Look for an extended-button secondary bound to the same action.
    for (int b = SDL_GAMEPAD_BUTTON_MISC1; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
        if (g_button_map_alt[b] == action)
            return CTRL_GAMEPAD_SCAN_BASE + b;
    return -1;
}

bool gamepad_is_motionsensors_supported()
{
    return g_motion_sensors_supported;
}

bool gamepad_is_trigger_rumble_supported()
{
    return g_trigger_rumble_supported;
}

bool gamepad_is_last_input_gamepad()
{
    if (g_alpine_game_config.input_prompt_override == 1) return true;
    if (g_alpine_game_config.input_prompt_override == 2) return false;
    return g_last_input_was_gamepad;
}

bool gamepad_is_menu_only_action(int action_idx)
{
    return is_menu_only_action(action_idx);
}

void gamepad_set_last_input_keyboard()
{
    g_last_input_was_gamepad = false;
}

int gamepad_get_button_for_action(int action_idx)
{
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
        if (g_button_map[b] == action_idx || g_menu_button_map[b] == action_idx)
            return b;
    return -1;
}

// Returns the primary and secondary button indices for a gameplay action.
// The secondary is the extended-button (paddle/misc/touchpad) secondary binding, if any.
// Either output is set to -1 if not present.
void gamepad_get_buttons_for_action(int action_idx, int* btn_primary, int* btn_secondary)
{
    *btn_primary   = -1;
    *btn_secondary = -1;
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
        if (*btn_primary < 0 && (g_button_map[b] == action_idx || g_menu_button_map[b] == action_idx))
            *btn_primary = b;
        if (*btn_secondary < 0 && g_button_map_alt[b] == action_idx)
            *btn_secondary = b;
    }
}

int gamepad_get_trigger_for_action(int action_idx)
{
    if (g_trigger_action[0] == action_idx || g_menu_trigger_action[0] == action_idx) return 0;
    if (g_trigger_action[1] == action_idx || g_menu_trigger_action[1] == action_idx) return 1;
    return -1;
}

int gamepad_get_button_count()
{
    return SDL_GAMEPAD_BUTTON_COUNT;
}

const char* gamepad_get_scan_code_name(int scan_code)
{
    auto icon_pref = static_cast<ControllerIconType>(g_alpine_game_config.gamepad_icon_override);
    int menu_offset = scan_code - CTRL_GAMEPAD_MENU_BASE;
    if (menu_offset >= 0 && menu_offset < SDL_GAMEPAD_BUTTON_COUNT)
        return gamepad_get_effective_display_name(icon_pref, g_gamepad, menu_offset);
    int offset = scan_code - CTRL_GAMEPAD_SCAN_BASE;
    if (offset >= 0 && offset < SDL_GAMEPAD_BUTTON_COUNT + 2)
        return gamepad_get_effective_display_name(icon_pref, g_gamepad, offset);
    return "<none>";
}

void gamepad_clear_all_bindings()
{
    memset(g_button_map,     -1, sizeof(g_button_map));
    memset(g_button_map_alt, -1, sizeof(g_button_map_alt));
    g_trigger_action[0] = g_trigger_action[1] = -1;
    memset(g_menu_button_map, -1, sizeof(g_menu_button_map));
    g_menu_trigger_action[0] = g_menu_trigger_action[1] = -1;
}

void gamepad_sync_bindings_from_scan_codes()
{
    if (!rf::local_player) return;
    gamepad_clear_all_bindings();
    auto& cc = rf::local_player->settings.controls;
    for (int i = 0; i < cc.num_bindings; ++i) {
        // Primary slot (scan_codes[0])
        {
            int16_t sc = cc.bindings[i].scan_codes[0];
            bool menu_only = is_menu_only_action(i);

            int menu_offset = static_cast<int>(sc) - CTRL_GAMEPAD_MENU_BASE;
            if (menu_only && menu_offset >= 0 && menu_offset < SDL_GAMEPAD_BUTTON_COUNT) {
                if (menu_offset != SDL_GAMEPAD_BUTTON_START)
                    g_menu_button_map[menu_offset] = i;
            }
            else {
                int offset = static_cast<int>(sc) - CTRL_GAMEPAD_SCAN_BASE;
                if (offset >= 0 && offset < SDL_GAMEPAD_BUTTON_COUNT) {
                    if (offset != SDL_GAMEPAD_BUTTON_START) { // Start is reserved, never rebindable
                        if (menu_only)
                            g_menu_button_map[offset] = i;
                        else
                            g_button_map[offset] = i;
                    }
                }
                else if (sc == static_cast<int16_t>(CTRL_GAMEPAD_LEFT_TRIGGER)) {
                    if (menu_only) g_menu_trigger_action[0] = i;
                    else           g_trigger_action[0] = i;
                }
                else if (sc == static_cast<int16_t>(CTRL_GAMEPAD_RIGHT_TRIGGER)) {
                    if (menu_only) g_menu_trigger_action[1] = i;
                    else           g_trigger_action[1] = i;
                }
            }
        }
        // Secondary slot (scan_codes[1]) — extended-button secondary for gameplay actions only.
        if (!is_menu_only_action(i)) {
            int16_t sc1 = cc.bindings[i].scan_codes[1];
            int offset1 = static_cast<int>(sc1) - CTRL_GAMEPAD_SCAN_BASE;
            if (offset1 >= SDL_GAMEPAD_BUTTON_MISC1 && offset1 < SDL_GAMEPAD_BUTTON_COUNT)
                g_button_map_alt[offset1] = i;
        }
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

    auto new_code = g_rebind_pending_sc >= 0 ? static_cast<int16_t>(g_rebind_pending_sc) : int16_t{-1};
    g_rebind_pending_sc = -1;

    for (int i = 0; i < cc.num_bindings; ++i) {
        if (cc.bindings[i].scan_codes[0] != CTRL_REBIND_SENTINEL)
            continue;

        if (new_code != -1) {
            bool target_is_menu_only = is_menu_only_action(i);
            int new_offset = static_cast<int>(new_code) - CTRL_GAMEPAD_SCAN_BASE;
            bool new_is_extended = (new_offset >= SDL_GAMEPAD_BUTTON_MISC1 && new_offset < SDL_GAMEPAD_BUTTON_COUNT);

            // Menu-only actions use the CTRL_GAMEPAD_MENU_BASE scan-code namespace so they are
            // never confused with gameplay actions that share the same physical button.
            if (target_is_menu_only && new_offset >= 0 && new_offset < SDL_GAMEPAD_BUTTON_COUNT)
                new_code = static_cast<int16_t>(CTRL_GAMEPAD_MENU_BASE + new_offset);

            // For gameplay actions: if binding an extended button (paddle/misc/touchpad) and this
            // action already has a standard primary in g_button_map OR a trigger, store as
            // secondary instead of replacing the primary.
            if (new_is_extended && !target_is_menu_only) {
                // Check standard buttons first.
                int existing_primary = -1;
                for (int b = 0; b < SDL_GAMEPAD_BUTTON_MISC1; ++b)
                    if (g_button_map[b] == i) { existing_primary = b; break; }

                // Also check triggers — they can be the primary for this action.
                int existing_trigger = -1; // 0 = LT, 1 = RT
                if (g_trigger_action[0] == i)       existing_trigger = 0;
                else if (g_trigger_action[1] == i)  existing_trigger = 1;

                if (existing_primary >= 0 || existing_trigger >= 0) {
                    // Conflict-clear this extended button from other actions' secondary slots.
                    for (int j = 0; j < cc.num_bindings; ++j)
                        if (j != i && cc.bindings[j].scan_codes[1] == new_code)
                            cc.bindings[j].scan_codes[1] = -1;
                    // Determine the scan code that represents the existing primary.
                    int16_t primary_sc;
                    if (existing_primary >= 0)
                        primary_sc = static_cast<int16_t>(CTRL_GAMEPAD_SCAN_BASE + existing_primary);
                    else
                        primary_sc = (existing_trigger == 0)
                            ? static_cast<int16_t>(CTRL_GAMEPAD_LEFT_TRIGGER)
                            : static_cast<int16_t>(CTRL_GAMEPAD_RIGHT_TRIGGER);
                    // Restore sc[0] to the known primary and set sc[1] as secondary.
                    cc.bindings[i].scan_codes[0] = primary_sc;
                    cc.bindings[i].scan_codes[1] = new_code;
                    break;
                }
            }

            // Standard primary rebind (or extended button with no existing standard primary).
            for (int j = 0; j < cc.num_bindings; ++j) {
                if (j == i) continue;
                // Clear from primary if the same binding context.
                if (cc.bindings[j].scan_codes[0] == new_code
                    && is_menu_only_action(j) == target_is_menu_only) {
                    cc.bindings[j].scan_codes[0] = -1;
                    // If j still has an extended secondary, promote it to primary now — otherwise
                    // it would become orphaned (secondary with no primary → shows as empty in UI).
                    int16_t sc1_j = cc.bindings[j].scan_codes[1];
                    int off1_j    = static_cast<int>(sc1_j) - CTRL_GAMEPAD_SCAN_BASE;
                    if (sc1_j != -1
                        && off1_j >= SDL_GAMEPAD_BUTTON_MISC1 && off1_j < SDL_GAMEPAD_BUTTON_COUNT
                        && !is_menu_only_action(j)) {
                        cc.bindings[j].scan_codes[0] = sc1_j;
                        cc.bindings[j].scan_codes[1] = -1;
                    }
                }
                // Always clear from secondary slots to avoid a button appearing in two places.
                if (cc.bindings[j].scan_codes[1] == new_code)
                    cc.bindings[j].scan_codes[1] = -1;
            }
            // If the target action itself already holds new_code as its secondary (e.g. the user
            // presses the same extended button again after its primary was moved away), clear the
            // secondary to prevent "Mic / Mic" after sc[0] is written below.
            if (cc.bindings[i].scan_codes[1] == new_code)
                cc.bindings[i].scan_codes[1] = -1;
        }

        cc.bindings[i].scan_codes[0] = new_code;
        // When clearing the primary binding, also clear any secondary.
        if (new_code == -1)
            cc.bindings[i].scan_codes[1] = -1;

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
    if (is_menu_only_action(action_idx))
        g_menu_button_map[button_idx] = action_idx;
    else
        g_button_map[button_idx] = action_idx;
}

int gamepad_get_button_alt_binding(int button_idx)
{
    if (button_idx < 0 || button_idx >= SDL_GAMEPAD_BUTTON_COUNT) return -1;
    return g_button_map_alt[button_idx];
}

void gamepad_set_button_alt_binding(int button_idx, int action_idx)
{
    if (button_idx < 0 || button_idx >= SDL_GAMEPAD_BUTTON_COUNT) return;
    // Secondary/alt bindings are only for gameplay (non-menu) actions.
    if (!is_menu_only_action(action_idx))
        g_button_map_alt[button_idx] = action_idx;
}

int gamepad_get_trigger_action(int trigger_idx)
{
    if (trigger_idx < 0 || trigger_idx > 1) return -1;
    return g_trigger_action[trigger_idx];
}

void gamepad_set_trigger_action(int trigger_idx, int action_idx)
{
    if (trigger_idx < 0 || trigger_idx > 1) return;
    if (is_menu_only_action(action_idx))
        g_menu_trigger_action[trigger_idx] = action_idx;
    else
        g_trigger_action[trigger_idx] = action_idx;
}

void gamepad_reset_to_defaults()
{
    memset(g_button_map, -1, sizeof(g_button_map));
    memset(g_button_map_alt, -1, sizeof(g_button_map_alt));
    memset(g_menu_button_map, -1, sizeof(g_menu_button_map));
    g_trigger_action[0] = g_trigger_action[1] = -1;
    g_menu_trigger_action[0] = g_menu_trigger_action[1] = -1;

    g_button_map[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = rf::CC_ACTION_PRIMARY_ATTACK;
    g_button_map[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER]  = rf::CC_ACTION_JUMP;
    g_button_map[SDL_GAMEPAD_BUTTON_SOUTH]          = rf::CC_ACTION_USE;
    g_button_map[SDL_GAMEPAD_BUTTON_NORTH]          = rf::CC_ACTION_RELOAD;
    g_button_map[SDL_GAMEPAD_BUTTON_EAST]           = rf::CC_ACTION_NEXT_WEAPON;
    g_button_map[SDL_GAMEPAD_BUTTON_WEST]           = rf::CC_ACTION_PREV_WEAPON;
    g_button_map[SDL_GAMEPAD_BUTTON_DPAD_LEFT]      = rf::CC_ACTION_HIDE_WEAPON;
    g_button_map[SDL_GAMEPAD_BUTTON_DPAD_RIGHT]     = rf::CC_ACTION_MESSAGES;
    g_menu_button_map[SDL_GAMEPAD_BUTTON_DPAD_DOWN] = static_cast<int>(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_CENTER_VIEW));
    g_menu_button_map[SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = rf::CC_ACTION_MP_STATS;
    g_menu_button_map[SDL_GAMEPAD_BUTTON_BACK]      = static_cast<int>(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_SKIP_CUTSCENE));

    // Spectator / multiplayer-only actions
    g_menu_button_map[SDL_GAMEPAD_BUTTON_EAST]      = static_cast<int>(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_TOGGLE_FREELOOK));
    g_menu_button_map[SDL_GAMEPAD_BUTTON_WEST]      = static_cast<int>(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_TOGGLE));
    g_menu_button_map[SDL_GAMEPAD_BUTTON_NORTH]     = static_cast<int>(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_MENU));

    // Trigger defaults (gameplay action)
    g_trigger_action[0] = rf::CC_ACTION_CROUCH;
    g_trigger_action[1] = rf::CC_ACTION_SECONDARY_ATTACK;
}

void gamepad_apply_patch()
{
    gamepad_reset_to_defaults();

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
    joy_scope_sens_cmd.register_cmd();
    joy_scanner_sens_cmd.register_cmd();
    gyro_scope_sens_cmd.register_cmd();
    gyro_scanner_sens_cmd.register_cmd();
    joy_flickstick_cmd.register_cmd();
    joy_flickstick_sweep_cmd.register_cmd();
    joy_flickstick_smoothing_cmd.register_cmd();
    joy_flickstick_deadzone_cmd.register_cmd();
    joy_flickstick_release_deadzone_cmd.register_cmd();
    joy_rumble_cmd.register_cmd();
    joy_rumble_triggers_cmd.register_cmd();
    joy_rumble_weapon_cmd.register_cmd();
    joy_rumble_environmental_cmd.register_cmd();
    joy_rumble_when_primary_cmd.register_cmd();
    joy_rumble_vibration_filter_cmd.register_cmd();
    gyro_sens_cmd.register_cmd();
    gyro_menu_cursor_sens_cmd.register_cmd();
    gyro_camera_cmd.register_cmd();
    gyro_vehicle_camera_cmd.register_cmd();
    input_prompts_cmd.register_cmd();
    gamepad_prompts_cmd.register_cmd();
    joy_reconnect_cmd.register_cmd();
    gyro_apply_patch();
}

static void gamepad_msg_handler(UINT msg, WPARAM w_param, LPARAM)
{
    if (msg != WM_ACTIVATEAPP || w_param)
        return;
    // Focus lost: release all gamepad input so nothing stays held while unfocused.
    if (g_gamepad) {
        release_movement_keys();
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
            inject_action_key(g_button_map[b], false);
            inject_action_key(g_button_map_alt[b], false);
        }
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

void gamepad_rumble(uint16_t low_freq, uint16_t high_freq, uint32_t duration_ms)
{
    if (!g_gamepad || !g_rumble_supported)
        return;
    if (g_alpine_game_config.gamepad_rumble_when_primary && !g_last_input_was_gamepad)
        return;
    if (g_alpine_game_config.gamepad_rumble_intensity <= 0.0f)
        return;
    low_freq = static_cast<uint16_t>(low_freq * g_alpine_game_config.gamepad_rumble_intensity);
    high_freq = static_cast<uint16_t>(high_freq * g_alpine_game_config.gamepad_rumble_intensity);
    int filter_mode = g_alpine_game_config.gamepad_rumble_vibration_filter;
    if (filter_mode == 2 || (filter_mode == 1 && g_motion_sensors_supported && g_alpine_game_config.gamepad_gyro_enabled))
        low_freq = static_cast<uint16_t>(low_freq * 0.02f);
    SDL_RumbleGamepad(g_gamepad, low_freq, high_freq, duration_ms);
}

void gamepad_play_rumble(const RumbleEffect& effect, bool is_alt_fire)
{
    if (!g_gamepad)
        return;

    // No trigger motor requested — plain body rumble.
    if (!effect.trigger_motor || g_alpine_game_config.gamepad_trigger_rumble_intensity <= 0.0f) {
        gamepad_rumble(effect.lo_motor, effect.hi_motor, effect.duration_ms);
        return;
    }

    constexpr int primary_idx   = static_cast<int>(rf::CC_ACTION_PRIMARY_ATTACK);
    constexpr int secondary_idx = static_cast<int>(rf::CC_ACTION_SECONDARY_ATTACK);

    // Resolve which trigger (if any) each fire action is bound to.
    // g_trigger_action[0] = Left Trigger,  g_trigger_action[1] = Right Trigger.
    bool primary_on_lt   = g_trigger_action[0] == primary_idx;
    bool primary_on_rt   = g_trigger_action[1] == primary_idx;
    bool secondary_on_lt = g_trigger_action[0] == secondary_idx;
    bool secondary_on_rt = g_trigger_action[1] == secondary_idx;

    // Check which fire action is active this frame or was active last frame.
    bool primary_active   = primary_idx   < k_action_count && (g_action_curr[primary_idx]   || g_action_prev[primary_idx]);
    bool secondary_active;
    if (is_alt_fire && (secondary_on_lt || secondary_on_rt)) {
        // Alt-fire confirmed by next_fire_secondary advancement: the secondary attack action
        // is responsible regardless of current button state.
        secondary_active = true;
    } else {
        secondary_active = secondary_idx < k_action_count && (g_action_curr[secondary_idx] || g_action_prev[secondary_idx]);
    }

    bool use_lt = (primary_active && primary_on_lt) || (secondary_active && secondary_on_lt);
    bool use_rt = (primary_active && primary_on_rt) || (secondary_active && secondary_on_rt);

    if (!use_lt && !use_rt) {
        // No fire action is bound to a trigger — fall back to standard body rumble.
        gamepad_rumble(effect.lo_motor, effect.hi_motor, effect.duration_ms);
        return;
    }

    // Route to the trigger motor(s) matching the active fire binding.
    uint16_t lt_motor = use_lt ? static_cast<uint16_t>(effect.trigger_motor * g_alpine_game_config.gamepad_trigger_rumble_intensity) : 0;
    uint16_t rt_motor = use_rt ? static_cast<uint16_t>(effect.trigger_motor * g_alpine_game_config.gamepad_trigger_rumble_intensity) : 0;
    if (!SDL_RumbleGamepadTriggers(g_gamepad, lt_motor, rt_motor, effect.duration_ms))
        gamepad_rumble(effect.lo_motor, effect.hi_motor, effect.duration_ms);
}

void gamepad_stop_rumble()
{
    if (!g_gamepad)
        return;
    SDL_RumbleGamepad(g_gamepad, 0, 0, 0);
    if (g_trigger_rumble_supported)
        SDL_RumbleGamepadTriggers(g_gamepad, 0, 0, 0);
}

void gamepad_sdl_init()
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3_SIXAXIS_DRIVER, "1");
    if (g_alpine_system_config.gamepad_rawinput_enabled) {
        SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "1");
        SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT_CORRELATE_XINPUT, "1");
    } else {
        SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT, "0");
        SDL_SetHint(SDL_HINT_JOYSTICK_RAWINPUT_CORRELATE_XINPUT, "0");
    }

    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        xlog::error("Failed to initialize SDL gamepad subsystem: {}", SDL_GetError());
        return;
    }

    // Load SDL_GameControllerDB
    // note: this might not work right now...
    for (const auto& dir : {get_module_dir(g_hmodule), get_module_dir(nullptr)}) {
        std::string path = dir + "gamecontrollerdb.txt";
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            continue;
        int n = SDL_AddGamepadMappingsFromFile(path.c_str());
        if (n < 0)
            xlog::warn("SDL_GameControllerDB: failed to load {}: {}", path, SDL_GetError());
        else
            xlog::info("SDL_GameControllerDB: loaded {} mappings from {}", n, path);
        break;
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
    // Flush ADDED events from subsystem init during gamepad connection
    SDL_FlushEvents(SDL_EVENT_GAMEPAD_ADDED, SDL_EVENT_GAMEPAD_ADDED);

    rf::os_add_msg_handler(gamepad_msg_handler);
    xlog::info("Gamepad support initialized");
}
