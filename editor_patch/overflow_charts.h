#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <common/lightmap/alpine_lightmap.h>

struct GSolid;
struct GFace;

// Alpine overflow charts: charts for the static faces RED's surface pass leaves without a surface once its 32767
// are used up, on D3D11-only levels. alpine_lightmaps.cpp lays them out after the surface, mover and terrain
// charts, shades them after the stock bake and writes them as the section's overflow table.

struct OverflowLayout
{
    std::vector<alpine_lightmap::Tile> tiles;          // reader order, from the first overflow tile
    std::uint32_t num_pages = 0;                       // pages from the first overflow page
};

// Lays the charts out, every chart starting at `density`: pages from `first_page`, at most `max_pages` of them.
// False when there is nothing to chart or it failed (reported).
bool overflow_bake_begin(const GSolid* solid, const std::vector<std::int32_t>& terrain_room_uids, float density,
                         std::uint32_t first_page, std::uint32_t max_pages);
const OverflowLayout& overflow_layout();
// Shades every chart into `pages` (page_size^2 RGB8 each), blends the seams between charts and makes a tiled
// chart's shared gutter texels agree, marking their blocks in `shared_blocks`. False when it failed (reported).
bool overflow_shade(std::vector<std::vector<std::uint8_t>>& pages, std::vector<std::uint8_t>& shared_blocks);
// After a failed shade: the table keeps its layout but carries a fingerprint no faces match.
void overflow_mark_unusable();
// Whether the bake has a table to write, and its tile count.
bool overflow_has_table();
std::uint32_t overflow_table_tiles();
std::uint64_t overflow_table_bytes();
// Appends the table body (not its TableHeader).
void overflow_append_table(std::vector<std::uint8_t>& body);
void overflow_bake_reset();

// The face_fingerprint the overflow table of section `body` must carry when `solid` is written as it is now.
enum class OverflowStamp
{
    none,  // no overflow table
    match, // the stored fingerprint still holds
    stale, // write `value` at `offset`: a fingerprint these faces do not have
};
OverflowStamp overflow_save_stamp(const std::uint8_t* body, std::size_t len, const GSolid* solid,
                                  std::size_t& offset, std::uint64_t& value);

// The viewport's view of the overflow lighting: a face's chart mean, as a stock lightmap texel (drawn doubled).
// Rebuilt after a bake and when a level's section is retained; dropped with the level.
void overflow_preview_from_bake();
void overflow_preview_from_section(const std::uint8_t* body, std::size_t len, const GSolid* solid);
void overflow_preview_clear();
// The preview faces and their colours, for the viewport (overflow_preview.cpp).
const std::unordered_map<const GFace*, std::array<std::uint8_t, 3>>& overflow_preview_faces();
// Defined in overflow_preview.cpp: the preview faces changed; after a bake the level's room caches are rebuilt.
void overflow_preview_changed(bool after_bake);
void overflow_preview_install();
