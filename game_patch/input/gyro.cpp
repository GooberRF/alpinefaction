#include "gyro.h"
#include "input.h"
#include "gamepad.h"
#include <cmath>
#include <optional>
#include <GamepadMotion.hpp>
#include <xlog/xlog.h>
#include "../os/console.h"
#include "../misc/alpine_settings.h"
#include "../rf/player/player.h"

static GamepadMotion g_motion;

void gyro_update_calibration_mode()
{
    using CM = GamepadMotionHelpers::CalibrationMode;
    g_motion.SetCalibrationMode(g_alpine_game_config.gamepad_gyro_autocalibration
        ? CM::Stillness
        : CM::Manual);
}

void gyro_reset()
{
    g_motion.Reset();
    gyro_update_calibration_mode();
}

void gyro_set_autocalibration(bool enable)
{
    g_alpine_game_config.gamepad_gyro_autocalibration = enable;
    gyro_update_calibration_mode();
}

float gyro_get_autocalibration_confidence()
{
    return g_motion.GetAutoCalibrationConfidence();
}

bool gyro_is_autocalibration_steady()
{
    return g_motion.GetAutoCalibrationIsSteady();
}

void gyro_process_motion(float gyro_x, float gyro_y, float gyro_z,
                         float accel_x, float accel_y, float accel_z, float delta_time)
{
    g_motion.ProcessMotion(gyro_x, gyro_y, gyro_z, accel_x, accel_y, accel_z, delta_time);
}

void gyro_get_calibrated(float& x, float& y, float& z)
{
    g_motion.GetCalibratedGyro(x, y, z);
}

static const char* gyro_space_names[] = { "Yaw", "Roll", "Local", "Player", "World" };

const char* gyro_get_space_name(int space)
{
    if (space >= 0 && space <= 4) return gyro_space_names[space];
    return gyro_space_names[0];
}

void gyro_get_axis_orientation(float& out_pitch_dps, float& out_yaw_dps)
{
    auto space = static_cast<GyroSpace>(g_alpine_game_config.gamepad_gyro_space);
    float x, y, z;

    switch (space) {
    case GyroSpace::PlayerSpace:
        g_motion.GetPlayerSpaceGyro(x, y);
        out_pitch_dps = x;
        out_yaw_dps   = y;
        break;
    case GyroSpace::WorldSpace:
        g_motion.GetWorldSpaceGyro(x, y);
        out_pitch_dps = x;
        out_yaw_dps   = y;
        break;
    case GyroSpace::Roll:
        g_motion.GetCalibratedGyro(x, y, z);
        out_pitch_dps = x;
        out_yaw_dps   = -z;
        break;
    case GyroSpace::LocalSpace:
        g_motion.GetCalibratedGyro(x, y, z);
        out_pitch_dps = x;
        out_yaw_dps   = y - z;
        break;
    default: // Yaw
        g_motion.GetCalibratedGyro(x, y, z);
        out_pitch_dps = x;
        out_yaw_dps   = y;
        break;
    }
}

void gyro_apply_tightening(float& pitch_dps, float& yaw_dps)
{
    float threshold = g_alpine_game_config.gamepad_gyro_tightening;
    if (threshold > 0.0f) {
        float mag = std::hypot(pitch_dps, yaw_dps);
        if (mag > 0.0f && mag < threshold) {
            float scale = mag / threshold;
            pitch_dps *= scale;
            yaw_dps   *= scale;
        }
    }
}

static constexpr int SMOOTH_BUF_SIZE = 16;
static float g_smooth_pitch[SMOOTH_BUF_SIZE] = {};
static float g_smooth_yaw[SMOOTH_BUF_SIZE]   = {};
static int   g_smooth_idx = 0;

void gyro_apply_smoothing(float& pitch_dps, float& yaw_dps)
{
    float threshold = g_alpine_game_config.gamepad_gyro_smoothing;
    if (threshold <= 0.0f) return;

    float mag = std::hypot(pitch_dps, yaw_dps);
    float t1  = threshold * 0.5f;
    float direct_weight = std::clamp((mag - t1) / t1, 0.0f, 1.0f);

    g_smooth_pitch[g_smooth_idx] = pitch_dps * (1.0f - direct_weight);
    g_smooth_yaw[g_smooth_idx]   = yaw_dps   * (1.0f - direct_weight);
    g_smooth_idx = (g_smooth_idx + 1) % SMOOTH_BUF_SIZE;

    float sum_pitch = 0.0f, sum_yaw = 0.0f;
    for (int i = 0; i < SMOOTH_BUF_SIZE; ++i) {
        sum_pitch += g_smooth_pitch[i];
        sum_yaw   += g_smooth_yaw[i];
    }

    pitch_dps = pitch_dps * direct_weight + sum_pitch / SMOOTH_BUF_SIZE;
    yaw_dps   = yaw_dps   * direct_weight + sum_yaw   / SMOOTH_BUF_SIZE;
}

static bool gyro_action_has_binding(rf::ControlConfigAction action)
{
    if (!rf::local_player) return false;
    auto& cc = rf::local_player->settings.controls;
    int idx = static_cast<int>(action);
    if (idx < 0 || idx >= cc.num_bindings) return false;
    const auto& b = cc.bindings[idx];
    return b.scan_codes[0] > 0 || b.scan_codes[1] > 0 || b.mouse_btn_id >= 0
        || gamepad_get_button_for_action(idx) >= 0
        || gamepad_get_trigger_for_action(idx) >= 0;
}

// Toggle state for Gyro Modifier binding.
static bool g_gyro_toggle_state = true;
static bool g_gyro_toggle_prev_down = false;

// Returns whether gyro input should be applied this frame.
// - None of the three modifier bindings assigned  -> always active.
// - Gyro Modifier (Hold)          -> active while held.
// - Gyro Modifier (Hold - Invert) -> active while NOT held.
// - Gyro Modifier (Toggle)        -> button press flips on/off (starts on).
// If multiple bindings are assigned, any active condition enables gyro (OR).
bool gyro_modifier_is_active()
{
    using namespace rf;

    if (!local_player) return true;

    const auto hold_action        = get_af_control(AlpineControlConfigAction::AF_ACTION_GYRO_MODIFIER_HOLD);
    const auto hold_invert_action = get_af_control(AlpineControlConfigAction::AF_ACTION_GYRO_MODIFIER_HOLD_INVERT);
    const auto toggle_action      = get_af_control(AlpineControlConfigAction::AF_ACTION_GYRO_MODIFIER_TOGGLE);

    const bool hold_bound        = gyro_action_has_binding(hold_action);
    const bool hold_invert_bound = gyro_action_has_binding(hold_invert_action);
    const bool toggle_bound      = gyro_action_has_binding(toggle_action);

    if (!hold_bound && !hold_invert_bound && !toggle_bound)
        return true; // no modifier bound — gyro always on

    auto& cc = local_player->settings.controls;

    // Toggle:
    if (toggle_bound) {
        bool down = control_is_control_down(&cc, toggle_action);
        if (down && !g_gyro_toggle_prev_down)
            g_gyro_toggle_state = !g_gyro_toggle_state;
        g_gyro_toggle_prev_down = down;
    }

    bool active = false;
    if (hold_bound)        active |= control_is_control_down(&cc, hold_action);
    if (hold_invert_bound) active |= !control_is_control_down(&cc, hold_invert_action);
    if (toggle_bound)      active |= g_gyro_toggle_state;
    return active;
}

ConsoleCommand2 gyro_autocalibration_cmd{
    "gyro_autocalibration",
    [](std::optional<int> val) {
        if (val) gyro_set_autocalibration(val.value() != 0);
        if (g_alpine_game_config.gamepad_gyro_autocalibration) {
            rf::console::print("Gyro autocalibration: enabled  confidence: {:.2f}  steady: {}",
                g_motion.GetAutoCalibrationConfidence(),
                g_motion.GetAutoCalibrationIsSteady() ? "yes" : "no");
        } else {
            rf::console::print("Gyro autocalibration: disabled");
        }
    },
    "Enable/disable gyro auto-calibration (default 1)",
    "gyro_autocalibration [0|1]",
};

ConsoleCommand2 gyro_reset_autocalibration_cmd{
    "gyro_reset_autocalibration",
    [](std::optional<int>) {
        g_motion.SetAutoCalibrationConfidence(0.0f);
        rf::console::print("Gyro auto-calibration reset");
    },
    "Reset gyro auto-calibration",
};

ConsoleCommand2 gyro_space_cmd{
    "gyro_space",
    [](std::optional<int> val) {
        if (val) {
            g_alpine_game_config.gamepad_gyro_space = std::clamp(val.value(), 0, 4);
            g_motion.ResetMotion();
        }
        int s = g_alpine_game_config.gamepad_gyro_space;
        rf::console::print("Gyro space: {} ({})", s, gyro_space_names[s]);
    },
    "Set gyro camera space: 0=Yaw 1=Roll 2=Local 3=Player 4=World",
    "gyro_space [0-4]",
};

ConsoleCommand2 gyro_invert_y_cmd{
    "gyro_invert_y",
    [](std::optional<int> val) {
        if (val) g_alpine_game_config.gamepad_gyro_invert_y = val.value() != 0;
        rf::console::print("Gyro invert Y: {}", g_alpine_game_config.gamepad_gyro_invert_y ? "on" : "off");
    },
    "Toggle Gyro Y-axis invert",
    "gyro_invert_y [0|1]",
};

ConsoleCommand2 gyro_tightening_cmd{
    "gyro_tightening",
    [](std::optional<float> val) {
        if (val) g_alpine_game_config.gamepad_gyro_tightening = std::max(0.0f, val.value());
        rf::console::print("Gyro tightening threshold: {:.1f}",
            g_alpine_game_config.gamepad_gyro_tightening);
    },
    "Set gyro tightening threshold (0 = disabled)",
    "gyro_tightening [value]",
};

ConsoleCommand2 gyro_smoothing_cmd{
    "gyro_smoothing",
    [](std::optional<float> val) {
        if (val) g_alpine_game_config.gamepad_gyro_smoothing = std::max(0.0f, val.value());
        rf::console::print("Gyro smoothing threshold: {:.1f}",
            g_alpine_game_config.gamepad_gyro_smoothing);
    },
    "Set gyro soft-tier smoothing threshold (0 = disabled)",
    "gyro_smoothing [value]",
};

void gyro_apply_patch()
{
    g_motion.Settings.MinStillnessCorrectionTime      = 1.0f; // default 2.0
    g_motion.Settings.StillnessCalibrationEaseInTime  = 1.5f; // default 3.0

    gyro_update_calibration_mode();
    gyro_autocalibration_cmd.register_cmd();
    gyro_reset_autocalibration_cmd.register_cmd();
    gyro_space_cmd.register_cmd();
    gyro_invert_y_cmd.register_cmd();
    gyro_tightening_cmd.register_cmd();
    gyro_smoothing_cmd.register_cmd();
    xlog::info("Gyro processing initialized");
}
