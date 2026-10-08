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

// Called from bm_free_entry_hook for TYPE_ATX bitmaps. Removes this handle from the
// controller's tracking set so subsequent dirty marks don't reach a freed handle. Does NOT
// release the controller or its child bm handles — multiple bm_entries can share one
// controller (via basename), and child release is bounded to atx_level_reset (level
// transition), matching the level-based lifetime of every other ATX-cached asset.
void atx_free(rf::bm::BitmapEntry& bm_entry);

// Per-frame tick: advance auto-playing controllers, dirty texture cache when frame index changes.
void atx_do_frame();

// Drop all controllers and release child handles. Called at level load to reset state.
void atx_level_reset();

// Canonical registry key for a raw handle: path dropped, lowercased, and one recognized texture
// extension removed.
std::string atx_canonical_handle(const std::string& handle);
// Takes a canonical key. True if that texture has been referenced through the bm system and has a
// controller. Silent — callers that want a diagnostic emit their own.
bool atx_has_controller(const std::string& canonical_key);
bool atx_set_frame(const std::string& handle, int frame_index);
bool atx_play(const std::string& handle);
bool atx_pause(const std::string& handle);
bool atx_set_frame_time(const std::string& handle, int frame_time_ms);
bool atx_set_live_feed(const std::string& canonical_key, int bm_handle);
bool atx_clear_live_feed(const std::string& canonical_key, int expected_bm);

namespace atx_detail
{
    // Controllers with a live feed — non-zero for as long as any projection is on. The common
    // case is none at all, which is what lets the per-draw resolve and the renderer's texture
    // cache invalidation both early-out.
    extern int g_live_feed_count;
    int lookup_live_feed(int bm_handle);
}

// True while any ATX is showing a live feed.
inline bool atx_any_live_feed()
{
    return atx_detail::g_live_feed_count > 0;
}

// Called per draw from the D3D11 texture manager. Returns -1 when the handle has no feed.
inline int atx_lookup_live_feed(int bm_handle)
{
    if (atx_detail::g_live_feed_count == 0) {
        return -1;
    }
    return atx_detail::lookup_live_feed(bm_handle);
}
