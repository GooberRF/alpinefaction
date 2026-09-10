#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <xlog/xlog.h>
#include "../rf/clutter.h"
#include "../rf/file/file.h"
#include "../rf/object.h"
#include "../misc/level.h"
#include "alpine_projection_camera.h"

static std::vector<int> g_camera_handles;

// Finite and roughly unit-length on each axis. Cheap enough to run per camera at load, and it
// only has to reject garbage, not validate handedness.
static bool orient_is_sane(const rf::Matrix3& orient)
{
    for (const rf::Vector3* v : {&orient.rvec, &orient.uvec, &orient.fvec}) {
        const float len_sq = v->x * v->x + v->y * v->y + v->z * v->z;
        if (!std::isfinite(len_sq) || len_sq < 0.9f || len_sq > 1.1f) {
            return false;
        }
    }
    return true;
}

// The camera is an invisible, mesh-less OT_CLUTTER so it owns a real object handle and uid, which
// is what makes stock event→object link resolution find it. Mirrors the anchor clutter alpine
// corona objects create; without a vmesh there is nothing to render or collide against.
void alpine_projection_camera_load_chunk(rf::File& file, std::size_t chunk_len)
{
    std::size_t remaining = chunk_len;

    rf::File::ChunkGuard chunk_guard{file, remaining};

    auto read_bytes = [&](void* dst, std::size_t n) -> bool {
        if (remaining < n) return false;
        int got = file.read(dst, n);
        if (got != static_cast<int>(n) || file.error()) {
            if (got > 0) remaining -= got;
            return false;
        }
        remaining -= n;
        return true;
    };

    auto read_string = [&](std::string& out) -> bool {
        uint16_t len = 0;
        if (!read_bytes(&len, sizeof(len))) return false;
        if (len == 0) return true;
        out.assign(len, '\0');
        return read_bytes(out.data(), len);
    };

    uint32_t count = 0;
    if (!read_bytes(&count, sizeof(count))) {
        xlog::warn("[AlpineProjectionCamera] Failed to read count from chunk (len={})", chunk_len);
        return;
    }
    if (count > 10000) {
        xlog::warn("[AlpineProjectionCamera] Chunk declares {} cameras, reading the first 10000", count);
        count = 10000;
    }

    xlog::info("[AlpineProjectionCamera] Loading {} projection camera(s) from chunk (len={})",
               count, chunk_len);

    uint32_t created = 0;
    for (uint32_t i = 0; i < count; i++) {
        int32_t uid = -1;
        rf::Vector3 pos{};
        rf::Matrix3 orient{};

        if (!read_bytes(&uid, sizeof(uid))) return;
        if (!read_bytes(&pos.x, sizeof(float))) return;
        if (!read_bytes(&pos.y, sizeof(float))) return;
        if (!read_bytes(&pos.z, sizeof(float))) return;
        if (!read_bytes(&orient.rvec.x, sizeof(float))) return;
        if (!read_bytes(&orient.rvec.y, sizeof(float))) return;
        if (!read_bytes(&orient.rvec.z, sizeof(float))) return;
        if (!read_bytes(&orient.uvec.x, sizeof(float))) return;
        if (!read_bytes(&orient.uvec.y, sizeof(float))) return;
        if (!read_bytes(&orient.uvec.z, sizeof(float))) return;
        if (!read_bytes(&orient.fvec.x, sizeof(float))) return;
        if (!read_bytes(&orient.fvec.y, sizeof(float))) return;
        if (!read_bytes(&orient.fvec.z, sizeof(float))) return;
        std::string script_name;
        if (!read_string(script_name)) return;

        // Unlike the other alpine objects this orientation is handed straight to gr::setup_3d as
        // a view basis, so a corrupt one would produce a garbage projection rather than a wrongly
        // drawn marker.
        if (!orient_is_sane(orient)) {
            xlog::warn("[AlpineProjectionCamera] uid={} has a malformed orientation, using identity", uid);
            orient = rf::Matrix3{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
        }

        rf::ObjectCreateInfo oci{};
        oci.pos = pos;
        oci.orient = orient;

        rf::Object* obj = rf::obj_create(rf::OT_CLUTTER, -1, 0, &oci, 0, nullptr);
        if (!obj) {
            xlog::warn("[AlpineProjectionCamera] Failed to create clutter for uid={}", uid);
            continue;
        }

        auto* clutter = reinterpret_cast<rf::Clutter*>(obj);
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

        clutter->prev = rf::clutter_list_tail;
        clutter->next = reinterpret_cast<rf::Clutter*>(&rf::clutter_list);
        rf::clutter_list_tail->next = clutter;
        rf::clutter_list_tail = clutter;
        rf::clutter_count++;

        obj->uid = uid;
        if (!script_name.empty()) {
            obj->name = script_name.c_str();
        }
        obj->obj_flags = static_cast<rf::ObjectFlags>(
            static_cast<int>(obj->obj_flags) | static_cast<int>(rf::OF_INVULNERABLE)
        );

        g_camera_handles.push_back(obj->handle);
        created++;
    }

    xlog::info("[AlpineProjectionCamera] Created {} projection camera(s)", created);
}

bool alpine_projection_camera_is_camera(int handle)
{
    return std::find(g_camera_handles.begin(), g_camera_handles.end(), handle) != g_camera_handles.end();
}

void alpine_projection_camera_clear_state()
{
    // The clutter objects belong to the object system and go away with the level.
    g_camera_handles.clear();
}
