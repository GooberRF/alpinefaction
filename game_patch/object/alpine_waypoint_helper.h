#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <common/alpine_waypoint_helper.h>
#include "../rf/file/file.h"
#include "../rf/math/vector.h"

// A Waypoint Helper read from the level: what waypoint generation builds the AWP's vehicle layer from (no object,
// render, collision or network presence).
struct AlpineWaypointHelper
{
    int32_t uid = -1;
    rf::Vector3 pos{};
    alpine_waypoint_helper::Type type = alpine_waypoint_helper::Type::air;
    uint8_t flags = 0;
    // Ground helpers only; a size past the known ones reads as the largest.
    alpine_waypoint_helper::SizeClass size_class = alpine_waypoint_helper::SizeClass::driller;
    float radius = alpine_waypoint_helper::default_radius;
    // The mapper's links as saved, less self links and repeats; targets are checked when the layer is generated.
    std::vector<int32_t> links;

    bool landing() const
    {
        return (flags & alpine_waypoint_helper::flag_landing) != 0;
    }
};

void alpine_waypoint_helper_load_chunk(rf::File& file, std::size_t chunk_len);
void alpine_waypoint_helper_clear_state();

const std::vector<AlpineWaypointHelper>& alpine_waypoint_helpers();
// Changes whenever the helper list does (each load and each clear).
int alpine_waypoint_helpers_serial();
