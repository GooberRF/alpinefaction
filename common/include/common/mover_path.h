#pragma once

// Mover path data (RFL chunk 0x0AFBAE0D) and the mover timing rules, shared by the game and RED so the editor preview
// plays what the game plays. Free of engine types: callers convert their own vectors to and from Vec3.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mover_path
{

inline constexpr uint32_t chunk_id = 0x0AFBAE0Du;

// ─── Limits and wire values ──────────────────────────────────────────────────

inline constexpr uint32_t max_records = 65536;
inline constexpr float max_handle = 100000.0f;

enum class SegmentMode : uint8_t
{
    straight = 0,
    curved = 1,
};

enum class TangentMode : uint8_t
{
    automatic = 0,
    aligned = 1,
    free = 2,
};

enum class Ease : uint8_t
{
    none = 0,
    in = 1,
    out = 2,
    in_out = 3,
};

enum class OrientMode : uint8_t
{
    none = 0,
    follow_path = 1,
    keyframe_rotation = 2,
};

// rf::MoverKeyframeMoveType, which RED stores as the same integers
enum class MoveType : int
{
    one_way = 1,
    ping_pong_once = 2,
    ping_pong_infinite = 3,
    loop_once = 4,
    loop_infinite = 5,
    lift = 6,
};

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b)
{
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 operator-(const Vec3& a, const Vec3& b)
{
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 operator*(const Vec3& a, float s)
{
    return {a.x * s, a.y * s, a.z * s};
}

inline float dot(const Vec3& a, const Vec3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 cross(const Vec3& a, const Vec3& b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float length(const Vec3& v)
{
    return std::sqrt(dot(v, v));
}

// Zero for a vector too short to have a direction
inline Vec3 normalized(const Vec3& v)
{
    const float len = length(v);
    return len > 1.0e-6f ? v * (1.0f / len) : Vec3{};
}

// What a keyframe adds to the stock record. in_handle and out_handle are offsets from the keyframe.
struct KeyframeData
{
    uint8_t segment_mode = static_cast<uint8_t>(SegmentMode::straight);
    uint8_t tangent_mode = static_cast<uint8_t>(TangentMode::automatic);
    uint8_t ease = static_cast<uint8_t>(Ease::none);
    Vec3 in_handle{};
    Vec3 out_handle{};
};

inline bool operator==(const Vec3& a, const Vec3& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

inline bool operator==(const KeyframeData& a, const KeyframeData& b)
{
    return a.segment_mode == b.segment_mode && a.tangent_mode == b.tangent_mode && a.ease == b.ease
        && a.in_handle == b.in_handle && a.out_handle == b.out_handle;
}

inline bool is_default(const KeyframeData& d)
{
    return d.segment_mode == static_cast<uint8_t>(SegmentMode::straight)
        && d.tangent_mode == static_cast<uint8_t>(TangentMode::automatic)
        && d.ease == static_cast<uint8_t>(Ease::none);
}

inline float sanitize_handle_axis(float v)
{
    return std::isfinite(v) ? std::clamp(v, -max_handle, max_handle) : 0.0f;
}

inline Vec3 sanitize_handle(const Vec3& v)
{
    return {sanitize_handle_axis(v.x), sanitize_handle_axis(v.y), sanitize_handle_axis(v.z)};
}

inline void sanitize(KeyframeData& d)
{
    if (d.segment_mode > static_cast<uint8_t>(SegmentMode::curved)) {
        d.segment_mode = static_cast<uint8_t>(SegmentMode::straight);
    }
    if (d.tangent_mode > static_cast<uint8_t>(TangentMode::free)) {
        d.tangent_mode = static_cast<uint8_t>(TangentMode::automatic);
    }
    if (d.ease > static_cast<uint8_t>(Ease::in_out)) {
        d.ease = static_cast<uint8_t>(Ease::none);
    }
    d.in_handle = sanitize_handle(d.in_handle);
    d.out_handle = sanitize_handle(d.out_handle);
}

inline uint8_t sanitize_orient_mode(uint8_t mode)
{
    return mode <= static_cast<uint8_t>(OrientMode::keyframe_rotation) ? mode
                                                                       : static_cast<uint8_t>(OrientMode::none);
}

// Reader provides read(T&), as RflChunkReader does.
template<class Reader>
bool read_keyframe_data(Reader& r, KeyframeData& d)
{
    uint8_t pad = 0;
    auto read_vec = [&](Vec3& v) { return r.read(v.x) && r.read(v.y) && r.read(v.z); };
    return r.read(d.segment_mode) && r.read(d.tangent_mode) && r.read(d.ease) && r.read(pad) && read_vec(d.in_handle)
        && read_vec(d.out_handle);
}

// Writer provides the rf::File write<T>(v) of each DLL.
template<class Writer>
void write_keyframe_data(Writer& w, const KeyframeData& d)
{
    w.template write<uint8_t>(d.segment_mode);
    w.template write<uint8_t>(d.tangent_mode);
    w.template write<uint8_t>(d.ease);
    w.template write<uint8_t>(0);
    for (const Vec3* v : {&d.in_handle, &d.out_handle}) {
        w.template write<float>(v->x);
        w.template write<float>(v->y);
        w.template write<float>(v->z);
    }
}

// Chunk body: u32 mover count, {i32 first keyframe uid, u8 orient mode, u8 pad[3]}, u32 keyframe count,
// {i32 keyframe uid, KeyframeData}. A .rfg stores the same body with keyframe indices in place of uids.
// on_mover(int32_t, uint8_t) and on_keyframe(int32_t, const KeyframeData&) take the sanitized records.
template<class Reader, class OnMover, class OnKeyframe>
bool read_chunk(Reader& r, OnMover&& on_mover, OnKeyframe&& on_keyframe)
{
    uint32_t mover_count = 0;
    if (!r.read(mover_count) || mover_count > max_records) {
        return false;
    }
    for (uint32_t i = 0; i < mover_count; i++) {
        int32_t first_keyframe = 0;
        uint8_t orient_mode = 0;
        uint8_t pad[3] = {};
        if (!r.read(first_keyframe) || !r.read(orient_mode) || !r.read(pad)) {
            return false;
        }
        on_mover(first_keyframe, sanitize_orient_mode(orient_mode));
    }

    uint32_t keyframe_count = 0;
    if (!r.read(keyframe_count) || keyframe_count > max_records) {
        return false;
    }
    for (uint32_t i = 0; i < keyframe_count; i++) {
        int32_t keyframe = 0;
        KeyframeData data;
        if (!r.read(keyframe) || !read_keyframe_data(r, data)) {
            return false;
        }
        sanitize(data);
        on_keyframe(keyframe, data);
    }
    return true;
}

// ─── Segments ────────────────────────────────────────────────────────────────

inline bool is_loop(MoveType type)
{
    return type == MoveType::loop_once || type == MoveType::loop_infinite;
}

// Only a loop of three or more keyframes has a closing segment of its own: a two keyframe loop goes back along its
// one segment, as does anything that bounces. Stock keeps no direction with a door's bounce, so the indices decide.
inline bool has_closing_segment(MoveType type, int count)
{
    return is_loop(type) && count > 2;
}

// The keyframe whose data shapes the segment between `from` and `to`, or -1 if they are not neighbours. The segment
// after a keyframe belongs to it in both directions, and the last keyframe owns a loop's closing segment.
inline int segment_owner(int from, int to, int count, bool closing_segment)
{
    if (to == from + 1) {
        return from;
    }
    if (from == to + 1) {
        return to;
    }
    if (closing_segment && ((from == count - 1 && to == 0) || (from == 0 && to == count - 1))) {
        return count - 1;
    }
    return -1;
}

struct Key
{
    Vec3 pos{};
    KeyframeData data{};
};

// Handle lengths are a third of the neighbouring span, and a curve meeting a straight segment keeps that segment's
// direction so the join has no corner.
inline void resolve_handles(std::vector<Key>& keys, bool closing_segment)
{
    const int count = static_cast<int>(keys.size());
    auto curved = [&](int owner) {
        return keys[owner].data.segment_mode == static_cast<uint8_t>(SegmentMode::curved);
    };
    for (int i = 0; i < count; i++) {
        Key& key = keys[i];
        if (key.data.tangent_mode != static_cast<uint8_t>(TangentMode::automatic)) {
            continue;
        }
        const bool has_prev = i > 0 || closing_segment;
        const bool has_next = i < count - 1 || closing_segment;
        const int prev = (i + count - 1) % count;
        const int next = (i + 1) % count;
        const Vec3 to_prev = has_prev ? keys[prev].pos - key.pos : Vec3{};
        const Vec3 to_next = has_next ? keys[next].pos - key.pos : Vec3{};
        const bool prev_curved = has_prev && curved(prev);
        const bool next_curved = has_next && curved(i);

        Vec3 dir{};
        if (prev_curved && next_curved) {
            dir = normalized(to_next - to_prev);
        }
        else if (next_curved) {
            dir = normalized(has_prev ? to_prev * -1.0f : to_next);
        }
        else if (prev_curved) {
            dir = normalized(has_next ? to_next : to_prev * -1.0f);
        }
        key.data.out_handle = dir * (length(to_next) / 3.0f);
        key.data.in_handle = dir * (length(to_prev) / -3.0f);
    }
}

// ─── Curves ──────────────────────────────────────────────────────────────────

inline constexpr int arc_table_steps = 32;

struct Curve
{
    Vec3 p0{};
    Vec3 p1{};
    Vec3 p2{};
    Vec3 p3{};
    float length = 0.0f;
    std::array<float, arc_table_steps + 1> arc{};
};

inline Vec3 sample(const Curve& c, float t)
{
    const float u = 1.0f - t;
    const float b0 = u * u * u;
    const float b1 = 3.0f * u * u * t;
    const float b2 = 3.0f * u * t * t;
    const float b3 = t * t * t;
    return {b0 * c.p0.x + b1 * c.p1.x + b2 * c.p2.x + b3 * c.p3.x,
            b0 * c.p0.y + b1 * c.p1.y + b2 * c.p2.y + b3 * c.p3.y,
            b0 * c.p0.z + b1 * c.p1.z + b2 * c.p2.z + b3 * c.p3.z};
}

inline Vec3 derivative(const Curve& c, float t)
{
    const float u = 1.0f - t;
    const Vec3 a = c.p1 - c.p0;
    const Vec3 b = c.p2 - c.p1;
    const Vec3 d = c.p3 - c.p2;
    return (a * (u * u) + b * (2.0f * u * t) + d * (t * t)) * 3.0f;
}

inline Curve make_curve(const Vec3& a, const Vec3& a_out, const Vec3& b, const Vec3& b_in)
{
    Curve c;
    c.p0 = a;
    c.p1 = a + a_out;
    c.p2 = b + b_in;
    c.p3 = b;
    Vec3 prev = a;
    float total = 0.0f;
    for (int i = 1; i <= arc_table_steps; i++) {
        const Vec3 p = sample(c, static_cast<float>(i) / static_cast<float>(arc_table_steps));
        total += mover_path::length(p - prev);
        c.arc[i] = total;
        prev = p;
    }
    c.length = total;
    return c;
}

inline Curve make_segment_curve(const std::vector<Key>& keys, int owner)
{
    const Key& a = keys[owner];
    const Key& b = keys[(owner + 1) % static_cast<int>(keys.size())];
    return make_curve(a.pos, a.data.out_handle, b.pos, b.data.in_handle);
}

inline float param_at_fraction(const Curve& c, float fraction)
{
    if (!(c.length > 0.0f)) {
        return std::clamp(fraction, 0.0f, 1.0f);
    }
    const float target = std::clamp(fraction, 0.0f, 1.0f) * c.length;
    const auto hi = std::lower_bound(c.arc.begin() + 1, c.arc.end() - 1, target);
    const int step = static_cast<int>(hi - c.arc.begin());
    const float start = c.arc[step - 1];
    const float span = c.arc[step] - start;
    const float within = span > 0.0f ? (target - start) / span : 0.0f;
    return (static_cast<float>(step - 1) + within) / static_cast<float>(arc_table_steps);
}

inline float directed_param(const Curve& c, float fraction, bool reversed)
{
    return param_at_fraction(c, reversed ? 1.0f - fraction : fraction);
}

// Where a mover is along its path: `fraction` of the way from `owner` to the keyframe after it. A mover at rest
// (to < 0) sits at the start of its keyframe's segment, or the end of the last one. owner is -1 for a jump between
// keyframes that are not neighbours.
struct PathSpot
{
    int owner = -1;
    float fraction = 0.0f;
};

inline PathSpot path_spot(int from, int to, float s, int count, bool closing_segment)
{
    if (to >= 0) {
        const int owner = segment_owner(from, to, count, closing_segment);
        return {owner, owner == from ? s : 1.0f - s};
    }
    if (from < count - 1 || closing_segment) {
        return {from, 0.0f};
    }
    return {from - 1, 1.0f};
}

// Always from the segment's owner to the keyframe after it. The ends of a curve with a zero length handle have no
// derivative, so the chord stands in.
inline Vec3 curve_direction(const Curve& c, float fraction)
{
    const Vec3 d = normalized(derivative(c, param_at_fraction(c, fraction)));
    return d == Vec3{} ? normalized(c.p3 - c.p0) : d;
}

// A mover backs along its path rather than turning round to reverse.
template<class PosFn>
inline Vec3 follow_direction(const PathSpot& spot, int from, int to, int count, const Curve* curve, PosFn&& pos)
{
    if (spot.owner < 0) {
        const Vec3 jump = normalized(pos(to) - pos(from));
        if (!(jump == Vec3{})) {
            return jump;
        }
    }
    if (curve) {
        const Vec3 d = curve_direction(*curve, spot.fraction);
        if (!(d == Vec3{})) {
            return d;
        }
    }
    // Coincident keyframes have no direction of their own: the path's next one stands in.
    const int first = spot.owner < 0 ? from : spot.owner;
    for (int i = 0; i < count; i++) {
        const int a = (first + i) % count;
        const Vec3 chord = normalized(pos((a + 1) % count) - pos(a));
        if (!(chord == Vec3{})) {
            return chord;
        }
    }
    return {};
}

// ─── Orientation ─────────────────────────────────────────────────────────────

struct Mat3
{
    Vec3 rvec{1.0f, 0.0f, 0.0f};
    Vec3 uvec{0.0f, 1.0f, 0.0f};
    Vec3 fvec{0.0f, 0.0f, 1.0f};
};

// Straight up or down there is no heading to keep, so +Z stands in for up.
inline Mat3 facing(const Vec3& dir)
{
    const Vec3 f = normalized(dir);
    if (f == Vec3{}) {
        return {};
    }
    Vec3 r = normalized(cross({0.0f, 1.0f, 0.0f}, f));
    if (r == Vec3{}) {
        r = normalized(cross({0.0f, 0.0f, 1.0f}, f));
    }
    return {r, cross(f, r), f};
}

inline Vec3 carry(const Mat3& from, const Mat3& to, const Vec3& v)
{
    const float x = dot(v, from.rvec);
    const float y = dot(v, from.uvec);
    const float z = dot(v, from.fvec);
    return to.rvec * x + to.uvec * y + to.fvec * z;
}

inline Mat3 carry(const Mat3& from, const Mat3& to, const Mat3& m)
{
    return {carry(from, to, m.rvec), carry(from, to, m.uvec), carry(from, to, m.fvec)};
}

struct Quat
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

inline Quat normalized(const Quat& q)
{
    const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return len > 1.0e-6f ? Quat{q.x / len, q.y / len, q.z / len, q.w / len} : Quat{};
}

inline Quat to_quat(const Mat3& m)
{
    const float m00 = m.rvec.x;
    const float m01 = m.rvec.y;
    const float m02 = m.rvec.z;
    const float m10 = m.uvec.x;
    const float m11 = m.uvec.y;
    const float m12 = m.uvec.z;
    const float m20 = m.fvec.x;
    const float m21 = m.fvec.y;
    const float m22 = m.fvec.z;
    const float trace = m00 + m11 + m22;
    Quat q;
    if (trace > 0.0f) {
        const float d = 2.0f * std::sqrt(1.0f + trace);
        q = {(m21 - m12) / d, (m02 - m20) / d, (m10 - m01) / d, 0.25f * d};
    }
    else if (m00 > m11 && m00 > m22) {
        const float d = 2.0f * std::sqrt(std::max(1.0f + m00 - m11 - m22, 0.0f));
        q = {0.25f * d, (m01 + m10) / d, (m02 + m20) / d, (m21 - m12) / d};
    }
    else if (m11 > m22) {
        const float d = 2.0f * std::sqrt(std::max(1.0f - m00 + m11 - m22, 0.0f));
        q = {(m01 + m10) / d, 0.25f * d, (m12 + m21) / d, (m02 - m20) / d};
    }
    else {
        const float d = 2.0f * std::sqrt(std::max(1.0f - m00 - m11 + m22, 0.0f));
        q = {(m02 + m20) / d, (m12 + m21) / d, 0.25f * d, (m10 - m01) / d};
    }
    return normalized(q);
}

inline Mat3 to_mat(const Quat& q)
{
    const float xx = q.x * q.x;
    const float yy = q.y * q.y;
    const float zz = q.z * q.z;
    const float xy = q.x * q.y;
    const float xz = q.x * q.z;
    const float yz = q.y * q.z;
    const float wx = q.w * q.x;
    const float wy = q.w * q.y;
    const float wz = q.w * q.z;
    return {{1.0f - 2.0f * (yy + zz), 2.0f * (xy - wz), 2.0f * (xz + wy)},
            {2.0f * (xy + wz), 1.0f - 2.0f * (xx + zz), 2.0f * (yz - wx)},
            {2.0f * (xz - wy), 2.0f * (yz + wx), 1.0f - 2.0f * (xx + yy)}};
}

inline Mat3 slerp(const Mat3& a, const Mat3& b, float s)
{
    const Quat qa = to_quat(a);
    Quat qb = to_quat(b);
    float cosine = qa.x * qb.x + qa.y * qb.y + qa.z * qb.z + qa.w * qb.w;
    if (cosine < 0.0f) {
        cosine = -cosine;
        qb = {-qb.x, -qb.y, -qb.z, -qb.w};
    }
    float ka = 1.0f - s;
    float kb = s;
    if (cosine < 0.9995f) {
        const float angle = std::acos(cosine);
        const float sine = std::sin(angle);
        ka = std::sin((1.0f - s) * angle) / sine;
        kb = std::sin(s * angle) / sine;
    }
    return to_mat(normalized(Quat{ka * qa.x + kb * qb.x, ka * qa.y + kb * qb.y, ka * qa.z + kb * qb.z,
                                  ka * qa.w + kb * qb.w}));
}

// ─── Timing ──────────────────────────────────────────────────────────────────

// `travel_param` is a speed if time_is_velocity is set
inline float travel_seconds(float travel_param, float dist, bool time_is_velocity)
{
    constexpr float fallback = 0.001f;
    if (!(travel_param > 0.0f)) {
        return fallback;
    }
    if (time_is_velocity) {
        return dist > 0.0f ? dist / travel_param : fallback;
    }
    return travel_param;
}

// The game raises a travel time to cover its ramps when it loads the level, unless it is a speed
inline float effective_travel_param(float travel_param, float accel, float decel, bool time_is_velocity)
{
    return time_is_velocity ? travel_param : std::max(travel_param, accel + decel);
}

inline void fit_ramps(float travel, bool time_is_velocity, float& accel, float& decel)
{
    if (!time_is_velocity || !(travel > 0.0f)) {
        return;
    }
    const float ramp_sum = accel + decel;
    const float max_ramp_sum = 0.9f * travel;
    if (ramp_sum > 0.0f && ramp_sum > max_ramp_sum) {
        const float scale = max_ramp_sum / ramp_sum;
        accel *= scale;
        decel *= scale;
    }
}

// out_s = [0-1] progress
// out_v_norm = normalized speed, integral over [0,T] == 1
inline void trapezoid(float t, float T, float accel, float decel, float& out_s, float& out_v_norm)
{
    if (T <= 0.0f) {
        out_s = 1.0f;
        out_v_norm = 0.0f;
        return;
    }
    if (t <= 0.0f) {
        out_s = 0.0f;
        out_v_norm = 0.0f;
        return;
    }
    if (t >= T) {
        out_s = 1.0f;
        out_v_norm = 0.0f;
        return;
    }

    if (accel <= 0.0f && decel <= 0.0f) {
        out_s = t / T;
        out_v_norm = 1.0f / T;
        return;
    }

    const float v_max_denom = T - 0.5f * (accel + decel);

    if (v_max_denom <= 0.0f) {
        out_s = t / T;
        out_v_norm = 1.0f / T;
        return;
    }

    const float v_max = 1.0f / v_max_denom;
    const float cruise_end = T - decel;
    const float area_accel = 0.5f * v_max * accel;

    if (t < accel) {
        const float a = v_max / accel;
        out_v_norm = a * t;
        out_s = 0.5f * a * t * t;
        return;
    }
    if (t < cruise_end) {
        out_v_norm = v_max;
        out_s = area_accel + v_max * (t - accel);
        return;
    }

    const float u = T - t;
    const float a = v_max / decel;
    out_v_norm = a * u;

    const float area_tail = 0.5f * v_max * (u * u / decel);
    out_s = 1.0f - area_tail;
}

inline void apply_ease(uint8_t ease, float& s, float& v_norm)
{
    const float x = s;
    switch (static_cast<Ease>(ease)) {
        case Ease::in:
            s = x * x;
            v_norm *= 2.0f * x;
            break;
        case Ease::out:
            s = x * (2.0f - x);
            v_norm *= 2.0f * (1.0f - x);
            break;
        case Ease::in_out:
            s = x * x * (3.0f - 2.0f * x);
            v_norm *= 6.0f * x * (1.0f - x);
            break;
        default:
            break;
    }
}

inline float segment_progress(float t, float T, float accel, float decel, uint8_t ease)
{
    float s = 0.0f;
    float v_norm = 0.0f;
    trapezoid(t, T, accel, decel, s, v_norm);
    apply_ease(ease, s, v_norm);
    return s;
}

inline float time_at_progress(float target, float T, float accel, float decel, uint8_t ease)
{
    float lo = 0.0f;
    float hi = T;
    for (int i = 0; i < 24; i++) {
        const float mid = 0.5f * (lo + hi);
        if (segment_progress(mid, T, accel, decel, ease) < target) {
            lo = mid;
        }
        else {
            hi = mid;
        }
    }
    return 0.5f * (lo + hi);
}

// ─── Following the keyframes ─────────────────────────────────────────────────

struct NextStep
{
    int next = -1;                     // -1: stop until activated again
    bool forward = true;               // the direction the mover keeps
    bool clear_stop_completely = false;
};

inline NextStep next_keyframe(MoveType type, int count, int from, int cur, bool forward)
{
    const int first = 0;
    const int last = count - 1;
    NextStep step;
    step.forward = forward;

    switch (type) {
        case MoveType::one_way:
            if (forward) {
                if (cur < last) {
                    step.next = cur + 1;
                }
                else {
                    step.forward = false;
                }
            }
            else {
                if (cur > first) {
                    step.next = cur - 1;
                }
                else {
                    step.forward = true;
                }
            }
            break;

        case MoveType::ping_pong_once:
            if (forward) {
                if (cur < last) {
                    step.next = cur + 1;
                }
                else {
                    step.forward = false;
                    if (count > 1) {
                        step.next = cur - 1;
                    }
                }
            }
            else {
                if (cur > first) {
                    step.next = cur - 1;
                }
                else {
                    step.forward = true;
                    step.clear_stop_completely = true;
                }
            }
            break;

        case MoveType::ping_pong_infinite:
            if (forward) {
                if (cur < last) {
                    step.next = cur + 1;
                }
                else {
                    step.forward = false;
                    if (count > 1) {
                        step.next = cur - 1;
                    }
                }
            }
            else {
                if (cur > first) {
                    step.next = cur - 1;
                }
                else {
                    step.forward = true;
                    if (count > 1) {
                        step.next = cur + 1;
                    }
                }
            }
            break;

        case MoveType::loop_once:
            if (forward) {
                if (from != last || cur != first) {
                    step.next = (cur < last) ? (cur + 1) : first;
                }
            }
            else {
                if (from != first || cur != last) {
                    step.next = (cur > first) ? (cur - 1) : last;
                }
            }
            break;

        case MoveType::loop_infinite:
            if (forward) {
                step.next = (cur < last) ? (cur + 1) : first;
            }
            else {
                step.next = (cur > first) ? (cur - 1) : last;
            }
            break;

        case MoveType::lift:
            if (cur == last) {
                step.forward = false;
            }
            else if (cur == first) {
                step.forward = true;
            }
            break;
    }
    return step;
}

} // namespace mover_path
