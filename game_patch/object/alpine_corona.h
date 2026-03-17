#pragma once

#include "../rf/file/file.h"
#include <cstddef>

void alpine_corona_load_chunk(rf::File& file, std::size_t chunk_len);
void alpine_corona_create_all();
void alpine_corona_clear_state();
