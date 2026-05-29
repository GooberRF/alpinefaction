#include <vector>
#include <xlog/xlog.h>
#include "alpine_bag.h"

static std::vector<AlpineBagInfo> g_bag_objects;

void alpine_bag_load_chunk(rf::File& file, std::size_t chunk_len)
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

    uint32_t count = 0;
    if (!read_bytes(&count, sizeof(count))) {
        xlog::warn("[AlpineBag] Failed to read bag count from chunk (len={})", chunk_len);
        return;
    }
    if (count > 100) count = 100; // hard cap — we only ever use the first

    xlog::info("[AlpineBag] Loading {} bag object(s) from chunk (len={})", count, chunk_len);

    for (uint32_t i = 0; i < count; ++i) {
        AlpineBagInfo info;
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
        g_bag_objects.push_back(info);
    }

    if (read_error) {
        xlog::warn("[AlpineBag] Read error while parsing bag chunk; loaded {} entries", g_bag_objects.size());
    }
}

void alpine_bag_clear_state()
{
    g_bag_objects.clear();
}

std::optional<AlpineBagInfo> get_first_bag_object()
{
    if (g_bag_objects.empty()) return std::nullopt;
    return g_bag_objects.front();
}
