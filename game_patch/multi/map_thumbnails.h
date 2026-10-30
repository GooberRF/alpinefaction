#pragma once

#include <string>

// FactionFiles preview images for levels, fetched in the background and kept as small bitmaps.
// Downloads are cached in the single file <rf_root>\screenshots\thumbnails.cache (up to 512 MB,
// least recently used images dropped first), so a level is only asked for once.
// Main thread only.

// Bitmap handle of the level's preview (out_w/out_h get its size), or -1 while it is still
// loading or when FactionFiles has none. The first call for a level queues the load; a few
// levels load at once, in request order, and each is attempted once per session.
int map_thumbnail_get(const std::string& level_filename, int* out_w, int* out_h);

// Releases the bitmaps. The downloaded images stay cached, so a later map_thumbnail_get
// rebuilds the bitmap without reading the disk or asking FactionFiles again.
void map_thumbnails_release_bitmaps();
