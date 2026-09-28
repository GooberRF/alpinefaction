#pragma once

#include <cstddef>
#include <cstdint>

namespace rf
{
    class File;
    struct VFile;
    struct GSolid;
    struct Vector3;
}

// Alpine Lightmaps (RFL section 0x0AFBAE09), game side. The D3D11 renderer holds the atlas
// (gr::d3d11::AfLightmapRenderer); terrain charts also feed the CPU light sampler on every renderer.

// A level whose alpine props set d3d11_only_lightmaps carries no stock 0x1200 lightmaps section,
// so the legacy renderers would draw its whole world fullbright. Pure, so the matrix that decides
// it can be exercised without a device.
constexpr bool af_lightmap_level_refused(bool d3d11_only, bool dedicated_server, bool headless,
                                         bool renderer_is_d3d11)
{
    return d3d11_only && !dedicated_server && !headless && !renderer_is_d3d11;
}

void af_lightmap_level_reset();

// Called for the stock 0x1200 lightmaps chunk; without one, stock pages sample a neutral texture.
void af_lightmap_note_stock_lightmaps();

// Run once the geometry section has been read: whitens the synthesised page's pixels, which the
// ambient colour sampler reads directly, so they say "no lightmap data" rather than heap garbage.
void af_lightmap_init_synthesized_page();

// Run once the geometry section has been read and the file cursor sits at its end: hashes the
// surface records the section just consumed, which is the fingerprint the bake recorded.
void af_lightmap_capture_surface_fingerprint(rf::File& file);

// Run by the movers section (0x2000) right after it reads a mover's solid from the memory VFile
// `reader`: hashes the surface records just consumed, the mover chart's fingerprint.
void af_lightmap_capture_mover(int uid, rf::GSolid* solid, const rf::VFile& reader);

void af_lightmap_load_chunk(rf::File& file, std::size_t chunk_len);

// Run once the level has loaded (terrains and the section both read): logs how many terrains have a
// chart (matched by uid, grid size and alpine_terrain::lighting_fingerprint when the section loaded).
void af_lightmap_resolve_terrains();

// The baked light at world (x, z) on terrain `terrain_index` (alpine_terrain_get_all order) as a
// stock lightmap texel, 0..1 per channel and drawn doubled; false when it has no matched chart.
bool af_lightmap_terrain_sample(int terrain_index, float world_x, float world_z, float (&texel)[3]);

// ─── charts for the D3D11 renderer, handed out only while it holds the atlas ───

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

// Surface charts are positional over the static solid's geometry::surfaces, mover charts over the
// surfaces of a mover solid their record matched; out.chart is the af_lm_index chart record.
bool af_lightmap_face_setup(rf::GSolid* solid, int surface_index, AfLightmapFace& out);
void af_lightmap_face_texel(const AfLightmapFace& face, const rf::Vector3& pos, float& out_u, float& out_v);

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
