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
        case SDL_EVENT_GAMEPAD_REMOVED:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
        case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
        case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
            process_gamepad_event(ev);
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            process_gamepad_event(ev);
            hud_mark_bindings_dirty();
            break;
        default:
            break;
        }
    }
}

static InputDevice g_active_device = InputDevice::KeyboardMouse;
static SDL_JoystickID g_recent_gamepad_id = 0;

void input_record_activity(InputDevice device, SDL_JoystickID gamepad_id)
{
    const bool gamepad_changed = device == InputDevice::Gamepad && gamepad_id != 0
        && gamepad_id != g_recent_gamepad_id;
    const bool device_changed = device != g_active_device;
    if (device == InputDevice::Gamepad && gamepad_id != 0)
        g_recent_gamepad_id = gamepad_id;
    g_active_device = device;
    if (device_changed || gamepad_changed)
        hud_mark_bindings_dirty();
}

InputDevice input_active_device()
{
    return g_active_device;
}

InputDevice input_prompt_device()
{
    if (g_alpine_game_config.input_prompt_override == 1) return InputDevice::Gamepad;
    if (g_alpine_game_config.input_prompt_override == 2) return InputDevice::KeyboardMouse;
    return g_active_device;
}

SDL_JoystickID input_recent_gamepad_id()
{
    return g_recent_gamepad_id;
}

void input_clear_recent_gamepad()
{
    g_recent_gamepad_id = 0;
    hud_mark_bindings_dirty();
}
