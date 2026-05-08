#include <SDL3/SDL.h>
#include "input.h"
#include "mouse.h"
#include "../os/os.h"

void sdl_input_poll()
{
    if (!g_sdl_window) return;

    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            process_keyboard_event(ev);
            break;
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
            process_mouse_event(ev);
            break;
        default:
            break;
        }
    }
}
