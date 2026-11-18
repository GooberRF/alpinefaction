#pragma once

#include <string>

// FactionFiles preview images for levels, fetched in the background and kept as small bitmaps.
// The thumbnails are kept as JPEGs in <rf_root>\alpine_cache\map_thumbs.cache (a cache pack, see
// common/cache_pack.h), so a level is only downloaded once.
// Main thread only.

// Releases the bitmaps; a later draw rebuilds them from the disk cache.
void map_thumbnails_release_bitmaps();

// The level's preview cropped to fill `w` x `h` (draw space), or a dark fill while there is none. The first draw of a
// level queues its load; a few levels load at once, and a failed one is not retried this session.
void map_thumbnail_draw(const std::string& level_filename, int x, int y, int w, int h);
// True once the level is known to have no preview.
bool map_thumbnail_missing(const std::string& level_filename);

// FactionFiles' listing of a level, asked once a session in the background.
struct MapLevelInfo
{
    std::string author;
};
// Null while it is being asked, or when FactionFiles does not list the level.
const MapLevelInfo* map_level_info_get(const std::string& level_filename);
