#include "gyro.h"
#include <optional>
#include <GamepadMotion.hpp>
#include <xlog/xlog.h>
#include "../os/console.h"
#include "../misc/alpine_settings.h"

static GamepadMotion g_motion;

void gyro_update_calibration_mode()
{
    using CM = GamepadMotionHelpers::CalibrationMode;
    g_motion.SetCalibrationMode(g_alpine_game_config.gamepad_gyro_autocalibration
        ? CM::Stillness | CM::SensorFusion
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

    if (space == GyroSpace::PlayerSpace) {
        float px, py;
        g_motion.GetPlayerSpaceGyro(px, py);
        out_pitch_dps = px;
        out_yaw_dps   = py;
        return;
    }
    if (space == GyroSpace::WorldSpace) {
        float wx, wy;
        g_motion.GetWorldSpaceGyro(wx, wy);
        out_pitch_dps = wx;
        out_yaw_dps   = wy;
        return;
    }

    float gx, gy, gz;
    g_motion.GetCalibratedGyro(gx, gy, gz);
    out_pitch_dps = gx;
    switch (space) {
    case GyroSpace::Roll:       out_yaw_dps = -gz;     break;
    case GyroSpace::LocalSpace: out_yaw_dps = gy - gz; break;
    default:                    out_yaw_dps = gy;       break; // Yaw
    }
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

void gyro_apply_patch()
{
    g_motion.Settings.MinStillnessCorrectionTime      = 1.0f; // default 2.0
    g_motion.Settings.StillnessCalibrationEaseInTime  = 1.5f; // default 3.0
    g_motion.Settings.SensorFusionCalibrationEaseInTime = 1.5f; // default 3.0

    gyro_update_calibration_mode();
    gyro_autocalibration_cmd.register_cmd();
    gyro_reset_autocalibration_cmd.register_cmd();
    gyro_space_cmd.register_cmd();
    gyro_invert_y_cmd.register_cmd();
    xlog::info("Gyro processing initialized");
}
