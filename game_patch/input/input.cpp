#include <SDL3/SDL.h>
#include "input.h"
#include "gamepad.h"
#include "../misc/misc.h"
#include "../misc/alpine_settings.h"

void sdl_input_poll()
{
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        case SDL_EVENT_GAMEPAD_ADDED:
        case SDL_EVENT_GAMEPAD_REMOVED:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
            process_gamepad_event(ev);
            break;
        default:
            break;
        }
    }
}

static bool g_last_input_was_gamepad = false;
static SDL_JoystickID g_last_active_gamepad_id = 0; // which controller last produced input

void input_active_gamepad(SDL_JoystickID which)
{
    if (which != 0)
        g_last_active_gamepad_id = which;
    if (!g_last_input_was_gamepad) {
        g_last_input_was_gamepad = true;
        hud_mark_bindings_dirty();
    }
}

void input_active_keyboard_mouse()
{
    if (g_last_input_was_gamepad) {
        g_last_input_was_gamepad = false;
        hud_mark_bindings_dirty();
    }
}

bool input_last_gamepad_active()
{
    if (g_alpine_game_config.input_prompt_override == 1) return true;
    if (g_alpine_game_config.input_prompt_override == 2) return false;
    return g_last_input_was_gamepad;
}

SDL_JoystickID input_get_last_active_gamepad_id()
{
    return g_last_active_gamepad_id;
}

void input_reset_last_active_gamepad_id()
{
    g_last_active_gamepad_id = 0;
}
