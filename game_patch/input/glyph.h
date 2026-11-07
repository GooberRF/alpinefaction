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
    Steam,
};

// Blocks MISC3-MISC6 (capsense/gripsense) from rebinding on hardware that fires them as buttons.
// TODO: remove this workaround when next major SDL3 release adds proper capsense/gripsense support.
inline bool is_capsense_gripsense_rebind_blocked(SDL_Gamepad* ctrl, int button)
{
    if (button < SDL_GAMEPAD_BUTTON_MISC3 || button > SDL_GAMEPAD_BUTTON_MISC6 || !ctrl)
        return false;
    SDL_GamepadType type = SDL_GetGamepadType(ctrl);
    return type == SDL_GAMEPAD_TYPE_STEAM;
}

// Positional (controller-agnostic) name for a button index
const char* gamepad_get_button_name(int button_idx);

// Controller-aware display name: controller-specific label if known, falls back to positional name
const char* gamepad_get_button_display_name(ControllerIconType type, int button_idx);

// Returns the display name for the given scan code, which may be a keyboard key or a gamepad button/trigger.
const char* gamepad_get_effective_display_name(ControllerIconType icon_pref, SDL_Gamepad* ctrl, int button_idx);
