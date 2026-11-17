#pragma once

// The level ambient and sun as the game's renderer derives them, shared by the game and RED's bakes.
//
// Pure functions of plain values: callers read engine or editor state and pass it in. The game's render
// path runs through these, so any change here changes what it draws.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace alpine_lighting
{

// The most a lightmap texel may give a mesh as its ambient (GSolid_get_ambient_color_hook); white reads as none.
inline constexpr float max_mesh_ambient_texel = 254.0f / 255.0f;

// A level ambient byte as the level loader (0x004618B0) hands it to light_set_ambient: times the float 1/255.
inline float level_ambient_channel(uint8_t c)
{
    return static_cast<float>(c) * (1.0f / 255.0f);
}

struct Direction
{
    float x, y, z;
};

// Unit vector pointing toward the sun. The light travel direction is its negation.
inline Direction sun_to_light_dir(float yaw_deg, float pitch_deg)
{
    constexpr float deg_to_rad = 3.14159265358979f / 180.0f;
    const float yaw = yaw_deg * deg_to_rad;
    const float pitch = pitch_deg * deg_to_rad;
    const float cp = std::cos(pitch);
    return {cp * std::sin(yaw), std::sin(pitch), cp * std::cos(yaw)};
}

struct SunState
{
    bool enabled = false;
    bool affects_meshes = true;
    bool drives_shadowmap_dir = true;
    int mesh_mode = 0;
    float travel_dir[3] = {0.0f, -1.0f, 0.0f}; // direction the light travels
    float color[3] = {0.0f, 0.0f, 0.0f};       // premultiplied by sun intensity
};

// From a level's sun properties: Props is either binary's AlpineLevelProperties.
template<typename Props>
SunState sun_state(const Props& props)
{
    SunState state;
    if (!props.enable_sun) {
        return state;
    }
    // A NaN direction would take a whole frame of lighting with it.
    if (!std::isfinite(props.sun_yaw) || !std::isfinite(props.sun_pitch)) {
        return state;
    }
    const float yaw = props.sun_yaw;
    const float pitch = props.sun_pitch;
    const float intensity =
        std::isfinite(props.sun_intensity) ? std::clamp(props.sun_intensity, 0.0f, 10.0f) : 0.0f;

    state.enabled = true;
    state.affects_meshes = props.sun_affects_meshes;
    state.drives_shadowmap_dir = props.sun_drives_shadowmap_dir;
    state.mesh_mode = props.sun_mesh_mode;
    const Direction to_sun = sun_to_light_dir(yaw, pitch);
    state.travel_dir[0] = -to_sun.x;
    state.travel_dir[1] = -to_sun.y;
    state.travel_dir[2] = -to_sun.z;
    state.color[0] = props.sun_color_r / 255.0f * intensity;
    state.color[1] = props.sun_color_g / 255.0f * intensity;
    state.color[2] = props.sun_color_b / 255.0f * intensity;
    return state;
}

// How much of the sun a mesh with this ambient takes: none unless the sun lights meshes, all in mesh mode 1,
// else scaled by the ambient's luminance, full from 0.5 up.
inline float sun_mesh_scale(bool sun_lights_meshes, int mesh_mode, const float* ambient)
{
    if (!sun_lights_meshes) {
        return 0.0f;
    }
    if (mesh_mode != 0) {
        return 1.0f;
    }
    float luminance = ambient[0] * 0.299f + ambient[1] * 0.587f + ambient[2] * 0.114f;
    return std::clamp(luminance * 2.0f, 0.0f, 1.0f);
}

// The D3D11 mesh ambient for a lightmap texel (0..1) under the mesh: mostly the level ambient, tinted by it.
inline void mesh_blend_ambient(const float (&level_ambient)[3], const float (&lightmap)[3], float (&out)[3])
{
    constexpr float blend = 0.45f;
    for (int i = 0; i < 3; i++) {
        out[i] = level_ambient[i] * (1.0f - blend) + lightmap[i] * blend;
    }
}

// A terrain's light where it has no baked chart (ter_base_light): the level ambient plus the sun's N.L, as a
// lightmap texel, which is drawn doubled, hence the half; times `scale`.
inline void terrain_fallback_texel(const float (&ambient)[3], const float (&sun_travel)[3],
                                   const float (&sun_color)[3], const float (&n)[3], float scale, float (&texel)[3])
{
    const float n_dot_l =
        std::clamp(-(n[0] * sun_travel[0] + n[1] * sun_travel[1] + n[2] * sun_travel[2]), 0.0f, 1.0f);
    for (int i = 0; i < 3; i++) {
        texel[i] = (ambient[i] + sun_color[i] * n_dot_l) * 0.5f * scale;
    }
}

} // namespace alpine_lighting
