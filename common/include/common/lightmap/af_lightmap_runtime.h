#pragma once

#include <cstdint>

// Alpine Lightmaps (RFL section 0x0AFBAE09): the chart records a binary's lightmap runtime hands the shared
// Direct3D 11 renderer.

// Everything the solid builder needs to place one face's vertices in the atlas. chart < 0
// means this face has no alpine chart and keeps the stock lightmap UVs.
struct AfLightmapFace
{
    int chart = -1;
    int axis_u = 0;
    int axis_v = 0;
    float scale_u = 0.0f;
    float scale_v = 0.0f;
    float add_u = 0.0f;
    float add_v = 0.0f;
    uint32_t lm_w = 0;
    uint32_t lm_h = 0;
    uint32_t surf_x = 0;
    uint32_t surf_y = 0;
    uint32_t k_u = 0;
    uint32_t k_v = 0;
};

struct AfLightmapConstants
{
    float enabled;
    float page_size;
    float tile_step;
    float gutter;
};

// The terrain chart the terrain pixel shader samples: its af_lm_index record and its XZ mapping.
struct AfTerrainChart
{
    int chart;
    float origin_x;
    float origin_z;
    float texel_size;
};

// The overflow chart lighting a face of the level solid, while the D3D11 renderer holds the atlas.
struct AfOverflowFace
{
    int chart = -1;
    uint32_t record = 0;
};
