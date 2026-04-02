#pragma once
#include <cstdint>

void gamepad_apply_patch();
void gamepad_sdl_init();


struct RumbleEffect
{
    uint16_t lo_motor      = 0; // low-frequency (left) body motor
    uint16_t hi_motor      = 0; // high-frequency (right) body motor
    uint16_t trigger_motor = 0; // trigger motor strength (if supported by SDL_RumbleGamepadTriggers)
    uint32_t duration_ms   = 0;
};

void gamepad_rumble(uint16_t low_freq, uint16_t high_freq, uint32_t duration_ms);
void gamepad_play_rumble(const RumbleEffect& effect, bool is_alt_fire = false);
void gamepad_stop_rumble(); // immediately silence all rumble motors

void gamepad_do_frame();
void consume_raw_gamepad_deltas(float& pitch_delta, float& yaw_delta);
void flush_freelook_gamepad_deltas();
bool gamepad_is_motionsensors_supported();
bool gamepad_is_trigger_rumble_supported();
bool gamepad_is_last_input_gamepad();
void gamepad_set_last_input_keyboard();

// Controller binding UI
int         gamepad_get_button_for_action(int action_idx);  // -1 if unbound (primary only)
void        gamepad_get_buttons_for_action(int action_idx, int* btn_primary, int* btn_secondary); // both primary and secondary
int         gamepad_get_trigger_for_action(int action_idx); // 0=LT, 1=RT, -1 if unbound
const char* gamepad_get_scan_code_name(int scan_code);
int         gamepad_get_button_count();
void        gamepad_reset_to_defaults();
void        gamepad_sync_bindings_from_scan_codes();

// Scan codes used while the CONTROLLER tab is active (unused gap in RF's key table)
static constexpr int CTRL_GAMEPAD_SCAN_BASE    = 0x59; // SDL button 0
static constexpr int CTRL_GAMEPAD_EXTENDED_BASE = CTRL_GAMEPAD_SCAN_BASE + 15; // SDL_GAMEPAD_BUTTON_MISC1 (0x68)
static constexpr int CTRL_GAMEPAD_LEFT_TRIGGER  = 0x73; // SCAN_BASE + 26
static constexpr int CTRL_GAMEPAD_RIGHT_TRIGGER = 0x74; // SCAN_BASE + 27
// Separate scan-code namespace for menu-only actions (spectate, vote, menus).
// Placed after the trigger slots so it never overlaps gameplay button codes.
static constexpr int CTRL_GAMEPAD_MENU_BASE    = 0x75; // SCAN_BASE + 28

// Returns true if an action index should live in g_menu_button_map
bool gamepad_is_menu_only_action(int action_idx);

// Per-binding get/set for save/load
int  gamepad_get_button_binding(int button_idx);
void gamepad_set_button_binding(int button_idx, int action_idx);
int  gamepad_get_button_alt_binding(int button_idx);     // secondary (extended button) binding
void gamepad_set_button_alt_binding(int button_idx, int action_idx);
int  gamepad_get_alt_sc_for_primary_sc(int primary_sc);  // combined name lookup for binding list
int  gamepad_get_trigger_action(int trigger_idx);
void gamepad_set_trigger_action(int trigger_idx, int action_idx);

// rebind gamepad buttons/triggers
void gamepad_apply_rebind();
bool gamepad_has_pending_rebind(); // true if a gamepad button/trigger was captured for the current rebind

// Returns and clears any pending scroll delta produced by the right-stick menu scroll tick (+1=up, -1=down, 0=none)
int gamepad_consume_menu_scroll();
