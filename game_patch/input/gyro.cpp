#include "gyro.h"
#include "input.h"
#include "gamepad.h"
#include "../rf/gameseq.h"
#include <cmath>
#include <optional>
#include <GamepadMotion.hpp>
#include <xlog/xlog.h>
#include "../os/console.h"
#include "../misc/alpine_settings.h"
#include "../rf/player/player.h"

static GamepadMotion g_motion;
static GamepadMotionHelpers::CalibrationMode g_last_calibration_mode = static_cast<GamepadMotionHelpers::CalibrationMode>(-1);
static float g_smooth_pitch_prev = 0.0f;
static float g_smooth_yaw_prev   = 0.0f;

void gyro_update_calibration_mode()
{
    using CM = GamepadMotionHelpers::CalibrationMode;
    int mode = std::clamp(g_alpine_game_config.gamepad_gyro_autocalibration_mode, 0, 2);

    // Autocalibration should only run when gyro input is in use (gameplay camera or menu cursor).
    bool gyro_active = gamepad_is_motionsensors_supported()
        && (g_alpine_game_config.gamepad_gyro_enabled
            || g_alpine_game_config.gamepad_gyro_menu_cursor_sensitivity > 0.0f);
    if (!gyro_active) {
        mode = 0; // force manual calibration when no gyro feature is active
    }

    CM desired;
    switch (mode) {
    case 1: // Menu Only — only calibrate when not in gameplay
        desired = rf::gameseq_in_gameplay()
            ? CM::Manual
            : (CM::Stillness);
        break;
    case 2: // Always - will try to calibrate whenever possible
        desired = CM::Stillness | CM::SensorFusion;
        break;
    default: // Off - disable auto calibration
        desired = CM::Manual;
        break;
    }

    if (desired == g_last_calibration_mode)
        return;
    g_last_calibration_mode = desired;

    // Preserve calibrated offset and confidence across mode changes.
    float ox, oy, oz;
    g_motion.GetCalibrationOffset(ox, oy, oz);
    float confidence = g_motion.GetAutoCalibrationConfidence();

    g_motion.SetCalibrationMode(desired);
    g_motion.SetCalibrationOffset(ox, oy, oz, 1);
    g_motion.SetAutoCalibrationConfidence(confidence);
}

void gyro_reset()
{
    g_motion.ResetContinuousCalibration();
    g_motion.ResetMotion();
    g_smooth_pitch_prev = 0.0f;
    g_smooth_yaw_prev   = 0.0f;
    gyro_update_calibration_mode();
}

void gyro_reset_full()
{
    // Invalidate the mode cache so Gyro Autocalibration modes unconditionally re-applies it.
    g_last_calibration_mode = static_cast<GamepadMotionHelpers::CalibrationMode>(-1);
    gyro_reset();
}

void gyro_set_autocalibration_mode(int mode)
{
    mode = std::clamp(mode, 0, 2);
    g_alpine_game_config.gamepad_gyro_autocalibration_mode = mode;
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
    case GyroSpace::LocalSpace: {
        // Local Space code is based on http://gyrowiki.jibbsmart.com/blog:player-space-gyro-and-alternatives-explained
        // Combines the gravity vector to avoid axis conflict
        float gx, gy, gz;
        g_motion.GetCalibratedGyro(x, y, z);
        g_motion.GetGravity(gx, gy, gz);
        out_pitch_dps = x;
        out_yaw_dps   = -(gy * y + gz * z);
        break;
    }
    default: // Yaw
        g_motion.GetCalibratedGyro(x, y, z);
        out_pitch_dps = x;
        out_yaw_dps   = y;
        break;
    }
}

void gyro_get_calibrated_rates(float& out_pitch_dps, float& out_yaw_dps)
{
    g_motion.GetWorldSpaceGyro(out_pitch_dps, out_yaw_dps);
}

// Gyro tightening is based on GyroWiki documents
// http://gyrowiki.jibbsmart.com/blog:good-gyro-controls-part-1:the-gyro-is-a-mouse#toc9
void gyro_apply_tightening(float& pitch_dps, float& yaw_dps)
{
    // 0-100 user value maps to 0-50 deg/s threshold (percentage scale)
    float threshold = g_alpine_game_config.gamepad_gyro_tightening * 0.5f;
    if (threshold > 0.0f) {
        float mag = std::hypot(pitch_dps, yaw_dps);
        if (mag > 0.0f && mag < threshold) {
            float scale = mag / threshold;
            pitch_dps *= scale;
            yaw_dps   *= scale;
        }
    }
}

void gyro_apply_smoothing(float& pitch_dps, float& yaw_dps)
{
    float factor = g_alpine_game_config.gamepad_gyro_smoothing / 100.0f;
    if (factor <= 0.0f) return;
    factor = std::min(factor, 0.99f); // prevent full freeze at 100

    g_smooth_pitch_prev = pitch_dps * (1.0f - factor) + g_smooth_pitch_prev * factor;
    g_smooth_yaw_prev   = yaw_dps   * (1.0f - factor) + g_smooth_yaw_prev   * factor;

    pitch_dps = g_smooth_pitch_prev;
    yaw_dps   = g_smooth_yaw_prev;
}

void gyro_apply_vh_mixer(float& pitch_dps, float& yaw_dps)
{
    int raw = std::clamp(g_alpine_game_config.gamepad_gyro_vh_mixer, -100, 100);
    if (raw == 0) return;
    float mixer = raw / 100.0f;
    float h_scale = mixer >= 0.0f ? (1.0f - mixer) : 1.0f;
    float v_scale = mixer <= 0.0f ? (1.0f + mixer) : 1.0f;
    yaw_dps   *= h_scale;
    pitch_dps *= v_scale;
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
// If Gyro Modifier is unbound -> always active.
// Behavior while bound is determined by gamepad_gyro_modifier_mode:
//   0 = Always  -> always active regardless of binding
//   1 = HoldOff -> active while NOT held
//   2 = HoldOn  -> active while held
//   3 = Toggle  -> button press flips on/off (starts on)
bool gyro_modifier_is_active()
{
    using namespace rf;

    if (!local_player) return true;

    const auto action = get_af_control(AlpineControlConfigAction::AF_ACTION_GYRO_MODIFIER);

    int mode = std::clamp(g_alpine_game_config.gamepad_gyro_modifier_mode, 0, 3);

    if (mode == 0) // Always
        return true;

    if (!gyro_action_has_binding(action))
        return true; // no modifier bound — gyro always on

    auto& cc = local_player->settings.controls;

    bool down = control_is_control_down(&cc, action);

    if (mode == 3) { // Toggle
        if (down && !g_gyro_toggle_prev_down)
            g_gyro_toggle_state = !g_gyro_toggle_state;
        g_gyro_toggle_prev_down = down;
        return g_gyro_toggle_state;
    }

    g_gyro_toggle_prev_down = down;

    if (mode == 1) // HoldOff
        return !down;

    return down; // HoldOn (mode == 2)
}

ConsoleCommand2 gyro_modifier_mode_cmd{
    "gyro_modifier_mode",
    [](std::optional<int> val) {
        if (val) {
            g_alpine_game_config.gamepad_gyro_modifier_mode = std::clamp(val.value(), 0, 3);
            g_gyro_toggle_state = true;
            g_gyro_toggle_prev_down = false;
        }
        int mode = g_alpine_game_config.gamepad_gyro_modifier_mode;
        static const char* mode_names[] = {"Always", "Hold Off", "Hold On", "Toggle"};
        rf::console::print("Gyro modifier mode: {} ({})", mode_names[mode], mode);
    },
    "Set gyro modifier mode: 0=Always, 1=Hold Off, 2=Hold On, 3=Toggle (default 0)",
    "gyro_modifier_mode [0|1|2|3]",
};

ConsoleCommand2 gyro_autocalibration_cmd{
    "gyro_autocalibration",
    [](std::optional<int> val) {
        if (val) {
            gyro_set_autocalibration_mode(val.value());
        }

        int mode = std::clamp(g_alpine_game_config.gamepad_gyro_autocalibration_mode, 0, 2);
        const char* mode_name = "unknown";
        switch (mode) {
        case 0:
            mode_name = "Off";
            break;
        case 1:
            mode_name = "Menu Only";
            break;
        case 2:
            mode_name = "Always";
            break;
        }

        rf::console::print("Gyro autocalibration mode: {} ({})", mode_name, mode);
    },
    "Set gyro auto-calibration mode: 0=Off, 1=Menu Only, 2=Always (default 2)",
    "gyro_autocalibration [0|1|2]",
};

ConsoleCommand2 gyro_reset_autocalibration_partial_cmd{
    "gyro_reset_autocalibration_partial",
    [](std::optional<int>) {
        g_motion.SetAutoCalibrationConfidence(0.0f);
        gyro_update_calibration_mode();
        rf::console::print("Gyro auto-calibration partial reset (offset preserved)");
    },
    "Reset gyro auto-calibration confidence (preserves offset)",
};

ConsoleCommand2 gyro_reset_autocalibration_full_cmd{
    "gyro_reset_autocalibration_full",
    [](std::optional<int>) {
        g_motion.SetAutoCalibrationConfidence(0.0f);
        g_motion.ResetContinuousCalibration();
        g_motion.ResetMotion();
        gyro_update_calibration_mode();
        rf::console::print("Gyro auto-calibration full reset");
    },
    "Reset gyro auto-calibration confidence and clear offset",
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
        if (val) g_alpine_game_config.gamepad_gyro_tightening = std::clamp(val.value(), 0.0f, 100.0f);
        rf::console::print("Gyro tightening threshold: {:.1f}",
            g_alpine_game_config.gamepad_gyro_tightening);
    },
    "Set gyro tightening threshold (0 = disabled, max 100)",
    "gyro_tightening [value]",
};

ConsoleCommand2 gyro_smoothing_cmd{
    "gyro_smoothing",
    [](std::optional<float> val) {
        if (val) g_alpine_game_config.gamepad_gyro_smoothing = std::clamp(val.value(), 0.0f, 100.0f);
        rf::console::print("Gyro smoothing threshold: {:.1f}",
            g_alpine_game_config.gamepad_gyro_smoothing);
    },
    "Set gyro soft-tier smoothing threshold (0 = disabled, max 100)",
    "gyro_smoothing [value]",
};

ConsoleCommand2 gyro_vh_cmd{
    "gyro_vh",
    [](std::optional<int> val) {
        if (val) g_alpine_game_config.gamepad_gyro_vh_mixer = std::clamp(val.value(), -100, 100);
        rf::console::print("Gyro V/H output mixer: {}", g_alpine_game_config.gamepad_gyro_vh_mixer);
    },
    "Set gyro V/H output mixer (-100 = reduce vertical, 0 = 1:1, 100 = reduce horizontal)",
    "gyro_vh_mixer [-100 to 100]",
};

void gyro_apply_patch()
{
    g_motion.Settings.MinStillnessCorrectionTime      = 1.0f; // default 2.0
    g_motion.Settings.StillnessCalibrationEaseInTime  = 1.5f; // default 3.0

    gyro_update_calibration_mode();
    gyro_modifier_mode_cmd.register_cmd();
    gyro_autocalibration_cmd.register_cmd();
    gyro_reset_autocalibration_partial_cmd.register_cmd();
    gyro_reset_autocalibration_full_cmd.register_cmd();
    gyro_space_cmd.register_cmd();
    gyro_invert_y_cmd.register_cmd();
    gyro_tightening_cmd.register_cmd();
    gyro_smoothing_cmd.register_cmd();
    gyro_vh_cmd.register_cmd();
    xlog::info("Gyro processing initialized");
}
