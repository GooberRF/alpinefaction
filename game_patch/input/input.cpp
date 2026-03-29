// Central SDL event pump for all input subsystems.
#include <SDL3/SDL.h>
#include "input.h"

void sdl_input_poll()
{
    if (SDL_WasInit(SDL_INIT_EVENTS) != 0) {
        SDL_PumpEvents();
    }
}
