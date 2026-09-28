#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <common/terrain/alpine_terrain.h>
#include "alpine_terrain.h"
#include "decoration_mesh_cache.h"

namespace rf
{
    struct GRoom;
    struct Vector3;
}

// Row k = {rvec[k], uvec[k], fvec[k]} * scale, pos[k]. light (RGBA8): mesh ambient + sun scale with D3D11; terrain
// light drawn as custom ambient with the legacy renderers.
struct GpuDecorationInstance
{
    std::array<float, 4> row0, row1, row2;
    std::uint32_t light;
};
static_assert(sizeof(GpuDecorationInstance) == 52);
static_assert(offsetof(GpuDecorationInstance, row1) == 16);
static_assert(offsetof(GpuDecorationInstance, row2) == 32);
static_assert(offsetof(GpuDecorationInstance, light) == 48);

// A terrain chunk's instances: decoration d holds [first[d], first[d] + count[d]) of TerrainDecorations::inst.
// lo/hi bound every instance's mesh and base, which craters test.
struct DecorationChunk
{
    float lo[3], hi[3];
    std::uint32_t first[alpine_terrain::max_decorations];
    std::uint32_t count[alpine_terrain::max_decorations];

    // Squared distance from `p` to the box, 0 inside
    float dist_sq(const rf::Vector3& p) const;
    rf::Vector3 lo_vec() const;
    rf::Vector3 hi_vec() const;
};

struct TerrainDecorations
{
    std::vector<GpuDecorationInstance> inst; // chunk-major, then decoration
    std::vector<DecorationChunk> chunks;     // by chunk index, empty for an undecorated terrain
    int mesh_slot[alpine_terrain::max_decorations] = {-1, -1, -1, -1, -1, -1, -1, -1};
    float draw_distance[alpine_terrain::max_decorations] = {};
    float vertical_offset[alpine_terrain::max_decorations] = {};
    // Chunks whose instance ranges changed since the D3D11 renderer last uploaded them
    std::vector<std::uint32_t> dirty_chunks;
};
static_assert(alpine_terrain::max_decorations == 8, "TerrainDecorations::mesh_slot initializes each slot to -1");

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
// Removes the instances within radius of pos on the terrain chunks among rooms. Recorded, so a crater replayed
// before level init still applies.
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
const DecorationChunk* alpine_terrain_decorations_chunk(const AlpineTerrainRoomRef& ref);
const DecorationMesh& alpine_terrain_decorations_mesh(int slot);
// This frame's counters, reset on the first call of a frame.
DecorationFrameStats& alpine_terrain_decorations_frame_stats();
