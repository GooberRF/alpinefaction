#pragma once

#include <cstddef>
#include "../rf/file/file.h"
#include "../rf/math/matrix.h"
#include "../rf/math/vector.h"

namespace rf
{
    struct GRoom;
}

void weather_apply_patch();
void weather_render();
void weather_clear_regions();
bool weather_set_region_enabled(int uid, bool enabled);
bool weather_move_region(int uid, const rf::Vector3& pos);
bool weather_move_region(int uid, const rf::Vector3& pos, const rf::Matrix3& orient);
void weather_notify_geomod(const rf::Vector3& pos, float radius);
void weather_load_chunk(rf::File& file, std::size_t chunk_len);
// The room's liquid surface height; false when the room holds no liquid.
bool room_liquid_surface_y(rf::GRoom* room, float& out_y);
