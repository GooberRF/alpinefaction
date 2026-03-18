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

// ─── Constants ───────────────────────────────────────────────────────────────

// Stock effects.tbl parser multiplies cone angle degrees by 0.5 before storing in GlareInfo.
// The render code uses this same 0.5 constant for smoothing and cone attenuation calculations.
static constexpr float cone_angle_factor = 0.5f;

// ─── Globals ─────────────────────────────────────────────────────────────────

// Pending corona data parsed from chunk (geometry not loaded yet during chunk parsing)
static std::vector<AlpineCoronaInfo> g_pending_coronas;

// Tracking for cleanup
static std::vector<int> g_corona_clutter_handles;
static std::vector<rf::GlareInfo*> g_corona_glare_infos;


// ─── Chunk Loading ───────────────────────────────────────────────────────────

void alpine_corona_load_chunk(rf::File& file, std::size_t chunk_len)
{
    std::size_t remaining = chunk_len;

    rf::File::ChunkGuard chunk_guard{file, remaining};

    bool read_error = false;

    auto read_bytes = [&](void* dst, std::size_t n) -> bool {
        if (remaining < n) { read_error = true; return false; }
        int got = file.read(dst, n);
        if (got != static_cast<int>(n)) {
            if (got > 0) remaining -= got;
            read_error = true;
            return false;
        }
        remaining -= n;
        return true;
    };

    auto read_string = [&]() -> std::string {
        uint16_t len = 0;
        if (!read_bytes(&len, sizeof(len))) return "";
        if (len == 0) return "";
        std::string result(len, '\0');
        if (!read_bytes(result.data(), len)) return "";
        return result;
    };

    uint32_t count = 0;
    if (!read_bytes(&count, sizeof(count))) {
        xlog::warn("[AlpineCorona] Failed to read corona count from chunk (len={})", chunk_len);
        return;
    }
    if (count > 10000) count = 10000;

    xlog::info("[AlpineCorona] Loading {} corona(s) from chunk (len={})", count, chunk_len);

    for (uint32_t i = 0; i < count; i++) {
        AlpineCoronaInfo info;

        if (!read_bytes(&info.uid, sizeof(info.uid))) return;
        if (!read_bytes(&info.pos.x, sizeof(float))) return;
        if (!read_bytes(&info.pos.y, sizeof(float))) return;
        if (!read_bytes(&info.pos.z, sizeof(float))) return;
        if (!read_bytes(&info.orient.rvec.x, sizeof(float))) return;
        if (!read_bytes(&info.orient.rvec.y, sizeof(float))) return;
        if (!read_bytes(&info.orient.rvec.z, sizeof(float))) return;
        if (!read_bytes(&info.orient.uvec.x, sizeof(float))) return;
        if (!read_bytes(&info.orient.uvec.y, sizeof(float))) return;
        if (!read_bytes(&info.orient.uvec.z, sizeof(float))) return;
        if (!read_bytes(&info.orient.fvec.x, sizeof(float))) return;
        if (!read_bytes(&info.orient.fvec.y, sizeof(float))) return;
        if (!read_bytes(&info.orient.fvec.z, sizeof(float))) return;
        info.script_name = read_string();
        if (read_error) return;
        if (!read_bytes(&info.color_r, sizeof(uint8_t))) return;
        if (!read_bytes(&info.color_g, sizeof(uint8_t))) return;
        if (!read_bytes(&info.color_b, sizeof(uint8_t))) return;
        if (!read_bytes(&info.color_a, sizeof(uint8_t))) return;
        info.corona_bitmap = read_string();
        if (read_error) return;
        if (!read_bytes(&info.cone_angle, sizeof(float))) return;
        if (!read_bytes(&info.intensity, sizeof(float))) return;
        if (!read_bytes(&info.radius_distance, sizeof(float))) return;
        if (!read_bytes(&info.radius_scale, sizeof(float))) return;
        if (!read_bytes(&info.diminish_distance, sizeof(float))) return;
        info.volumetric_bitmap = read_string();
        if (read_error) return;
        if (!info.volumetric_bitmap.empty()) {
            if (!read_bytes(&info.volumetric_height, sizeof(float))) return;
            if (!read_bytes(&info.volumetric_length, sizeof(float))) return;
        }

        g_pending_coronas.push_back(std::move(info));
    }
}

// ─── Object Creation ─────────────────────────────────────────────────────────

// Called from level_init_post after geometry is loaded.
// Creates OT_CLUTTER (anchor) + OT_GLARE (visual) pairs for each pending corona.
void alpine_corona_create_all()
{
    if (g_pending_coronas.empty()) return;

    rf::GSolid* solid = rf::g_level_solid;

    for (const auto& info : g_pending_coronas) {
        // --- Create anchor clutter (no mesh) ---
        rf::ObjectCreateInfo oci{};
        oci.pos = info.pos;
        oci.orient = info.orient;

        rf::Object* obj = rf::obj_create(rf::OT_CLUTTER, -1, 0, &oci, 0, nullptr);
        if (!obj) {
            xlog::warn("[AlpineCorona] Failed to create clutter for corona uid={}", info.uid);
            continue;
        }

        auto* clutter = reinterpret_cast<rf::Clutter*>(obj);

        // Set up clutter fields (same pattern as alpine_mesh)
        clutter->info = &rf::get_dummy_clutter_info();
        clutter->info_index = -1;
        clutter->corpse_index = -1;
        clutter->sound_handle = -1;
        clutter->delayed_kill_sound = -1;
        clutter->dmg_type_that_killed_me = 0;
        clutter->corpse_vmesh_handle = nullptr;
        clutter->current_skin_index = 0;
        clutter->already_spawned_glass = false;
        clutter->use_sound = -1;
        clutter->killable_index = 0xFFFF;
        *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(clutter) + 0x2D0) = -1;

        // Insert into clutter linked list
        clutter->prev = rf::clutter_list_tail;
        clutter->next = reinterpret_cast<rf::Clutter*>(&rf::clutter_list);
        rf::clutter_list_tail->next = clutter;
        rf::clutter_list_tail = clutter;
        rf::clutter_count++;

        // Set identity
        obj->uid = info.uid;
        if (!info.script_name.empty()) {
            obj->name = info.script_name.c_str();
        }

        // Make invulnerable (no mesh to break)
        obj->obj_flags = static_cast<rf::ObjectFlags>(
            static_cast<int>(obj->obj_flags) | static_cast<int>(rf::OF_INVULNERABLE)
        );

        // Find room
        if (solid) {
            obj->room = solid->find_room(nullptr, &obj->pos, &obj->pos, nullptr);
        }

        // --- Create child glare ---
        rf::ObjectCreateInfo glare_oci{};
        glare_oci.pos = info.pos;
        glare_oci.orient = info.orient;

        rf::Object* glare_obj = rf::obj_create(rf::OT_GLARE, -1, obj->handle, &glare_oci, 0x30000, nullptr);
        if (!glare_obj) {
            xlog::warn("[AlpineCorona] Failed to create glare for corona uid={}", info.uid);
            g_corona_clutter_handles.push_back(obj->handle);
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

        gi->cone_angle = info.cone_angle * cone_angle_factor;
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

        // Set glare room to match parent clutter
        glare_obj->room = obj->room;

        // Insert into glare linked list
        rf::Glare* tail = rf::glare_list_tail;
        glare->prev = tail;
        glare->next = &rf::glare_list;
        tail->next = glare;
        rf::glare_list_tail = glare;

        g_corona_clutter_handles.push_back(obj->handle);
        g_corona_glare_infos.push_back(gi);

        xlog::trace("[AlpineCorona] Created corona uid={} pos=({:.2f},{:.2f},{:.2f}) room={:#010x} clutter={:#x} glare={:#x}",
            info.uid, obj->pos.x, obj->pos.y, obj->pos.z,
            reinterpret_cast<uintptr_t>(obj->room),
            obj->handle, glare_obj->handle);
    }

    xlog::info("[AlpineCorona] Created {} corona(s)", g_corona_clutter_handles.size());
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
