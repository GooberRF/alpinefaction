#pragma once

// Catenary solver and uniform arc-length sampling for rope/cable geometry, shared by the game
// runtime and the editor preview. Y is up and gravity pulls toward -Y, matching both RF and RED.
// Deliberately free of engine types: callers convert their own vector type to and from Vec3.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rope_curve
{

struct Vec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

enum class CurveKind
{
    taut,     // rope no longer than the span: straight line between the anchors
    vertical, // horizontal span is ~0: rope folds straight down and back up
    catenary,
};

// Solved span. Everything sampling needs is here, so a solution can be cached and re-sampled.
struct Curve
{
    Vec3 a{};
    Vec3 b{};
    CurveKind kind = CurveKind::taut;
    float length = 0.0f; // arc length this curve actually represents
    float span = 0.0f;   // |b - a|

    // catenary frame
    Vec3 horiz_dir{};        // unit horizontal direction a -> b
    float horiz_dist = 0.0f;
    float param = 0.0f;      // 'a' in y = param*cosh((x - x0)/param) + c
    float u = 0.0f;          // horiz_dist / (2*param); curve parameter p runs over [0, u]
    float phi = 0.0f;        // asinh(dy / sqrt(L^2 - dy^2))

    // vertical fold
    float fold_bottom = 0.0f; // world y of the lowest point
};

// Anything past these is a level authoring mistake, not a shape worth reproducing.
inline constexpr float max_rope_length = 100000.0f;
inline constexpr float min_horiz_dist = 1.0e-4f;
inline constexpr float taut_epsilon = 1.0e-5f;

// ─── Authored value guards ──────────────────────────────────────────────────

// A non-finite serialized or typed value falls back instead of carrying a NaN through the clamp
// (every comparison against a NaN is false, so std::clamp would return it unchanged).
inline float clamp_finite(float v, float lo, float hi, float fallback)
{
    if (!std::isfinite(v)) {
        return fallback;
    }
    return std::clamp(v, lo, hi);
}

// Quantum for the dirty checks both sides run, in units of 1/1024 m: fine enough that no visible
// change slips past it, coarse enough that a sub-millimetre drag does not thrash the caches.
inline constexpr float quantum_1024 = 1024.0f;

inline int32_t quantize_1024(float v)
{
    if (!std::isfinite(v)) {
        return 0;
    }
    return static_cast<int32_t>(std::clamp(v * quantum_1024, -2.0e9f, 2.0e9f));
}

// ─── Rope shape limits ──────────────────────────────────────────────────────
// One derivation for the game parser and the editor dialog, so a value one side accepts is never
// one the other silently changes.

inline constexpr uint32_t rope_max_count = 10000;
inline constexpr std::size_t rope_max_script_name_len = 255;
inline constexpr int rope_min_segments = 2;
inline constexpr int rope_max_segments = 64;
inline constexpr float rope_min_thickness = 0.001f;
inline constexpr float rope_max_thickness = 10.0f;
inline constexpr float rope_max_slack = 1000.0f;
inline constexpr float rope_min_dangle = 0.01f;
inline constexpr float rope_max_dangle = 1000.0f;
inline constexpr float rope_min_weight = 0.01f;
inline constexpr float rope_max_weight = 100.0f;
inline constexpr float rope_max_uv_tiles = 1000.0f;
inline constexpr float rope_max_sway_amplitude = 100.0f;
inline constexpr float rope_max_sway_speed = 100.0f;

// sinh(u)/u, series-continued through u = 0 where the quotient is 0/0.
inline double sinh_over_u(double u)
{
    const double au = std::fabs(u);
    if (au < 1.0e-4) {
        const double u2 = u * u;
        return 1.0 + u2 / 6.0 + u2 * u2 / 120.0;
    }
    return std::sinh(u) / u;
}

// d/du of sinh(u)/u, same treatment.
inline double dsinh_over_u(double u)
{
    const double au = std::fabs(u);
    if (au < 1.0e-3) {
        return u / 3.0 + u * u * u / 30.0;
    }
    return (u * std::cosh(u) - std::sinh(u)) / (u * u);
}

// Solves sinh(u)/u = r (r > 1) for u > 0, which is the substitution u = d/(2a) of
// sqrt(L^2 - h^2) = 2a*sinh(d/(2a)). sinh(u)/u is strictly increasing from 1, so the root is
// unique and a bracket always exists; Newton is only ever allowed to step inside that bracket.
inline double solve_catenary_u(double r)
{
    constexpr double u_max = 40.0; // sinh(40)/40 ~ 2.9e15 slack-to-span ratio
    if (!(r > 1.0) || !std::isfinite(r)) {
        return 0.0;
    }
    double hi = 1.0;
    while (hi < u_max && sinh_over_u(hi) < r) {
        hi *= 2.0;
    }
    if (hi > u_max) {
        hi = u_max;
    }
    if (sinh_over_u(hi) < r) {
        return hi; // absurdly slack: clamp instead of chasing an exponential
    }

    double lo = 0.0;
    double u = 0.5 * (lo + hi);
    for (int i = 0; i < 80; ++i) {
        const double f = sinh_over_u(u) - r;
        if (f > 0.0) {
            hi = u;
        }
        else {
            lo = u;
        }
        const double d = dsinh_over_u(u);
        double next = (d > 0.0) ? u - f / d : 0.5 * (lo + hi);
        if (!std::isfinite(next) || !(next > lo) || !(next < hi)) {
            next = 0.5 * (lo + hi);
        }
        const double delta = std::fabs(next - u);
        u = next;
        if (delta <= 1.0e-12 * std::max(1.0, std::fabs(u))) {
            break;
        }
    }
    return u;
}

inline bool is_finite(const Vec3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

inline Vec3 lerp(const Vec3& a, const Vec3& b, float t)
{
    return Vec3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

// Solves the span a -> b for a rope of the given length. Every degenerate input resolves to a
// straight line rather than a NaN: non-finite anchors, non-positive or non-finite length, a rope
// shorter than the span, and a solver that fails to produce a usable parameter.
inline Curve solve(const Vec3& a, const Vec3& b_in, float rope_length)
{
    Curve c;

    if (!is_finite(a)) {
        c.a = Vec3{};
        c.b = Vec3{};
        return c;
    }
    // A non-finite far anchor degenerates to a point at the emitter rather than dragging both
    // ends of the rope to the origin.
    const Vec3 b = is_finite(b_in) ? b_in : a;
    c.a = a;
    c.b = b;

    const double dx = static_cast<double>(b.x) - a.x;
    const double dy = static_cast<double>(b.y) - a.y;
    const double dz = static_cast<double>(b.z) - a.z;
    const double horiz = std::sqrt(dx * dx + dz * dz);
    const double span = std::sqrt(dx * dx + dy * dy + dz * dz);
    c.span = static_cast<float>(span);

    double len = std::isfinite(rope_length) ? static_cast<double>(rope_length) : 0.0;
    len = std::clamp(len, 0.0, static_cast<double>(max_rope_length));

    if (!(len > span + taut_epsilon)) {
        c.kind = CurveKind::taut;
        c.length = static_cast<float>(span);
        return c;
    }
    c.length = static_cast<float>(len);

    if (horiz < min_horiz_dist) {
        // The rope has nowhere to bow sideways, so it folds: straight down to a low point and
        // straight back up. Arc lengths still add up to len.
        c.kind = CurveKind::vertical;
        c.fold_bottom = static_cast<float>(std::min(static_cast<double>(a.y), static_cast<double>(b.y))
                                           - (len - std::fabs(dy)) * 0.5);
        if (!std::isfinite(c.fold_bottom)) {
            c.kind = CurveKind::taut;
            c.length = static_cast<float>(span);
        }
        return c;
    }

    const double s = std::sqrt(std::max(len * len - dy * dy, 0.0));
    if (!(s > horiz)) {
        c.kind = CurveKind::taut;
        c.length = static_cast<float>(span);
        return c;
    }

    const double u = solve_catenary_u(s / horiz);
    const double param = (u > 0.0) ? horiz / (2.0 * u) : 0.0;
    const double phi = std::asinh(dy / s);
    if (!(param > 0.0) || !std::isfinite(param) || !std::isfinite(phi) || !std::isfinite(u)) {
        c.kind = CurveKind::taut;
        c.length = static_cast<float>(span);
        return c;
    }

    c.kind = CurveKind::catenary;
    c.horiz_dist = static_cast<float>(horiz);
    c.horiz_dir = Vec3{static_cast<float>(dx / horiz), 0.0f, static_cast<float>(dz / horiz)};
    c.param = static_cast<float>(param);
    c.u = static_cast<float>(u);
    c.phi = static_cast<float>(phi);
    return c;
}

// A rope with no far anchor hangs straight down its own length at rest.
inline Curve solve_dangle(const Vec3& a, float rope_length)
{
    const float len = (std::isfinite(rope_length) && rope_length > 0.0f)
                          ? std::min(rope_length, max_rope_length)
                          : 0.0f;
    return solve(a, Vec3{a.x, a.y - len, a.z}, len);
}

// Position at arc length s measured from `a`. s outside [0, length] is clamped, so a caller
// cannot walk the curve off its ends.
inline Vec3 sample_at_arclength(const Curve& c, float s)
{
    const float total = c.length;
    if (!(total > 0.0f)) {
        return c.a;
    }
    const double sc = std::clamp(static_cast<double>(s), 0.0, static_cast<double>(total));

    switch (c.kind) {
        case CurveKind::taut:
            return lerp(c.a, c.b, static_cast<float>(sc / total));

        case CurveKind::vertical: {
            const double down = static_cast<double>(c.a.y) - c.fold_bottom;
            const double y = (sc <= down) ? (static_cast<double>(c.a.y) - sc)
                                          : (static_cast<double>(c.fold_bottom) + (sc - down));
            const float t = static_cast<float>(sc / total);
            return Vec3{c.a.x + (c.b.x - c.a.x) * t, static_cast<float>(y),
                        c.a.z + (c.b.z - c.a.z) * t};
        }

        case CurveKind::catenary: {
            // Curve point p in [0, u]: x = 2*param*p, y = 2*param*sinh(p)*sinh(p - u + phi),
            // s = 2*param*sinh(p)*cosh(p - u + phi). Inverting s with
            // 2*cosh(A)*sinh(B) = sinh(A+B) - sinh(A-B) keeps every intermediate O(1) in the
            // exponent, which the plain cosh/sinh form does not for near-taut spans.
            const double param = c.param;
            const double u = c.u;
            const double phi = c.phi;
            const double inner = sc / param + std::sinh(phi - u);
            double p = 0.5 * (u - phi + std::asinh(inner));
            if (!std::isfinite(p)) {
                p = 0.0;
            }
            p = std::clamp(p, 0.0, u);
            const double q = 2.0 * param * std::sinh(p); // <= sqrt(L^2 - h^2), never overflows
            const double x = 2.0 * param * p;
            const double y = q * std::sinh(p - u + phi);
            const Vec3 out{static_cast<float>(c.a.x + c.horiz_dir.x * x),
                           static_cast<float>(c.a.y + y),
                           static_cast<float>(c.a.z + c.horiz_dir.z * x)};
            return is_finite(out) ? out : c.a;
        }
    }
    return c.a;
}

inline Vec3 sample(const Curve& c, float t)
{
    return sample_at_arclength(c, std::clamp(t, 0.0f, 1.0f) * c.length);
}

// Convenience for one-off sampling; re-solves on every call.
inline Vec3 catenary_sample(const Vec3& a, const Vec3& b, float rope_length, float t)
{
    return sample(solve(a, b, rope_length), t);
}

// Fills num_points positions spaced uniformly in arc length, plus the matching cumulative arc
// lengths (which is what uniform texture density along the rope needs). num_points < 2 is a no-op.
inline void build_polyline(const Curve& c, int num_points, std::vector<Vec3>& points,
                           std::vector<float>& arc)
{
    points.clear();
    arc.clear();
    if (num_points < 2) {
        return;
    }
    points.reserve(static_cast<std::size_t>(num_points));
    arc.reserve(static_cast<std::size_t>(num_points));
    const float total = c.length;
    const float step = total / static_cast<float>(num_points - 1);
    for (int i = 0; i < num_points; ++i) {
        const float s = (i == num_points - 1) ? total : step * static_cast<float>(i);
        points.push_back(sample_at_arclength(c, s));
        arc.push_back(s);
    }
}

// ─── Decoration placement ───────────────────────────────────────────────────
// One derivation for both sides: the tick markers RED draws and the meshes RF duplicates land on
// exactly the same arc positions because they come from this function.

enum class DecoSpacing
{
    fixed_count = 0,    // count instances spread evenly over the whole rope
    every_n_meters = 1, // one instance every `spacing` meters, as many as fit
    both_ends = 2,      // one instance at each anchor
    only_start = 3,     // one instance at the emitter end
    only_target = 4,    // one instance at the far end
};

inline constexpr int deco_spacing_mode_max = 4;

inline constexpr int deco_max_meshes = 6;
inline constexpr int deco_min_count = 1;
inline constexpr int deco_max_count = 64;
inline constexpr float deco_min_spacing = 0.05f;
inline constexpr float deco_max_spacing = 100.0f;
inline constexpr int deco_max_instances = 256;

// Arc positions of every instance, increasing. In the two spread modes instances sit at slot
// centres, so neither the first nor the last ever lands on an anchor; the three ends modes place
// single instances on the anchors themselves.
inline void build_decoration_arcs(float length, int mode, int count, float spacing,
                                  std::vector<float>& out)
{
    out.clear();
    if (!std::isfinite(length) || length <= 0.0f) {
        return;
    }

    if (mode == static_cast<int>(DecoSpacing::both_ends)) {
        out.push_back(0.0f);
        out.push_back(length);
        return;
    }
    if (mode == static_cast<int>(DecoSpacing::only_start)) {
        out.push_back(0.0f);
        return;
    }
    if (mode == static_cast<int>(DecoSpacing::only_target)) {
        out.push_back(length);
        return;
    }

    if (mode == static_cast<int>(DecoSpacing::every_n_meters)) {
        const float step = std::isfinite(spacing)
                               ? std::clamp(spacing, deco_min_spacing, deco_max_spacing)
                               : deco_min_spacing;
        for (int i = 0; i < deco_max_instances; ++i) {
            const float s = step * (static_cast<float>(i) + 0.5f);
            if (s > length) {
                break;
            }
            out.push_back(s);
        }
        return;
    }

    const int n = std::clamp(count, deco_min_count, deco_max_count);
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        out.push_back(length * (static_cast<float>(i) + 0.5f) / static_cast<float>(n));
    }
}

// ─── Decoration slot FX ─────────────────────────────────────────────────────
// One derivation of the per-slot FX limits for the game parser, the editor loader and the editor
// dialog, so a value one side accepts is never one the other silently changes.

inline constexpr uint8_t deco_slot_flag_glare = 0x1;
inline constexpr uint8_t deco_slot_flag_light = 0x2;
inline constexpr uint8_t deco_slot_flag_mask = 0x3;

inline constexpr float deco_fx_max_pos_offset = 100.0f;
inline constexpr float deco_fx_max_rot_offset = 360.0f;
inline constexpr float deco_fx_max_cone_angle = 360.0f;
inline constexpr float deco_fx_max_intensity = 1000.0f;
inline constexpr float deco_fx_max_radius_distance = 10000.0f;
inline constexpr float deco_fx_max_radius_scale = 1000.0f;
// Signed: the stock corona authoring range includes small negative diminish distances.
inline constexpr float deco_fx_max_diminish_distance = 100000.0f;
inline constexpr float deco_fx_max_volumetric = 1000.0f;
inline constexpr float deco_fx_min_light_radius = 0.1f;
inline constexpr float deco_fx_max_light_radius = 50.0f;
inline constexpr float deco_fx_max_light_intensity = 100.0f;

// The three glare shape fields are stock authoring FACTORS, not distances: effects.tbl's own field
// documentation gives 0.6 / 0.8 / -0.05 as the example set, every authored $Glares entry sits in
// 0.2..0.6 / 1.0..1.5 / -0.05, and a negative diminish distance is what stock means by "always
// visible". The same three numbers the editor's DedCorona defaults to.
// Stock $Glares entries author 90..180 here, and the value is PRE-factor degrees on both the wire
// and the dialog: the 0.5 the effects.tbl parser applies is applied at glare creation instead. A
// default of 0 would leave the render path attenuating at every view angle, so a mapper who ticks
// Glare and changes nothing gets a near-invisible glare.
inline constexpr float deco_fx_default_cone_angle = 90.0f;
inline constexpr float deco_fx_default_intensity = 1.0f;
inline constexpr float deco_fx_default_radius_distance = 0.6f;
inline constexpr float deco_fx_default_radius_scale = 0.8f;
inline constexpr float deco_fx_default_diminish_distance = -0.05f;
inline constexpr float deco_fx_default_light_radius = 5.0f;
inline constexpr float deco_fx_default_light_intensity = 1.0f;

} // namespace rope_curve
