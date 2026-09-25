#pragma once

#include <cstddef>

namespace rf
{
    class File;
}

// Alpine Lightmaps (RFL section 0x0AFBAE09), game side; implemented in gr_d3d11_af_lightmap.cpp. Terrain
// charts also feed the CPU light sampler on every renderer.

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

void af_lightmap_load_chunk(rf::File& file, std::size_t chunk_len);

// Run once the level has loaded (terrains and the section both read): logs how many terrains have a
// chart (matched by uid, grid size and alpine_terrain::lighting_fingerprint when the section loaded).
void af_lightmap_resolve_terrains();

// The baked light at world (x, z) on terrain `terrain_index` (alpine_terrain_get_all order) as a
// stock lightmap texel, 0..1 per channel and drawn doubled; false when it has no matched chart.
bool af_lightmap_terrain_sample(int terrain_index, float world_x, float world_z, float (&texel)[3]);
