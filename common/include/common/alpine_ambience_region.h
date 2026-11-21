#pragma once

// Alpine Ambience Region (RFL chunk 0x0AFBAE10), shared by RED and the game: a box or sphere that plays a looping,
// non-positional sound while the listener is inside it.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <common/utils/math-utils.h>

namespace alpine_ambience_region
{

inline constexpr uint32_t chunk_id = 0x0AFBAE10u;

inline constexpr uint32_t max_regions = 1024;
// rf::Sound::filename is char[32], and the engine copies the name into it unbounded.
inline constexpr std::size_t max_sound_name = 31;

inline constexpr float max_dim = 10000.0f;
inline constexpr float max_fade_time = 30.0f;

inline constexpr float default_size = 10.0f;
inline constexpr float default_radius = 5.0f;
inline constexpr float default_volume = 1.0f;
inline constexpr float default_fade_distance = 2.0f;
inline constexpr float default_fade_time = 1.0f;

// Append-only on the wire.
enum class Shape : uint8_t
{
    box = 0,
    sphere = 1,
};
inline constexpr uint8_t shape_max = static_cast<uint8_t>(Shape::sphere);

inline constexpr uint8_t flag_initially_enabled = 0x1;
inline constexpr uint8_t flag_always_show_range = 0x2; // editor only

struct Record
{
    int32_t uid = -1;
    float pos[3] = {};
    float orient[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f}; // rvec, uvec, fvec
    std::string script_name;
    std::string sound;
    uint8_t shape = static_cast<uint8_t>(Shape::box);
    // Full box size; both shapes are always written so the record stays fixed in layout.
    float width = default_size;
    float height = default_size;
    float depth = default_size;
    float radius = default_radius;
    float volume = default_volume;
    float fade_distance = default_fade_distance; // inside the edge, from silent to full volume
    float fade_time = default_fade_time;         // seconds from silent to full volume
    uint8_t flags = flag_initially_enabled;
};

// Writer provides write<T>(T) and write(const void*, size), as RED's rf::File does.
template<class Writer>
void write_record(Writer& w, const Record& rec)
{
    auto write_string = [&w](const std::string& s) {
        const auto len = static_cast<uint16_t>(std::min<std::size_t>(s.size(), UINT16_MAX));
        w.template write<uint16_t>(len);
        if (len > 0) {
            w.write(s.data(), len);
        }
    };
    w.template write<int32_t>(rec.uid);
    for (float v : rec.pos) {
        w.template write<float>(v);
    }
    for (float v : rec.orient) {
        w.template write<float>(v);
    }
    write_string(rec.script_name);
    write_string(rec.sound);
    w.template write<uint8_t>(rec.shape);
    w.template write<float>(rec.width);
    w.template write<float>(rec.height);
    w.template write<float>(rec.depth);
    w.template write<float>(rec.radius);
    w.template write<float>(rec.volume);
    w.template write<float>(rec.fade_distance);
    w.template write<float>(rec.fade_time);
    w.template write<uint8_t>(rec.flags);
}

// Reader provides read(T&) and read_string(std::string&), as RflChunkReader does. False on a truncated record.
template<class Reader>
bool read_record(Reader& r, Record& rec)
{
    if (!r.read(rec.uid)) {
        return false;
    }
    for (float& v : rec.pos) {
        if (!r.read(v)) return false;
    }
    for (float& v : rec.orient) {
        if (!r.read(v)) return false;
    }
    return r.read_string(rec.script_name) && r.read_string(rec.sound) && r.read(rec.shape) && r.read(rec.width)
        && r.read(rec.height) && r.read(rec.depth) && r.read(rec.radius) && r.read(rec.volume)
        && r.read(rec.fade_distance) && r.read(rec.fade_time) && r.read(rec.flags);
}

inline bool sound_name_valid(std::string_view name)
{
    return !name.empty() && name.size() <= max_sound_name && name.find_first_of("\\/:") == std::string_view::npos;
}

template<std::size_t N>
bool all_finite(const float (&values)[N])
{
    return std::all_of(std::begin(values), std::end(values), [](float v) { return std::isfinite(v); });
}

inline float sanitize_dim(float value, float fallback)
{
    return clamp_finite(value, 0.0f, max_dim, fallback);
}

inline float sanitize_volume(float value)
{
    return clamp_finite(value, 0.0f, 1.0f, default_volume);
}

inline float sanitize_fade_distance(float value)
{
    return clamp_finite(value, 0.0f, max_dim, default_fade_distance);
}

inline float sanitize_fade_time(float value)
{
    return clamp_finite(value, 0.0f, max_fade_time, default_fade_time);
}

} // namespace alpine_ambience_region
