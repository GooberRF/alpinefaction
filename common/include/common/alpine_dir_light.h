#pragma once

// Alpine Directional Light (RFL chunk 0x0AFBAE0C), shared by RED and the game.
// The runtime HLSL weight must stay a transcription of volume_inside_distance / volume_weight.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace alpine_dir_light
{

inline constexpr uint32_t chunk_id = 0x0AFBAE0Cu;

// ─── Limits and defaults ─────────────────────────────────────────────────────

inline constexpr uint32_t max_lights = 1024;
inline constexpr float max_intensity = 10.0f;
inline constexpr float max_extent = 10000.0f;
inline constexpr float max_feather = 10000.0f;
inline constexpr float max_spread = 45.0f;

inline constexpr float default_intensity = 1.0f;
inline constexpr float default_extent = 10.0f;

enum class Shape : uint8_t
{
    none = 0,
    box = 1,      // centred on pos, world-aligned after box_yaw; full width X / height Y / depth Z
    sphere = 2,   // radius extent_x
    cylinder = 3, // along the travel direction, centred on pos; radius extent_x, length extent_y
};
inline constexpr uint8_t shape_max = static_cast<uint8_t>(Shape::cylinder);

enum class MeshMode : uint8_t
{
    lightmap_scaled = 0, // scaled by the sampled lightmap luminance, as the sun is
    everywhere = 1,
};
inline constexpr uint8_t mesh_mode_max = static_cast<uint8_t>(MeshMode::everywhere);

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// Every field of a record but the script name, in wire order.
struct Record
{
    int32_t uid = -1;
    Vec3 pos{};
    Vec3 rvec{1.0f, 0.0f, 0.0f};
    Vec3 uvec{0.0f, 0.0f, 1.0f};
    Vec3 fvec{0.0f, -1.0f, 0.0f};
    uint8_t color_r = 255, color_g = 255, color_b = 255, color_a = 255;
    float intensity = default_intensity;
    uint8_t initially_on = 1;
    uint8_t shape = static_cast<uint8_t>(Shape::none);
    float extent_x = default_extent;
    float extent_y = default_extent;
    float extent_z = default_extent;
    float box_yaw = 0.0f;
    float feather = 0.0f;
    float spread = 0.0f;
    uint8_t cast_baked_shadows = 1;
    uint8_t liquid_occludes = 1;
    uint8_t sky_passes = 1;
    uint8_t outside_casts = 1;
    uint8_t affects_meshes = 1;
    uint8_t mesh_mode = static_cast<uint8_t>(MeshMode::lightmap_scaled);
    uint8_t always_show_range = 0; // editor display only
};

// One record in wire order, as the editor's directional_light_serialize_chunk writes it. Reader provides
// read(T&) and read_string(std::string&), as RflChunkReader does.
template<class Reader>
bool read_record(Reader& r, Record& rec, std::string& script_name)
{
    auto read_vec = [&](Vec3& v) { return r.read(v.x) && r.read(v.y) && r.read(v.z); };
    return r.read(rec.uid) && read_vec(rec.pos) && read_vec(rec.rvec) && read_vec(rec.uvec)
        && read_vec(rec.fvec) && r.read_string(script_name) && r.read(rec.color_r) && r.read(rec.color_g)
        && r.read(rec.color_b) && r.read(rec.color_a) && r.read(rec.intensity) && r.read(rec.initially_on)
        && r.read(rec.shape) && r.read(rec.extent_x) && r.read(rec.extent_y) && r.read(rec.extent_z)
        && r.read(rec.box_yaw) && r.read(rec.feather) && r.read(rec.spread) && r.read(rec.cast_baked_shadows)
        && r.read(rec.liquid_occludes) && r.read(rec.sky_passes) && r.read(rec.outside_casts)
        && r.read(rec.affects_meshes) && r.read(rec.mesh_mode) && r.read(rec.always_show_range);
}

// ─── Vector helpers ──────────────────────────────────────────────────────────

inline float dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline Vec3 sub(const Vec3& a, const Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 scale(const Vec3& a, float s)
{
    return {a.x * s, a.y * s, a.z * s};
}

inline bool is_finite(const Vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// ─── Sanitisation ────────────────────────────────────────────────────────────

inline float clamp_finite(float v, float lo, float hi, float fallback)
{
    return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
}

inline float normalize_degrees(float deg)
{
    if (!std::isfinite(deg)) {
        return 0.0f;
    }
    float r = std::fmod(deg, 360.0f);
    if (r < 0.0f) {
        r += 360.0f;
    }
    return r >= 360.0f ? 0.0f : r;
}

inline uint8_t sanitize_bool(uint8_t v, uint8_t fallback)
{
    return v > 1 ? fallback : v;
}

// An orthonormal basis (rvec = uvec x fvec, the RF convention) around the travel direction. A
// non-finite or near-zero fvec falls back to straight down; uvec is kept as far as it stays usable.
inline void sanitize_basis(Vec3& rvec, Vec3& uvec, Vec3& fvec)
{
    const float flen = is_finite(fvec) ? std::sqrt(dot(fvec, fvec)) : 0.0f;
    if (!(flen >= 1e-6f && std::isfinite(flen))) {
        fvec = {0.0f, -1.0f, 0.0f};
    }
    else {
        fvec = scale(fvec, 1.0f / flen);
    }

    Vec3 up = is_finite(uvec) ? sub(uvec, scale(fvec, dot(uvec, fvec))) : Vec3{};
    float ulen = std::sqrt(dot(up, up));
    if (!(ulen >= 1e-4f && std::isfinite(ulen))) {
        const Vec3 hint = std::fabs(fvec.y) > 0.99f ? Vec3{0.0f, 0.0f, 1.0f} : Vec3{0.0f, 1.0f, 0.0f};
        up = sub(hint, scale(fvec, dot(hint, fvec)));
        ulen = std::sqrt(dot(up, up));
    }
    uvec = scale(up, 1.0f / ulen);
    rvec = cross(uvec, fvec);
}

// Brings every field into range: non-finite floats and out of range enum/bool bytes take their
// defaults, the orientation is re-orthonormalised. True when anything changed.
inline bool sanitize_record(Record& rec)
{
    const Record in = rec;
    const Record def{};

    if (!is_finite(rec.pos)) {
        rec.pos = {std::isfinite(rec.pos.x) ? rec.pos.x : 0.0f, std::isfinite(rec.pos.y) ? rec.pos.y : 0.0f,
                   std::isfinite(rec.pos.z) ? rec.pos.z : 0.0f};
    }
    sanitize_basis(rec.rvec, rec.uvec, rec.fvec);
    rec.intensity = clamp_finite(rec.intensity, 0.0f, max_intensity, def.intensity);
    rec.initially_on = sanitize_bool(rec.initially_on, def.initially_on);
    if (rec.shape > shape_max) {
        rec.shape = def.shape;
    }
    rec.extent_x = clamp_finite(rec.extent_x, 0.0f, max_extent, def.extent_x);
    rec.extent_y = clamp_finite(rec.extent_y, 0.0f, max_extent, def.extent_y);
    rec.extent_z = clamp_finite(rec.extent_z, 0.0f, max_extent, def.extent_z);
    rec.box_yaw = normalize_degrees(rec.box_yaw);
    rec.feather = clamp_finite(rec.feather, 0.0f, max_feather, def.feather);
    rec.spread = clamp_finite(rec.spread, 0.0f, max_spread, def.spread);
    rec.cast_baked_shadows = sanitize_bool(rec.cast_baked_shadows, def.cast_baked_shadows);
    rec.liquid_occludes = sanitize_bool(rec.liquid_occludes, def.liquid_occludes);
    rec.sky_passes = sanitize_bool(rec.sky_passes, def.sky_passes);
    rec.outside_casts = sanitize_bool(rec.outside_casts, def.outside_casts);
    rec.affects_meshes = sanitize_bool(rec.affects_meshes, def.affects_meshes);
    if (rec.mesh_mode > mesh_mode_max) {
        rec.mesh_mode = def.mesh_mode;
    }
    rec.always_show_range = sanitize_bool(rec.always_show_range, def.always_show_range);

    auto same = [](float a, float b) { return a == b; }; // NaN in the input always reads as changed
    auto same3 = [&](const Vec3& a, const Vec3& b) { return same(a.x, b.x) && same(a.y, b.y) && same(a.z, b.z); };
    // Renormalising a unit vector may move its last bit, which is not a correction.
    const bool fvec_changed = !is_finite(in.fvec) || !(std::fabs(dot(in.fvec, rec.fvec) - 1.0f) <= 1e-3f);
    return !same3(in.pos, rec.pos) || fvec_changed || !same(in.intensity, rec.intensity)
        || in.initially_on != rec.initially_on || in.shape != rec.shape || !same(in.extent_x, rec.extent_x)
        || !same(in.extent_y, rec.extent_y) || !same(in.extent_z, rec.extent_z) || !same(in.box_yaw, rec.box_yaw)
        || !same(in.feather, rec.feather) || !same(in.spread, rec.spread)
        || in.cast_baked_shadows != rec.cast_baked_shadows || in.liquid_occludes != rec.liquid_occludes
        || in.sky_passes != rec.sky_passes || in.outside_casts != rec.outside_casts
        || in.affects_meshes != rec.affects_meshes || in.mesh_mode != rec.mesh_mode
        || in.always_show_range != rec.always_show_range;
}

// ─── Volume ──────────────────────────────────────────────────────────────────

// World directions of a box's local +X and +Z after box_yaw degrees about world Y (yaw 0 = world
// axes; yaw 90 turns local +Z onto world +X, the sun yaw convention).
inline void box_yaw_axes(float box_yaw_deg, Vec3& right, Vec3& forward)
{
    const float yaw = box_yaw_deg * (3.14159265358979f / 180.0f);
    const float c = std::cos(yaw);
    const float s = std::sin(yaw);
    right = {c, 0.0f, -s};
    forward = {s, 0.0f, c};
}

struct Volume
{
    Shape shape = Shape::none;
    Vec3 center{};
    Vec3 axis{0.0f, -1.0f, 0.0f}; // unit travel direction, every shape (the cylinder's axis)
    Vec3 box_right{1.0f, 0.0f, 0.0f};
    Vec3 box_forward{0.0f, 0.0f, 1.0f};
    float half_x = 0.0f; // box half extents
    float half_y = 0.0f;
    float half_z = 0.0f;
    float radius = 0.0f;      // sphere and cylinder
    float half_length = 0.0f; // cylinder
    float feather = 0.0f;
};

// `rec.fvec` must be unit length (a sanitized record).
inline Volume make_volume(const Record& rec)
{
    Volume v;
    v.shape = static_cast<Shape>(rec.shape);
    v.center = rec.pos;
    v.axis = rec.fvec;
    v.feather = rec.feather;
    switch (v.shape) {
    case Shape::box:
        box_yaw_axes(rec.box_yaw, v.box_right, v.box_forward);
        v.half_x = rec.extent_x * 0.5f;
        v.half_y = rec.extent_y * 0.5f;
        v.half_z = rec.extent_z * 0.5f;
        break;
    case Shape::sphere:
        v.radius = rec.extent_x;
        break;
    case Shape::cylinder:
        v.radius = rec.extent_x;
        v.half_length = rec.extent_y * 0.5f;
        break;
    case Shape::none:
        break;
    }
    return v;
}

// Distance from p to the nearest boundary, positive inside and <= 0 outside (outside it is only a
// sign, not a true distance). Unbounded: +infinity.
inline float volume_inside_distance(const Volume& v, const Vec3& p)
{
    const Vec3 d = sub(p, v.center);
    switch (v.shape) {
    case Shape::box: {
        const float lx = dot(d, v.box_right);
        const float lz = dot(d, v.box_forward);
        return std::min({v.half_x - std::fabs(lx), v.half_y - std::fabs(d.y), v.half_z - std::fabs(lz)});
    }
    case Shape::sphere:
        return v.radius - std::sqrt(dot(d, d));
    case Shape::cylinder: {
        const float t = dot(d, v.axis);
        const Vec3 radial = sub(d, scale(v.axis, t));
        return std::min(v.radius - std::sqrt(dot(radial, radial)), v.half_length - std::fabs(t));
    }
    case Shape::none:
        break;
    }
    return std::numeric_limits<float>::infinity();
}

// Light weight at p: clamp(inside_distance / feather, 0, 1); feather 0 is a hard edge (inside or on
// the boundary = 1). Unbounded = 1.
inline float volume_weight(const Volume& v, const Vec3& p)
{
    if (v.shape == Shape::none) {
        return 1.0f;
    }
    const float inside = volume_inside_distance(v, p);
    if (v.feather > 0.0f) {
        return std::clamp(inside / v.feather, 0.0f, 1.0f);
    }
    return inside >= 0.0f ? 1.0f : 0.0f;
}

// Radius of a sphere about the centre enclosing the volume; +infinity when unbounded.
inline float volume_bounding_radius(const Volume& v)
{
    switch (v.shape) {
    case Shape::box:
        return std::sqrt(v.half_x * v.half_x + v.half_y * v.half_y + v.half_z * v.half_z);
    case Shape::sphere:
        return v.radius;
    case Shape::cylinder:
        return std::sqrt(v.radius * v.radius + v.half_length * v.half_length);
    case Shape::none:
        break;
    }
    return std::numeric_limits<float>::infinity();
}

// Distance along the unit direction `dir` from `origin` to where the ray leaves the volume: +infinity
// when unbounded or it never leaves, 0 when origin is already outside.
inline float volume_ray_exit_distance(const Volume& v, const Vec3& origin, const Vec3& dir)
{
    constexpr float inf = std::numeric_limits<float>::infinity();
    constexpr float eps = 1e-12f;
    const Vec3 o = sub(origin, v.center);

    switch (v.shape) {
    case Shape::box: {
        const float ol[3] = {dot(o, v.box_right), o.y, dot(o, v.box_forward)};
        const float dl[3] = {dot(dir, v.box_right), dir.y, dot(dir, v.box_forward)};
        const float h[3] = {v.half_x, v.half_y, v.half_z};
        float t_exit = inf;
        for (int i = 0; i < 3; i++) {
            if (std::fabs(ol[i]) > h[i]) {
                return 0.0f;
            }
            if (std::fabs(dl[i]) > eps) {
                const float t = ((dl[i] > 0.0f ? h[i] : -h[i]) - ol[i]) / dl[i];
                t_exit = std::min(t_exit, t);
            }
        }
        return std::max(t_exit, 0.0f);
    }
    case Shape::sphere: {
        const float c = dot(o, o) - v.radius * v.radius;
        if (c > 0.0f) {
            return 0.0f;
        }
        const float b = dot(o, dir);
        const float disc = b * b - c;
        return std::max(-b + std::sqrt(std::max(disc, 0.0f)), 0.0f);
    }
    case Shape::cylinder: {
        const float s0 = dot(o, v.axis);
        const float sd = dot(dir, v.axis);
        const Vec3 op = sub(o, scale(v.axis, s0));
        const Vec3 dp = sub(dir, scale(v.axis, sd));
        const float cr = dot(op, op) - v.radius * v.radius;
        if (cr > 0.0f || std::fabs(s0) > v.half_length) {
            return 0.0f;
        }
        float t_side = inf;
        const float a = dot(dp, dp);
        if (a > eps) {
            const float b = dot(op, dp);
            const float disc = b * b - a * cr;
            t_side = (-b + std::sqrt(std::max(disc, 0.0f))) / a;
        }
        float t_cap = inf;
        if (std::fabs(sd) > eps) {
            t_cap = ((sd > 0.0f ? v.half_length : -v.half_length) - s0) / sd;
        }
        return std::max(std::min(t_side, t_cap), 0.0f);
    }
    case Shape::none:
        break;
    }
    return inf;
}

} // namespace alpine_dir_light
