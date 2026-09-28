#pragma once

#include <array>
#include <cstdint>
#include <vector>
#include <common/terrain/alpine_terrain.h>
#include "alpine_terrain.h"

namespace rf
{
    struct GRoom;
    struct VMesh;
    struct Vector3;
}

// One placed mesh as the D3D11 decoration shader reads it: row k = {rvec[k], uvec[k], fvec[k]} * scale and
// pos[k] (the mesh origin). light is RGBA8: with D3D11 the mesh ambient in rgb and the sun's mesh scale in a;
// with the legacy renderers the terrain light under the base, drawn as the mesh's custom ambient.
struct GpuDecorationInstance
{
    std::array<float, 4> row0, row1, row2;
    std::uint32_t light;
};
static_assert(sizeof(GpuDecorationInstance) == 52);

// A terrain chunk's instances: decoration d holds [first[d], first[d] + count[d]) of TerrainDecorations::inst.
// lo/hi bound every instance's mesh and base, which craters test.
struct DecoChunk
{
    float lo[3], hi[3];
    std::uint32_t first[alpine_terrain::max_decorations];
    std::uint32_t count[alpine_terrain::max_decorations];
};

struct TerrainDecorations
{
    std::vector<GpuDecorationInstance> inst; // chunk-major, then decoration
    std::vector<DecoChunk> chunks;           // by chunk index, empty for an undecorated terrain
    int mesh_slot[alpine_terrain::max_decorations] = {-1, -1, -1, -1, -1, -1, -1, -1};
    float draw_distance[alpine_terrain::max_decorations] = {};
    float vertical_offset[alpine_terrain::max_decorations] = {};
    // Chunks whose instance ranges changed since the D3D11 renderer last uploaded them
    std::vector<std::uint32_t> dirty_chunks;
};
static_assert(alpine_terrain::max_decorations == 8, "TerrainDecorations::mesh_slot initializes each slot to -1");

struct DecorationMesh
{
    rf::VMesh* mesh;
    // About the mesh origin
    float radius;
};

struct DecorationFrameStats
{
    int frame = -1;
    std::uint32_t visible_chunks = 0;
    std::uint32_t draws = 0;
    std::uint32_t instances = 0;
    double cpu_ms = 0.0;
};

// Places every resolved terrain's decorations and loads their meshes. Level init only, on a client that
// renders: mesh loads at render time corrupt the bitmap manager.
void alpine_terrain_decorations_level_init();
void alpine_terrain_decorations_clear_state();
// Hides the instances standing within `radius` of a crater at `pos` on the terrain chunks among `rooms` (the
// geoable detail rooms it carves); kept for a level still loading.
void alpine_terrain_decorations_notify_crater(const rf::Vector3& pos, float radius,
                                              const std::vector<rf::GRoom*>& rooms);
// Direct3D 8/9: the nearest instances through vmesh_render.
void alpine_terrain_decorations_render_legacy();
void alpine_terrain_decorations_apply_patch();

// Drawing is on and something was placed.
bool alpine_terrain_decorations_active();
// Indexed like alpine_terrain_get_all.
std::vector<TerrainDecorations>& alpine_terrain_decorations_get_all();
// Null for a chunk without instances.
const DecoChunk* alpine_terrain_decoration_chunk(const AlpineTerrainRoomRef& ref);
const DecorationMesh& alpine_terrain_decoration_mesh(int slot);
// This frame's counters, reset on the first call of a frame.
DecorationFrameStats& alpine_terrain_decorations_frame_stats();
