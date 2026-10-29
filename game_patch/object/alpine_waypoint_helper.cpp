#include <algorithm>
#include <unordered_set>
#include <vector>
#include <xlog/xlog.h>
#include "../misc/level.h"
#include "alpine_waypoint_helper.h"

namespace awh = alpine_waypoint_helper;

namespace
{

std::vector<AlpineWaypointHelper> g_helpers;
int g_helpers_serial = 0;

} // namespace

// Must stay in step with the editor's waypoint_helper_serialize_chunk (alpine_waypoint_helper::write_record).
void alpine_waypoint_helper_load_chunk(rf::File& file, std::size_t chunk_len)
{
    std::size_t remaining = chunk_len;
    rf::File::ChunkGuard chunk_guard{file, remaining};
    AlpineChunkReader reader{file, remaining};

    uint32_t count = 0;
    if (!reader.read(count)) {
        xlog::warn("[WaypointHelper] failed to read the helper count (chunk len {})", chunk_len);
        return;
    }
    count = std::min(count, awh::max_helpers);

    std::unordered_set<int32_t> uids;
    for (const AlpineWaypointHelper& h : g_helpers) {
        uids.insert(h.uid);
    }

    int rejected = 0;
    for (uint32_t i = 0; i < count; ++i) {
        awh::Record rec;
        if (!awh::read_record(reader, rec)) {
            xlog::warn("[WaypointHelper] chunk truncated after {} of {} helpers", i, count);
            break;
        }
        // The whole record is read before any rejection so the stream stays in step. An unknown type is a newer
        // level's: navigation has no use for it.
        if (!awh::pos_is_finite(rec) || rec.type > awh::type_max || rec.uid == -1 || !uids.insert(rec.uid).second
            || g_helpers.size() >= awh::max_helpers) {
            ++rejected;
            continue;
        }

        AlpineWaypointHelper h;
        h.uid = rec.uid;
        h.pos = rf::Vector3{rec.pos_x, rec.pos_y, rec.pos_z};
        h.type = static_cast<awh::Type>(rec.type);
        h.flags = rec.flags;
        h.size_class = static_cast<awh::SizeClass>(std::min(rec.size_class, awh::size_class_max));
        h.radius = awh::sanitize_radius(rec.radius);
        for (const int32_t link : rec.links) {
            if (link != rec.uid && std::find(h.links.begin(), h.links.end(), link) == h.links.end()) {
                h.links.push_back(link);
            }
        }
        g_helpers.push_back(std::move(h));
    }

    ++g_helpers_serial;
    if (rejected > 0) {
        xlog::warn("[WaypointHelper] rejected {} helper record(s): non-finite position, unknown type, uid -1, "
                   "duplicate uid or past the {} helper limit", rejected, awh::max_helpers);
    }
    xlog::debug("[WaypointHelper] {} helper(s) loaded", g_helpers.size());
}

void alpine_waypoint_helper_clear_state()
{
    g_helpers.clear();
    ++g_helpers_serial;
}

const std::vector<AlpineWaypointHelper>& alpine_waypoint_helpers()
{
    return g_helpers;
}

int alpine_waypoint_helpers_serial()
{
    return g_helpers_serial;
}
