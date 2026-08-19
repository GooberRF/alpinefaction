#pragma once

#include <cstddef>
#include "../rf/file/file.h"
#include "../rf/math/matrix.h"
#include "../rf/math/vector.h"

void weather_apply_patch();
void weather_render();
void weather_clear_regions();
bool weather_set_region_enabled(int uid, bool enabled);
bool weather_move_region(int uid, const rf::Vector3& pos);
bool weather_move_region(int uid, const rf::Vector3& pos, const rf::Matrix3& orient);
void weather_load_chunk(rf::File& file, std::size_t chunk_len);
