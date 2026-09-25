#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <common/terrain/alpine_terrain.h>
#include "../rf/file/file.h"

namespace rf
{
    struct GRoom;
    struct GFace;
}

struct AlpineTerrainLayer
{
    std::string texture;
    float uv_scale = alpine_terrain::default_uv_scale;
    bool triplanar = false;
};

struct AlpineTerrainOverlay
{
    std::string texture;
    float uv_scale = alpine_terrain::default_uv_scale;
    bool triplanar = false;
    bool break_tiling = false;
};

// One validated record of the 0x0AFBAE0B chunk; array sizes match `header` exactly.
struct AlpineTerrain
{
    int uid = -1;
    std::string script_name;
    alpine_terrain::Header header{};
    std::string underside_texture;
    std::string crater_texture; // empty = level geomod texture
    std::vector<AlpineTerrainLayer> layers;
    std::vector<AlpineTerrainOverlay> overlays;
    std::vector<alpine_terrain::ChunkMapping> build_mapping; // empty = not built
    std::vector<std::uint16_t> heights;
    // weights and overlay_coverage are freed at load unless the D3D11 renderer draws the terrain
    std::vector<std::uint8_t> weights;
    std::vector<std::uint8_t> holes;
    std::vector<std::uint8_t> diag;
    std::vector<std::uint8_t> geo_chunks; // alpine_terrain::Record::geo_chunks
    std::vector<std::uint8_t> overlay_coverage; // alpine_terrain::Record::overlay_coverage
    // Set by alpine_terrain_resolve_rooms when every chunk matched its compiled room.
    bool resolved = false;
};

// A resolved terrain chunk's compiled room.
struct AlpineTerrainRoomRef
{
    int terrain;
    int chunk;
};

void alpine_terrain_load_chunk(rf::File& file, std::size_t chunk_len);
void alpine_terrain_clear_state();
// Matches every terrain's build mapping against the loaded static geometry. Runs once the level
// has loaded and before anything builds a render cache.
void alpine_terrain_resolve_rooms();
const std::vector<AlpineTerrain>& alpine_terrain_get_all();
// Null unless `room` is a chunk room of a resolved terrain.
const AlpineTerrainRoomRef* alpine_terrain_find_room(const rf::GRoom* room);
// A detail room of `parent` that every renderer draws from its own cache, once per pass, and never
// as part of the parent's. Never under the sky room, whose renderers draw its detail rooms with it.
bool alpine_terrain_is_separate_chunk(const rf::GRoom* parent, const rf::GRoom* detail_room);
// weights is null when freed; only emission (dominant_layer) and material_fingerprint read it.
alpine_terrain::GridView alpine_terrain_grid(const AlpineTerrain& t);
// alpine_terrain::face_kind of a chunk face with no surface.
alpine_terrain::FaceKind alpine_terrain_face_kind(const alpine_terrain::GridView& g, const rf::GFace& face);
// The light at `pos` on a face of `kind` of a resolved terrain as a stock lightmap texel (0..1, drawn
// doubled), so an entity standing there is lit like one on ordinary geometry that renders as bright.
// Off the top, `face_normal` shades instead of the heightmap normal.
void alpine_terrain_sample_light(const AlpineTerrain& t, alpine_terrain::FaceKind kind, const float (&pos)[3],
                                 const float (&face_normal)[3], float (&texel)[3]);
