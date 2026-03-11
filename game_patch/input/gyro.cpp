#include "gyro.h"
#include <optional>
#include <GamepadMotion.hpp>
#include <xlog/xlog.h>
#include "../os/console.h"
#include "../misc/alpine_settings.h"

static GamepadMotion g_motion;

static void apply_calibration_mode()
{
    using CM = GamepadMotionHelpers::CalibrationMode;
    if (g_alpine_game_config.gamepad_gyro_autocalibration)
        g_motion.SetCalibrationMode(CM::Stillness | CM::SensorFusion);
    else
        g_motion.SetCalibrationMode(CM::Manual);
}

void gyro_reset()
{
    g_motion.Reset();
    apply_calibration_mode();
}

void gyro_set_autocalibration(bool enable)
{
    g_alpine_game_config.gamepad_gyro_autocalibration = enable;
    apply_calibration_mode();
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

void gyro_get_camera_axes(float& out_pitch_dps, float& out_yaw_dps)
{
    switch (static_cast<GyroSpace>(g_alpine_game_config.gamepad_gyro_space)) {
    default:
    case GyroSpace::Yaw: {
        float gx, gy, gz;
        g_motion.GetCalibratedGyro(gx, gy, gz);
        out_pitch_dps = gx;
        out_yaw_dps   = gy;
        break;
    }
    case GyroSpace::Roll: {
        float gx, gy, gz;
        g_motion.GetCalibratedGyro(gx, gy, gz);
        out_pitch_dps = gx;
        out_yaw_dps   = -gz;
        break;
    }
    case GyroSpace::LocalSpace: {
        float gx, gy, gz;
        g_motion.GetCalibratedGyro(gx, gy, gz);
        out_pitch_dps = gx;
        out_yaw_dps   = gy - gz;
        break;
    }
    case GyroSpace::PlayerSpace: {
        float px, py;
        g_motion.GetPlayerSpaceGyro(px, py);
        out_pitch_dps = px;
        out_yaw_dps   = py;
        break;
    }
    case GyroSpace::WorldSpace: {
        float wx, wy;
        g_motion.GetWorldSpaceGyro(wx, wy);
        out_pitch_dps = wx;
        out_yaw_dps   = wy;
        break;
    }
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

ConsoleCommand2 gyro_recalibrate_cmd{
    "gyro_recalibrate",
    [](std::optional<int>) {
        g_motion.SetCalibrationOffset(0.0f, 0.0f, 0.0f, 0);
        g_motion.SetAutoCalibrationConfidence(0.0f);
        rf::console::print("Gyro calibration reset");
    },
    "Restarting Gyro auto-calibration",
};

ConsoleCommand2 gyro_space_cmd{
    "gyro_space",
    [](std::optional<int> val) {
        if (val) g_alpine_game_config.gamepad_gyro_space = std::clamp(val.value(), 0, 4);
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
    apply_calibration_mode();
    gyro_autocalibration_cmd.register_cmd();
    gyro_recalibrate_cmd.register_cmd();
    gyro_space_cmd.register_cmd();
    gyro_invert_y_cmd.register_cmd();
    xlog::info("Gyro processing initialized");
}
