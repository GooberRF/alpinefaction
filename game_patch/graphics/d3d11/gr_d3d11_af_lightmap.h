#pragma once

#include <cstdint>
#include <vector>
#include <d3d11.h>

namespace rf
{
    struct GSolid;
    struct GSurface;
    struct Vector3;
}

namespace gr::d3d11
{
    // Batch key slot used instead of the stock lightmap bm handle for faces that carry an alpine
    // chart. Negative so the texture manager resolves it to a null SRV, but not -1 so the alpha
    // render mode a face gets still depends on whether it had a stock lightmap.
    constexpr int af_lightmap_batch_key = -2;

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
        std::uint32_t lm_w = 0;
        std::uint32_t lm_h = 0;
        std::uint32_t surf_x = 0;
        std::uint32_t surf_y = 0;
        std::uint32_t k_u = 0;
        std::uint32_t k_v = 0;
    };

    // With no stock lightmaps section, the engine's synthesised page's bitmap handle, else -1.
    // Geomod pages created later are real.
    int af_lightmap_synthesized_page_bm();

    // Charts are positional over the STATIC solid's geometry::surfaces, so a mover's own solid
    // never resolves one.
    bool af_lightmap_face_setup(rf::GSolid* solid, int surface_index, AfLightmapFace& out);
    void af_lightmap_face_texel(const AfLightmapFace& face, const rf::Vector3& pos, float& out_u,
                                float& out_v);

    struct AfLightmapConstants
    {
        float enabled;
        float page_size;
        float tile_step;
        float gutter;
    };
    AfLightmapConstants af_lightmap_constants();

    // The terrain chart the terrain pixel shader samples: its af_lm_index record and its XZ mapping.
    struct AfTerrainChart
    {
        int chart;
        float origin_x;
        float origin_z;
        float texel_size;
    };
    // For terrain `terrain_index` (alpine_terrain_get_all order), when the atlas is live and the level
    // carries a chart af_lightmap_resolve_terrains matched to it.
    bool af_lightmap_terrain_chart(int terrain_index, AfTerrainChart& out);

    void af_lightmap_bind(ID3D11Device* device, ID3D11DeviceContext* context);

    // Defined in gr_d3d11_hooks.cpp, which owns the renderer instance. Null before the renderer
    // exists and after it is torn down.
    ID3D11Device* af_lightmap_device();

    bool af_lightmap_upload(ID3D11Device* device, const std::vector<std::uint8_t>& blocks);
    void af_lightmap_release_gpu();
}
