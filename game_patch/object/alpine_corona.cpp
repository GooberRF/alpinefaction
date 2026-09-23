#include <string>
#include <vector>
#include <cmath>
#include <xlog/xlog.h>
#include "../rf/object.h"
#include "../rf/clutter.h"
#include "../rf/glare.h"
#include "../rf/geometry.h"
#include "../rf/file/file.h"
#include "../rf/bmpman.h"
#include "../misc/level.h"
#include "alpine_obj_common.h"
#include "object.h"

// ─── Globals ─────────────────────────────────────────────────────────────────

// Corona data with associated clutter handle (clutter created during chunk loading,
// glare deferred to level_init_post when geometry is available for room assignment)
struct PendingCorona {
    AlpineCoronaInfo info;
    int clutter_handle;
};

static std::vector<PendingCorona> g_pending_coronas;

// Tracking for cleanup
static std::vector<int> g_corona_clutter_handles;
static std::vector<rf::GlareInfo*> g_corona_glare_infos;


// ─── Chunk Loading ───────────────────────────────────────────────────────────

void alpine_corona_load_chunk(rf::File& file, std::size_t chunk_len)
{
    std::size_t remaining = chunk_len;

    rf::File::ChunkGuard chunk_guard{file, remaining};

    AlpineChunkReader reader{file, remaining};

    uint32_t count = 0;
    if (!reader.read_bytes(&count, sizeof(count))) {
        xlog::warn("[AlpineCorona] Failed to read corona count from chunk (len={})", chunk_len);
        return;
    }
    if (count > 10000) count = 10000;

    xlog::info("[AlpineCorona] Loading {} corona(s) from chunk (len={})", count, chunk_len);

    for (uint32_t i = 0; i < count; i++) {
        AlpineCoronaInfo info;

        if (!reader.read_bytes(&info.uid, sizeof(info.uid))) return;
        if (!reader.read_bytes(&info.pos.x, sizeof(float))) return;
        if (!reader.read_bytes(&info.pos.y, sizeof(float))) return;
        if (!reader.read_bytes(&info.pos.z, sizeof(float))) return;
        if (!reader.read_bytes(&info.orient.rvec.x, sizeof(float))) return;
        if (!reader.read_bytes(&info.orient.rvec.y, sizeof(float))) return;
        if (!reader.read_bytes(&info.orient.rvec.z, sizeof(float))) return;
        if (!reader.read_bytes(&info.orient.uvec.x, sizeof(float))) return;
        if (!reader.read_bytes(&info.orient.uvec.y, sizeof(float))) return;
        if (!reader.read_bytes(&info.orient.uvec.z, sizeof(float))) return;
        if (!reader.read_bytes(&info.orient.fvec.x, sizeof(float))) return;
        if (!reader.read_bytes(&info.orient.fvec.y, sizeof(float))) return;
        if (!reader.read_bytes(&info.orient.fvec.z, sizeof(float))) return;
        if (!reader.read_string(info.script_name)) return;
        if (!reader.read_bytes(&info.color_r, sizeof(uint8_t))) return;
        if (!reader.read_bytes(&info.color_g, sizeof(uint8_t))) return;
        if (!reader.read_bytes(&info.color_b, sizeof(uint8_t))) return;
        if (!reader.read_bytes(&info.color_a, sizeof(uint8_t))) return;
        if (!reader.read_string(info.corona_bitmap)) return;
        if (info.corona_bitmap.size() >= max_bitmap_name) {
            xlog::warn("[AlpineCorona] Ignoring over-long corona bitmap name on corona uid {}", info.uid);
            info.corona_bitmap.clear();
        }
        if (!reader.read_bytes(&info.cone_angle, sizeof(float))) return;
        if (!reader.read_bytes(&info.intensity, sizeof(float))) return;
        if (!reader.read_bytes(&info.radius_distance, sizeof(float))) return;
        if (!reader.read_bytes(&info.radius_scale, sizeof(float))) return;
        if (!reader.read_bytes(&info.diminish_distance, sizeof(float))) return;
        if (!reader.read_string(info.volumetric_bitmap)) return;
        if (!info.volumetric_bitmap.empty()) {
            if (!reader.read_bytes(&info.volumetric_height, sizeof(float))) return;
            if (!reader.read_bytes(&info.volumetric_length, sizeof(float))) return;
        }
        if (info.volumetric_bitmap.size() >= max_bitmap_name) {
            xlog::warn("[AlpineCorona] Ignoring over-long volumetric bitmap name on corona uid {}", info.uid);
            info.volumetric_bitmap.clear();
        }

        // Create anchor clutter immediately so it exists before the stock link
        // resolver runs. This lets event→corona link UIDs resolve to handles
        rf::Clutter* clutter =
            alpine_create_anchor_clutter(info.pos, info.orient, info.uid, info.script_name.c_str());
        if (!clutter) {
            xlog::warn("[AlpineCorona] Failed to create clutter for corona uid={}", info.uid);
            continue;
        }

        const int clutter_handle = clutter->handle;
        g_corona_clutter_handles.push_back(clutter_handle);
        g_pending_coronas.push_back({std::move(info), clutter_handle});
    }
}

// ─── Glare Creation ─────────────────────────────────────────────────────────

// Called from level_init_post after geometry is loaded.
// Creates OT_GLARE children for each corona clutter that was created during chunk loading.
void alpine_corona_create_all()
{
    if (g_pending_coronas.empty()) return;

    for (const auto& pending : g_pending_coronas) {
        const auto& info = pending.info;

        // Look up the clutter we created during chunk loading
        rf::Object* obj = rf::obj_from_handle(pending.clutter_handle);
        if (!obj) {
            xlog::warn("[AlpineCorona] Clutter handle {:#x} no longer valid for corona uid={}", pending.clutter_handle, info.uid);
            continue;
        }

        // --- Create child glare ---
        rf::ObjectCreateInfo glare_oci{};
        glare_oci.pos = info.pos;
        glare_oci.orient = info.orient;

        rf::Object* glare_obj = rf::obj_create(rf::OT_GLARE, -1, obj->handle, &glare_oci, 0x30000, nullptr);
        if (!glare_obj) {
            xlog::warn("[AlpineCorona] Failed to create glare for corona uid={}", info.uid);
            continue;
        }

        auto* glare = reinterpret_cast<rf::Glare*>(glare_obj);

        // Allocate and populate GlareInfo
        auto* gi = new rf::GlareInfo{};

        gi->light_color = rf::gr::Color{info.color_r, info.color_g, info.color_b, info.color_a};

        if (!info.corona_bitmap.empty()) {
            gi->corona_bitmap = rf::bm::load(info.corona_bitmap.c_str(), -1, true);
            if (gi->corona_bitmap < 0) {
                xlog::warn("[AlpineCorona] Failed to load corona bitmap '{}'", info.corona_bitmap);
                gi->corona_bitmap = 0;
            }
        }

        gi->cone_angle = info.cone_angle * alpine_glare_cone_angle_factor;
        gi->intensity = info.intensity;
        gi->radius_scale = info.radius_scale;
        gi->radius_distance = info.radius_distance;
        gi->diminish_distance = info.diminish_distance;

        if (!info.volumetric_bitmap.empty()) {
            gi->volumetric_bitmap = rf::bm::load(info.volumetric_bitmap.c_str(), -1, true);
            if (gi->volumetric_bitmap < 0) {
                xlog::warn("[AlpineCorona] Failed to load volumetric bitmap '{}'", info.volumetric_bitmap);
                gi->volumetric_bitmap = 0;
            }
            gi->volumetric_height = info.volumetric_height;
            gi->volumetric_length = info.volumetric_length;
        }

        gi->reflection_bitmap = 0;

        // Set up Glare fields (matching stock FUN_00413d20 pattern)
        glare->enabled = true;
        glare->info = gi;
        glare->info_index = -1;
        glare->flags = 0;
        glare->last_covering_objh = -1;
        glare->last_covering_mover_brush = nullptr;
        glare->last_covering_face = nullptr;
        glare->last_rendered_intensity[0] = 0.0f;
        glare->last_rendered_intensity[1] = 0.0f;
        glare->last_rendered_radius[0] = 0.0f;
        glare->last_rendered_radius[1] = 0.0f;
        glare->is_rod = false;
        glare->parent_player = nullptr;

        // Insert into glare linked list
        rf::Glare* tail = rf::glare_list_tail;
        glare->prev = tail;
        glare->next = &rf::glare_list;
        tail->next = glare;
        rf::glare_list_tail = glare;

        g_corona_glare_infos.push_back(gi);

        xlog::trace("[AlpineCorona] Created corona uid={} pos=({:.2f},{:.2f},{:.2f}) room={:#010x} clutter={:#x} glare={:#x}",
            info.uid, obj->pos.x, obj->pos.y, obj->pos.z,
            reinterpret_cast<uintptr_t>(obj->room),
            obj->handle, glare_obj->handle);
    }

    xlog::info("[AlpineCorona] Created {} corona(s)", g_pending_coronas.size());
    g_pending_coronas.clear();
}

// ─── Cleanup ─────────────────────────────────────────────────────────────────

void alpine_corona_clear_state()
{
    // GlareInfo objects are heap-allocated by us, not in the global array
    for (auto* gi : g_corona_glare_infos) {
        delete gi;
    }
    g_corona_glare_infos.clear();

    // Clutter and glare objects are managed by the object system (obj_create),
    // so they get destroyed during normal level cleanup. We just clear our tracking.
    g_corona_clutter_handles.clear();

    g_pending_coronas.clear();
}
