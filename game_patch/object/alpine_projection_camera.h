#pragma once

#include "../rf/file/file.h"
#include <cstddef>

void alpine_projection_camera_load_chunk(rf::File& file, std::size_t chunk_len);
void alpine_projection_camera_clear_state();

// True if the handle belongs to a Projection Camera created from this level's chunk. Lets
// Display_Projection pick its viewpoint out of links that may point at anything.
bool alpine_projection_camera_is_camera(int handle);
