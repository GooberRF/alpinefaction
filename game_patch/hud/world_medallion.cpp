#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include "world_medallion.h"
#include "hud_world.h"
#include "zone_marker.h"

namespace
{

constexpr float two_pi = 2.0f * std::numbers::pi_v<float>;
constexpr float deg_to_rad = two_pi / 360.0f;

constexpr float half_thickness = 0.04f;
constexpr double spin_deg_per_s = 60.0;
constexpr float bob_height = 0.05f;
constexpr double bob_period_ms = 3000.0;
constexpr int rim_segments = 32;
constexpr float rim_brightness = 0.6f;

constexpr float ring_radius = world_medallion_radius * 1.3f;
constexpr float ring_half_width = world_medallion_radius * 0.07f;
constexpr int ring_notches = 8;
constexpr float ring_notch_gap_deg = 5.0f;
constexpr float ring_step_deg = 8.0f;

// Clockwise from the top as the camera sees it, from `start_deg` to `end_deg`
void draw_ring_band(int white_bm, const rf::Vector3& center, float scale, float start_deg, float end_deg,
                    const rf::Color& color, bool through_walls)
{
    if (end_deg <= start_deg) {
        return;
    }
    const rf::Vector3& right = rf::gr::eye_matrix.rvec;
    const rf::Vector3& up = rf::gr::eye_matrix.uvec;
    const auto point = [&](float radius, float deg) {
        const float a = deg * deg_to_rad;
        return center + (up * std::cos(a) + right * std::sin(a)) * radius;
    };
    const float r_in = (ring_radius - ring_half_width) * scale;
    const float r_out = (ring_radius + ring_half_width) * scale;
    const int steps = std::max(1, static_cast<int>(std::ceil((end_deg - start_deg) / ring_step_deg)));
    const float step = (end_deg - start_deg) / steps;
    for (int i = 0; i < steps; ++i) {
        const float a0 = start_deg + step * i;
        const float a1 = a0 + step;
        zone_marker_draw_quad(white_bm, {point(r_in, a0), point(r_out, a0), point(r_out, a1), point(r_in, a1)},
            color, through_walls);
    }
}

void draw_ring(int white_bm, const rf::Vector3& center, float scale, const WorldMedallionRing& ring,
               bool through_walls)
{
    const float fill_deg = 360.0f * std::clamp(ring.progress, 0.0f, 1.0f);
    const float notch_deg = 360.0f / ring_notches;
    for (int k = 0; k < ring_notches; ++k) {
        const float start = notch_deg * k + 0.5f * ring_notch_gap_deg;
        const float end = notch_deg * (k + 1) - 0.5f * ring_notch_gap_deg;
        const float split = std::clamp(fill_deg, start, end);
        draw_ring_band(white_bm, center, scale, start, split, ring.fill_color, through_walls);
        draw_ring_band(white_bm, center, scale, split, end, ring.track_color, through_walls);
    }
}

} // namespace

float world_medallion_ring_reach()
{
    return ring_radius + ring_half_width;
}

void world_medallion_draw(const rf::Vector3& center, float scale, int face_bm, const rf::Color& color, int64_t now_ms,
                          bool through_walls, const std::optional<WorldMedallionRing>& ring)
{
    const int white_bm = world_hud_white_bitmap();
    if (face_bm == -1 || white_bm == -1) {
        return;
    }

    const auto now = static_cast<double>(now_ms);
    rf::Vector3 c = center;
    c.y += bob_height * scale * std::sin(two_pi * static_cast<float>(std::fmod(now / bob_period_ms, 1.0)));
    const float reach = (ring ? world_medallion_ring_reach() : world_medallion_radius + half_thickness) * scale;
    const float radius = world_medallion_radius * scale;
    const rf::Vector3 extent{reach, reach, reach};
    if (rf::gr::cull_bounding_box(c - extent, c + extent)) {
        return;
    }

    const float yaw = two_pi * static_cast<float>(std::fmod(now / 1000.0 * spin_deg_per_s / 360.0, 1.0));
    const rf::Vector3 normal{std::sin(yaw), 0.0f, std::cos(yaw)};
    const rf::Vector3 side{std::cos(yaw), 0.0f, -std::sin(yaw)};
    const rf::Vector3 up{0.0f, 1.0f, 0.0f};
    const rf::Vector3 depth = normal * (half_thickness * scale);

    // Only the face toward the eye, its texture flipped from the back so it never reads mirrored
    const bool front = normal.dot_prod(rf::gr::eye_pos - c) >= 0.0f;
    const rf::Vector3 face = front ? c + depth : c - depth;
    const rf::Vector3 right = (front ? -side : side) * radius;
    const rf::Vector3 top = up * radius;
    zone_marker_draw_quad(face_bm, {face - right + top, face + right + top, face + right - top, face - right - top},
        color, through_walls);

    // The rim gives the disc a body edge-on
    const rf::Color rim_color = zone_marker_scaled_color(color, rim_brightness);
    for (int i = 0; i < rim_segments; ++i) {
        const float a0 = two_pi * static_cast<float>(i) / rim_segments;
        const float a1 = two_pi * static_cast<float>(i + 1) / rim_segments;
        const rf::Vector3 p0 = c + (side * std::cos(a0) + up * std::sin(a0)) * radius;
        const rf::Vector3 p1 = c + (side * std::cos(a1) + up * std::sin(a1)) * radius;
        const rf::Vector3 mid = (p0 + p1) * 0.5f;
        if ((mid - c).dot_prod(rf::gr::eye_pos - mid) < 0.0f) {
            continue;
        }
        zone_marker_draw_quad(white_bm, {p0 + depth, p1 + depth, p1 - depth, p0 - depth}, rim_color, through_walls);
    }

    if (ring) {
        draw_ring(white_bm, c, scale, *ring, through_walls);
    }
}
