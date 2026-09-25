#pragma once

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>
#include <common/terrain/alpine_terrain.h>

struct CDedLevel;
struct DedTerrain;
struct DedTerrainData;
struct TerrainGrid;
struct Vector3;
struct BrushNode;

void ApplyTerrainBuildPatches();

// "Terrain <uid> '<script name>'" for messages.
std::string terrain_label(const DedTerrain& t);

// RED numbers a level's lightmap surfaces as int16 (FUN_004aa610).
inline constexpr uint32_t red_max_level_surfaces = 32767;

// The wire header of `d` placed at `pos` with grid `g` (no flag_chunk_geo_mask or overlays; zero sizes
// without a grid).
alpine_terrain::Header terrain_header(const Vector3& pos, const DedTerrainData& d, const TerrainGrid* g);

// The emitter's view of `d` placed at `pos`, reading grid `g`.
alpine_terrain::GridView terrain_grid_view(const Vector3& pos, const DedTerrainData& d, const TerrainGrid& g);

// The chunk layout the terrain's geo mask covers now (alpine_terrain::geo_chunk_layout).
alpine_terrain::ChunkLayout terrain_geo_chunk_layout(const DedTerrainData& d);
// Whether the game marks chunk `index` of that layout geoable (when the terrain is geoable).
bool terrain_chunk_geoable(const DedTerrainData& d, uint32_t index);
// The geo mask over the current layout, every chunk set when none was set.
std::vector<uint8_t> terrain_geo_chunks(const DedTerrainData& d);

// Uids of the temporary chunk brushes of the Build Geometry in progress, each of which the room
// builder isolates into a room of its own.
void terrain_build_isolated_brush_uids(std::unordered_set<int32_t>& uids);

// Save backstop: removes chunk brushes a build left in the brush list.
void terrain_build_strip_leftovers(CDedLevel& level);

// Save: fills the terrain's build_mapping from its compiled rooms, or clears it and returns why when
// the terrain changed since the last Build Geometry or a chunk has no room.
std::string terrain_build_fill_mapping(CDedLevel& level, DedTerrain& terrain);

// Save: why the compiled solid still holds terrain rooms no terrain owns (a terrain deleted since the
// last Build Geometry), or empty.
std::string terrain_build_orphan_note(CDedLevel& level);

// Level load: adopts the mapping read from the level as the terrain's built state.
void terrain_build_note_loaded(CDedLevel& level, DedTerrain& terrain);

// A terrain that did not come from this level's last build (clone, paste, group import) has none.
void terrain_reset_built_state(DedTerrain& terrain);

// The compiled level geometry holds this terrain's geometry as it is now (built, and its shape not
// edited since; paint may differ).
bool terrain_build_is_current(const DedTerrain& terrain);

// Nearest t in [0, t_max] where the ray meets a face of the compiled level the viewport draws
// opaque and front facing (terrain rooms excluded), else t_max.
float terrain_build_level_ray_hit(CDedLevel& level, const float (&o)[3], const float (&d)[3], float t_max);

// Convert to Brushes: the faces the terrain would emit and the stock lightmap surfaces they would
// make, and the level's surfaces now (numbered by the last Calculate Lighting when measured, else an
// upper bound of eligible faces).
struct TerrainBakeEstimate
{
    uint64_t faces = 0;
    uint32_t new_surfaces = 0;
    uint32_t chunks = 0;
    uint32_t level_surfaces = 0;
    bool level_surfaces_measured = false;
};
TerrainBakeEstimate terrain_bake_estimate(CDedLevel& level, DedTerrain& terrain);

// Inserts one permanent detail brush per chunk from the Build Geometry emitter, without an undo
// record, and returns them.
std::vector<BrushNode*> terrain_bake_to_brushes(CDedLevel& level, DedTerrain& terrain);
