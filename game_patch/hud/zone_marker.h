#pragma once

#include <array>
#include <cstdint>
#include "../rf/gr/gr.h"
#include "../rf/math/vector.h"

inline constexpr int zone_marker_burst_ms = 500;

struct ZoneMarkerBurst
{
    rf::Vector3 pos{};
    float radius = 0.0f;
    float floor_y = 0.0f;
    int64_t start_ms = 0;
};

rf::Color zone_marker_scaled_color(const rf::Color& base, float intensity);
rf::Color zone_marker_lerp_color(const rf::Color& a, const rf::Color& b, float t);
// Brightness of the marker's slow pulse at `phase` (0-1) through its cycle
float zone_marker_pulse_brightness(float phase);
// The same, on a cycle of `period_ms` by the clock
float zone_marker_pulse_at(int64_t now_ms, double period_ms);
void zone_marker_draw(const rf::Vector3& pos, float floor_y, float radius, int64_t now_ms, const rf::Color& color,
                      const rf::Color& column_color);
// The floor ring and the low outer ring, without the column
void zone_marker_draw_floor(const rf::Vector3& pos, float floor_y, float radius, int64_t now_ms,
                            const rf::Color& color);
// Sweeps clockwise seen from above and ends on the +Z axis
void zone_marker_draw_inner_arc(const rf::Vector3& pos, float floor_y, float radius, int64_t now_ms, float sweep_deg,
                                const rf::Color& color);
// The marker, burst included, is wholly out of view
bool zone_marker_culled(const rf::Vector3& pos, float floor_y, float radius);
// A textured quad in the markers' additive mode, so textures are premultiplied
void zone_marker_draw_quad(int bm, const std::array<rf::Vector3, 4>& verts, const rf::Color& color,
                           bool through_walls = false);
// Floor ring and outer ring growing and fading out over zone_marker_burst_ms
void zone_marker_draw_burst(const ZoneMarkerBurst& burst, int64_t now_ms, const rf::Color& base);
