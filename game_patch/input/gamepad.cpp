#include "gamepad.h"
#include "input.h"
#include "glyph.h"
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
#include "gyro.h"

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

// Per-frame action state, indexed by rf::ControlConfigAction. 128 = hard limit.
static constexpr int k_action_count = 128;
static bool g_action_prev[k_action_count] = {};
static bool g_action_curr[k_action_count] = {};

static bool g_lt_was_down = false;
static bool g_rt_was_down = false;

// Gamepad scan code captured during a rebind, or -1 if none pending.
static int g_rebind_pending_sc = -1;

// True when the last meaningful input (button/key press, not mouse movement) came from the gamepad.
static bool g_last_input_was_gamepad = false;

// Normalize an axis value, strip the deadzone band, and rescale the remainder to [-1, 1].
static float get_axis(SDL_GamepadAxis axis, float deadzone)
{
    if (!g_gamepad) return 0.0f;
    float v = SDL_GetGamepadAxis(g_gamepad, axis) / (float)SDL_MAX_SINT16;
    if (v >  deadzone) return (v - deadzone) / (1.0f - deadzone);
    if (v < -deadzone) return (v + deadzone) / (1.0f - deadzone);
    return 0.0f;
}

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

// Injects the keyboard scan code bound to `action` so all game code paths see the input.
static void inject_action_key(int action, bool down)
{
    if (ui_ctrl_bindings_view_active()) return;
    if (!rf::local_player || action < 0 || action >= rf::local_player->settings.controls.num_bindings)
        return;
    int16_t sc = rf::local_player->settings.controls.bindings[action].scan_codes[0];
    if (sc > 0)
        rf::key_process_event(sc, down ? 1 : 0, 0);
}

// Syncs g_action_curr for any binding whose scan_codes contain `sc`,
// skipping `primary_action` (already handled by g_button_map / g_trigger_action).
// Does NOT inject key events — the hook-based path is sufficient for alpine controls.
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
                release_movement_keys();
                for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b)
                    inject_action_key(g_button_map[b], false);
                inject_action_key(g_trigger_action[0], false);
                inject_action_key(g_trigger_action[1], false);
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
            if (!g_gamepad || SDL_GetGamepadID(g_gamepad) != ev.gbutton.which) break;
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
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            if (!g_gamepad || SDL_GetGamepadID(g_gamepad) != ev.gaxis.which) break;
            {
                float v = ev.gaxis.value / (float)SDL_MAX_SINT16;
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
            }
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

    SDL_GamepadAxis cam_x = g_alpine_game_config.gamepad_swap_sticks ? SDL_GAMEPAD_AXIS_LEFTX  : SDL_GAMEPAD_AXIS_RIGHTX;
    SDL_GamepadAxis cam_y = g_alpine_game_config.gamepad_swap_sticks ? SDL_GAMEPAD_AXIS_LEFTY  : SDL_GAMEPAD_AXIS_RIGHTY;
    float cam_dz          = g_alpine_game_config.gamepad_swap_sticks ? g_alpine_game_config.gamepad_move_deadzone
                                                                       : g_alpine_game_config.gamepad_look_deadzone;
    float rx = get_axis(cam_x, cam_dz);
    float ry = get_axis(cam_y, cam_dz);

    float joy_pitch_sign = g_alpine_game_config.gamepad_joy_invert_y ? 1.0f : -1.0f;
    yaw_delta   =              rf::frametime * g_alpine_game_config.gamepad_joy_sensitivity * rx;
    pitch_delta = joy_pitch_sign * rf::frametime * g_alpine_game_config.gamepad_joy_sensitivity * ry;

    if (g_motion_sensors_active && g_alpine_game_config.gamepad_gyro_enabled
                                && g_alpine_game_config.gamepad_gyro_sensitivity > 0.0f
                                && gyro_modifier_is_active()) {
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

        // Pre-sim: inject gamepad stick + gyro into vehicle rotation input (ci.rot)
        // ci.rot expects values in the range of keyboard input (±1.0).
        // The vehicle's own turn rate and frametime scaling apply on top.
        if (g_gamepad && is_local_player_vehicle(entity)) {
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
                gyro_apply_smoothing(gyro_pitch, gyro_yaw);
                gyro_apply_tightening(gyro_pitch, gyro_yaw);

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

ConsoleCommand2 gyro_vehicle_camera_cmd{
    "gyro_vehicle_camera",
    [](std::optional<int> val) {
        if (val) g_alpine_game_config.gamepad_gyro_vehicle_camera = val.value() != 0;
        rf::console::print("Gyro camera for vehicles: {}", g_alpine_game_config.gamepad_gyro_vehicle_camera ? "enabled" : "disabled");
    },
    "Enable/disable gyro camera while in vehicles (default 0)",
    "gyro_vehicle_camera [0|1]",
};

ConsoleCommand2 gamepad_icons_cmd{
    "joy_icons",
    [](std::optional<int> val) {
        static const char* icon_names[] = {
            "Auto", "Generic", "Xbox 360 Controller", "Xbox Wireless Controller",
            "DualShock 3", "DualShock 4", "DualSense", "Nintendo Switch", "Nintendo GameCube",
        };
        if (val) g_alpine_game_config.gamepad_icon_override = std::clamp(val.value(), 0, 8);
        rf::console::print("Gamepad icons: {} ({})",
            icon_names[g_alpine_game_config.gamepad_icon_override],
            g_alpine_game_config.gamepad_icon_override);
    },
    "Set gamepad button icon style: 0=Auto 1=Generic 2=Xbox360 3=XboxOne 4=DS3 5=DS4 6=DualSense 7=NintendoSwitch 8=NintendoGameCube",
    "joy_icons [0-8]",
};


bool gamepad_is_motionsensors_supported()
{
    return g_motion_sensors_active;
}

bool gamepad_is_last_input_gamepad()
{
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
        SDL_GamepadType type = g_gamepad ? SDL_GetGamepadType(g_gamepad) : SDL_GAMEPAD_TYPE_UNKNOWN;
        auto icon_pref = static_cast<ControllerIconType>(g_alpine_game_config.gamepad_icon_override);
        return gamepad_get_effective_display_name(icon_pref, type, offset);
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
    gyro_vehicle_camera_cmd.register_cmd();
    gamepad_icons_cmd.register_cmd();
    gyro_apply_patch();
    xlog::info("Gamepad support initialized");
}
