#pragma once

#include <functional>
#include <string_view>
#include <xlog/xlog.h>
#include "../rf/bmpman.h"
#include <common/bitmap/bm_format.h>

namespace rf::gr {
    struct Color;
    struct LockInfo;
}

// rf::bm::load never fails: a missing file gets a generated 32x32 checkerboard
// placeholder with a real handle, so "handle != -1" is not a presence test. Probe
// the VFS first when the caller wants a genuine "not installed" answer (-1).
// Defined in misc/game.cpp.
int bm_load_if_exists(const char* name, int unk, bool generate_mipmaps);

// Drops a trailing extension only when bm can actually resolve a texture from it, so a name
// like "mon.01" keeps its last segment. The game's supersede probes, ATX registry key and ATX
// lookups all go through this — they have to agree or a texture registers under one key and is
// looked up under another. Path and case are left alone. The editor keeps its own narrower
// supersede list, because RED's loader chain is not this one.
std::string_view bm_strip_texture_ext(std::string_view filename);

void bm_set_dynamic(int bm_handle, bool dynamic);
bool bm_is_dynamic(int bm_handle);
// The GPU texture may be promoted to 8888; write text into such bitmaps with TTF fonts only
// (the stock font blit needs equal lock formats).
void bm_set_user_mipmap(int bm_handle, bool mipmap);
bool bm_is_user_mipmap(int bm_handle);
void bm_change_format(int bm_handle, rf::bm::Format format);
void bm_apply_patch();
size_t bm_calculate_total_bytes(int w, int h, rf::bm::Format format);
// Reads dst, so it must hold initialised pixels (lock with LOCK_READ_ONLY_WRITE).
bool bm_blend_pixels(const rf::gr::LockInfo& dst_lock, int dst_x, int dst_y, const rf::gr::LockInfo& src_lock,
                     int src_x, int src_y, int w, int h);
bool bm_fill(int bm_handle, uint32_t argb);
// An 8888 bitmap with every pixel from argb_at, referenced for the session (texture_add_ref).
// -1 on failure.
int bm_create_procedural(int w, int h, const std::function<uint32_t(int x, int y)>& argb_at);
