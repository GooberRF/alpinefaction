#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include "level.h"

// A solid's GSolid::surfaces; empty for no solid.
inline std::span<GSurface* const> solid_surfaces(const GSolid* solid)
{
    if (!solid || solid->surfaces.size <= 0 || !solid->surfaces.data_ptr) {
        return {};
    }
    return {solid->surfaces.data_ptr, static_cast<std::size_t>(solid->surfaces.size)};
}

// Bake side, driven from editor_patch/lightmap.cpp. bake_begin lays the charts out and returns the
// page count; bake_allocate then allocates the pages.
std::uint32_t alpine_lm_bake_begin();
void alpine_lm_bake_allocate();
void alpine_lm_bake_end();
// Drops a bake that did not run to completion without encoding it.
void alpine_lm_bake_abort();
void alpine_lm_shade_surface(GSolid* solid, GSurface* surface, int mode);
// Calculate Lighting's address-space check before anything is freed; reports and returns false on a refusal.
bool lighting_calc_memory_admits();
// Reports why Calculate Lighting was not run, and marks a headless bake refused.
void lighting_calc_report_refusal(const char* msg);
void alpine_lm_blend_edge(const GSurface* surf_a, const GSurface* surf_b, const float* p0, const float* p1);

// True while a per-tile shading call is running inside FUN_004ac470, i.e. while the surface is
// repointed at a tile view. The preview-texture blocks of that function must not run then.
bool alpine_lm_tile_pass_active();

// Calculate Lighting's surface pass records whether its terrain check refused the bake; the bake
// that follows takes (and clears) the verdict so a refused Calculate Lighting bakes nothing.
void alpine_lm_note_lighting_refused(bool refused);
bool alpine_lm_take_lighting_refused();

// Save/load side.
void alpine_lm_save_begin(CDedLevel& level);
void alpine_lm_serialize_chunk(CDedLevel& level, rf::File& file);
void alpine_lm_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len);
void alpine_lm_drop_retained();
// alpine_lm_drop_retained without the repaint, for a level being reset while the views may be gone.
void alpine_lm_reset_level_state();
// Valid from alpine_lm_save_begin on: whether this save omits the stock lightmaps section.
bool alpine_lm_stock_suppressed();

// Defined in editor_patch/lightmap.cpp: the surfaces still point at the page RED synthesised for a
// level loaded without stock lightmaps, so their rects overlap until the next repack.
bool lightmap_stock_layout_synthesized();
// Gives that synthesised page the edge the level's alpine lightmap section records; nothing otherwise.
void lightmap_synthesized_page_resize(int edge);

// Defined in editor_patch/lightmap.cpp, for the terrain charts of the Calculate Lighting in progress.
struct LightmapPoint
{
    float pos[3];
    float normal[3];
};
// Refreshes the per-room ambient table for the level solid; run once before the terrain texels.
void lightmap_prepare_terrain_bake();
// Lights `count` world points exactly as that bake lights a texel of the level solid's surfaces,
// shadow rays leaving `lift` off each point along its normal; writes the float accumulators (the
// values FUN_004ac470 converts at x255) to out_r/g/b[count]. False outside a bake.
bool lightmap_light_terrain_points(const LightmapPoint* points, int count, float lift, float* out_r,
                                   float* out_g, float* out_b);
// The fixed pipeline's texel conversion (3x3 box, renormalised at the edges) of a width x height
// float chart to RGB8.
void lightmap_encode_float_texels(const float* r, const float* g, const float* b, int width, int height,
                                  std::uint8_t* out);

// Baked terrain lighting for the viewport preview, from the last bake or the level as loaded.
struct TerrainBakedLight;
// Changes whenever that lighting is replaced or dropped; a TerrainBakedLight lives until then.
std::uint32_t terrain_baked_light_generation();
// Terrain `uid`'s chart when it lights `data` placed at `pos` as it is now, else null. Hashes the
// heightmap: call when the terrain changed, not per frame.
const TerrainBakedLight* terrain_baked_light_find(int uid, const Vector3& pos, const DedTerrainData& data);
// The chart's light at world (x, z) as a stock lightmap texel (0..1 per channel, drawn doubled), bilinear.
void terrain_baked_light_sample(const TerrainBakedLight& light, float world_x, float world_z, float (&texel)[3]);

void ApplyAlpineLightmapPatches();
