#pragma once

#include "../rf/bmpman.h"

// Called from bm_read_header_hook. Reads the file from packfiles/disk via rf::File, peeks at
// the header to fill width/height/format/num_levels. Returns TYPE_STB on success or TYPE_NONE
// if the file can't be opened, decoded, or the format is unsupported.
rf::bm::Type read_stb_header(const char* filename, int* width_out, int* height_out,
    rf::bm::Format* format_out, int* num_levels_out);

// Called from bm_lock_hook for TYPE_STB bitmaps. Decodes the file into an rf-allocated buffer,
// stores it in bm_entry.locked_data (matching the DDS pattern, so stock unlock frees it).
int lock_stb_bitmap(rf::bm::BitmapEntry& bm_entry);
