#pragma once

void gamepad_apply_patch();
void gamepad_sdl_init();
void gamepad_do_frame();
void gamepad_get_camera(float& pitch_delta, float& yaw_delta);
bool gamepad_is_motionsensors_supported();
bool gamepad_is_last_input_gamepad();
void gamepad_set_last_input_keyboard();

// Controller binding UI
int         gamepad_get_button_for_action(int action_idx);  // -1 if unbound
int         gamepad_get_trigger_for_action(int action_idx); // 0=LT, 1=RT, -1 if unbound
const char* gamepad_get_scan_code_name(int scan_code);
int         gamepad_get_button_count();
void        gamepad_reset_to_defaults();
void        gamepad_sync_bindings_from_scan_codes();

// Scan codes used while the CONTROLLER tab is active (unused gap in RF's key table)
static constexpr int CTRL_GAMEPAD_SCAN_BASE   = 0x59; // SDL button 0
static constexpr int CTRL_GAMEPAD_LEFT_TRGGER  = 0x73; // SCAN_BASE + 26
static constexpr int CTRL_GAMEPAD_RIGHT_TRGGER = 0x74; // SCAN_BASE + 27
static constexpr int CTRL_REBIND_SENTINEL      = 0x58; // KEY_F12, injected during rebind

// Per-binding get/set for save/load
int  gamepad_get_button_binding(int button_idx);
void gamepad_set_button_binding(int button_idx, int action_idx);
int  gamepad_get_trigger_action(int trigger_idx);
void gamepad_set_trigger_action(int trigger_idx, int action_idx);

// rebind gamepad buttons/triggers
void gamepad_apply_rebind();
bool gamepad_has_pending_rebind(); // true if a gamepad button/trigger was captured for the current rebind
