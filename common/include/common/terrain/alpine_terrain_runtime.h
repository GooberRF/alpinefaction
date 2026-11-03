#pragma once

#include <cstdint>
#include <common/terrain/alpine_terrain_reader.h>

// A loaded record. At load, overlay_coverage is kept only for the D3D11 renderer, decoration_coverage only on a
// client that renders, and weights for either; alpine_terrain_release_decoration_maps frees the decoration-only
// ones once placed.
struct AlpineTerrain : alpine_terrain::Record
{
    // Set by alpine_terrain_resolve_rooms when every chunk matched its compiled room.
    bool resolved = false;
    // alpine_terrain::decoration_lighting_hash, taken at load while the maps it reads are still there.
    uint64_t decoration_lighting_hash = 0;

    bool fullbright() const
    {
        return (header.flags & alpine_terrain::flag_fullbright) != 0;
    }
};

// A resolved terrain chunk's compiled room.
struct AlpineTerrainRoomRef
{
    int terrain;
    int chunk;
};
