#pragma once
#include <SDL3/SDL.h>

// Valve Corporation VID/PID definitions
#define VALVE_VENDOR_ID               0x28de
#define STEAM_CONTROLLER_LEGACY_PID   0x1101  // Valve Legacy Steam Controller (CHELL)
#define STEAM_CONTROLLER_WIRED_PID    0x1102  // Valve wired Steam Controller (D0G)
#define STEAM_CONTROLLER_BT_1_PID     0x1105  // Valve Bluetooth Steam Controller (D0G)
#define STEAM_CONTROLLER_BT_2_PID     0x1106  // Valve Bluetooth Steam Controller (D0G)
#define STEAM_CONTROLLER_WIRELESS_PID 0x1142  // Valve wireless Steam Controller
#define STEAM_CONTROLLER_V2_WIRED_PID 0x1201  // Valve wired Steam Controller (HEADCRAB)
#define STEAM_CONTROLLER_V2_BT_PID    0x1202  // Valve Bluetooth Steam Controller (HEADCRAB)
#define STEAM_VIRTUAL_GAMEPAD_PID     0x11ff  // Steam Virtual Gamepad
#define STEAM_DECK_BUILTIN_PID        0x1205  // Steam Deck Builtin
#define STEAM_TRITON_WIRED_PID        0x1302  // Steam Controller 2 (GORDON) wired
#define STEAM_TRITON_BLE_PID          0x1303  // Steam Controller 2 (GORDON) BLE

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
    SteamController,
    SteamDeck,
};

// Returns true if the product ID matches any Steam Controller (2015) variant
inline bool is_steam_controller_pid(Uint16 pid)
{
    return (pid == STEAM_CONTROLLER_LEGACY_PID ||
            pid == STEAM_CONTROLLER_WIRED_PID  ||
            pid == STEAM_CONTROLLER_BT_1_PID   ||
            pid == STEAM_CONTROLLER_BT_2_PID   ||
            pid == STEAM_CONTROLLER_WIRELESS_PID ||
            pid == STEAM_CONTROLLER_V2_WIRED_PID ||
            pid == STEAM_CONTROLLER_V2_BT_PID);
}

// Returns true if the product ID matches any Steam Triton / Steam Controller 2 variant
inline bool is_steam_triton_controller_pid(Uint16 pid)
{
    return (pid == STEAM_TRITON_WIRED_PID ||
            pid == STEAM_TRITON_BLE_PID);
}

// Checks Valve VID/PID to resolve Steam-specific controller icon types.
// For Steam Virtual Gamepad (0x11ff), passes through the supplied fallback icon type.
// For Steam Deck/SteamController 2, returns Steam Deck glyphs; for Steam Controller (2015), returns Steam Controller glyphs.
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

    if (product == STEAM_DECK_BUILTIN_PID)
        return ControllerIconType::SteamDeck;

    if (is_steam_controller_pid(product))
        return ControllerIconType::SteamController;

    if (is_steam_triton_controller_pid(product))
        return ControllerIconType::SteamDeck;

    return fallback;
}

// Positional (controller-agnostic) name for a button index
const char* gamepad_get_button_name(int button_idx);

// Controller-aware display name: controller-specific label if known, falls back to positional name
const char* gamepad_get_button_display_name(ControllerIconType type, int button_idx);

// Returns the display name for the given scan code, which may be a keyboard key or a gamepad button/trigger.
const char* gamepad_get_effective_display_name(ControllerIconType icon_pref, SDL_Gamepad* ctrl, int button_idx);
