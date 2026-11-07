#pragma once

#include <string>

// FactionFiles preview images for levels, fetched in the background and kept as small bitmaps.
// Downloads are cached in the single file <rf_root>\screenshots\thumbnails.cache (size-capped,
// least recently used images dropped first), so a level is only asked for once.
// Main thread only.

// Bitmap handle of the level's preview (out_w/out_h get its size), or -1 while it is still
// loading or when FactionFiles has none. The first call for a level queues the load; a few
// levels load at once, and a failed one is not retried this session. The handle is only valid
// for the current frame: bitmaps not drawn for a while are released and rebuilt from the cache.
int map_thumbnail_get(const std::string& level_filename, int* out_w, int* out_h);

// Releases the bitmaps; a later map_thumbnail_get rebuilds them from the disk cache.
void map_thumbnails_release_bitmaps();
