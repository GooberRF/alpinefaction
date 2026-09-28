#include <algorithm>
#include <cmath>
#include <new>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include "../misc/alpine_options.h"
#include "../misc/alpine_settings.h"
#include "../multi/multi.h"
#include "../main/main.h"
#include "../misc/misc.h"
#include "../rf/geometry.h"
#include "../rf/level.h"
#include "../rf/bmpman.h"
#include "../rf/event.h"
#include "../rf/mover.h"
#include "../rf/file/file.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/os/frametime.h"
#include "../rf/player/camera.h"
#include "../rf/gameseq.h"
#include "../os/console.h"
#include "../os/os.h"
#include "../bmpman/bmpman.h"
#include "../bmpman/fmt_conv_templates.h"
#include "../rf/gr/gr_light.h"
#include "destruction.h"
#include "level.h"
#include "alpine_terrain.h"

constexpr auto reference_fps = 30.0f;
constexpr auto reference_frametime = 1.0f / reference_fps;
static int g_max_decals = 512;
static float g_crater_autotexture_ppm = 32.0f;
static bool g_show_room_clip_wnd = false;

std::optional<int> g_sky_room_uid_override;
std::optional<rf::Object*> g_sky_room_eye_anchor;
std::optional<float> g_sky_room_eye_offset_scale;
static rf::Vector3 g_adjusted_sky_room_eye_position;


// Legacy renderers draw terrain faces single-textured without a lightmap, modulated by the level ambient;
// a fullbright terrain's are left at full brightness, as a stock fullbright face is.
static void geo_cache_tint_terrain_batches(rf::GCache& cache)
{
    float ambient[3];
    rf::gr::light_get_ambient(&ambient[0], &ambient[1], &ambient[2]);
    unsigned color = 0xFF;
    for (float c : ambient) {
        color = (color << 8) | static_cast<unsigned>(std::clamp(c * 255.0f + 0.5f, 0.0f, 255.0f));
    }

    rf::GCacheBatch* const batches = cache.batches;
    const int num_batches = static_cast<unsigned short>(cache.num_batches);
    for (int i = 0; i < num_batches; ++i) {
        rf::GCacheBatch& b = batches[i];
        if (b.bm0 < 0 || b.bm1 != -1 || b.num_faces <= 0 ||
            b.mode.get_color_source() != rf::gr::COLOR_SOURCE_TEXTURE || b.mode == rf::gr_decal_self_illuminated_mode) {
            continue;
        }
        // Batches group by texture alone here, so one may mix in unlit faces of other rooms.
        bool all_terrain = true;
        bool all_fullbright = true;
        for (int f = 0; f < b.num_faces && all_terrain; ++f) {
            const rf::GFace* face = b.faces[f].face;
            const AlpineTerrainRoomRef* ref =
                face && face->attributes.surface_index < 0 ? alpine_terrain_find_room(face->which_room) : nullptr;
            all_terrain = ref != nullptr;
            all_fullbright = all_fullbright && ref && alpine_terrain_get_all()[ref->terrain].fullbright();
        }
        if (!all_terrain) {
            continue;
        }
        // Single-textured either way: the batch's mode still expects the lightmap stage it lacks
        rf::gr::TextureSource ts = b.mode.get_texture_source();
        if (ts == rf::gr::TEXTURE_SOURCE_CLAMP_1_WRAP_0 || ts == rf::gr::TEXTURE_SOURCE_CLAMP_1_WRAP_0_MOD2X) {
            ts = rf::gr::TEXTURE_SOURCE_WRAP;
        }
        else if (ts == rf::gr::TEXTURE_SOURCE_CLAMP_1_CLAMP_0) {
            ts = rf::gr::TEXTURE_SOURCE_CLAMP;
        }
        b.mode.set_texture_source(ts);
        if (all_fullbright) {
            continue;
        }
        b.mode.set_color_source(rf::gr::COLOR_SOURCE_VERTEX_TIMES_TEXTURE);
        b.color = color;
    }
}

// Transformed-vertex array on 0x0055F5E0's stack
constexpr int legacy_max_cache_vertices = 8000;

// The rooms g_cache_clear 0x004F0B90 drops the caches of. Stock holds 256 with no bounds check, and
// every terrain chunk has a cache of its own.
static rf::GRoom* g_geo_cache_rooms[8192];

// The detail rooms 0x004F0C00 merged into the cache it builds. Stock holds 256 with no bounds check:
// the 257th overwrites the count, which 0x004F0660 then walks as a room.
static rf::GRoom* g_geo_cache_detail_rooms[8192];

// Rooms too big for the legacy vertex array, skipped until g_cache_clear next runs. The first build
// after a clear finds the cache room list empty.
static std::vector<rf::GRoom*> g_geo_cache_oversized_rooms;

CodeInjection geo_cache_prepare_room_add_detail_room_injection{
    0x004F0DFD,
    [](auto& regs) {
        if (rf::geo_cache_num_detail_rooms >= static_cast<int>(std::size(g_geo_cache_detail_rooms))) {
            regs.eip = 0x004F0EE8; // next detail room, as for an invisible one
        }
    },
};

FunHook<int(rf::GSolid*, rf::GRoom*)> geo_cache_prepare_room_hook{
    0x004F0C00,
    [](rf::GSolid* solid, rf::GRoom* room) {
        // Failing makes the caller drop every cache and retry, as it does when cache memory runs out
        if (!room->geo_cache && rf::geo_cache_num_rooms >= static_cast<int>(std::size(g_geo_cache_rooms))) {
            return -1;
        }
        if (rf::geo_cache_num_rooms == 0) {
            g_geo_cache_oversized_rooms.clear();
        }
        if (!room->geo_cache && std::find(g_geo_cache_oversized_rooms.begin(), g_geo_cache_oversized_rooms.end(),
                                          room) != g_geo_cache_oversized_rooms.end()) {
            return 0;
        }
        // Terrain chunks draw from caches of their own (gr_render_static_solid_hook)
        const bool terrain_chunk = room->is_detail && alpine_terrain_is_chunk_room(room);
        std::vector<rf::GRoom*> hidden;
        if (terrain_chunk) {
            // The builder walks the root's detail rooms; an RF2 carve can leave garbage in a chunk's own
            // detail list, so clear it.
            if (room->detail_rooms.size() > 0) {
                room->detail_rooms.clear();
            }
        }
        else if (!room->is_detail) {
            // Out of memory leaves the rest in the parent's cache (drawn twice), never an exception
            try {
                for (rf::GRoom* detail : room->detail_rooms) {
                    if (detail && !detail->is_invisible && alpine_terrain_is_separate_chunk(room, detail)) {
                        hidden.push_back(detail);
                        detail->is_invisible = true;
                    }
                }
            }
            catch (const std::bad_alloc&) {
            }
        }
        char* const arena_pos = rf::geo_cache_arena_pos;
        const int num_cache_rooms = rf::geo_cache_num_rooms;
        int ret = geo_cache_prepare_room_hook.call_target(solid, room);
        for (rf::GRoom* detail : hidden) {
            detail->is_invisible = false;
        }
        if (ret == 0 && room->geo_cache) {
            int num_verts = room->geo_cache->num_vertices;
            if (num_verts > legacy_max_cache_vertices) {
                static int num_warnings = 0;
                if (num_warnings < 32) {
                    num_warnings++;
                    xlog::warn("Not rendering room {} (uid {}): {} vertices exceed the legacy renderer's {}",
                               room->room_index, room->uid, num_verts, legacy_max_cache_vertices);
                }
                try {
                    g_geo_cache_oversized_rooms.push_back(room);
                }
                catch (const std::bad_alloc&) {
                }
                // Undo the build's last writes: its arena allocation and its entry in the cache room list
                room->geo_cache = nullptr;
                rf::geo_cache_arena_pos = arena_pos;
                rf::geo_cache_num_rooms = num_cache_rooms;
                // Failing would drop and rebuild every cache each frame for one room
                return 0;
            }
            geo_cache_tint_terrain_batches(*room->geo_cache);
        }
        return ret;
    },
};

// Legacy renderers draw each terrain chunk once per pass, after the pass's rooms, from its own cache.
// A chunk is seen through every visible room listing it, so it culls against the union of their
// portal windows (detail rooms have none of their own).
CallHook<void(rf::GSolid*, rf::GRoom**, int)> gr_render_static_solid_hook{
    0x00516D28,
    [](rf::GSolid* solid, rf::GRoom** rooms, int num_rooms) {
        gr_render_static_solid_hook.call_target(solid, rooms, num_rooms);
        if (is_d3d11()) {
            return;
        }
        struct Chunk
        {
            rf::GRoom* room;
            rf::GClipWnd saved_clip_wnd;
        };
        static std::vector<Chunk> chunks;
        static std::vector<int> chunk_of_room; // by room_index, -1 when not collected
        static std::vector<rf::GRoom*> draw;
        chunks.clear();
        draw.clear();
        // Out of memory skips the chunks this pass; every window changed so far is in `chunks`.
        try {
            for (int i = 0; i < num_rooms; ++i) {
                rf::GRoom* room = rooms[i];
                for (rf::GRoom* detail : room->detail_rooms) {
                    if (!detail || detail->is_invisible || detail->face_list.empty() ||
                        !alpine_terrain_is_separate_chunk(room, detail)) {
                        continue;
                    }
                    const auto idx = static_cast<std::size_t>(detail->room_index);
                    if (idx >= chunk_of_room.size()) {
                        chunk_of_room.resize(idx + 1, -1);
                    }
                    rf::GClipWnd& wnd = detail->clip_wnd;
                    if (chunk_of_room[idx] < 0) {
                        chunks.push_back({detail, wnd});
                        chunk_of_room[idx] = static_cast<int>(chunks.size() - 1);
                        wnd = room->clip_wnd;
                        continue;
                    }
                    wnd.left = std::min(wnd.left, room->clip_wnd.left);
                    wnd.top = std::min(wnd.top, room->clip_wnd.top);
                    wnd.right = std::max(wnd.right, room->clip_wnd.right);
                    wnd.bot = std::max(wnd.bot, room->clip_wnd.bot);
                }
            }
            // Culled the way 0x0055F5E0 culls a room, before it builds a cache for the chunk
            for (const Chunk& c : chunks) {
                rf::set_currently_rendered_room(c.room);
                if (!rf::gr::cull_bounding_box(c.room->bbox_min, c.room->bbox_max)) {
                    draw.push_back(c.room);
                }
            }
        }
        catch (const std::bad_alloc&) {
            draw.clear();
            xlog::warn("Out of memory drawing terrain chunks");
        }
        if (!draw.empty()) {
            // The renderer advances its batch scroll clock by a frame per call
            const float clock = rf::gr_static_solid_scroll_clock;
            rf::gr_static_solid_scroll_clock = clock - rf::frametime;
            gr_render_static_solid_hook.call_target(solid, draw.data(), static_cast<int>(draw.size()));
            rf::gr_static_solid_scroll_clock = clock;
        }
        for (const Chunk& c : chunks) {
            c.room->clip_wnd = c.saved_clip_wnd;
            chunk_of_room[static_cast<std::size_t>(c.room->room_index)] = -1;
        }
    },
};

CodeInjection GSurface_calculate_lightmap_color_conv_patch{
    0x004F2F23,
    [](auto& regs) {
        // Always skip original code
        regs.eip = 0x004F3023;

        rf::GSurface* surface = regs.esi;
        rf::GLightmap& lightmap = *surface->lightmap;
        rf::gr::LockInfo lock;
        if (!rf::gr::lock(lightmap.bm_handle, 0, &lock, rf::gr::LOCK_WRITE_ONLY)) {
            return;
        }

        int offset_x = surface->xstart;
        int offset_y = surface->ystart;
        int src_width = lightmap.w;
        int dst_pixel_size = bm_bytes_per_pixel(lock.format);
        uint8_t* src_data = lightmap.buf + 3 * (offset_x + offset_y * src_width);
        uint8_t* dst_data = &lock.data[dst_pixel_size * offset_x + offset_y * lock.stride_in_bytes];
        int height = surface->height;
        int src_pitch = 3 * src_width;
        bool success = bm_convert_format(dst_data, lock.format, src_data, rf::bm::FORMAT_888_BGR,
            surface->width, height, lock.stride_in_bytes, src_pitch);
        if (!success)
            xlog::error("bm_convert_format failed for geomod (fmt {})", static_cast<int>(lock.format));
        rf::gr::unlock(&lock);
    },
};

CodeInjection GSurface_alloc_lightmap_color_conv_patch{
    0x004E487B,
    [](auto& regs) {
        // Skip original code
        regs.eip = 0x004E4993;

        rf::GSurface* surface = regs.esi;
        rf::GLightmap& lightmap = *surface->lightmap;
        rf::gr::LockInfo lock;
        if (!rf::gr::lock(lightmap.bm_handle, 0, &lock, rf::gr::LOCK_WRITE_ONLY)) {
            return;
        }

        int offset_y = surface->ystart;
        int src_width = lightmap.w;
        int offset_x = surface->xstart;
        uint8_t* src_data_begin = lightmap.buf;
        int src_offset = 3 * (offset_x + src_width * surface->ystart); // src offset
        uint8_t* src_data = src_offset + src_data_begin;
        int height = surface->height;
        int dst_pixel_size = bm_bytes_per_pixel(lock.format);
        uint8_t* dst_row_ptr = &lock.data[dst_pixel_size * offset_x + offset_y * lock.stride_in_bytes];
        int src_pitch = 3 * src_width;
        bool success = bm_convert_format(dst_row_ptr, lock.format, src_data, rf::bm::FORMAT_888_BGR,
                                                 surface->width, height, lock.stride_in_bytes, src_pitch);
        if (!success)
            xlog::error("ConvertBitmapFormat failed for geomod2 (fmt {})", static_cast<int>(lock.format));
        rf::gr::unlock(&lock);
    },
};

CodeInjection GSolid_get_ambient_color_from_lightmap_patch{
    0x004E5CE3,
    [](auto& regs) {
        auto stack_frame = regs.esp + 0x34;
        int x = regs.edi;
        int y = regs.ebx;
        auto& lm = *static_cast<rf::GLightmap*>(regs.esi);
        auto& color = *reinterpret_cast<rf::Color*>(stack_frame - 0x28);

        // Skip original code
        regs.eip = 0x004E5D57;

        // Optimization: instead of locking the lightmap texture get color data from lightmap pixels stored in RAM
        const uint8_t* src_ptr = lm.buf + (y * lm.w + x) * 3;
        color.set(src_ptr[0], src_ptr[1], src_ptr[2], 255);
    },
};

// Terrain faces have no surface, so stock returns white ("no lightmap") for anything on them.
FunHook<rf::Color* __fastcall(rf::GSolid*, int, rf::Color*, rf::GFace*, rf::Vector3*)> GSolid_get_ambient_color_hook{
    0x004E5C60,
    [](rf::GSolid* solid, int edx, rf::Color* out, rf::GFace* face, rf::Vector3* pos) FASTCALL_LAMBDA -> rf::Color* {
        if (face && pos && face->attributes.surface_index < 0) {
            if (const AlpineTerrainRoomRef* ref = alpine_terrain_find_room(face->which_room)) {
                const AlpineTerrain& t = alpine_terrain_get_all()[ref->terrain];
                const float p[3] = {pos->x, pos->y, pos->z};
                const float n[3] = {face->plane.normal.x, face->plane.normal.y, face->plane.normal.z};
                float texel[3];
                const auto kind = alpine_terrain_face_kind(alpine_terrain_grid(t), *face);
                alpine_terrain_sample_light(ref->terrain, kind, p, n, texel);
                // Capped below 255 so the D3D11 mesh path never reads it as "no lightmap"
                auto to_byte = [](float v) {
                    return static_cast<rf::ubyte>(std::clamp(v * 255.0f + 0.5f, 0.0f, 254.0f));
                };
                out->set(to_byte(texel[0]), to_byte(texel[1]), to_byte(texel[2]), 255);
                return out;
            }
        }
        return GSolid_get_ambient_color_hook.call_target(solid, edx, out, face, pos);
    },
};

// perhaps this code should be in g_solid.cpp but we don't have access to PixelsReader/Writer there
void gr_copy_water_bitmap(rf::gr::LockInfo& src_lock, rf::gr::LockInfo& dst_lock)
{
    int src_pixel_size = bm_bytes_per_pixel(src_lock.format);
    try {
        call_with_format(src_lock.format, [=](auto s) {
            call_with_format(dst_lock.format, [=](auto d) {
                if constexpr (decltype(s)::value == rf::bm::FORMAT_8_PALETTED
                    || decltype(d)::value == rf::bm::FORMAT_8_PALETTED) {
                    assert(false);
                    return;
                }
                uint8_t* dst_row_ptr = dst_lock.data;
                for (int y = 0; y < dst_lock.h; ++y) {
                    auto& byte_1370f90 = addr_as_ref<uint8_t[256]>(0x1370F90);
                    auto& byte_1371b14 = addr_as_ref<uint8_t[256]>(0x1371B14);
                    auto& byte_1371090 = addr_as_ref<uint8_t[512]>(0x1371090);
                    int t1 = byte_1370f90[y];
                    int t2 = byte_1371b14[y];
                    uint8_t* off_arr = &byte_1371090[-t1];
                    PixelsWriter<decltype(d)::value> wrt{dst_row_ptr};
                    for (int x = 0; x < dst_lock.w; ++x) {
                        int src_x = t1;
                        int src_y = t2 + off_arr[t1];
                        int src_x_limited = src_x & (dst_lock.w - 1);
                        int src_y_limited = src_y & (dst_lock.h - 1);
                        const uint8_t* src_ptr = src_lock.data + src_x_limited * src_pixel_size + src_y_limited * src_lock.stride_in_bytes;
                        PixelsReader<decltype(s)::value> rdr{src_ptr};
                        wrt.write(rdr.read());
                        ++t1;
                    }
                    dst_row_ptr += dst_lock.stride_in_bytes;
                }
            });
        });
    }
    catch (const std::exception& e) {
        xlog::error("Pixel format conversion failed for liquid wave texture: {}", e.what());
    }
}

CodeInjection g_proctex_update_water_patch{
    0x004E68D1,
    [](auto& regs) {
        // Skip original code
        regs.eip = 0x004E6B68;

        auto& proctex = *static_cast<rf::GProceduralTexture*>(regs.esi);
        rf::gr::LockInfo base_bm_lock;
        if (!rf::gr::lock(proctex.base_bm_handle, 0, &base_bm_lock, rf::gr::LOCK_READ_ONLY)) {
            return;
        }
        bm_set_dynamic(proctex.user_bm_handle, true);
        rf::gr::LockInfo user_bm_lock;
        if (!rf::gr::lock(proctex.user_bm_handle, 0, &user_bm_lock, rf::gr::LOCK_WRITE_ONLY)) {
            rf::gr::unlock(&base_bm_lock);
            return;
        }

        gr_copy_water_bitmap(base_bm_lock, user_bm_lock);

        rf::gr::unlock(&base_bm_lock);
        rf::gr::unlock(&user_bm_lock);
    }
};

CodeInjection g_proctex_create_bm_create_injection{
    0x004E66A2,
    [](auto& regs) {
        int bm_handle = regs.eax;
        bm_set_dynamic(bm_handle, true);
    },
};

CodeInjection face_scroll_fix{
    0x004EE1D6,
    [](auto& regs) {
        rf::GSolid* solid = regs.ebp;
        auto& texture_movers = solid->texture_movers;
        for (auto& tm : texture_movers) {
            tm->update_solid(solid);
        }
    },
};

// D3D11 scrolls texture-mover faces in the vertex shader (pan_speed * time), so the
// stock CPU-side UV accumulation would be applied a second time whenever a room
// render cache is rebuilt.
CallHook<void __fastcall(rf::GTextureMover*, int, float)> texture_mover_process_hook{
    0x004E617D,
    [](rf::GTextureMover* mover, int edx, float dt) FASTCALL_LAMBDA {
        if (!is_d3d11()) {
            texture_mover_process_hook.call_target(mover, edx, dt);
        }
    },
};

CodeInjection ingame_add_lightmap_to_face_fullbright_fix{
    0x004E5BBE,
    [](auto& regs) {
        // idea for future AlpineLevelProps bool:
        // Use level ambient light for surfaces without lighting (geo craters and uncalced)
        // would need to do the same operation as below
        rf::GSurface* surface = regs.esi;
        rf::GFace* face = regs.edi;

        if (!surface || !face)
            return;

        if (face->attributes.flags & rf::GFaceFlags::FACE_FULL_BRIGHT) {
            surface->fullbright = true; // should be set already anyway, but just in case

            xlog::debug("Skipping lightmap generation for fullbright face {} (surface {})",
                face->attributes.face_id,
                surface->index);

            regs.eip = 0x004E5C33;
        }
    },
};

CodeInjection g_proctex_update_water_speed_fix{
    0x004E68A0,
    [](auto& regs) {
        auto& pt = *static_cast<rf::GProceduralTexture*>(regs.esi);
        pt.slide_pos_xt += 12.8f * rf::frametime / reference_frametime;
        pt.slide_pos_yc += 4.27f * rf::frametime / reference_frametime;
        pt.slide_pos_yt += 3.878788f * rf::frametime / reference_frametime;
    },
};

CodeInjection g_face_does_point_lie_in_face_crashfix{
    0x004E1F93,
    [](auto& regs) {
        void* face_vertex = regs.esi;
        if (!face_vertex) {
            regs.bl = false;
            regs.eip = 0x004E206F;
        }
    },
};

CodeInjection level_load_lightmaps_color_conv_patch{
    0x004ED3E9,
    [](auto& regs) {
        // Always skip original code
        regs.eip = 0x004ED4FA;

        rf::GLightmap* lightmap = regs.ebx;

        rf::gr::LockInfo lock;
        if (!rf::gr::lock(lightmap->bm_handle, 0, &lock, rf::gr::LOCK_WRITE_ONLY))
            return;

        uint32_t floor_clamp = 0; // no floor
        uint32_t ceiling_clamp = 0xFFFFFFFF; // no ceiling
        bool should_clamp = false; // no clamping by default
        bool floor_clamp_defined = false;

        // Check if the level explicitly defines clamp floor
        if (!g_alpine_game_config.ignore_tbl_lightmap_clamping
            && g_alpine_level_info_config
            .is_option_loaded(
                rf::level.filename,
                AlpineLevelInfoID::LightmapClampFloor
            )
        ) {
            floor_clamp = get_level_info_value<uint32_t>(
                AlpineLevelInfoID::LightmapClampFloor
            );
            floor_clamp_defined = true;
            should_clamp = true;
        }

        // Check if the level explicitly defines clamp ceiling
        if (!g_alpine_game_config.ignore_tbl_lightmap_clamping
            && g_alpine_level_info_config
            .is_option_loaded(
                rf::level.filename,
                AlpineLevelInfoID::LightmapClampCeiling
            )
        ) {
            ceiling_clamp = get_level_info_value<uint32_t>(
                AlpineLevelInfoID::LightmapClampCeiling
            );
            should_clamp = true;
        }

        // If no per-level floor clamp, consider using legacy clamping for non-Alpine levels
        if (!floor_clamp_defined && rf::level.version < 300) {
            if ((g_alpine_game_config.always_clamp_official_lightmaps
                && rf::level.version < 200)
                || (!DashLevelProps::instance().lightmaps_full_depth
                && !g_alpine_game_config.full_range_lighting)) {
                should_clamp = true;
                constexpr int default_floor_clamp = 0x202020FF;
                floor_clamp = default_floor_clamp;
            }
        }

        // Clamp lightmaps only if:
        // - mapname_info.tbl says to (takes priority unless cl_ignore_tbl_lightmap_clamping is enabled)
        // - Is an official Volition map AND "Always clamp official lightmaps" is turned on
        // - Is a version 200 map, AF "Full range lights" is turned off, AND DF "Lightmaps full depth" is turned off (or not set)

        // Apply clamping
        if (should_clamp) {
            xlog::debug("Applying lightmap clamping");

            const rf::gr::Color floor = rf::gr::Color::from_hex(floor_clamp);
            const rf::gr::Color ceiling = rf::gr::Color::from_hex(ceiling_clamp);

            if (ceiling.red < floor.red
                || ceiling.green < floor.green
                || ceiling.blue < floor.blue)
            {
                xlog::warn("Normalizing an invalid lightmap clamping range");
            }

            const auto [r_min, r_max] = std::minmax(floor.red, ceiling.red);
            const auto [g_min, g_max] = std::minmax(floor.green, ceiling.green);
            const auto [b_min, b_max] = std::minmax(floor.blue, ceiling.blue);

            for (int i = 0; i < lightmap->w * lightmap->h * 3; i += 3) {
                lightmap->buf[i + 0] = std::clamp(lightmap->buf[i + 0], r_min, r_max);
                lightmap->buf[i + 1] = std::clamp(lightmap->buf[i + 1], g_min, g_max);
                lightmap->buf[i + 2] = std::clamp(lightmap->buf[i + 2], b_min, b_max);
            }
        }

        bool success = bm_convert_format(lock.data, lock.format, lightmap->buf,
            rf::bm::FORMAT_888_BGR, lightmap->w, lightmap->h, lock.stride_in_bytes, 3 * lightmap->w, nullptr);
        if (!success)
            xlog::error("ConvertBitmapFormat failed for lightmap (dest format {})", static_cast<int>(lock.format));

        rf::gr::unlock(&lock);
    },
};

ConsoleCommand2 lighting_color_range_cmd{
    "r_fullrangelighting",
    []() {
        g_alpine_game_config.full_range_lighting = !g_alpine_game_config.full_range_lighting;        
        rf::console::printf("Full range lighting is: %s", g_alpine_game_config.full_range_lighting ? "enabled" : "disabled");
    },
    "Toggle full range lighting. Only affects new level loads.",
};

ConsoleCommand2 clamp_official_lightmaps_cmd{
    "r_clampofficiallightmaps",
    []() {
        g_alpine_game_config.always_clamp_official_lightmaps = !g_alpine_game_config.always_clamp_official_lightmaps;
        rf::console::printf("Forced clamping of lightmaps in official levels is: %s", g_alpine_game_config.always_clamp_official_lightmaps ? "enabled" : "disabled");
    },
    "Toggle forced lightmap clamping for official Volition levels. Only affects new level loads. Only applicable if full range lighting is enabled.",
};

ConsoleCommand2 ignore_tbl_lightmap_clamping_cmd{
    "cl_ignore_tbl_lightmap_clamping",
    []() {
        g_alpine_game_config.ignore_tbl_lightmap_clamping = !g_alpine_game_config.ignore_tbl_lightmap_clamping;
        rf::console::printf("Ignore TBL lightmap clamping override: %s", g_alpine_game_config.ignore_tbl_lightmap_clamping ? "enabled" : "disabled");
        rf::console::printf("Reload the level for this to take effect.");
    },
    "Toggle ignoring per-map lightmap clamping overrides from mapname_info.tbl. Only affects new level loads.",
};

CodeInjection shadow_render_one_injection{
    0x004CB195,
    [](auto& regs) {
        void* svol = regs.eax;
        auto& bbox_min = struct_field_ref<rf::Vector3>(svol, 0xE4);
        auto& bbox_max = struct_field_ref<rf::Vector3>(svol, 0xF0);
        rf::MoverBrush *mb = regs.esi;
        if (!rf::bbox_intersect(bbox_min, bbox_max, mb->p_data.bbox_min, mb->p_data.bbox_max)) {
            regs.eip = 0x004CB1DA;
        }
    },
};

void* __fastcall decals_farray_ctor(void* that)
{
    return AddrCaller{0x004D3120}.this_call<void*>(that, 64);
}

CodeInjection g_decal_add_internal_cmp_global_weak_limit_injection{
    0x004D54AC,
    [](auto& regs) {
        // total decals soft limit, 96 by default
        if (regs.esi < g_max_decals * 3 / 4) {
            regs.eip = 0x004D54D6;
        }
        else {
            regs.eip = 0x004D54B1;
        }
    },
};

// Stock decal passes keep their room bbox worklist in a 1024-slot stack array with no bounds check,
// filled with every detail room of up to two rooms: a parent owning ~1000 detail rooms (large
// terrains) overwrote the return address. Same traversal, unbounded worklist. Neither pass nests
// (0x004D5440 runs them in turn), so the worklist is kept between calls. Out of memory ends the
// traversal early: the decal keeps the faces it reached, since the stock worklist is what overflows.
template<typename F>
static void decal_for_each_room_bbox_face(rf::GDecal* decal, F&& fn)
{
    static std::vector<rf::GBBox*> work;
    work.clear();
    try {
        for (rf::GRoom* room : {decal->room, decal->room2}) {
            if (!room) {
                continue;
            }
            work.push_back(room->bbox);
            for (rf::GRoom* detail : room->detail_rooms) {
                if (detail) {
                    work.push_back(detail->bbox);
                }
            }
        }
        while (!work.empty()) {
            rf::GBBox* box = work.back();
            work.pop_back();
            if (!box || !rf::bbox_overlap(&box->min, &box->max, &decal->bb_min, &decal->bb_max)) {
                continue;
            }
            for (rf::GBBox* child : box->children) {
                if (child) {
                    work.push_back(child);
                }
            }
            for (rf::GFace* face = box->face_list.first(); face; face = face->next[rf::FACE_LIST_BBOX]) {
                fn(face);
            }
        }
    }
    catch (const std::bad_alloc&) {
        work.clear();
        xlog::warn("Out of memory placing a decal");
    }
}

FunHook<void(rf::GDecal*)> g_decal_clip_to_geometry_hook{
    0x004D6910,
    [](rf::GDecal* decal) {
        if (rf::g_decal_pass_list_a.size() > 0 || !decal->room) {
            g_decal_clip_to_geometry_hook.call_target(decal);
            return;
        }
        decal_for_each_room_bbox_face(decal, [decal](rf::GFace* face) {
            if (!(face->attributes.flags & rf::FACE_SCROLL_TEXTURE)) {
                rf::g_decal_clip_to_face(decal, face, false);
            }
        });
    },
};

// Room branch of the decal orientation pass 0x004D5DA0; the solid branch and the tail stay stock.
CodeInjection g_decal_orient_room_faces_injection{
    0x004D5DC2,
    [](auto& regs) {
        auto* decal = addr_as_ref<rf::GDecal*>(regs.esp + 0x1040);
        if (!decal->room) {
            return;
        }
        const float min_dot = rf::g_decal_orient_min_dot;
        decal_for_each_room_bbox_face(decal, [decal, min_dot](rf::GFace* face) {
            const auto& attr = face->attributes;
            if (attr.portal_id > 0 || attr.is_liquid() || (attr.flags & rf::FACE_SCROLL_TEXTURE)) {
                return;
            }
            if (!(rf::vector_dot_prod(&face->plane.normal, &decal->orient.fvec) > min_dot)) {
                return;
            }
            if (rf::g_decal_face_in_reach(face, decal)) {
                rf::g_decal_pass_list_b_add_unique(face->plane.normal);
                rf::g_decal_pass_list_a_add(face);
            }
        });
        regs.edi = decal;
        regs.eip = 0x004D5FE2;
    },
};

void decal_patch_limit(int max_decals)
{
    unsigned decal_farray_get_addresses[] = {
        0x00492298,
        0x004BBEC9,

        0x004D5515,
        0x004D5525,
        0x004D5545,
        0x004D5579,
        0x004D5589,
        0x004D55AD,
        0x004D55D9,
        0x004D55E9,
        0x004D5609,
        0x004D5635,
        0x004D5645,
        0x004D5666,
        0x004D569E,
        0x004D56AE,
        0x004D56CE,
        0x004D56FA,
        0x004D570A,
        0x004D572B,
    };
    unsigned decal_farray_add_addresses[] = {
        0x004D58AE,
        0x004D58BE,
        0x004D58D2,
    };
    unsigned decal_farray_add_unique_addresses[] = {
        0x004D67B1,
    };
    unsigned decal_farray_remove_matching_addresses[] = {
        0x004D6C68,
        0x004D6C93,
        0x004D6CB6,
        0x004D6CE1,
        0x004D6D10,
    };
    unsigned decal_farray_ctor_addresses[] = {
        0x004CCC51,
        0x004CF152,
    };
    unsigned decal_farray_dtor_addresses[] = {
        0x004CCE79,
        0x004CF452,
    };
    for (auto addr : decal_farray_get_addresses) {
        AsmWriter{addr}.call(0x0040A480);
    }
    for (auto addr : decal_farray_add_addresses) {
        AsmWriter{addr}.call(0x0045EC40);
    }
    for (auto addr : decal_farray_add_unique_addresses) {
        AsmWriter{addr}.call(0x0040A450);
    }
    for (auto addr : decal_farray_remove_matching_addresses) {
        AsmWriter{addr}.call(0x004BF550);
    }
    for (auto addr : decal_farray_ctor_addresses) {
        AsmWriter{addr}.call(decals_farray_ctor);
    }
    for (auto addr : decal_farray_dtor_addresses) {
        AsmWriter{addr}.call(0x0040EC50);
    }
    max_decals = std::min(max_decals, 512);
    constexpr int i8_max = std::numeric_limits<i8>::max();
    static rf::GDecal decal_slots[512]; // 128 by default
    write_mem_ptr(0x004D4F51 + 1, &decal_slots);
    write_mem_ptr(0x004D4F93 + 1, &decal_slots[std::size(decal_slots)]);
    // crossing soft limit causes fading out of old decals
    // crossing hard limit causes deletion of old decals
    write_mem<i32>(0x004D5456 + 2, max_decals); // total hard limit,  128 by default
    // Note: total soft level is handled in g_decal_add_internal_cmp_global_weak_limit_injection
    int max_decals_in_room = std::min<int>(max_decals / 3, i8_max);
    write_mem<i8>(0x004D55C4 + 2, max_decals_in_room); // room hard limit,   48 by default
    write_mem<i8>(0x004D5620 + 2, max_decals_in_room * 5 / 6); // room soft limit,   40 by default
    write_mem<i8>(0x004D5689 + 2, max_decals_in_room); // room hard limit,   48 by default
    write_mem<i8>(0x004D56E5 + 2, max_decals_in_room * 5 / 6); // room soft limit,   40 by default
    write_mem<i8>(0x004D5500 + 2, max_decals_in_room); // solid hard limit,  48 by default
    write_mem<i8>(0x004D555C + 2, max_decals_in_room * 5 / 6); // solid soft limit,  40 by default
    int max_weapon_decals = std::min<int>(max_decals / 8, i8_max);
    write_mem<i8>(0x004D5752 + 2, max_weapon_decals);  // weapon hard limit, 16 by default
    write_mem<i8>(0x004D579F + 2, max_weapon_decals);  // weapon hard limit, 16 by default
    write_mem<i8>(0x004D57AA + 2, max_weapon_decals * 7 / 8);  // weapon soft limit, 14 by default
    int max_geomod_decals = std::min<int>(max_decals / 4, i8_max);
    write_mem<i8>(0x004D584D + 2, max_geomod_decals);  // geomod hard limit, 32 by default
    write_mem<i8>(0x004D5852 + 2, max_geomod_decals * 7 / 8);  // geomod soft limit, 30 by default
}

ConsoleCommand2 max_decals_cmd{
    "max_decals",
    [](std::optional<int> max_decals) {
        if (max_decals) {
            g_max_decals = std::clamp(max_decals.value(), 128, 512);
            decal_patch_limit(g_max_decals);
        }
        rf::console::print("Max decals: {}", g_max_decals);
    },
};

static void render_rooms_clip_wnds() {
    rf::GRoom** rooms = nullptr;
    int num_rooms = 0;
    rf::g_get_room_render_list(&rooms, &num_rooms);
    rf::gr::set_color(255, 255, 255, 255);
    for (int i = 0; i < num_rooms; ++i) {
        const rf::GRoom* const room = rooms[i];
        char buf[256];
        std::snprintf(buf, sizeof(buf), "room %d", room->room_index);
        rf::gr::string(
            std::lround(room->clip_wnd.left),
            std::lround(room->clip_wnd.top),
            buf
        );
        rf::gr::rect_border(
            std::lround(room->clip_wnd.left),
            std::lround(room->clip_wnd.top),
            std::lround(room->clip_wnd.right - room->clip_wnd.left),
            std::lround(room->clip_wnd.bot - room->clip_wnd.top)
        );
    }
}

static ConsoleCommand2 dbg_room_clip_wnd_cmd{
    "dbg_room_clip_wnd",
    []() {
        g_show_room_clip_wnd = !g_show_room_clip_wnd;
        rf::console::print("Show room clip windows: {}", g_show_room_clip_wnd);
    },
};


void g_solid_render_ui()
{
    if (g_show_room_clip_wnd && rf::gameseq_in_gameplay()) {
        render_rooms_clip_wnds();
    }
}

CodeInjection levelmod_do_blast_autotexture_ppm_patch{
    0x00466C00,
    [](auto& regs) {
        rf::GSolid* solid = regs.ecx;
        int bitmap_handle = regs.edi;
        int bitmap_w = 256; // default to dimensions of stock geomod bitmaps
        int bitmap_h = 256;
        rf::bm::get_dimensions(bitmap_handle, &bitmap_w, &bitmap_h); // get dimensions of geomod bitmap if different
        float ppm_default = 256.0f / g_crater_autotexture_ppm;       // ppm of stock geomod bitmaps

        // New bitmap is likely square anyway, but if not, use maximum between its dimensions to prevent
        // one dimension from displaying with higher pixel density than expected
        float ppm_new = std::max(bitmap_w, bitmap_h) / ppm_default; // apply same ppm to new bitmap as default
        solid->set_levelmod_blast_autotexture_ppm(ppm_new);
    },
};

void set_levelmod_autotexture_ppm() {
    if (g_alpine_level_info_config.is_option_loaded(rf::level.filename, AlpineLevelInfoID::CraterTexturePPM)) {
        g_crater_autotexture_ppm = get_level_info_value<float>(AlpineLevelInfoID::CraterTexturePPM);
    }
    else {
        g_crater_autotexture_ppm = 32.0f;
    }
}

// verify proposed new sky room UID is a sky room and if so, set it as the override
void set_sky_room_uid_override(int room_uid, int anchor_uid, bool relative_position, float position_scale)
{
    // stock behaviour if no room specified
    if (room_uid <= 0) {
        g_sky_room_uid_override.reset();
        g_sky_room_eye_anchor.reset();
        g_sky_room_eye_offset_scale.reset();
        return;
    }

    // set new sky room if it's a valid room
    if (auto* possible_new_sky_room = rf::level_room_from_uid(room_uid)) {
        g_sky_room_uid_override = room_uid;
    }
    else {
        g_sky_room_uid_override.reset();
        g_sky_room_eye_anchor.reset();
        g_sky_room_eye_offset_scale.reset();
        return;
    }

    // no anchor specified
    if (anchor_uid <= 0) {
        g_sky_room_eye_anchor.reset();
        g_sky_room_eye_offset_scale.reset();
        return;
    }

    if (auto* possible_new_sky_eye_anchor = rf::obj_lookup_from_uid(anchor_uid)) {
        if (check_if_object_is_event_type(possible_new_sky_eye_anchor, rf::EventType::Anchor_Marker) ||
            check_if_object_is_event_type(possible_new_sky_eye_anchor, rf::EventType::Anchor_Marker_Orient)) {
            g_sky_room_eye_anchor = possible_new_sky_eye_anchor;

            g_sky_room_eye_offset_scale =
                (relative_position && position_scale > 0)
                ? std::make_optional(position_scale)
                : std::nullopt;
        }
    }
}

FunHook<rf::GRoom*(rf::GSolid*)> find_sky_room_hook{
    0x004D4B90,
    [](rf::GSolid* solid) {
        // Use original game behavior if no override is set
        if (!g_sky_room_uid_override) {
            return find_sky_room_hook.call_target(solid);
        }

        // Attempt to fetch the room directly by UID
        const int new_sky_room_uid = *g_sky_room_uid_override;
        if (auto* room = rf::level_room_from_uid(new_sky_room_uid)) {
            return room;
        }

        // Fallback to original game behavior if the room isn't found
        return find_sky_room_hook.call_target(solid);
    },
};

CodeInjection sky_room_eye_position_patch{
    0x004D3A0C,
    [](auto& regs) {
        // if we don't have an anchor, go to center of the room with original game logic
        if (!g_sky_room_eye_anchor) {
            return;
        }

        // anchor in world space
        rf::Object* eye_anchor = *g_sky_room_eye_anchor;
        rf::Vector3 anchor_pos = eye_anchor->pos;

        // translate position is on, calculate based on camera distance from 0,0,0 world space
        if (g_sky_room_eye_offset_scale && *g_sky_room_eye_offset_scale > 0) {

            rf::Player* local_player = rf::local_player;
            if (!local_player || !local_player->cam) {
                return;
            }

            // camera in world space
            rf::Camera* camera = local_player->cam;
            rf::Vector3 camera_pos = rf::camera_get_pos(camera);

            // translate camera position (relative to world origin) to eye position (relative to anchor)
            g_adjusted_sky_room_eye_position = {anchor_pos.x + (camera_pos.x * *g_sky_room_eye_offset_scale),
                                                anchor_pos.y + (camera_pos.y * *g_sky_room_eye_offset_scale),
                                                anchor_pos.z + (camera_pos.z * *g_sky_room_eye_offset_scale)};
        }
        else {
            // translate position is off, just use anchor position as the eye position
            g_adjusted_sky_room_eye_position = anchor_pos;
        }

        regs.eax = reinterpret_cast<int32_t>(&g_adjusted_sky_room_eye_position);
    },
};

// clean up sky room overrides and destruction state when shutting down level
CodeInjection level_release_sky_room_shutdown_patch{
    0x0045CAF9,
    [](auto& regs) {
        set_sky_room_uid_override(-1, -1, false, -1);
        destruction_level_cleanup();
    },
};

// The rooms g_solid_collect_visible_rooms_recursive 0x004D4860 lists for the frame. Stock holds 1024 with
// no bounds check.
static rf::GRoom* g_visible_rooms[8192];

// Rooms past the end are left out of the list
CodeInjection collect_visible_rooms_append_injection{
    0x004D48E3,
    [](auto& regs) {
        if (rf::g_num_visible_rooms < static_cast<int>(std::size(g_visible_rooms))) {
            g_visible_rooms[rf::g_num_visible_rooms++] = regs.edi;
        }
        regs.eip = 0x004D48F0;
    },
    false,
};

void g_solid_do_patch()
{
    // allow Set_Skybox to set a specific sky room
    find_sky_room_hook.install();
    sky_room_eye_position_patch.install();
    level_release_sky_room_shutdown_patch.install();

    // Buffer overflows in solid_read
    // Note: Buffer size is 1024 but opcode allows only 1 byte size
    //       What is more important bm_load copies texture name to 32 bytes long buffers
    write_mem<i8>(0x004ED612 + 1, 32);
    write_mem<i8>(0x004ED66E + 1, 32);
    write_mem<i8>(0x004ED72E + 1, 32);
    write_mem<i8>(0x004EDB02 + 1, 32);

    // Fix crash in geometry rendering
    geo_cache_prepare_room_hook.install();

    // Legacy renderers: terrain chunks draw from caches of their own
    gr_render_static_solid_hook.install();
    write_mem_ptr(0x004F0BA7 + 1, &g_geo_cache_rooms); // g_cache_clear
    write_mem_ptr(0x004F0BD9 + 1, &g_geo_cache_rooms); // g_cache_reset_after_boolean
    write_mem_ptr(0x004F1BF6 + 3, &g_geo_cache_rooms); // geo_cache_prepare_room
    write_mem_ptr(0x004F06F5 + 1, &g_geo_cache_detail_rooms); // 0x004F0660
    write_mem_ptr(0x004F0E07 + 3, &g_geo_cache_detail_rooms); // geo_cache_prepare_room
    geo_cache_prepare_room_add_detail_room_injection.install();

    // Visible room list
    write_mem_ptr(0x004D333E + 2, &g_visible_rooms); // g_get_room_render_list
    write_mem_ptr(0x004D4691 + 1, &g_visible_rooms); // g_solid_portal_renderer
    write_mem_ptr(0x004D46FD + 1, &g_visible_rooms); // g_solid_portal_renderer
    write_mem_ptr(0x004D471A + 3, &g_visible_rooms); // g_solid_portal_renderer
    write_mem_ptr(0x004D4971 + 1, &g_visible_rooms); // g_solid_collect_visible_rooms_recursive
    write_mem_ptr(0x004D4989 + 3, &g_visible_rooms); // g_solid_collect_visible_rooms_recursive
    // g_solid_collect_visible_rooms_recursive addresses the last room as [count * 4 + base - 4]
    write_mem<uintptr_t>(0x004D499D + 3, reinterpret_cast<uintptr_t>(&g_visible_rooms) - sizeof(rf::GRoom*));
    collect_visible_rooms_append_injection.install();

    // 32-bit color format - geomod
    GSurface_calculate_lightmap_color_conv_patch.install();
    GSurface_alloc_lightmap_color_conv_patch.install();
    // 32-bit color format - ambient color
    GSolid_get_ambient_color_from_lightmap_patch.install();
    GSolid_get_ambient_color_hook.install();
    // water
    AsmWriter(0x004E68B0, 0x004E68B6).nop();
    g_proctex_update_water_patch.install();

    // Fix face scroll in levels after version 0xB4
    face_scroll_fix.install();
    texture_mover_process_hook.install();

    // Fix faces with "fullbright" flag having randomly generated lightmaps applied after geomod
    ingame_add_lightmap_to_face_fullbright_fix.install();

    // Fix water waves animation on high FPS
    AsmWriter(0x004E68A0, 0x004E68A9).nop();
    AsmWriter(0x004E68B6, 0x004E68D1).nop();
    g_proctex_update_water_speed_fix.install();

    // Set dynamic flag on proctex texture
    g_proctex_create_bm_create_injection.install();

    // Add a missing check if face has any vertex in GFace::does_point_lie_in_face
    g_face_does_point_lie_in_face_crashfix.install();

    // fix pixel format for lightmaps
    write_mem<u8>(0x004F5EB8 + 1, rf::bm::FORMAT_888_RGB);

    // lightmaps format conversion
    level_load_lightmaps_color_conv_patch.install();

    // When rendering shadows check mover's bounding box before processing its faces
    shadow_render_one_injection.install();

    // Change decals limit
    decal_patch_limit(512);
    AsmWriter{0x004D54AF}.nop(2); // fix subhook trampoline preparation error
    g_decal_add_internal_cmp_global_weak_limit_injection.install();
    g_decal_clip_to_geometry_hook.install();
    g_decal_orient_room_faces_injection.install();

    // When rendering semi-transparent objects do not group objects that are behind a detail room.
    // Grouping breaks sorting in many cases because orientation and sizes of detail rooms and objects
    // are not taken into account.
    AsmWriter{0x004D4409}.jmp(0x004D44B1);
    AsmWriter{0x004D44C7}.nop(2);

    // Set PPM for geo crater texture based on its resolution instead of static value of 32.0
    levelmod_do_blast_autotexture_ppm_patch.install();

    // Commands
    max_decals_cmd.register_cmd();
    dbg_room_clip_wnd_cmd.register_cmd();
    lighting_color_range_cmd.register_cmd();
    clamp_official_lightmaps_cmd.register_cmd();
    ignore_tbl_lightmap_clamping_cmd.register_cmd();
}
