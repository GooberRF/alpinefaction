#pragma once

#include <random>
#include <ctime>
#include <common/config/GameConfig.h>
#include <common/config/AlpineCoreConfig.h>

extern GameConfig g_game_config;
extern AlpineCoreConfig g_alpine_system_config;
extern std::mt19937 g_rng;
extern std::time_t g_process_startup_time;

// The Alpine world effects that need no player or session (weather, ropes, terrain decorations)
void render_level_world_effects();

#ifdef _WINDOWS_
extern HMODULE g_hmodule;
#endif
