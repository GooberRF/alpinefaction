#include "glyph.h"
#include <SDL3/SDL.h>


struct ButtonOverride {
    int         button_idx;
    const char* name;
};

template<int N>
static const char* search_overrides(const ButtonOverride (&table)[N], int button_idx)
{
    for (const auto& entry : table)
        if (entry.button_idx == button_idx)
            return entry.name;
    return nullptr;
}

// Maps SDL face-button labels to display strings.
static const char* get_label_name(SDL_GamepadButtonLabel label)
{
    switch (label) {
        case SDL_GAMEPAD_BUTTON_LABEL_A:        return "A";
        case SDL_GAMEPAD_BUTTON_LABEL_B:        return "B";
        case SDL_GAMEPAD_BUTTON_LABEL_X:        return "X";
        case SDL_GAMEPAD_BUTTON_LABEL_Y:        return "Y";
        case SDL_GAMEPAD_BUTTON_LABEL_CROSS:    return "Cross";
        case SDL_GAMEPAD_BUTTON_LABEL_CIRCLE:   return "Circle";
        case SDL_GAMEPAD_BUTTON_LABEL_SQUARE:   return "Square";
        case SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE: return "Triangle";
        default:                                return nullptr;
    }
}

static const ButtonOverride xbox360_overrides[] = {
    {  4, "Back"  },
    {  5, "Xbox"  },
    {  6, "Start" },
    {  7, "LS"    },
    {  8, "RS"    },
    {  9, "LB"    },
    { 10, "RB"    },
    { 26, "LT"    },
    { 27, "RT"    },
};

static const ButtonOverride xboxone_overrides[] = {
    {  4, "View"     },
    {  5, "Xbox"     },
    {  6, "Menu"     },
    {  7, "LS"       },
    {  8, "RS"       },
    {  9, "LB"       },
    { 10, "RB"       },
    { 15, "Share"    },
    { 16, "Paddle 1" },
    { 17, "Paddle 2" },
    { 18, "Paddle 3" },
    { 19, "Paddle 4" },
    { 26, "LT"       },
    { 27, "RT"       },
};

static const ButtonOverride ps3_overrides[] = {
    {  4, "Select"   },
    {  5, "PS"       },
    {  6, "Start"    },
    {  7, "L3"       },
    {  8, "R3"       },
    {  9, "L1"       },
    { 10, "R1"       },
    { 20, "Touchpad" },
    { 26, "L2"       },
    { 27, "R2"       },
};

static const ButtonOverride ps4_overrides[] = {
    {  4, "Share"    },
    {  5, "PS"       },
    {  6, "Options"  },
    {  7, "L3"       },
    {  8, "R3"       },
    {  9, "L1"       },
    { 10, "R1"       },
    { 20, "Touchpad" },
    { 26, "L2"       },
    { 27, "R2"       },
};

static const ButtonOverride ps5_overrides[] = {
    {  4, "Create"    },
    {  5, "PS"        },
    {  6, "Options"   },
    {  7, "L3"        },
    {  8, "R3"        },
    {  9, "L1"        },
    { 10, "R1"        },
    { 15, "Mic"       },
    { 16, "RB Paddle" },
    { 17, "LB Paddle" },
    { 18, "Right Fn"  },
    { 19, "Left Fn"   },
    { 20, "Touchpad"  },
    { 26, "L2"        },
    { 27, "R2"        },
};

static const ButtonOverride switchpro_overrides[] = {
    {  4, "-"        },
    {  5, "Home"     },
    {  6, "+"        },
    {  7, "LS Click" },
    {  8, "RS Click" },
    {  9, "L"        },
    { 10, "R"        },
    { 15, "Capture"  },
    { 16, "Right SR" },
    { 17, "Left SL"  },
    { 18, "Right SL" },
    { 19, "Left SR"  },
    { 26, "ZL"       },
    { 27, "ZR"       },
};

static const ButtonOverride switchjoycon_overrides[] = {
    {  5, "Home"     },
    {  7, "LS Click" },
    {  8, "RS Click" },
    {  9, "L"        },
    { 10, "R"        },
    { 16, "Right SR" },
    { 17, "Left SL"  },
    { 18, "Right SL" },
    { 19, "Left SR"  },
    { 26, "ZL"       },
    { 27, "ZR"       },
};

static const ButtonOverride gamecube_overrides[] = {
    {  9, "Z"       },  // SDL_GAMEPAD_BUTTON_LEFT_SHOULDER  → Z
    { 10, "R"       },  // SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER → R
    { 22, "L Click" },  // SDL_GAMEPAD_BUTTON_MISC3          → L trigger digital
    { 23, "R Click" },  // SDL_GAMEPAD_BUTTON_MISC4          → R trigger digital
    { 26, "L"       },
    { 27, "R"       },
};

const char* gamepad_get_button_name(int button_idx)
{
    static const char* names[] = {
        "South",           // SDL_GAMEPAD_BUTTON_SOUTH          0
        "East",            // SDL_GAMEPAD_BUTTON_EAST           1
        "West",            // SDL_GAMEPAD_BUTTON_WEST           2
        "North",           // SDL_GAMEPAD_BUTTON_NORTH          3
        "Back",            // SDL_GAMEPAD_BUTTON_BACK           4
        "Guide",           // SDL_GAMEPAD_BUTTON_GUIDE          5
        "Start",           // SDL_GAMEPAD_BUTTON_START          6
        "Left Stick",      // SDL_GAMEPAD_BUTTON_LEFT_STICK     7
        "Right Stick",     // SDL_GAMEPAD_BUTTON_RIGHT_STICK    8
        "Left Shoulder",   // SDL_GAMEPAD_BUTTON_LEFT_SHOULDER  9
        "Right Shoulder",  // SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER 10
        "D-Pad Up",        // SDL_GAMEPAD_BUTTON_DPAD_UP        11
        "D-Pad Down",      // SDL_GAMEPAD_BUTTON_DPAD_DOWN      12
        "D-Pad Left",      // SDL_GAMEPAD_BUTTON_DPAD_LEFT      13
        "D-Pad Right",     // SDL_GAMEPAD_BUTTON_DPAD_RIGHT     14
        "Misc 1",          // SDL_GAMEPAD_BUTTON_MISC1          15
        "Right Paddle 1",  // SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1  16
        "Left Paddle 1",   // SDL_GAMEPAD_BUTTON_LEFT_PADDLE1   17
        "Right Paddle 2",  // SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2  18
        "Left Paddle 2",   // SDL_GAMEPAD_BUTTON_LEFT_PADDLE2   19
        "Touchpad",        // SDL_GAMEPAD_BUTTON_TOUCHPAD       20
        "Misc 2",          // SDL_GAMEPAD_BUTTON_MISC2          21
        "Misc 3",          // SDL_GAMEPAD_BUTTON_MISC3          22
        "Misc 4",          // SDL_GAMEPAD_BUTTON_MISC4          23
        "Misc 5",          // SDL_GAMEPAD_BUTTON_MISC5          24
        "Misc 6",          // SDL_GAMEPAD_BUTTON_MISC6          25
        "Left Trigger",    // Left Trigger                      26
        "Right Trigger",   // Right Trigger                     27
    };
    static_assert(sizeof(names) / sizeof(names[0]) == SDL_GAMEPAD_BUTTON_COUNT + 2,
        "Input name table size mismatch — update when SDL_GAMEPAD_BUTTON_COUNT changes");
    if (button_idx < 0 || button_idx >= static_cast<int>(sizeof(names) / sizeof(names[0])))
        return "<none>";
    return names[button_idx];
}

const char* gamepad_get_button_display_name(SDL_GamepadType type, int button_idx)
{
    // Unknown / generic controllers: use positional names only.
    if (type == SDL_GAMEPAD_TYPE_UNKNOWN || type == SDL_GAMEPAD_TYPE_STANDARD)
        return gamepad_get_button_name(button_idx);

    // Face buttons (0–3): SDL handles per-controller label mapping, including Switch A/B swap.
    if (button_idx >= 0 && button_idx < 4) {
        const char* label_name = get_label_name(
            SDL_GetGamepadButtonLabelForType(type, static_cast<SDL_GamepadButton>(button_idx)));
        if (label_name)
            return label_name;
    }

    // Non-face buttons: look up in the controller-specific table.
    const char* result = nullptr;
    switch (type) {
        case SDL_GAMEPAD_TYPE_XBOX360:
            result = search_overrides(xbox360_overrides, button_idx); break;
        case SDL_GAMEPAD_TYPE_XBOXONE:
            result = search_overrides(xboxone_overrides, button_idx); break;
        case SDL_GAMEPAD_TYPE_PS3:
            result = search_overrides(ps3_overrides, button_idx); break;
        case SDL_GAMEPAD_TYPE_PS4:
            result = search_overrides(ps4_overrides, button_idx); break;
        case SDL_GAMEPAD_TYPE_PS5:
            result = search_overrides(ps5_overrides, button_idx); break;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
            result = search_overrides(switchpro_overrides, button_idx); break;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
            result = search_overrides(switchjoycon_overrides, button_idx); break;
        case SDL_GAMEPAD_TYPE_GAMECUBE:
            result = search_overrides(gamecube_overrides, button_idx); break;
        default:
            break;
    }

    return result ? result : gamepad_get_button_name(button_idx);
}

static SDL_GamepadType icon_type_to_sdl(ControllerIconType icon)
{
    switch (icon) {
        case ControllerIconType::Xbox360:         return SDL_GAMEPAD_TYPE_XBOX360;
        case ControllerIconType::XboxOne:         return SDL_GAMEPAD_TYPE_XBOXONE;
        case ControllerIconType::PS3:             return SDL_GAMEPAD_TYPE_PS3;
        case ControllerIconType::PS4:             return SDL_GAMEPAD_TYPE_PS4;
        case ControllerIconType::PS5:             return SDL_GAMEPAD_TYPE_PS5;
        case ControllerIconType::NintendoSwitch:  return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO;
        case ControllerIconType::NintendoGameCube: return SDL_GAMEPAD_TYPE_GAMECUBE;
        default:                                  return SDL_GAMEPAD_TYPE_UNKNOWN;
    }
}

const char* gamepad_get_effective_display_name(ControllerIconType icon_pref, SDL_GamepadType connected_type, int button_idx)
{
    SDL_GamepadType type = (icon_pref == ControllerIconType::Auto)
        ? connected_type
        : icon_type_to_sdl(icon_pref);
    return gamepad_get_button_display_name(type, button_idx);
}
