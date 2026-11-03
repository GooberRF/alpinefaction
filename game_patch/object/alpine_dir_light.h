#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <common/alpine_dir_light.h>
#include "../rf/file/file.h"

void alpine_dir_light_load_chunk(rf::File& file, std::size_t chunk_len, int content_version);
void alpine_dir_light_clear_state();
const std::vector<AlpineDirLight>& alpine_dir_light_get_all();
// Bumped by every change to the list or to a light in it.
uint32_t alpine_dir_light_generation();
// Both apply to every light with that uid; false when there is none.
bool alpine_dir_light_set_on(int uid, bool on);
bool alpine_dir_light_set_color(int uid, float r, float g, float b);
