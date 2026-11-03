#pragma once

// Include after the binary's rf engine types (rf::Vector3).
// Terrain decoration runtime records the shared Direct3D 11 renderer draws.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <vector>
#include <common/terrain/alpine_terrain.h>

namespace rf
{
    struct VMesh;
}

// Row k = {rvec[k], uvec[k], fvec[k]} * scale, pos[k]. light (RGBA8): mesh ambient + sun scale with D3D11; terrain
// light drawn as custom ambient with the legacy renderers.
struct GpuDecorationInstance
{
    std::array<float, 4> row0, row1, row2;
    uint32_t light;
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
    uint32_t first[alpine_terrain::max_decorations];
    uint32_t count[alpine_terrain::max_decorations];

    // Squared distance from `p` to the box, 0 inside
    float dist_sq(const rf::Vector3& p) const
    {
        const float d[3] = {std::max({lo[0] - p.x, 0.0f, p.x - hi[0]}), std::max({lo[1] - p.y, 0.0f, p.y - hi[1]}),
                            std::max({lo[2] - p.z, 0.0f, p.z - hi[2]})};
        return d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    }

    rf::Vector3 lo_vec() const
    {
        return {lo[0], lo[1], lo[2]};
    }

    rf::Vector3 hi_vec() const
    {
        return {hi[0], hi[1], hi[2]};
    }
};

struct TerrainDecorations
{
    std::vector<GpuDecorationInstance> inst; // chunk-major, then decoration
    std::vector<DecorationChunk> chunks;     // by chunk index, empty for an undecorated terrain
    int mesh_slot[alpine_terrain::max_decorations];
    float draw_distance[alpine_terrain::max_decorations] = {};
    float vertical_offset[alpine_terrain::max_decorations] = {};
    bool dither_fade[alpine_terrain::max_decorations] = {};
    alpine_terrain::DecorationEdges edges[alpine_terrain::max_decorations] = {};
    // Chunks whose instance ranges changed since the D3D11 renderer last uploaded them
    std::vector<uint32_t> dirty_chunks;

    TerrainDecorations()
    {
        std::fill(std::begin(mesh_slot), std::end(mesh_slot), -1);
    }
};

struct DecorationFrameStats
{
    int frame = -1;
    uint32_t visible_chunks = 0;
    uint32_t draws = 0;
    uint32_t instances = 0;
    double cpu_ms = 0.0;
};

struct DecorationMesh
{
    rf::VMesh* mesh;
    // About the mesh origin, clamped to the cache's max_radius
    float radius;
};
