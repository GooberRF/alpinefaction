#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include "../rf/bmpman.h"

// ATX is a text-based (TOML) container that bundles a list of existing texture files
// (.tga/.dds/.vbm/etc.) into a single virtual bitmap addressable as "<name>.atx".
// Frames share width/height/format with frame[0]; the loader validates this.
// The handle used by events is the .atx filename without its extension.

// Called from bm_read_header_hook when an .atx file should be loaded for the requested name.
// The `atx_filename` argument is the resolved .atx filename (e.g. "mytexture.atx"), regardless
// of which extension the caller originally requested.
rf::bm::Type read_atx_header(const char* atx_filename, int* width_out, int* height_out,
    rf::bm::Format* format_out, int* num_levels_out, int* num_frames_out);

// True while parse_and_load is loading a child texture. The bm_read_header hook checks this to
// suppress .atx supercede during ATX child loads, which prevents both nested-ATX and
// recursive-cycle scenarios in one stroke.
bool atx_is_loading_child();

// Called from bm_lock_hook for TYPE_ATX bitmaps. Forwards to the currently-selected
// child texture's locked pixels. Returns the format from the bitmap entry.
rf::bm::Format lock_atx_bitmap(rf::bm::BitmapEntry& bm_entry, void** pixels_out, void** palette_out);

// Called from bm_unlock_hook for TYPE_ATX bitmaps (no-op; child stays locked for fast frame swaps).
void unlock_atx_bitmap(rf::bm::BitmapEntry& bm_entry);

// Called from the bm_get_cached_material_idx hook. Returns the effective material override for
// the ATX's current frame, or nullopt if the ATX has no overrides configured (caller falls
// through to the stock material logic in that case).
std::optional<uint8_t> atx_material_override(const rf::bm::BitmapEntry& bm_entry);

// Called from bm_free_entry_hook for TYPE_ATX bitmaps. Releases child handles.
void atx_free(rf::bm::BitmapEntry& bm_entry);

// Per-frame tick: advance auto-playing controllers, dirty texture cache when frame index changes.
void atx_do_frame();

// Drop all controllers and release child handles. Called at level load to reset state.
void atx_level_reset();

// Event control entry points. `handle` is the .atx filename without extension (case-insensitive).
// If the controller isn't loaded yet, attempts to load it lazily.
bool atx_set_frame(const std::string& handle, int frame_index);
bool atx_play(const std::string& handle);
bool atx_pause(const std::string& handle);
bool atx_set_frame_time(const std::string& handle, int frame_time_ms);
