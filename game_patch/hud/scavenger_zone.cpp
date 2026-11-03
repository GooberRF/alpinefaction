#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include "scavenger_zone.h"
#include "hud_world.h"
#include "../bmpman/bmpman.h"
#include "../multi/scavenger.h"
#include "../os/os.h"
#include "../rf/gr/gr.h"
#include "../rf/multi.h"
#include "../rf/os/frametime.h"

namespace
{

constexpr float kTwoPi = 6.28318530718f;
constexpr float kDegToRad = kTwoPi / 360.0f;

constexpr float kFloorRingLift = 0.05f;
// Zone visual size relative to the capture radius: a 1.25 m ring at the default 1.5 m.
constexpr float kVisualScale = 1.25f / 1.5f;

constexpr int kRingTextureSize = 128;
constexpr float kRingTexturePeak = 0.92f;
constexpr float kRingTextureInnerFade = 0.70f;
constexpr int kGradientTextureWidth = 16;
constexpr int kGradientTextureHeight = 64;

constexpr float kColumnRadiusScale = 0.55f;
constexpr float kColumnHeight = 1.75f;
constexpr int kColumnSegments = 12;

struct HoloRing
{
    float radius_scale;
    float thickness_scale;
    float height;
    int segments;
    float gap_deg;
    float spin_deg_per_s;
    float bob_phase;
};
constexpr HoloRing kOuterRing{0.9f, 0.08f, 0.125f, 12, 10.0f, 15.0f, 0.0f};
constexpr HoloRing kMiddleRing{0.68f, 0.06f, 0.225f, 8, 15.0f, -25.0f, 0.33f};
constexpr float kInnerArcRadiusScale = 0.45f;
constexpr float kInnerArcThicknessScale = 0.1f;
constexpr float kInnerArcHeight = 0.325f;
constexpr float kInnerArcBobPhase = 0.67f;
constexpr float kArcStepDeg = 4.0f;

constexpr float kBobAmplitude = 0.015f;
constexpr float kBobPeriodS = 3.0f;

constexpr float kPulseMin = 0.65f;
constexpr float kPulseMax = 1.0f;
constexpr float kPulsePeriodIdleS = 2.0f;
constexpr float kPulsePeriodUrgentS = 0.5f;

constexpr int kBurstMs = 500;
constexpr float kBurstScaleEnd = 1.5f;

// Additive, so overlapping pieces only brighten. ONE/ONE blending ignores alpha, which is why the
// generated textures are premultiplied and fades scale the vertex colour itself.
constexpr rf::gr::Mode kZoneMode{
    rf::gr::TEXTURE_SOURCE_CLAMP,
    rf::gr::COLOR_SOURCE_VERTEX_TIMES_TEXTURE,
    rf::gr::ALPHA_SOURCE_VERTEX_TIMES_TEXTURE,
    rf::gr::ALPHA_BLEND_ADDITIVE,
    rf::gr::ZBUFFER_TYPE_READ,
    rf::gr::FOG_NOT_ALLOWED,
};

const std::array<rf::Vector2, 4> kQuadUvs{{{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}}};

struct ZoneBurst
{
    rf::Vector3 pos{};
    float radius = 0.0f;
    float floor_y = 0.0f;
    int64_t start_ms = 0;
};

struct ZoneVisualState
{
    bool have_zone = false;
    uint32_t seen_change_serial = 0;
    uint32_t seen_capture_serial = 0;
    rf::Vector3 pos{};
    float radius = 0.0f;
    float floor_y = 0.0f;
    // The new zone stays hidden until here, so a capture burst plays out first.
    int64_t appear_ms = 0;
    int64_t burst_start_ms = 0;
    bool capture_burst_active = false;
    ZoneBurst capture_burst;
    float pulse_phase = 0.0f;
};

ZoneVisualState g_zone_visual;

uint32_t premultiplied_white(float alpha)
{
    const auto a = static_cast<uint32_t>(std::clamp(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    return (a << 24) | (a << 16) | (a << 8) | a;
}

int ring_bitmap()
{
    static const int bm = bm_create_procedural(kRingTextureSize, kRingTextureSize, [](int x, int y) {
        constexpr float half = kRingTextureSize * 0.5f;
        const float dx = (x + 0.5f - half) / half;
        const float dy = (y + 0.5f - half) / half;
        const float d = std::sqrt(dx * dx + dy * dy);
        float alpha = 0.0f;
        if (d >= 1.0f) {
            alpha = 0.0f;
        }
        else if (d >= kRingTexturePeak) {
            alpha = (1.0f - d) / (1.0f - kRingTexturePeak);
        }
        else if (d >= kRingTextureInnerFade) {
            alpha = (d - kRingTextureInnerFade) / (kRingTexturePeak - kRingTextureInnerFade);
        }
        return premultiplied_white(alpha);
    });
    return bm;
}

int gradient_bitmap()
{
    static const int bm = bm_create_procedural(kGradientTextureWidth, kGradientTextureHeight, [](int x, int y) {
        const float vertical = static_cast<float>(y) / (kGradientTextureHeight - 1);
        const float horizontal = std::sin(kTwoPi * 0.5f * (x + 0.5f) / kGradientTextureWidth);
        return premultiplied_white(vertical * horizontal);
    });
    return bm;
}

rf::Color scaled_color(const rf::Color& base, float intensity)
{
    const float k = std::clamp(intensity, 0.0f, 1.0f);
    return rf::Color{
        static_cast<rf::ubyte>(base.red * k),
        static_cast<rf::ubyte>(base.green * k),
        static_cast<rf::ubyte>(base.blue * k),
        static_cast<rf::ubyte>(255.0f * k),
    };
}

rf::Color lerp_color(const rf::Color& a, const rf::Color& b, float t)
{
    const auto mix = [t](rf::ubyte x, rf::ubyte y) {
        return static_cast<rf::ubyte>(x + (static_cast<float>(y) - x) * t);
    };
    return rf::Color{mix(a.red, b.red), mix(a.green, b.green), mix(a.blue, b.blue), 255};
}

rf::Vector3 ring_point(const rf::Vector3& center, float y, float radius, float angle_rad)
{
    return rf::Vector3{center.x + radius * std::sin(angle_rad), y, center.z + radius * std::cos(angle_rad)};
}

float bob_offset(int64_t now_ms, float phase)
{
    const auto cycle = static_cast<float>(std::fmod(static_cast<double>(now_ms) / 1000.0 / kBobPeriodS + phase, 1.0));
    return kBobAmplitude * std::sin(kTwoPi * cycle);
}

float spin_deg(int64_t now_ms, float deg_per_s)
{
    return static_cast<float>(std::fmod(static_cast<double>(now_ms) * deg_per_s / 1000.0, 360.0));
}

void draw_quad(int bm, const std::array<rf::Vector3, 4>& verts, const std::array<rf::Vector2, 4>& uvs,
               const rf::Color& color)
{
    rf::gr::world_poly(bm, 4, verts.data(), uvs.data(), kZoneMode, color);
}

// Flat band around center from start_deg, sweeping clockwise seen from above.
void draw_arc(const rf::Vector3& center, float y, float radius, float thickness, float start_deg, float sweep_deg,
              const rf::Color& color)
{
    const int white_bm = world_hud_white_bitmap();
    if (white_bm == -1 || sweep_deg <= 0.0f) return;

    const int steps = std::max(1, static_cast<int>(std::ceil(sweep_deg / kArcStepDeg)));
    const float step_deg = sweep_deg / steps;
    const float r_in = radius - thickness * 0.5f;
    const float r_out = radius + thickness * 0.5f;
    for (int i = 0; i < steps; ++i) {
        const float a0 = (start_deg + i * step_deg) * kDegToRad;
        const float a1 = (start_deg + (i + 1) * step_deg) * kDegToRad;
        draw_quad(white_bm,
            {ring_point(center, y, r_in, a0), ring_point(center, y, r_out, a0), ring_point(center, y, r_out, a1),
             ring_point(center, y, r_in, a1)},
            kQuadUvs, color);
    }
}

void draw_holo_ring(const HoloRing& ring, const rf::Vector3& center, float floor_y, float zone_radius, float scale,
                    int64_t now_ms, const rf::Color& color)
{
    const float y = floor_y + ring.height + bob_offset(now_ms, ring.bob_phase);
    const float segment_deg = 360.0f / ring.segments;
    const float rotation = spin_deg(now_ms, ring.spin_deg_per_s);
    for (int k = 0; k < ring.segments; ++k) {
        draw_arc(center, y, zone_radius * ring.radius_scale * scale, zone_radius * ring.thickness_scale * scale,
            rotation + k * segment_deg, segment_deg - ring.gap_deg, color);
    }
}

void draw_floor_ring(const rf::Vector3& center, float floor_y, float radius, const rf::Color& color)
{
    const int bm = ring_bitmap();
    if (bm == -1) return;
    const float y = floor_y + kFloorRingLift;
    draw_quad(bm,
        {rf::Vector3{center.x - radius, y, center.z + radius}, rf::Vector3{center.x + radius, y, center.z + radius},
         rf::Vector3{center.x + radius, y, center.z - radius}, rf::Vector3{center.x - radius, y, center.z - radius}},
        kQuadUvs, color);
}

void draw_column(const rf::Vector3& center, float floor_y, float zone_radius, const rf::Color& color)
{
    const int bm = gradient_bitmap();
    if (bm == -1) return;
    const float radius = zone_radius * kColumnRadiusScale;
    const float y0 = floor_y + kFloorRingLift;
    const float y1 = y0 + kColumnHeight;
    // v = 1 is the gradient's opaque bottom row.
    static const std::array<rf::Vector2, 4> uvs{{{0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f}, {0.0f, 0.0f}}};
    for (int k = 0; k < kColumnSegments; ++k) {
        const float a0 = kTwoPi * k / kColumnSegments;
        const float a1 = kTwoPi * (k + 1) / kColumnSegments;
        draw_quad(bm,
            {ring_point(center, y0, radius, a0), ring_point(center, y0, radius, a1),
             ring_point(center, y1, radius, a1), ring_point(center, y1, radius, a0)},
            uvs, color);
    }
}

// Floor ring and outer ring growing and fading out over kBurstMs.
void draw_burst(const ZoneBurst& burst, int64_t now_ms, const rf::Color& base)
{
    const float t = std::clamp(static_cast<float>(now_ms - burst.start_ms) / kBurstMs, 0.0f, 1.0f);
    const float scale = 1.0f + (kBurstScaleEnd - 1.0f) * t;
    const rf::Color color = scaled_color(base, 1.0f - t);
    draw_floor_ring(burst.pos, burst.floor_y, burst.radius * scale, color);
    draw_holo_ring(kOuterRing, burst.pos, burst.floor_y, burst.radius, scale, now_ms, color);
}

// Picks up zone moves and captures from the replicated zone state.
void track_zone_changes(const ScavengerZoneView& view, int64_t now_ms)
{
    auto& vis = g_zone_visual;
    if (view.capture_serial != vis.seen_capture_serial) {
        vis.seen_capture_serial = view.capture_serial;
        if (vis.have_zone) {
            vis.capture_burst = ZoneBurst{view.captured_pos, view.captured_radius * kVisualScale,
                view.captured_floor_y, now_ms};
            vis.capture_burst_active = true;
            vis.appear_ms = now_ms + kBurstMs;
        }
    }
    if (view.change_serial != vis.seen_change_serial || !vis.have_zone) {
        vis.seen_change_serial = view.change_serial;
        vis.pos = view.pos;
        vis.floor_y = view.floor_y;
        vis.burst_start_ms = std::max(now_ms, vis.appear_ms);
    }
    vis.radius = view.radius * kVisualScale;
    vis.have_zone = true;
}

} // namespace

void scavenger_zone_render()
{
    ScavengerZoneView view{};
    if (!rf::is_multi || !scavenger_get_zone_view(&view)) {
        g_zone_visual.have_zone = false;
        g_zone_visual.capture_burst_active = false;
        g_zone_visual.appear_ms = 0;
        scavenger_clear_zone_light();
        return;
    }

    const int64_t now_ms = timer::get_i64(1000);
    track_zone_changes(view, now_ms);
    auto& vis = g_zone_visual;

    const float urgency = view.time_left_ms <= kScavZoneCountdownVisibleMs
        ? 1.0f - std::max(0, view.time_left_ms) / static_cast<float>(kScavZoneCountdownVisibleMs)
        : 0.0f;
    const rf::Color base = lerp_color(kScavZoneColorIdle, kScavZoneColorUrgent, urgency);
    const float pulse_period = kPulsePeriodIdleS + (kPulsePeriodUrgentS - kPulsePeriodIdleS) * urgency;
    vis.pulse_phase = std::fmod(vis.pulse_phase + rf::frametime / pulse_period, 1.0f);
    const float brightness =
        kPulseMin + (kPulseMax - kPulseMin) * (0.5f + 0.5f * std::sin(kTwoPi * vis.pulse_phase));

    if (vis.capture_burst_active) {
        if (now_ms - vis.capture_burst.start_ms < kBurstMs) {
            draw_burst(vis.capture_burst, now_ms, kScavZoneColorIdle);
        }
        else {
            vis.capture_burst_active = false;
        }
    }

    if (now_ms < vis.appear_ms) {
        scavenger_clear_zone_light();
        return;
    }
    scavenger_update_zone_light(base, vis.floor_y);

    const int64_t since_burst_ms = now_ms - vis.burst_start_ms;
    const float column_fade = std::clamp(static_cast<float>(since_burst_ms) / kBurstMs, 0.0f, 1.0f);
    if (since_burst_ms < kBurstMs) {
        draw_burst(ZoneBurst{vis.pos, vis.radius, vis.floor_y, vis.burst_start_ms}, now_ms, base);
    }

    const rf::Color color = scaled_color(base, brightness);
    draw_floor_ring(vis.pos, vis.floor_y, vis.radius, color);
    draw_column(vis.pos, vis.floor_y, vis.radius, scaled_color(base, brightness * column_fade));
    draw_holo_ring(kOuterRing, vis.pos, vis.floor_y, vis.radius, 1.0f, now_ms, color);
    draw_holo_ring(kMiddleRing, vis.pos, vis.floor_y, vis.radius, 1.0f, now_ms, color);

    const float arc_fraction = view.time_left_ms <= kScavZoneCountdownVisibleMs
        ? std::max(0, view.time_left_ms) / static_cast<float>(kScavZoneCountdownVisibleMs)
        : 1.0f;
    const float arc_sweep = 360.0f * arc_fraction;
    draw_arc(vis.pos, vis.floor_y + kInnerArcHeight + bob_offset(now_ms, kInnerArcBobPhase),
        vis.radius * kInnerArcRadiusScale, vis.radius * kInnerArcThicknessScale, 360.0f - arc_sweep, arc_sweep, color);
}
