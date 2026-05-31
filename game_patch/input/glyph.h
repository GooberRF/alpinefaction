#pragma once
#include <SDL3/SDL.h>

// Valve hardware VID/PID definitions
#define VALVE_VENDOR_ID               0x28de
#define STEAM_VIRTUAL_GAMEPAD_PID     0x11ff  // Steam Virtual Gamepad
#define STEAM_DECK_BUILTIN_PID        0x1205  // Valve Steam Deck Controller
#define STEAM_TRITON_WIRED_PID        0x1302  // Valve Steam Controller (TRITON) wired
#define STEAM_TRITON_BLE_PID          0x1303  // Valve Steam Controller (TRITON) Bluetooth
#define STEAM_TRITON_PROTEUS_PID      0x1304  // Valve Steam Controller Puck
#define STEAM_NEREID_DONGLE_PID       0x1305  // Valve Steam Nereid Dongle (most likely for Steam Machine 2 or Steam Frame?)

enum class ControllerIconType {
    Auto = 0,
    Generic,
    Xbox360,
    XboxOne,
    PS3,
    PS4,
    PS5,
    NintendoSwitch,
    NintendoGameCube,
    Steam,
};

// Returns true if the product ID matches any Steam Controller 2 / Triton variant
inline bool is_steam_triton_controller_pid(Uint16 pid)
{
    return (pid == STEAM_TRITON_WIRED_PID ||
            pid == STEAM_TRITON_BLE_PID   ||
            pid == STEAM_TRITON_PROTEUS_PID ||
            pid == STEAM_NEREID_DONGLE_PID);
}

// Checks Valve VID/PID to resolve Steam icon type for Valve hardware.
// For Steam Virtual Gamepad (0x11ff), passes through the supplied fallback icon type.
// For Steam Deck and Steam Controller 2/Triton hardware, returns Steam glyphs.
inline ControllerIconType get_steam_virtual_controller_detection(SDL_Gamepad* ctrl, ControllerIconType fallback)
{
    if (!ctrl)
        return fallback;

    Uint16 vendor = SDL_GetGamepadVendor(ctrl);
    if (vendor != VALVE_VENDOR_ID)
        return fallback;

    Uint16 product = SDL_GetGamepadProduct(ctrl);

    if (product == STEAM_VIRTUAL_GAMEPAD_PID)
        return fallback;

    if (product == STEAM_DECK_BUILTIN_PID || is_steam_triton_controller_pid(product))
        return ControllerIconType::Steam;

    return fallback;
}

// Positional (controller-agnostic) name for a button index
const char* gamepad_get_button_name(int button_idx);

// Controller-aware display name: controller-specific label if known, falls back to positional name
const char* gamepad_get_button_display_name(ControllerIconType type, int button_idx);

// Returns the display name for the given scan code, which may be a keyboard key or a gamepad button/trigger.
const char* gamepad_get_effective_display_name(ControllerIconType icon_pref, SDL_Gamepad* ctrl, int button_idx);
