#pragma once

// Gyro Axis orientation modes
enum class GyroSpace : int {
    Yaw         = 0,
    Roll        = 1,
    LocalSpace  = 2,
    PlayerSpace = 3,
    WorldSpace  = 4,
};

// Gyro autocalibration modes.
enum class GyroAutocalibrationMode : int {
    Off = 0,
    MenuOnly = 1,
    Always = 2,
};

void gyro_reset();
void gyro_reset_full(); // Full reset: clears offset, confidence, and motion state (use on controller hotswap)
void gyro_update_calibration_mode();
void gyro_set_autocalibration_mode(int mode); // 0=Off,1=MenuOnly,2=Always
float gyro_get_autocalibration_confidence();
bool gyro_is_autocalibration_steady();
void gyro_process_motion(float gyro_x, float gyro_y, float gyro_z,
                         float accel_x, float accel_y, float accel_z, float delta_time);
void gyro_get_axis_orientation(float& out_pitch_dps, float& out_yaw_dps);
void gyro_get_calibrated_rates(float& out_pitch_dps, float& out_yaw_dps);
void gyro_apply_tightening(float& pitch_dps, float& yaw_dps);
void gyro_apply_smoothing(float& pitch_dps, float& yaw_dps);
void gyro_apply_vh_mixer(float& pitch_dps, float& yaw_dps);
const char* gyro_get_space_name(int space);
bool gyro_modifier_is_active();
void gyro_apply_patch();
