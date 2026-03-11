#include "gamepad.h"
#include <algorithm>
#include <optional>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <xlog/xlog.h>
#include "../os/console.h"
#include "../rf/input.h"
#include "../rf/player/player.h"
#include "../rf/player/camera.h"
#include "../rf/player/control_config.h"
#include "../rf/entity.h"
#include "../rf/os/frametime.h"
#include "../misc/alpine_settings.h"
#include <SDL3/SDL.h>
#include <GamepadMotion.hpp>

static SDL_Gamepad* g_gamepad = nullptr;

// GamepadMotionHelpers instance for sensor fusion and calibration
static GamepadMotion g_gamepad_motion;

static bool g_gyro_enabled = false;
static bool g_accel_enabled = false;

// Latest sensor samples - accumulated from events, processed once per frame
static float g_gyro_x = 0.0f;  // degrees/sec
static float g_gyro_y = 0.0f;
static float g_gyro_z = 0.0f;
static float g_accel_x = 0.0f; // g-force
static float g_accel_y = 0.0f;
static float g_accel_z = 0.0f;

// Button mapping: indexed by SDL_GamepadButton, value is rf::ControlConfigAction cast to int, -1 = unbound
static int g_button_map[SDL_GAMEPAD_BUTTON_COUNT];

// Gameplay action layer state, indexed by rf::ControlConfigAction.
static constexpr int k_action_count = static_cast<int>(rf::CC_ACTION_QUICK_LOAD) + 1;
static bool g_action_prev[k_action_count] = {};
static bool g_action_curr[k_action_count] = {};

// Normalize an axis value, strip the deadzone band, and rescale the remainder to [0, 1].
static float get_axis(SDL_GamepadAxis axis, float deadzone)
{
    if (!g_gamepad) return 0.0f;
    float v = SDL_GetGamepadAxis(g_gamepad, axis) / (float)SDL_MAX_SINT16;
    if (v >  deadzone) return (v - deadzone) / (1.0f - deadzone);
    if (v < -deadzone) return (v + deadzone) / (1.0f - deadzone);
    return 0.0f;
}

static bool action_is_down(rf::ControlConfigAction action)
{
    int i = static_cast<int>(action);
    return i >= 0 && i < k_action_count && g_action_curr[i];
}

static bool action_just_pressed(rf::ControlConfigAction action)
{
    int i = static_cast<int>(action);
    return i >= 0 && i < k_action_count && g_action_curr[i] && !g_action_prev[i];
}

static void try_open_gamepad(SDL_JoystickID id)
{
    g_gamepad = SDL_OpenGamepad(id);
    if (g_gamepad) {
        xlog::info("Gamepad connected: {}", SDL_GetGamepadName(g_gamepad));
        
        // Enable sensors if available
        g_gyro_enabled = SDL_SetGamepadSensorEnabled(g_gamepad, SDL_SENSOR_GYRO, true);
        g_accel_enabled = SDL_SetGamepadSensorEnabled(g_gamepad, SDL_SENSOR_ACCEL, true);
        
        if (g_gyro_enabled) {
            xlog::info("Gyro sensor enabled");
        }
        if (g_accel_enabled) {
            xlog::info("Accelerometer sensor enabled");
        }
        
        // Reset GamepadMotion for new controller
        g_gamepad_motion.Reset();
    } else {
        xlog::warn("Failed to open gamepad: {}", SDL_GetError());
    }
}

void gamepad_do_frame()
{
    memcpy(g_action_prev, g_action_curr, sizeof(g_action_curr));
    SDL_UpdateGamepads();

    SDL_Event ev;
    while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT,
                          SDL_EVENT_GAMEPAD_AXIS_MOTION,
                          SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED) > 0) {
        switch (ev.type) {
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!g_gamepad)
                try_open_gamepad(ev.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (g_gamepad && SDL_GetGamepadID(g_gamepad) == ev.gdevice.which) {
                xlog::info("Gamepad disconnected");
                SDL_CloseGamepad(g_gamepad);
                g_gamepad = nullptr;
                g_gyro_enabled = false;
                g_accel_enabled = false;
                g_gyro_x = g_gyro_y = g_gyro_z = 0.0f;
                g_accel_x = g_accel_y = g_accel_z = 0.0f;
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
            if (!g_gamepad || SDL_GetGamepadID(g_gamepad) != ev.gsensor.which) break;
            if (ev.gsensor.sensor == SDL_SENSOR_GYRO && g_gyro_enabled) {
                g_gyro_x = ev.gsensor.data[0] * (180.0f / 3.14159265f);
                g_gyro_y = ev.gsensor.data[1] * (180.0f / 3.14159265f);
                g_gyro_z = ev.gsensor.data[2] * (180.0f / 3.14159265f);
            } else if (ev.gsensor.sensor == SDL_SENSOR_ACCEL && g_accel_enabled) {
                g_accel_x = ev.gsensor.data[0] / SDL_STANDARD_GRAVITY;
                g_accel_y = ev.gsensor.data[1] / SDL_STANDARD_GRAVITY;
                g_accel_z = ev.gsensor.data[2] / SDL_STANDARD_GRAVITY;
            }
            break;
        }
    }

    // Axis-driven actions: evaluated each frame after the prev snapshot.
    if (g_gamepad) {
        float rt = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) / (float)SDL_MAX_SINT16;
        float lt = SDL_GetGamepadAxis(g_gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)  / (float)SDL_MAX_SINT16;
        g_action_curr[rf::CC_ACTION_SECONDARY_ATTACK] = rt > 0.5f;
        g_action_curr[rf::CC_ACTION_CROUCH]           = lt > 0.5f;

        // Left stick movement: RF's control loop checks key_is_down() directly, so we
        // synthesize key events through RF's own key state system using the scan codes
        // the player has bound to each movement action.
        float ly = get_axis(SDL_GAMEPAD_AXIS_LEFTY, g_alpine_game_config.gamepad_move_deadzone);
        float lx = get_axis(SDL_GAMEPAD_AXIS_LEFTX, g_alpine_game_config.gamepad_move_deadzone);

        if (rf::local_player) {
            auto& bindings = rf::local_player->settings.controls.bindings;
            // 8-way directional overlay: each cardinal direction spans a 135° arc so
            // diagonals (NE/NW/SE/SW) activate both adjacent cardinal keys simultaneously.
            // tan(67.5°) ≈ 2.414 is the axis-ratio threshold between pure and diagonal sectors.
            static constexpr float k_8way = 2.414f;
            bool go_fwd   = ly < 0.0f && std::abs(lx) < std::abs(ly) * k_8way;
            bool go_back  = ly > 0.0f && std::abs(lx) < std::abs(ly) * k_8way;
            bool go_right = lx > 0.0f && std::abs(ly) < std::abs(lx) * k_8way;
            bool go_left  = lx < 0.0f && std::abs(ly) < std::abs(lx) * k_8way;
            struct { rf::ControlConfigAction action; bool now; } moves[] = {
                { rf::CC_ACTION_FORWARD,     go_fwd   },
                { rf::CC_ACTION_BACKWARD,    go_back  },
                { rf::CC_ACTION_SLIDE_LEFT,  go_left  },
                { rf::CC_ACTION_SLIDE_RIGHT, go_right },
            };
            for (auto& m : moves) {
                int idx = static_cast<int>(m.action);
                bool was = g_action_curr[idx];
                if (m.now != was) {
                    int16_t sc = bindings[idx].scan_codes[0];
                    if (sc >= 0)
                        rf::key_process_event(sc, m.now ? 1 : 0, 0);
                    g_action_curr[idx] = m.now;
                }
            }
        }
    } else {
        // No gamepad - release any movement keys that were held via stick
        if (rf::local_player) {
            auto& bindings = rf::local_player->settings.controls.bindings;
            for (auto action : { rf::CC_ACTION_FORWARD, rf::CC_ACTION_BACKWARD,
                                  rf::CC_ACTION_SLIDE_LEFT, rf::CC_ACTION_SLIDE_RIGHT }) {
                int idx = static_cast<int>(action);
                if (g_action_curr[idx]) {
                    int16_t sc = bindings[idx].scan_codes[0];
                    if (sc >= 0)
                        rf::key_process_event(sc, 0, 0);
                    g_action_curr[idx] = false;
                }
            }
        }
    }
}

void gamepad_get_camera(float& pitch_delta, float& yaw_delta)
{
    pitch_delta = 0.0f;
    yaw_delta = 0.0f;

    if (!g_gamepad || !rf::keep_mouse_centered)
        return;

    float rx = get_axis(SDL_GAMEPAD_AXIS_RIGHTX, g_alpine_game_config.gamepad_look_deadzone);
    float ry = get_axis(SDL_GAMEPAD_AXIS_RIGHTY, g_alpine_game_config.gamepad_look_deadzone);

    // Quake-style: frametime * sensitivity * input
    yaw_delta = rf::frametime * g_alpine_game_config.gamepad_joy_sensitivity * rx;
    pitch_delta = -rf::frametime * g_alpine_game_config.gamepad_joy_sensitivity * ry;
    
    // Add gyro rotation if enabled
    if (g_gyro_enabled && g_alpine_game_config.gamepad_gyro_enabled && g_alpine_game_config.gamepad_gyro_sensitivity > 0.0f) {
        // Process accumulated sensor data once per frame
        g_gamepad_motion.ProcessMotion(g_gyro_x, g_gyro_y, g_gyro_z,
                                       g_accel_x, g_accel_y, g_accel_z, rf::frametime);
        
        // Get calibrated gyro from GamepadMotionHelpers (in degrees/sec)
        float gyro_x, gyro_y, gyro_z;
        g_gamepad_motion.GetCalibratedGyro(gyro_x, gyro_y, gyro_z);
        
        // Convert from degrees/sec to radians/sec and apply
        yaw_delta -= (gyro_y * (3.14159265f / 180.0f)) * g_alpine_game_config.gamepad_gyro_sensitivity * rf::frametime;
        pitch_delta += (gyro_x * (3.14159265f / 180.0f)) * g_alpine_game_config.gamepad_gyro_sensitivity * rf::frametime;
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
        // Let keyboard/native input handle first.
        bool result = control_config_check_pressed_hook.call_target(ccp, action, just_pressed);
        if (result) return true;

        // Gamepad path: mirrors what the controls processor does for keyboard, so that
        // player_execute_action fires at the same point in player_do_frame as it would
        // for a keyboard press. This keeps timing consistent for sound, animation, etc.
        int idx = static_cast<int>(action);
        if (idx < 0 || idx >= k_action_count || !g_action_curr[idx])
            return false;

        bool is_just_pressed = !g_action_prev[idx];
        // Repeat actions (press_mode != 0, e.g. firing) fire every held frame.
        // Edge actions (press_mode == 0, e.g. reload) fire only on the transition frame.
        if (ccp->bindings[idx].press_mode != 0 || is_just_pressed) {
            if (just_pressed) *just_pressed = is_just_pressed;
            return true;
        }
        return false;
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
    "Set gyro look sensitivity (default 1.0, 0 to disable)",
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
    joy_sens_cmd.register_cmd();
    joy_move_deadzone_cmd.register_cmd();
    joy_look_deadzone_cmd.register_cmd();
    gyro_sens_cmd.register_cmd();
    gyro_camera_cmd.register_cmd();
    xlog::info("Gamepad support initialized");
}
