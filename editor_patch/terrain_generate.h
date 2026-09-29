#pragma once

#include <cstdint>
#include <vector>

// Procedural heightmaps (and optionally a splat map) for a terrain grid. Pure computation: no editor state.
// The same settings and input always give the same bytes.

enum class TerrainNoiseType
{
    fbm,    // smooth hills
    ridged, // ridged multifractal: sharp mountain ridges
    billow, // rounded, puffy hills
    hybrid, // ridged peaks over fBm lowlands
};

enum class TerrainEdgeFalloff
{
    none,
    island, // edges sink to the lowest height
    basin,  // edges rise to the highest height
};

struct TerrainGenSettings
{
    uint32_t seed = 1;
    TerrainNoiseType type = TerrainNoiseType::hybrid;
    float feature_size = 128.0f; // world units of the largest octave
    int octaves = 6;
    float roughness = 0.45f; // amplitude kept per octave
    float lacunarity = 2.0f; // frequency gained per octave
    float warp = 0.25f;      // domain warp, in feature sizes

    float exponent = 1.0f; // above 1 flattens valleys and sharpens peaks
    int terraces = 0;
    TerrainEdgeFalloff falloff = TerrainEdgeFalloff::none;
    float falloff_width = 0.3f; // share of the half extent that blends to the edge
    float offset = 0.0f;        // added before shaping; clamping it makes flat floors or plateaus

    int thermal_iterations = 0;
    float talus_deg = 35.0f;
    int droplets = 20000;
    float erosion_strength = 0.5f;
    int smooth_passes = 1;

    bool splat = false;
    float slope_deg = 35.0f; // steeper is rock
    float slope_blend_deg = 8.0f;
    float high_start = 0.5f; // share of the height range above which is the high layer
    float high_blend = 0.1f;
    float variation = 0.3f;       // share of flatter ground the variation layer covers
    float variation_size = 24.0f; // world units of its patches
    float ridge_emphasis = 0.5f;
};

// Splat channels in the order they fill a block of four layers.
enum TerrainGenLayer
{
    terrain_gen_base = 0,
    terrain_gen_slope = 1,
    terrain_gen_high = 2,
    terrain_gen_variation = 3,
    terrain_gen_layer_count = 4,
};

struct TerrainGenInput
{
    uint32_t nx = 2;
    uint32_t nz = 2;
    uint32_t weight_res_mul = 1;
    float cell_size = 1.0f;
    float height_range = 1.0f;
};

struct TerrainGenResult
{
    uint32_t nx = 0;
    uint32_t nz = 0;
    uint32_t weight_res_mul = 1;
    float cell_size = 1.0f;
    float height_range = 1.0f;
    // Grid order (heights[z * nx + x]), spanning 0 to 65535 unless the result is flat.
    std::vector<uint16_t> heights;
    // With settings.splat: RGBA per weight texel in image order (row 0 at the +Z edge), each texel summing
    // to 255, channel c for TerrainGenLayer c.
    std::vector<uint8_t> splat;
    float slope_mean_deg = 0.0f;
    float slope_max_deg = 0.0f;
    float steep_share = 0.0f; // share of vertices steeper than settings.slope_deg
    float coverage[terrain_gen_layer_count] = {};
};

// Settings forced into their supported ranges; NaN takes the default.
TerrainGenSettings terrain_gen_clamp(const TerrainGenSettings& s);

// Throws std::bad_alloc when memory runs out.
void terrain_generate(const TerrainGenSettings& settings, const TerrainGenInput& input, TerrainGenResult& out);

enum class TerrainGenPreview
{
    height,
    shaded,
    splat,
};

// A size x size top-down image of the result (+Z up), letterboxed when the grid isn't square, as 0x00RRGGBB
// pixels top row first.
void terrain_generate_preview(const TerrainGenResult& result, TerrainGenPreview mode, int size,
                              std::vector<uint32_t>& pixels);
