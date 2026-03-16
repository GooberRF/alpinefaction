// Central SDL event pump for all input subsystems.
#include <SDL3/SDL.h>
#include "input.h"

void sdl_input_poll()
{
    if (SDL_IsMainThread())
        SDL_PumpEvents();
    keyboard_sdl_poll();
    mouse_sdl_poll();
}
