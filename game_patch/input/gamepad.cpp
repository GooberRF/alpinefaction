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
#include <SDL3/SDL.h>
#include <GamepadMotion.hpp>

static SDL_Gamepad* g_gamepad = nullptr;

// GamepadMotionHelpers instance for sensor fusion and calibration
static GamepadMotion g_gamepad_motion;

static float g_deadzone = 0.25f;
static float g_gamepad_joy_sensitivity = 2.5f;
static float g_gamepad_gyro_sensitivity = 1.0f;
static bool g_gamepad_gyro_user_enabled = true;  // User toggle for gyro camera

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

// Normalize an axis value and strip the deadzone band.
static float get_axis(SDL_GamepadAxis axis)
{
    if (!g_gamepad) return 0.0f;
    float v = SDL_GetGamepadAxis(g_gamepad, axis) / (float)SDL_MAX_SINT16;
    if (v >  g_deadzone) return (v - g_deadzone) / (1.0f - g_deadzone);
    if (v < -g_deadzone) return (v + g_deadzone) / (1.0f - g_deadzone);
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

static void gamepad_update()
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

        // Left stick movement 
        float ly = get_axis(SDL_GAMEPAD_AXIS_LEFTY);
        float lx = get_axis(SDL_GAMEPAD_AXIS_LEFTX);
        const float stick_threshold = 0.3f;
        
        // Set movement actions based on stick position (don't reset - OR logic in hook handles this)
        g_action_curr[static_cast<int>(rf::CC_ACTION_FORWARD)]     = ly < -stick_threshold;  // stick up = forward (W key)
        g_action_curr[static_cast<int>(rf::CC_ACTION_BACKWARD)]    = ly >  stick_threshold;  // stick down = backward (S key)
        g_action_curr[static_cast<int>(rf::CC_ACTION_SLIDE_LEFT)]  = lx < -stick_threshold;  // stick left = slide left (A key)
        g_action_curr[static_cast<int>(rf::CC_ACTION_SLIDE_RIGHT)] = lx >  stick_threshold;  // stick right = slide right (D key)
    } else {
        // No gamepad - ensure movement actions are not set
        g_action_curr[static_cast<int>(rf::CC_ACTION_FORWARD)]     = false;
        g_action_curr[static_cast<int>(rf::CC_ACTION_BACKWARD)]    = false;
        g_action_curr[static_cast<int>(rf::CC_ACTION_SLIDE_LEFT)]  = false;
        g_action_curr[static_cast<int>(rf::CC_ACTION_SLIDE_RIGHT)] = false;
    }
}

void gamepad_get_camera(float& pitch_delta, float& yaw_delta)
{
    pitch_delta = 0.0f;
    yaw_delta = 0.0f;

    if (!g_gamepad || !rf::keep_mouse_centered)
        return;

    float rx = get_axis(SDL_GAMEPAD_AXIS_RIGHTX);
    float ry = get_axis(SDL_GAMEPAD_AXIS_RIGHTY);

    // Quake-style: frametime * sensitivity * input
    yaw_delta = rf::frametime * g_gamepad_joy_sensitivity * rx;
    pitch_delta = -rf::frametime * g_gamepad_joy_sensitivity * ry;
    
    // Add gyro rotation if enabled
    if (g_gyro_enabled && g_gamepad_gyro_user_enabled && g_gamepad_gyro_sensitivity > 0.0f) {
        // Process accumulated sensor data once per frame
        g_gamepad_motion.ProcessMotion(g_gyro_x, g_gyro_y, g_gyro_z,
                                       g_accel_x, g_accel_y, g_accel_z, rf::frametime);
        
        // Get calibrated gyro from GamepadMotionHelpers (in degrees/sec)
        float gyro_x, gyro_y, gyro_z;
        g_gamepad_motion.GetCalibratedGyro(gyro_x, gyro_y, gyro_z);
        
        // Convert from degrees/sec to radians/sec and apply
        yaw_delta -= (gyro_y * (3.14159265f / 180.0f)) * g_gamepad_gyro_sensitivity * rf::frametime;
        pitch_delta += (gyro_x * (3.14159265f / 180.0f)) * g_gamepad_gyro_sensitivity * rf::frametime;
    }
}

FunHook<void(int&, int&, int&)> mouse_get_delta_hook{
    0x0051E630,
    [](int& dx, int& dy, int& dz) {
        mouse_get_delta_hook.call_target(dx, dy, dz);
        gamepad_update();
        // Gamepad rotation is now applied via injection in mouse.cpp at 0x0049DEC9
    },
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
        if (!result && action_just_pressed(action)) {
            if (just_pressed) *just_pressed = true;
            return true;
        }
        return result;
    },
};

ConsoleCommand2 joy_sens_cmd{
    "joy_sens",
    [](std::optional<float> val) {
        if (val) g_gamepad_joy_sensitivity = std::max(0.0f, val.value());
        rf::console::print("Gamepad sensitivity: {:.4f}", g_gamepad_joy_sensitivity);
    },
    "Set gamepad look sensitivity (default 5.0)",
    "joy_sens [value]",
};

ConsoleCommand2 joy_deadzone_cmd{
    "joy_deadzone",
    [](std::optional<float> val) {
        if (val) g_deadzone = std::clamp(val.value(), 0.0f, 0.9f);
        rf::console::print("Gamepad stick deadzone: {:.2f}", g_deadzone);
    },
    "Set gamepad stick deadzone 0.0-0.9 (default 0.25)",
    "joy_deadzone [value]",
};

ConsoleCommand2 gyro_sens_cmd{
    "gyro_sens",
    [](std::optional<float> val) {
        if (val) g_gamepad_gyro_sensitivity = std::max(0.0f, val.value());
        rf::console::print("Gyro sensitivity: {:.4f}", g_gamepad_gyro_sensitivity);
    },
    "Set gyro look sensitivity (default 1.0, 0 to disable)",
    "gyro_sens [value]",
};

ConsoleCommand2 gyro_camera_cmd{
    "gyro_camera",
    [](std::optional<int> val) {
        if (val) g_gamepad_gyro_user_enabled = val.value() != 0;
        rf::console::print("Gyro camera: {}", g_gamepad_gyro_user_enabled ? "enabled" : "disabled");
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

    mouse_get_delta_hook.install();
    control_is_control_down_hook.install();
    control_config_check_pressed_hook.install();
    joy_sens_cmd.register_cmd();
    joy_deadzone_cmd.register_cmd();
    gyro_sens_cmd.register_cmd();
    gyro_camera_cmd.register_cmd();
    xlog::info("Gamepad support initialized");
}
