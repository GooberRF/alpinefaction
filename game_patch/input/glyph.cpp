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
// Face button glyphs takes priority
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

// shared names — shared by PlayStation and Steam Deck families.
// Provides L1/R1/L2/R2/L3/R3/L4/R4/L5/R5 and misc button names for buttons
// not covered by a family-specific override table.
static const ButtonOverride shared_glyphs[] = {
    {  7, "L3"        },  // Left stick click
    {  8, "R3"        },  // Right stick click
    {  9, "L1"        },  // Left shoulder
    { 10, "R1"        },  // Right shoulder
    { 15, "M1"        },
    { 16, "L4"        },  // Left back upper paddle
    { 17, "L5"        },  // Left back lower paddle
    { 18, "R4"        },  // Right back upper paddle
    { 19, "R5"        },  // Right back lower paddle
    { 21, "M2"        },
    { 22, "M3"        },
    { 23, "M4"        },
    { 26, "L2"        },
    { 27, "R2"        },
};

// Xbox 360-specific overrides (Back/Start differ from Xbox One's View/Menu)
static const ButtonOverride xbox360_overrides[] = {
    {  4, "Back"  },
    {  6, "Start" },
};

// Xbox One/Series overrides — also used as fallback for Xbox 360
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
    {  4, "Select" },
    {  5, "PS"     },
    {  6, "Start"  },
};

static const ButtonOverride ps4_overrides[] = {
    {  4, "Share"          },
    {  5, "PS"             },
    {  6, "Options"        },
    { 20, "Touchpad Click" },
};

static const ButtonOverride ps5_overrides[] = {
    {  4, "Create"    },
    {  5, "PS"        },
    {  6, "Options"   },
    { 15, "Mic"       },
    { 16, "RB Paddle" },
    { 17, "LB Paddle" },
    { 18, "Right Fn"        },
    { 19, "Left Fn"          },
    { 20, "Touchpad Click"   },
};

static const ButtonOverride switchpro_overrides[] = {
    {  4, "-"        },
    {  5, "Home"     },
    {  6, "+"        },
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

static const ButtonOverride gamecube_overrides[] = {
    {  9, "Z"       },  // SDL_GAMEPAD_BUTTON_LEFT_SHOULDER  → Z
    { 10, "R"       },  // SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER → R
    { 22, "L Click" },  // SDL_GAMEPAD_BUTTON_MISC3          → L trigger digital
    { 23, "R Click" },  // SDL_GAMEPAD_BUTTON_MISC4          → R trigger digital
    { 26, "L"       },
    { 27, "R"       },
};

static const ButtonOverride steamdeck_overrides[] = {
    {  4, "View"  },
    {  5, "Steam" },
    {  6, "Menu"  },
    { 15, "..."   },   // Quick Access button
};

static const ButtonOverride steamcontroller_overrides[] = {
    {  4, "Back"                 },
    {  5, "Steam"                },
    {  6, "Start"                },
    {  7, "Stick Click"          },
    {  8, "Right Trackpad Click" },
    { 16, "RG"                   },  // Right grip
    { 17, "LG"                   },  // Left grip
};

const char* gamepad_get_button_name(int button_idx)
{
    static const char* names[] = {
        "South",              // SDL_GAMEPAD_BUTTON_SOUTH          0
        "East",               // SDL_GAMEPAD_BUTTON_EAST           1
        "West",               // SDL_GAMEPAD_BUTTON_WEST           2
        "North",              // SDL_GAMEPAD_BUTTON_NORTH          3
        "Back",               // SDL_GAMEPAD_BUTTON_BACK           4
        "Guide",              // SDL_GAMEPAD_BUTTON_GUIDE          5
        "Start",              // SDL_GAMEPAD_BUTTON_START          6
        "Left Stick Click",   // SDL_GAMEPAD_BUTTON_LEFT_STICK     7
        "Right Stick Click",  // SDL_GAMEPAD_BUTTON_RIGHT_STICK    8
        "Left Shoulder",      // SDL_GAMEPAD_BUTTON_LEFT_SHOULDER  9
        "Right Shoulder",     // SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER 10
        "D-Pad Up",           // SDL_GAMEPAD_BUTTON_DPAD_UP        11
        "D-Pad Down",         // SDL_GAMEPAD_BUTTON_DPAD_DOWN      12
        "D-Pad Left",         // SDL_GAMEPAD_BUTTON_DPAD_LEFT      13
        "D-Pad Right",        // SDL_GAMEPAD_BUTTON_DPAD_RIGHT     14
        "Misc 1",             // SDL_GAMEPAD_BUTTON_MISC1          15
        "Right Paddle 1",     // SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1  16
        "Left Paddle 1",      // SDL_GAMEPAD_BUTTON_LEFT_PADDLE1   17
        "Right Paddle 2",     // SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2  18
        "Left Paddle 2",      // SDL_GAMEPAD_BUTTON_LEFT_PADDLE2   19
        "Touchpad",           // SDL_GAMEPAD_BUTTON_TOUCHPAD       20
        "Misc 2",             // SDL_GAMEPAD_BUTTON_MISC2          21
        "Misc 3",             // SDL_GAMEPAD_BUTTON_MISC3          22
        "Misc 4",             // SDL_GAMEPAD_BUTTON_MISC4          23
        "Misc 5",             // SDL_GAMEPAD_BUTTON_MISC5          24
        "Misc 6",             // SDL_GAMEPAD_BUTTON_MISC6          25
        "Left Trigger",       // Left Trigger                      26
        "Right Trigger",      // Right Trigger                     27
    };
    static_assert(sizeof(names) / sizeof(names[0]) == SDL_GAMEPAD_BUTTON_COUNT + 2,
        "Input name table size mismatch — update when SDL_GAMEPAD_BUTTON_COUNT changes");
    if (button_idx < 0 || button_idx >= static_cast<int>(sizeof(names) / sizeof(names[0])))
        return "<none>";
    return names[button_idx];
}

// Maps ControllerIconType to SDL_GamepadType for face-button label lookup.
static SDL_GamepadType icon_type_to_sdl(ControllerIconType icon)
{
    switch (icon) {
        case ControllerIconType::Xbox360:          return SDL_GAMEPAD_TYPE_XBOX360;
        case ControllerIconType::XboxOne:          return SDL_GAMEPAD_TYPE_XBOXONE;
        case ControllerIconType::PS3:              return SDL_GAMEPAD_TYPE_PS3;
        case ControllerIconType::PS4:              return SDL_GAMEPAD_TYPE_PS4;
        case ControllerIconType::PS5:              return SDL_GAMEPAD_TYPE_PS5;
        case ControllerIconType::NintendoSwitch:   return SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO;
#if SDL_VERSION_ATLEAST(3, 2, 0)
        case ControllerIconType::NintendoGameCube: return SDL_GAMEPAD_TYPE_GAMECUBE;
#endif
        // Steam devices use Xbox-style A/B/X/Y face labels
        case ControllerIconType::SteamController:
        case ControllerIconType::SteamDeck:        return SDL_GAMEPAD_TYPE_XBOXONE;
        default:                                   return SDL_GAMEPAD_TYPE_UNKNOWN;
    }
}

// Maps SDL_GamepadType to ControllerIconType for auto-detection.
static ControllerIconType sdl_type_to_icon(SDL_GamepadType type)
{
    switch (type) {
        case SDL_GAMEPAD_TYPE_XBOX360:                       return ControllerIconType::Xbox360;
        case SDL_GAMEPAD_TYPE_XBOXONE:                       return ControllerIconType::XboxOne;
        case SDL_GAMEPAD_TYPE_PS3:                           return ControllerIconType::PS3;
        case SDL_GAMEPAD_TYPE_PS4:                           return ControllerIconType::PS4;
        case SDL_GAMEPAD_TYPE_PS5:                           return ControllerIconType::PS5;
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
        case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:  return ControllerIconType::NintendoSwitch;
#if SDL_VERSION_ATLEAST(3, 2, 0)
        case SDL_GAMEPAD_TYPE_GAMECUBE:                      return ControllerIconType::NintendoGameCube;
#endif
        default:                                             return ControllerIconType::Generic;
    }
}

// Returns true for controller families that use shared naming as their shared standard tier
// (L1/R1/L2/R2/L3/R3 etc.) for buttons not covered by a platform-specific override.
static bool uses_shared_glyphs(ControllerIconType type)
{
    switch (type) {
        case ControllerIconType::PS3:
        case ControllerIconType::PS4:
        case ControllerIconType::PS5:
        case ControllerIconType::SteamDeck:
            return true;
        default:
            return false;
    }
}

const char* gamepad_get_button_display_name(ControllerIconType type, int button_idx)
{
    // Tier 1: SDL face-button label (buttons 0–3).
    // SDL handles per-controller A/B/X/Y label mapping including Switch A/B swap.
    // Generic/Auto have no SDL type (→ UNKNOWN), so this tier is naturally skipped for them.
    if (button_idx >= 0 && button_idx < 4) {
        SDL_GamepadType sdl_type = icon_type_to_sdl(type);
        if (sdl_type != SDL_GAMEPAD_TYPE_UNKNOWN) {
            const char* label = get_label_name(
                SDL_GetGamepadButtonLabelForType(sdl_type, static_cast<SDL_GamepadButton>(button_idx)));
            if (label)
                return label;
        }
    }

    // Tier 2: Family-specific override table.
    const char* result = nullptr;
    switch (type) {
        case ControllerIconType::Xbox360:
            result = search_overrides(xbox360_overrides, button_idx);
            if (result) return result;
            result = search_overrides(xboxone_overrides, button_idx);  // LB/RB/LS/RS/LT/RT/etc.
            break;
        case ControllerIconType::XboxOne:
            result = search_overrides(xboxone_overrides, button_idx);
            break;
        case ControllerIconType::PS3:
            result = search_overrides(ps3_overrides, button_idx);
            break;
        case ControllerIconType::PS4:
            result = search_overrides(ps4_overrides, button_idx);
            break;
        case ControllerIconType::PS5:
            result = search_overrides(ps5_overrides, button_idx);
            break;
        case ControllerIconType::NintendoSwitch:
            result = search_overrides(switchpro_overrides, button_idx);
            break;
        case ControllerIconType::NintendoGameCube:
            result = search_overrides(gamecube_overrides, button_idx);
            break;
        case ControllerIconType::SteamController:
            result = search_overrides(steamcontroller_overrides, button_idx);
            if (result) return result;
            result = search_overrides(xboxone_overrides, button_idx);  // LB/RB/LT/RT
            break;
        case ControllerIconType::SteamDeck:
            result = search_overrides(steamdeck_overrides, button_idx);
            break;
        default:
            break;
    }

    if (result)
        return result;

    // Tier 3: Apple MFi shared standard (L1/R1/L2/R2/L3/R3 etc.)
    // Covers stick clicks, shoulders, and paddles for PlayStation and Steam Deck families.
    if (uses_shared_glyphs(type)) {
        const char* mfi = search_overrides(shared_glyphs, button_idx);
        if (mfi)
            return mfi;
    }

    // Tier 4: SDL positional fallback — always defined for every button index.
    return gamepad_get_button_name(button_idx);
}

const char* gamepad_get_effective_display_name(ControllerIconType icon_pref, SDL_Gamepad* ctrl, int button_idx)
{
    ControllerIconType type;
    if (icon_pref == ControllerIconType::Auto) {
        SDL_GamepadType sdl_type = ctrl ? SDL_GetGamepadType(ctrl) : SDL_GAMEPAD_TYPE_UNKNOWN;
        type = get_steam_virtual_controller_detection(ctrl, sdl_type_to_icon(sdl_type));
    } else {
        type = icon_pref;
    }
    return gamepad_get_button_display_name(type, button_idx);
}
