#pragma once

// Include after the binary's rf engine types (rf::Vector3).

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
