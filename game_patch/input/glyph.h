#pragma once
#include <SDL3/SDL.h>

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
};

// Positional (controller-agnostic) name for a button index
const char* gamepad_get_button_name(int button_idx);

// Controller-aware display name: controller-specific label if known, falls back to positional name
const char* gamepad_get_button_display_name(SDL_GamepadType type, int button_idx);

// Returns the display name for the given scan code, which may be a keyboard key or a gamepad button/trigger.
const char* gamepad_get_effective_display_name(ControllerIconType icon_pref, SDL_GamepadType connected_type, int button_idx);
