#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <common/alpine_dir_light.h>
#include "../rf/file/file.h"

// Runtime state of one Directional Light object (RFL chunk 0x0AFBAE0C). Its baked contribution lives in
// the lightmaps; this drives the per-pixel term on meshes only.
struct AlpineDirLight
{
    int32_t uid = -1;
    float color[3] = {1.0f, 1.0f, 1.0f}; // 0..1, not premultiplied by intensity
    float intensity = alpine_dir_light::default_intensity;
    bool on = true;
    bool affects_meshes = true;
    uint8_t mesh_mode = static_cast<uint8_t>(alpine_dir_light::MeshMode::lightmap_scaled);
    alpine_dir_light::Volume volume{};
    float bounding_radius = 0.0f; // about volume.center; +infinity when unbounded
};

void alpine_dir_light_load_chunk(rf::File& file, std::size_t chunk_len, int content_version);
void alpine_dir_light_clear_state();
const std::vector<AlpineDirLight>& alpine_dir_light_get_all();
// Bumped by every change to the list or to a light in it.
uint32_t alpine_dir_light_generation();
// Both apply to every light with that uid; false when there is none.
bool alpine_dir_light_set_on(int uid, bool on);
bool alpine_dir_light_set_color(int uid, float r, float g, float b);
