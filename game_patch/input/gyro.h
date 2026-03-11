#pragma once

// Gyro processing and calibration.
// GamepadMotion instance lives here; gamepad.cpp feeds raw sensor data per-frame.

enum class GyroSpace : int {
    Yaw         = 0,
    Roll        = 1,
    LocalSpace  = 2,
    PlayerSpace = 3,
    WorldSpace  = 4,
};

void gyro_reset();
void gyro_set_autocalibration(bool enable);
float gyro_get_autocalibration_confidence();
bool gyro_is_autocalibration_steady();
void gyro_process_motion(float gyro_x, float gyro_y, float gyro_z,
                         float accel_x, float accel_y, float accel_z, float delta_time);
void gyro_get_calibrated(float& x, float& y, float& z);
void gyro_get_camera_axes(float& out_pitch_dps, float& out_yaw_dps);
const char* gyro_get_space_name(int space);
void gyro_apply_patch();
