#pragma once
#include <SDL3/SDL.h>

// User-selectable controller icon set.
// Auto = detect from the connected gamepad; everything else forces a specific icon family.
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

// Resolves the user's icon preference against the currently connected gamepad type,
// then returns the appropriate display name. Pass the live SDL_GamepadType for auto-detect.
const char* gamepad_get_effective_display_name(ControllerIconType icon_pref, SDL_GamepadType connected_type, int button_idx);
