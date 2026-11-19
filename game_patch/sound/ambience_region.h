#pragma once

#include <cstddef>
#include "../rf/file/file.h"
#include "../rf/math/matrix.h"
#include "../rf/math/vector.h"

void ambience_region_load_chunk(rf::File& file, std::size_t chunk_len);
void ambience_region_clear_state();
bool ambience_region_set_enabled(int uid, bool enabled);
bool ambience_region_move(int uid, const rf::Vector3& pos);
bool ambience_region_move(int uid, const rf::Vector3& pos, const rf::Matrix3& orient);
void ambience_region_update(const rf::Vector3& listener_pos);
