#pragma once

// Include after the binary's rf engine types (rf::Vector3).

#include <algorithm>
#include <common/lighting/alpine_lighting.h>

// Per-level directional sunlight (AlpineLevelProperties v5)
struct SunLightState
{
    bool enabled = false;
    bool affects_meshes = true;
    bool drives_shadowmap_dir = true;
    int mesh_mode = 0;
    rf::Vector3 travel_dir{0.0f, -1.0f, 0.0f}; // direction the light travels
    float color[3] = {0.0f, 0.0f, 0.0f};       // premultiplied by sun intensity
};

inline SunLightState sun_light_state(const alpine_lighting::SunState& sun)
{
    SunLightState state;
    state.enabled = sun.enabled;
    state.affects_meshes = sun.affects_meshes;
    state.drives_shadowmap_dir = sun.drives_shadowmap_dir;
    state.mesh_mode = sun.mesh_mode;
    state.travel_dir = {sun.travel_dir[0], sun.travel_dir[1], sun.travel_dir[2]};
    std::copy(std::begin(sun.color), std::end(sun.color), state.color);
    return state;
}
