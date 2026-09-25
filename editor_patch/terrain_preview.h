#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>

struct CDedLevel;
struct DedTerrain;
struct DedTerrainData;
struct GRoom;
struct TerrainGrid;

// Cells [x0, x1) x [z0, z1) of a terrain's grid.
struct TerrainCellRect
{
    std::uint32_t x0, z0, x1, z1;
};

// Selection red and the unselected green of the Alpine object family
inline constexpr std::uint8_t terrain_selected_rgb[3] = {0xff, 0x00, 0x00};
inline constexpr std::uint8_t terrain_unselected_rgb[3] = {0x50, 0xc0, 0x50};

void ApplyTerrainPreviewPatches();

// Draws one terrain into the viewport being painted, from `data` (the terrain's own, or the
// properties dialog's staged copy). Called from the Alpine object pass (0x0041f9b2).
void terrain_preview_draw(CDedLevel& level, const DedTerrain& terrain, const DedTerrainData& data, bool selected);

// After the pass: frees the previews of terrains no longer in the level and asks for another paint
// while composites are still outstanding.
void terrain_preview_frame_end(CDedLevel& level);

// Whether a paint since the last call left composites for another paint; clears the flag.
bool terrain_preview_take_pending_work();

// Recomposites the chunks a paint stroke or edit touched (every chunk when `cells` is null) the next
// time the terrain is drawn; the vertex shading is redone only when heights changed. Edits that
// replace the grid, move the terrain or change its layers are also picked up without a call.
void terrain_preview_invalidate(const DedTerrain* terrain, const TerrainCellRect* cells,
                                bool heights_changed = true);

// Sculpting changed the heights of vertices [x0, x1) x [z0, z1) (every vertex when null): the
// shading and chunk bounds around them are redone at the next draw, and the composites are kept.
void terrain_preview_heights_changed(const DedTerrain* terrain, const TerrainCellRect* verts);

// The terrain's geometry changed in place (heights, holes): its baked light is not drawn until it is
// checked against the terrain again, once no stroke is running.
void terrain_preview_lighting_changed(const DedTerrain* terrain);

// Reload Textures ran: layer textures are read again and every composite is redone.
void terrain_preview_textures_reloaded();

// A paint stroke or paint undo swapped the terrain's grid for a copy that differs only where
// terrain_preview_invalidate says: the preview keeps its composites instead of redoing them all.
void terrain_preview_rebind_grid(const DedTerrain* terrain, const TerrainGrid* old_grid,
                                 const std::shared_ptr<const TerrainGrid>& new_grid);

// The average colour of a layer texture as the preview blends it; false when it cannot be read.
bool terrain_preview_layer_color(const std::string& texture, std::uint8_t (&rgb)[3]);

// Releases a terrain's preview bitmaps; called before the object is freed.
void terrain_preview_forget(const DedTerrain* terrain);

// Compiled room of terrain chunk faces, which the viewport leaves to the preview.
bool terrain_preview_hides_room(const GRoom* room);

// The nearest terrain surface under the screen point of the viewport last set up, holes skipped, unless
// level geometry the view draws is in front of it.
DedTerrain* terrain_surface_pick(CDedLevel& level, float screen_x, float screen_y);

// The stock object pick's reach
inline constexpr float terrain_pick_reach = 50000.0f;

struct TerrainRay
{
    float o[3];
    float d[3]; // unit
};
// The world ray through a screen point of the viewport last set up (FUN_004c5fb0).
TerrainRay terrain_screen_ray(float screen_x, float screen_y);
// Distance along `ray` to the terrain's surface within [0, t_max], holes skipped.
bool terrain_ray_hit(const DedTerrain& t, const TerrainRay& ray, float t_max, float& hit_t);

// A lightmapped texel as RED draws a brush (texture x lightmap, MODULATE2X) and the game's terrain shader
// draws the chart (albedo x 2 x chart texel, saturated), dynamic lights aside: albedo 0..255, texel a
// stock lightmap texel 0..1, result 0..255.
inline float terrain_preview_lit(float albedo, float texel)
{
    return std::min(albedo * 2.0f * texel, 255.0f);
}

inline std::uint8_t terrain_preview_byte(float v)
{
    return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f) + 0.5f);
}

// The level-wide composite budget has room for one more res x res composite.
inline bool composite_budget_fits(int bitmaps, std::uint64_t texels, std::uint32_t res, int max_bitmaps,
                                  std::uint64_t max_texels)
{
    return bitmaps < max_bitmaps && texels + static_cast<std::uint64_t>(res) * res <= max_texels;
}

// A composite last drawn in paint `last_drawn` may be freed for another in paint `frame`.
inline bool composite_evictable(std::uint32_t last_drawn, std::uint32_t frame, std::uint32_t keep_frames)
{
    return frame - last_drawn >= keep_frames;
}
