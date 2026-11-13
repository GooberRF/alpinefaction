#include <algorithm>
#include <cmath>
#include <cstdint>
#include "scavenger_zone.h"
#include "zone_marker.h"
#include "../multi/scavenger.h"
#include "../os/os.h"
#include "../rf/multi.h"
#include "../rf/os/frametime.h"

namespace
{

// Zone visual size relative to the capture radius: a 1.25 m ring at the default 1.5 m.
constexpr float kVisualScale = 1.25f / 1.5f;

constexpr float kPulsePeriodIdleS = 2.0f;
constexpr float kPulsePeriodUrgentS = 0.5f;

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
    ZoneMarkerBurst capture_burst;
    float pulse_phase = 0.0f;
};

ZoneVisualState g_zone_visual;

// Picks up zone moves and captures from the replicated zone state.
void track_zone_changes(const ScavengerZoneView& view, int64_t now_ms)
{
    auto& vis = g_zone_visual;
    if (view.capture_serial != vis.seen_capture_serial) {
        vis.seen_capture_serial = view.capture_serial;
        if (vis.have_zone) {
            vis.capture_burst = ZoneMarkerBurst{view.captured_pos, view.captured_radius * kVisualScale,
                view.captured_floor_y, now_ms};
            vis.capture_burst_active = true;
            vis.appear_ms = now_ms + zone_marker_burst_ms;
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
    const rf::Color base = zone_marker_lerp_color(kScavZoneColorIdle, kScavZoneColorUrgent, urgency);
    const float pulse_period = kPulsePeriodIdleS + (kPulsePeriodUrgentS - kPulsePeriodIdleS) * urgency;
    vis.pulse_phase = std::fmod(vis.pulse_phase + rf::frametime / pulse_period, 1.0f);
    const float brightness = zone_marker_pulse_brightness(vis.pulse_phase);

    if (vis.capture_burst_active) {
        if (now_ms - vis.capture_burst.start_ms < zone_marker_burst_ms) {
            zone_marker_draw_burst(vis.capture_burst, now_ms, kScavZoneColorIdle);
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
    const float column_fade = std::clamp(static_cast<float>(since_burst_ms) / zone_marker_burst_ms, 0.0f, 1.0f);
    if (since_burst_ms < zone_marker_burst_ms) {
        zone_marker_draw_burst(ZoneMarkerBurst{vis.pos, vis.radius, vis.floor_y, vis.burst_start_ms}, now_ms, base);
    }

    const rf::Color color = zone_marker_scaled_color(base, brightness);
    zone_marker_draw(vis.pos, vis.floor_y, vis.radius, now_ms, color,
        zone_marker_scaled_color(base, brightness * column_fade));

    const float arc_fraction = view.time_left_ms <= kScavZoneCountdownVisibleMs
        ? std::max(0, view.time_left_ms) / static_cast<float>(kScavZoneCountdownVisibleMs)
        : 1.0f;
    zone_marker_draw_inner_arc(vis.pos, vis.floor_y, vis.radius, now_ms, 360.0f * arc_fraction, color);
}
