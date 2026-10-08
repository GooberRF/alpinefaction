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
#include "alpine_obj_common.h"
#include "alpine_projection_camera.h"

static std::vector<int> g_camera_handles;

// The camera is an invisible, mesh-less OT_CLUTTER so it owns a real object handle and uid, which
// is what makes stock event→object link resolution find it. Mirrors the anchor clutter alpine
// corona objects create; without a vmesh there is nothing to render or collide against.
void alpine_projection_camera_load_chunk(rf::File& file, std::size_t chunk_len)
{
    std::size_t remaining = chunk_len;

    rf::File::ChunkGuard chunk_guard{file, remaining};

    AlpineChunkReader reader{file, remaining};

    uint32_t count = 0;
    if (!reader.read_bytes(&count, sizeof(count))) {
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

        if (!reader.read_bytes(&uid, sizeof(uid))) return;
        if (!reader.read_bytes(&pos.x, sizeof(float))) return;
        if (!reader.read_bytes(&pos.y, sizeof(float))) return;
        if (!reader.read_bytes(&pos.z, sizeof(float))) return;
        if (!reader.read_bytes(&orient.rvec.x, sizeof(float))) return;
        if (!reader.read_bytes(&orient.rvec.y, sizeof(float))) return;
        if (!reader.read_bytes(&orient.rvec.z, sizeof(float))) return;
        if (!reader.read_bytes(&orient.uvec.x, sizeof(float))) return;
        if (!reader.read_bytes(&orient.uvec.y, sizeof(float))) return;
        if (!reader.read_bytes(&orient.uvec.z, sizeof(float))) return;
        if (!reader.read_bytes(&orient.fvec.x, sizeof(float))) return;
        if (!reader.read_bytes(&orient.fvec.y, sizeof(float))) return;
        if (!reader.read_bytes(&orient.fvec.z, sizeof(float))) return;
        std::string script_name;
        if (!reader.read_string(script_name)) return;

        // Unlike the other alpine objects this orientation is handed straight to gr::setup_3d as
        // a view basis, so a corrupt one would produce a garbage projection rather than a wrongly
        // drawn marker.
        if (!alpine_orient_is_sane(orient)) {
            xlog::warn("[AlpineProjectionCamera] uid={} has a malformed orientation, using identity", uid);
            orient = rf::Matrix3{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
        }

        rf::Clutter* clutter = alpine_create_anchor_clutter(pos, orient, uid, script_name.c_str());
        if (!clutter) {
            xlog::warn("[AlpineProjectionCamera] Failed to create clutter for uid={}", uid);
            continue;
        }

        g_camera_handles.push_back(clutter->handle);
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
