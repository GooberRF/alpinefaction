#pragma once

#include "../rf/math/vector.h"

// Where a Payload medallion sits over the cart's centre at `cart_center`; a checkpoint's and the cart's share it
rf::Vector3 payload_medallion_center(const rf::Vector3& cart_center);

void payload_world_render();
void payload_world_level_unload();
