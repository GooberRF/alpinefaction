#include "gamepad.h"
#include <algorithm>
#include <cmath>
#include <optional>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <xlog/xlog.h>
#include "../os/console.h"
#include "../rf/input.h"
#include "../rf/player/player.h"
#include "../rf/player/camera.h"
#include "../rf/player/control_config.h"
#include "../rf/player/player_fpgun.h"
#include "../rf/vmesh.h"
#include "../rf/entity.h"
#include "../rf/os/frametime.h"
#include "../rf/gameseq.h"
#include "../misc/alpine_settings.h"
#include <SDL3/SDL.h>
#include "gyro.h"

static SDL_Gamepad* g_gamepad = nullptr;
static bool g_motion_sensors_active = false; // true only when the connected gamepad has gyro+accel and both are enabled

// Latest sensor samples — latched from SDL events, consumed once per frame.
static float g_gyro_x = 0.0f, g_gyro_y = 0.0f, g_gyro_z = 0.0f;   // deg/s
static float g_accel_x = 0.0f, g_accel_y = 0.0f, g_accel_z = 0.0f; // g-force
static bool g_gyro_fresh = false;

// Button → action mapping: indexed by SDL_GamepadButton, value is rf::ControlConfigAction, -1 = unbound.
static int g_button_map[SDL_GAMEPAD_BUTTON_COUNT];

// Per-frame action state, indexed by rf::ControlConfigAction.
static constexpr int k_action_count = static_cast<int>(rf::CC_ACTION_QUICK_LOAD) + 1;
static bool g_action_prev[k_action_count] = {};
static bool g_action_curr[k_action_count] = {};

// Normalize an axis value, strip the deadzone band, and rescale the remainder to [-1, 1].
static float get_axis(SDL_GamepadAxis axis, float deadzone)
{
    if (!g_gamepad) return 0.0f;
    float v = SDL_GetGamepadAxis(g_gamepad, axis) / (float)SDL_MAX_SINT16;
    if (v >  deadzone) return (v - deadzone) / (1.0f - deadzone);
    if (v < -deadzone) return (v + deadzone) / (1.0f - deadzone);
    return 0.0f;
}

//  Left stick movement 
static float g_move_lx = 0.0f, g_move_ly = 0.0f;
static float g_move_mag = 0.0f;
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

static void update_trigger_actions()
{
    float rt = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / (float)SDL_MAX_SINT16;
    float lt = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)  / (float)SDL_MAX_SINT16;
    g_action_curr[rf::CC_ACTION_SECONDARY_ATTACK] = rt > 0.5f;
    g_action_curr[rf::CC_ACTION_CROUCH]           = lt > 0.5f;
}

static void set_movement_key(rf::ControlConfigAction action, bool down)
{
    int idx = static_cast<int>(action);
    if (g_action_curr[idx] == down) return;
    if (rf::local_player && !rf::console::console_is_visible()) {
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

    if (!rf::gameseq_in_gameplay()) {
        release_movement_keys();
        return;
    }

    float lx = get_axis(SDL_GAMEPAD_AXIS_LEFTX, g_alpine_game_config.gamepad_move_deadzone);
    float ly = get_axis(SDL_GAMEPAD_AXIS_LEFTY, g_alpine_game_config.gamepad_move_deadzone);

    g_move_lx = lx;
    g_move_ly = ly;
    g_move_mag = std::min(1.0f, std::sqrt(lx * lx + ly * ly));

    set_movement_key(rf::CC_ACTION_FORWARD,     ly < 0.0f);
    set_movement_key(rf::CC_ACTION_BACKWARD,    ly > 0.0f);
    set_movement_key(rf::CC_ACTION_SLIDE_LEFT,  lx < 0.0f);
    set_movement_key(rf::CC_ACTION_SLIDE_RIGHT, lx > 0.0f);
}

void gamepad_do_frame()
{
    memcpy(g_action_prev, g_action_curr, sizeof(g_action_curr));
    SDL_UpdateGamepads();
    SDL_Event ev;
    while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_EVENT_FIRST, SDL_EVENT_LAST) > 0) {
        switch (ev.type) {
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!g_gamepad)
                try_open_gamepad(ev.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (g_gamepad && SDL_GetGamepadID(g_gamepad) == ev.gdevice.which) {
                xlog::info("Gamepad disconnected");
                release_movement_keys(); // send key-up events before clearing state
                SDL_CloseGamepad(g_gamepad);
                g_gamepad             = nullptr;
                g_motion_sensors_active = false;
                g_gyro_x = g_gyro_y = g_gyro_z = 0.0f;
                g_accel_x = g_accel_y = g_accel_z = 0.0f;
                g_gyro_fresh = false;
                memset(g_action_curr, 0, sizeof(g_action_curr));
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (!g_gamepad || SDL_GetGamepadID(g_gamepad) != ev.gbutton.which) break;
            if (ev.gbutton.button == SDL_GAMEPAD_BUTTON_START) {
                rf::key_process_event(rf::KEY_ESC, 1, 0);
                rf::key_process_event(rf::KEY_ESC, 0, 0);
            }
            if (ev.gbutton.button < SDL_GAMEPAD_BUTTON_COUNT && g_button_map[ev.gbutton.button] >= 0)
                g_action_curr[g_button_map[ev.gbutton.button]] = true;
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            if (!g_gamepad || SDL_GetGamepadID(g_gamepad) != ev.gbutton.which) break;
            if (ev.gbutton.button < SDL_GAMEPAD_BUTTON_COUNT && g_button_map[ev.gbutton.button] >= 0)
                g_action_curr[g_button_map[ev.gbutton.button]] = false;
            break;
        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
            if (!g_motion_sensors_active || SDL_GetGamepadID(g_gamepad) != ev.gsensor.which) break;
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

    if (g_gamepad) {
        update_trigger_actions();
        update_stick_movement();
        g_local_player_body_vmesh = rf::local_player ? rf::get_player_entity_parent_vmesh(rf::local_player) : nullptr;

        if (g_motion_sensors_active && g_alpine_game_config.gamepad_gyro_enabled && g_gyro_fresh) {
            gyro_process_motion(g_gyro_x, g_gyro_y, g_gyro_z,
                                g_accel_x, g_accel_y, g_accel_z, rf::frametime);
            g_gyro_fresh = false;
        }
    }
}

void gamepad_get_camera(float& pitch_delta, float& yaw_delta)
{
    pitch_delta = 0.0f;
    yaw_delta   = 0.0f;

    if (!g_gamepad || !rf::keep_mouse_centered)
        return;

    float rx = get_axis(SDL_GAMEPAD_AXIS_RIGHTX, g_alpine_game_config.gamepad_look_deadzone);
    float ry = get_axis(SDL_GAMEPAD_AXIS_RIGHTY, g_alpine_game_config.gamepad_look_deadzone);

    yaw_delta   =  rf::frametime * g_alpine_game_config.gamepad_joy_sensitivity * rx;
    pitch_delta = -rf::frametime * g_alpine_game_config.gamepad_joy_sensitivity * ry;

    if (g_motion_sensors_active && g_alpine_game_config.gamepad_gyro_enabled
                                && g_alpine_game_config.gamepad_gyro_sensitivity > 0.0f) {
        float gyro_pitch, gyro_yaw;
        gyro_get_axis_orientation(gyro_pitch, gyro_yaw);
        gyro_apply_smoothing(gyro_pitch, gyro_yaw);
        gyro_apply_tightening(gyro_pitch, gyro_yaw);

        constexpr float deg2rad = 3.14159265f / 180.0f;
        float pitch_sign = g_alpine_game_config.gamepad_gyro_invert_y ? -1.0f : 1.0f;
        yaw_delta   -= gyro_yaw   * deg2rad * g_alpine_game_config.gamepad_gyro_sensitivity * rf::frametime;
        pitch_delta += pitch_sign * gyro_pitch * deg2rad * g_alpine_game_config.gamepad_gyro_sensitivity * rf::frametime;
    }
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

FunHook<void(rf::Entity*)> physics_simulate_entity_hook{
    0x0049F3C0,
    [](rf::Entity* entity) {
        if (g_gamepad && entity == rf::local_player_entity && g_move_mag > 0.001f) {
            if (rf::is_multi) {
                float inv_mag = 1.0f / g_move_mag;
                entity->ai.ci.move.x = g_move_lx * inv_mag;
                entity->ai.ci.move.z = -g_move_ly * inv_mag;
            } else {
                entity->ai.ci.move.x = g_move_lx;
                entity->ai.ci.move.z = -g_move_ly;
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

ConsoleCommand2 gyro_sens_cmd{
    "gyro_sens",
    [](std::optional<float> val) {
        if (val) g_alpine_game_config.gamepad_gyro_sensitivity = std::max(0.0f, val.value());
        rf::console::print("Gyro sensitivity: {:.4f}", g_alpine_game_config.gamepad_gyro_sensitivity);
    },
    "Set gyro sensitivity (default 2.5)",
    "gyro_sens [value]",
};

ConsoleCommand2 gyro_camera_cmd{
    "gyro_camera",
    [](std::optional<int> val) {
        if (val) g_alpine_game_config.gamepad_gyro_enabled = val.value() != 0;
        rf::console::print("Gyro camera: {}", g_alpine_game_config.gamepad_gyro_enabled ? "enabled" : "disabled");
    },
    "Enable/disable gyro camera (default 1)",
    "gyro_camera [0|1]",
};


bool gamepad_is_motionsensors_supported()
{
    return g_motion_sensors_active;
}

void gamepad_apply_patch()
{
    // Default button → action bindings. All unbound slots stay -1.
    memset(g_button_map, -1, sizeof(g_button_map));
    g_button_map[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = rf::CC_ACTION_PRIMARY_ATTACK;
    g_button_map[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER]  = rf::CC_ACTION_JUMP;
    g_button_map[SDL_GAMEPAD_BUTTON_SOUTH]          = rf::CC_ACTION_USE;
    g_button_map[SDL_GAMEPAD_BUTTON_NORTH]          = rf::CC_ACTION_RELOAD;
    g_button_map[SDL_GAMEPAD_BUTTON_EAST]           = rf::CC_ACTION_NEXT_WEAPON;
    g_button_map[SDL_GAMEPAD_BUTTON_WEST]           = rf::CC_ACTION_PREV_WEAPON;
    g_button_map[SDL_GAMEPAD_BUTTON_DPAD_LEFT]      = rf::CC_ACTION_HIDE_WEAPON;
    g_button_map[SDL_GAMEPAD_BUTTON_DPAD_RIGHT]     = rf::CC_ACTION_MESSAGES;
    // SDL_GAMEPAD_BUTTON_BACK, DPAD_UP, DPAD_DOWN, LEFT_STICK, RIGHT_STICK,
    // MISC1, RIGHT_PADDLE1/2, LEFT_PADDLE1/2, TOUCHPAD, MISC2-6 — unbound by default.

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

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

    control_is_control_down_hook.install();
    control_config_check_pressed_hook.install();
    physics_simulate_entity_hook.install();
    player_fpgun_process_hook.install();
    vmesh_process_hook.install();
    joy_sens_cmd.register_cmd();
    joy_move_deadzone_cmd.register_cmd();
    joy_look_deadzone_cmd.register_cmd();
    gyro_sens_cmd.register_cmd();
    gyro_camera_cmd.register_cmd();
    gyro_apply_patch();
    xlog::info("Gamepad support initialized");
}
