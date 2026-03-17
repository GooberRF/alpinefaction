// Central SDL event pump for all input subsystems.
#include <SDL3/SDL.h>
#include "input.h"
#include "../misc/alpine_settings.h"

void sdl_input_poll()
{
    if (SDL_IsMainThread())
        SDL_PumpEvents();
    if (g_alpine_game_config.input_mode == 2)
        keyboard_sdl_poll();
    mouse_sdl_poll();
}
