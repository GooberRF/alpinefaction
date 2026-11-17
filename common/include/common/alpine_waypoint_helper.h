#pragma once

// Alpine Waypoint Helper (RFL chunk 0x0AFBAE0D), shared by RED and the game: a mapper-placed navigation point for
// vehicle bots, joined to other helpers through its object links.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace alpine_waypoint_helper
{

inline constexpr uint32_t chunk_id = 0x0AFBAE0Du;

inline constexpr uint32_t max_helpers = 4096;
inline constexpr uint32_t max_links = 64; // per helper; extra links on the wire are read and dropped

inline constexpr float min_radius = 0.5f;
inline constexpr float max_radius = 64.0f;
inline constexpr float default_radius = 4.0f;

// Append-only on the wire.
enum class Type : uint8_t
{
    air = 0,
    water = 1,
    ground = 2,
};
inline constexpr uint8_t type_max = static_cast<uint8_t>(Type::ground);

// Ground helpers: the largest hull the road takes, so driller admits every ground vehicle. Append-only on the wire.
enum class SizeClass : uint8_t
{
    jeep = 0,
    apc = 1,
    driller = 2,
};
inline constexpr uint8_t size_class_max = static_cast<uint8_t>(SizeClass::driller);

inline constexpr uint8_t flag_landing = 0x1; // a spot to set down and get out

struct Record
{
    int32_t uid = -1;
    float pos_x = 0.0f;
    float pos_y = 0.0f;
    float pos_z = 0.0f;
    uint8_t type = static_cast<uint8_t>(Type::air);
    uint8_t flags = 0;
    uint8_t size_class = static_cast<uint8_t>(SizeClass::driller);
    float radius = default_radius;
    std::vector<int32_t> links;
};

// Wire order: uid, pos, type, flags, size_class, radius, u32 link count, that many link uids. Writer provides
// write<T>(T), as each DLL's rf::File does.
template<class Writer>
void write_record(Writer& w, const Record& rec)
{
    w.template write<int32_t>(rec.uid);
    w.template write<float>(rec.pos_x);
    w.template write<float>(rec.pos_y);
    w.template write<float>(rec.pos_z);
    w.template write<uint8_t>(rec.type);
    w.template write<uint8_t>(rec.flags);
    w.template write<uint8_t>(rec.size_class);
    w.template write<float>(rec.radius);
    const auto count = static_cast<uint32_t>(std::min<std::size_t>(rec.links.size(), max_links));
    w.template write<uint32_t>(count);
    for (uint32_t i = 0; i < count; i++) {
        w.template write<int32_t>(rec.links[i]);
    }
}

// Reader provides read(T&), as RflChunkReader does. False on a truncated record. The kept links are capped, so a
// corrupt count costs reads, never memory.
template<class Reader>
bool read_record(Reader& r, Record& rec)
{
    uint32_t count = 0;
    if (!(r.read(rec.uid) && r.read(rec.pos_x) && r.read(rec.pos_y) && r.read(rec.pos_z) && r.read(rec.type)
          && r.read(rec.flags) && r.read(rec.size_class) && r.read(rec.radius) && r.read(count))) {
        return false;
    }
    rec.links.clear();
    for (uint32_t i = 0; i < count; i++) {
        int32_t link = -1;
        if (!r.read(link)) {
            return false;
        }
        if (rec.links.size() < max_links) {
            rec.links.push_back(link);
        }
    }
    return true;
}

inline float sanitize_radius(float radius)
{
    return std::isfinite(radius) ? std::clamp(radius, min_radius, max_radius) : default_radius;
}

inline bool pos_is_finite(const Record& rec)
{
    return std::isfinite(rec.pos_x) && std::isfinite(rec.pos_y) && std::isfinite(rec.pos_z);
}

} // namespace alpine_waypoint_helper
