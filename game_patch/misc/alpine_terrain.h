#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <common/terrain/alpine_terrain_reader.h>
#include "../rf/file/file.h"

namespace rf
{
    struct GRoom;
    struct GFace;
}

// A loaded record. At load, overlay_coverage is kept only for the D3D11 renderer, decoration_coverage only on a
// client that renders, and weights for either; alpine_terrain_release_decoration_maps frees the decoration-only
// ones once placed.
struct AlpineTerrain : alpine_terrain::Record
{
    // Set by alpine_terrain_resolve_rooms when every chunk matched its compiled room.
    bool resolved = false;
    // alpine_terrain::decoration_lighting_hash, taken at load while the maps it reads are still there.
    std::uint64_t decoration_lighting_hash = 0;

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

void alpine_terrain_load_chunk(rf::File& file, std::size_t chunk_len);
void alpine_terrain_clear_state();
// Frees every decoration_coverage, and the weights unless the D3D11 renderer draws the terrain.
void alpine_terrain_release_decoration_maps();
// Matches every terrain's build mapping against the loaded static geometry. Runs once the level
// has loaded and before anything builds a render cache.
void alpine_terrain_resolve_rooms();
const std::vector<AlpineTerrain>& alpine_terrain_get_all();
// Null unless `room` is a chunk room of a resolved terrain.
const AlpineTerrainRoomRef* alpine_terrain_find_room(const rf::GRoom* room);
inline bool alpine_terrain_is_chunk_room(const rf::GRoom* room)
{
    return alpine_terrain_find_room(room) != nullptr;
}
// A detail room of `parent` that every renderer draws from its own cache, once per pass, and never
// as part of the parent's. Never under the sky room, whose renderers draw its detail rooms with it.
bool alpine_terrain_is_separate_chunk(const rf::GRoom* parent, const rf::GRoom* detail_room);
// weights is null when freed; only emission (dominant_layer) and material_fingerprint read it.
alpine_terrain::GridView alpine_terrain_grid(const AlpineTerrain& t);
// Views of t's decorations over its coverage planes (none once freed); returns how many.
std::uint32_t alpine_terrain_decoration_views(const AlpineTerrain& t,
                                              alpine_terrain::DecorationView (&out)[alpine_terrain::max_decorations]);
// alpine_terrain::face_kind of a chunk face with no surface.
alpine_terrain::FaceKind alpine_terrain_face_kind(const alpine_terrain::GridView& g, const rf::GFace& face);
// The light at `pos` on a face of `kind` of resolved terrain `terrain` (alpine_terrain_get_all index) as a
// stock lightmap texel (0..1, drawn doubled), so an entity standing there is lit like one on ordinary
// geometry that renders as bright. Off the top, `face_normal` shades instead of the heightmap normal. A
// fullbright terrain lights it as unbaked ground: level ambient and sun, craters undimmed.
void alpine_terrain_sample_light(int terrain, alpine_terrain::FaceKind kind, const float (&pos)[3],
                                 const float (&face_normal)[3], float (&texel)[3]);
