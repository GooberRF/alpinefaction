#pragma once

#include <random>
#include <common/config/GameConfig.h>
#include <common/config/AlpineCoreConfig.h>

extern GameConfig g_game_config;
extern AlpineCoreConfig g_alpine_system_config;

// random number generator
extern std::mt19937 g_rng;
void initialize_random_generator();

#ifdef _WINDOWS_
extern HMODULE g_hmodule;
#endif

void evaluate_fullbright_meshes();
