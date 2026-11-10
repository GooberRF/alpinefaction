#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>
#include <common/rope_curve.h>
#include "payload_world.h"
#include "hud_world.h"
#include "multi_spectate.h"
#include "world_medallion.h"
#include "zone_marker.h"
#include "../graphics/gr.h"
#include "../misc/alpine_settings.h"
#include "../multi/payload.h"
#include "../rf/entity.h"
#include "../rf/gr/gr.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"

namespace
{

constexpr float checkpoint_marker_radius = 1.25f;
constexpr double checkpoint_next_pulse_period_ms = 2000.0;
constexpr float checkpoint_locked_brightness = 0.4f;
constexpr float checkpoint_captured_brightness = 0.6f;

// From the cart's top to the medallion's bottom, so the cart passes under it
constexpr float medallion_lift = 0.6f;
// Along the track; it fades out over the cart, where the cart's own icon is
constexpr float hologram_fade_near = 1.5f;
constexpr float hologram_fade_length = 3.0f;

constexpr float push_zone_active_brightness = 0.8f;
constexpr float push_zone_idle_brightness = 0.5f;
constexpr double push_zone_blocked_pulse_period_ms = 1000.0;

constexpr double track_pulse_period_ms = 3000.0;
constexpr float track_pulse_speed = 20.0f;
constexpr float track_pulse_length = 3.0f;
constexpr float track_pulse_row_spacing = 0.5f;
constexpr int track_pulse_steps = static_cast<int>(track_pulse_length / track_pulse_row_spacing);
// The rows on the spacing, the peak, and the two ends where the cart or the track's end cuts the pulse
constexpr int track_pulse_max_rows = track_pulse_steps + 2;
constexpr float track_pulse_lift = 0.05f;
// Where the alpha peaks, as a share of the way from the tail to the head
constexpr float track_pulse_peak = 0.8f;
// The head fades out over this much track before the end
constexpr float track_pulse_end_fade = 0.5f;
constexpr float track_pulse_alpha = 0.6f;
constexpr float track_pulse_half_width = 0.3f;
constexpr float track_pulse_band = 0.4f;
constexpr float track_pulse_min_half_px = 1.5f;

constexpr int tether_rows = 16;
constexpr float tether_slack = 0.01f;
constexpr float tether_min_span = 0.1f;
constexpr float tether_half_width = 0.03f;
// Widens with distance, so a far tether stays a line instead of breaking up
constexpr float tether_half_width_per_meter = 0.0015f;
constexpr float tether_alpha = 0.8f;
constexpr float tether_blocked_attacker_alpha = 0.3f;
constexpr float tether_fade_near = 0.3f;
constexpr float tether_fade_length = 1.5f;
// The viewed player's own tether starts in front of and below the eye
constexpr float tether_self_forward = 0.5f;
constexpr float tether_self_down = 0.4f;
constexpr int heal_pulse_ms = 400;
constexpr float heal_pulse_half_length = 0.15f;
constexpr float heal_pulse_alpha = 0.8f;

struct CheckpointVisual
{
    bool captured = false;
    int64_t burst_start_ms = -1;
};

struct PayloadWorldState
{
    bool seeded = false;
    std::vector<CheckpointVisual> checkpoints;
    std::optional<uint8_t> heal_serial;
    std::optional<int64_t> last_heal_ms;
};

PayloadWorldState g_payload_world;

void track_captures(int64_t now_ms)
{
    const auto& cps = payload_checkpoints();
    auto& seen = g_payload_world.checkpoints;
    const bool seed = !g_payload_world.seeded || seen.size() != cps.size();
    if (seed) {
        seen.assign(cps.size(), CheckpointVisual{});
    }
    for (std::size_t i = 0; i < cps.size(); ++i) {
        const bool captured = payload_checkpoint_captured(cps[i]);
        // The cart stands on a checkpoint as it is captured; a late joiner's sync of older captures does not burst
        const bool cart_there = std::abs(payload_distance() - cps[i].distance) < 2.0f * checkpoint_marker_radius;
        if (captured && !seen[i].captured && !seed && cart_there) {
            seen[i].burst_start_ms = now_ms;
        }
        seen[i].captured = captured;
    }
    g_payload_world.seeded = true;
}

void track_heal_ticks(int64_t now_ms)
{
    const std::optional<uint8_t> serial = payload_heal_serial();
    if (g_payload_world.heal_serial && serial && *g_payload_world.heal_serial != *serial) {
        g_payload_world.last_heal_ms = now_ms;
    }
    g_payload_world.heal_serial = serial;
}

void render_checkpoint_markers(int64_t now_ms)
{
    const auto& cps = payload_checkpoints();
    const int attacking_team = payload_attacking_team();
    const int defending_team = payload_defending_team();
    const rf::Color attackers = hud_team_color(attacking_team);
    const rf::Color defenders = hud_team_color(defending_team);
    const int next = payload_next_checkpoint_index();
    const float half_height = payload_cart_half_height();
    const float pulse = zone_marker_pulse_at(now_ms, checkpoint_next_pulse_period_ms);

    for (std::size_t i = 0; i < cps.size(); ++i) {
        const PayloadCheckpoint& cp = cps[i];
        const rf::Vector3 pos = payload_track_point(cp.distance);
        const float floor_y = pos.y - half_height;
        CheckpointVisual& vis = g_payload_world.checkpoints[i];
        if (vis.burst_start_ms >= 0 && now_ms - vis.burst_start_ms >= zone_marker_burst_ms) {
            vis.burst_start_ms = -1;
        }
        const bool is_next = !vis.captured && static_cast<int>(i) == next;
        const float brightness =
            vis.captured ? checkpoint_captured_brightness : is_next ? pulse : checkpoint_locked_brightness;
        const rf::Color& owner = vis.captured ? attackers : defenders;
        const float clear = std::clamp(
            (std::abs(payload_distance() - cp.distance) - hologram_fade_near) / hologram_fade_length, 0.0f, 1.0f);
        if (clear > 0.0f) {
            world_medallion_draw(payload_medallion_center(pos), 1.0f,
                world_hud_medallion_face(vis.captured ? attacking_team : defending_team),
                zone_marker_scaled_color(owner, brightness * clear), now_ms, false);
        }
        if (zone_marker_culled(pos, floor_y, checkpoint_marker_radius)) {
            continue;
        }

        if (vis.burst_start_ms >= 0) {
            zone_marker_draw_burst(ZoneMarkerBurst{pos, checkpoint_marker_radius, floor_y, vis.burst_start_ms},
                now_ms, attackers);
        }

        const rf::Color color = zone_marker_scaled_color(owner, brightness);
        zone_marker_draw(pos, floor_y, checkpoint_marker_radius, now_ms, color, color);
        if (is_next) {
            zone_marker_draw_inner_arc(pos, floor_y, checkpoint_marker_radius, now_ms,
                360.0f * payload_next_checkpoint_progress(), zone_marker_scaled_color(attackers, pulse));
        }
    }
}

void render_push_zone(int64_t now_ms)
{
    const auto center = payload_cart_center();
    const PayloadCartState state = payload_state();
    if (!center || state == PayloadCartState::finale) {
        return;
    }
    const float radius = payload_push_radius();
    const float floor_y = center->y - payload_cart_half_height();
    if (zone_marker_culled(*center, floor_y, radius)) {
        return;
    }

    float brightness = push_zone_idle_brightness;
    if (state == PayloadCartState::pushing) {
        brightness = push_zone_active_brightness;
    }
    else if (state == PayloadCartState::blocked) {
        brightness = push_zone_active_brightness * zone_marker_pulse_at(now_ms, push_zone_blocked_pulse_period_ms);
    }
    const int team = payload_defenders_have_cart() ? payload_defending_team() : payload_attacking_team();
    const rf::Color color = zone_marker_scaled_color(hud_team_color(team), brightness);
    zone_marker_draw_floor(*center, floor_y, radius, now_ms, color);
}

// The box around the points, grown by `margin`, is wholly out of view
bool points_culled(const rf::Vector3* points, int count, float margin)
{
    rf::Vector3 lo = points[0];
    rf::Vector3 hi = points[0];
    for (int i = 1; i < count; ++i) {
        const rf::Vector3& p = points[i];
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    const rf::Vector3 reach{margin, margin, margin};
    return rf::gr::cull_bounding_box(lo - reach, hi + reach);
}

// The part of the pulse from `tail` on that lies between the cart and the end of the track
void draw_track_pulse(float tail, float cart_distance, float total, const rf::Color& color)
{
    const float from = std::max(tail, cart_distance);
    const float to = std::min(tail + track_pulse_length, total);
    if (!(to - from > 0.05f)) {
        return;
    }
    // Fixed along the pulse, so its shape holds as the cart or the track's end cuts into it
    std::array<float, track_pulse_max_rows> along{};
    int rows = 0;
    // A row next to an end would leave a near-zero-length segment there
    constexpr float min_row_gap = 0.01f;
    const auto add_inside = [&](float d) {
        if (d > from + min_row_gap && d < to - min_row_gap) {
            along[rows++] = d;
        }
    };
    along[rows++] = from;
    for (int k = 1; k < track_pulse_steps; ++k) {
        add_inside(tail + track_pulse_row_spacing * static_cast<float>(k));
    }
    add_inside(tail + track_pulse_length * track_pulse_peak);
    along[rows++] = to;
    std::sort(along.begin(), along.begin() + rows);

    const float floor_offset = payload_cart_half_height() - track_pulse_lift;
    std::array<rf::Vector3, track_pulse_max_rows> points{};
    std::array<rf::Vector3, track_pulse_max_rows> sides{};
    std::array<float, track_pulse_max_rows> alphas{};
    for (int i = 0; i < rows; ++i) {
        const float d = along[i];
        points[i] = payload_track_point(d);
        points[i].y -= floor_offset;
        const float u = (d - tail) / track_pulse_length;
        alphas[i] = (u < track_pulse_peak ? u / track_pulse_peak : (1.0f - u) / (1.0f - track_pulse_peak))
            * std::clamp((total - d) / track_pulse_end_fade, 0.0f, 1.0f);
    }

    // Not fogged, so faded by hand, and dropped past the far plane, where it would depth-test against the cleared sky
    const rf::Vector3& mid = points[rows / 2];
    const float dist = (mid - rf::gr::eye_pos).len();
    if (dist >= level_projection_far()) {
        return;
    }
    const float fade = gr_fog_fade(dist);
    if (!(fade > 0.0f)) {
        return;
    }
    const float half_width =
        gr_floored_half_width(gr_pixels_per_meter(mid), track_pulse_half_width, track_pulse_min_half_px);
    if (points_culled(points.data(), rows, half_width)) {
        return;
    }

    for (int i = 0; i < rows; ++i) {
        alphas[i] *= track_pulse_alpha * fade;
    }
    gr_camera_facing_sides(rows, points.data(), half_width, sides.data());
    gr_band_strip(rows, points.data(), sides.data(), alphas.data(), track_pulse_band, color, additive_vertex_color_3d);
}

// Pulses leave the cart on a fixed beat and run to the end of the track, so they follow the cart without state
void render_track_pulses(int64_t now_ms)
{
    const float total = payload_total_length();
    const float cart_distance = payload_distance();
    if (!payload_cart_center() || !(total > cart_distance)) {
        return;
    }
    const rf::Color color = hud_team_color(payload_attacking_team());
    rf::gr::set_texture(-1, -1);
    const double newest_age_ms = std::fmod(static_cast<double>(now_ms), track_pulse_period_ms);
    for (double age_ms = newest_age_ms;; age_ms += track_pulse_period_ms) {
        const float tail = cart_distance + track_pulse_speed * static_cast<float>(age_ms / 1000.0) - track_pulse_length;
        if (tail >= total) {
            break;
        }
        draw_track_pulse(tail, cart_distance, total, color);
    }
}

rope_curve::Vec3 to_curve(const rf::Vector3& v)
{
    return {v.x, v.y, v.z};
}

rf::Vector3 from_curve(const rope_curve::Vec3& v)
{
    return {v.x, v.y, v.z};
}

std::optional<rf::Vector3> tether_start(const rf::Player& pl)
{
    rf::Entity* ep = rf::entity_from_handle(pl.entity_handle);
    if (!ep || rf::entity_is_dying(ep)) {
        return std::nullopt;
    }
    if (multi_spectate_view_is_first_person_of(ep)) {
        return rf::gr::eye_pos + rf::gr::eye_matrix.fvec * tether_self_forward
            - rf::gr::eye_matrix.uvec * tether_self_down;
    }
    return ep->pos;
}

// A soft line, transparent at both edges, with a slight sag. `pulse_t` runs 0 at `from` to 1 at `to`.
void draw_tether(const rf::Vector3& from, const rf::Vector3& to, const rf::Color& color, float alpha,
                 std::optional<float> pulse_t)
{
    const float span = (to - from).len();
    if (!(span > tether_min_span)) {
        return;
    }
    const rope_curve::Curve curve = rope_curve::solve(to_curve(from), to_curve(to), span * (1.0f + tether_slack));
    std::array<rf::Vector3, tether_rows> points{};
    for (int i = 0; i < tether_rows; ++i) {
        points[i] = from_curve(rope_curve::sample(curve, static_cast<float>(i) / (tether_rows - 1)));
    }
    if (points_culled(points.data(), tether_rows, 0.0f)) {
        return;
    }

    std::array<rf::Vector3, tether_rows> sides{};
    std::array<float, tether_rows> alphas{};
    gr_camera_facing_sides(tether_rows, points.data(), 1.0f, sides.data());
    for (int i = 0; i < tether_rows; ++i) {
        const float dist = (points[i] - rf::gr::eye_pos).len();
        sides[i] *= std::max(tether_half_width, tether_half_width_per_meter * dist);

        const float t = static_cast<float>(i) / (tether_rows - 1);
        float a = alpha;
        if (pulse_t) {
            a += heal_pulse_alpha * std::max(0.0f, 1.0f - std::abs(t - *pulse_t) / heal_pulse_half_length);
        }
        alphas[i] = a * std::clamp((dist - tether_fade_near) / tether_fade_length, 0.0f, 1.0f);
    }
    gr_band_strip(tether_rows, points.data(), sides.data(), alphas.data(), 0.0f, color, additive_vertex_color_3d);
}

void render_tethers(int64_t now_ms)
{
    const auto center = payload_cart_center();
    if (!center || payload_state() == PayloadCartState::finale) {
        return;
    }
    const bool blocked = payload_state() == PayloadCartState::blocked;
    std::optional<float> pulse_t;
    if (const auto& last = g_payload_world.last_heal_ms; last && now_ms - *last < heal_pulse_ms) {
        // From the cart to the healed player
        pulse_t = 1.0f - static_cast<float>(now_ms - *last) / heal_pulse_ms;
    }

    const auto draw_from = [&](const std::vector<uint8_t>& ids, const rf::Color& color, float alpha,
                               std::optional<float> pulse) {
        for (const uint8_t id : ids) {
            const rf::Player* pl = rf::multi_find_player_by_id(id);
            if (!pl) {
                continue;
            }
            if (const auto from = tether_start(*pl)) {
                draw_tether(*from, *center, color, alpha, pulse);
            }
        }
    };

    rf::gr::set_texture(-1, -1);
    draw_from(payload_pusher_ids(), hud_team_color(payload_attacking_team()),
        blocked ? tether_blocked_attacker_alpha : tether_alpha, pulse_t);
    if (blocked) {
        draw_from(payload_blocker_ids(), hud_team_color(payload_defending_team()), tether_alpha, std::nullopt);
    }
}

} // namespace

rf::Vector3 payload_medallion_center(const rf::Vector3& cart_center)
{
    rf::Vector3 center = cart_center;
    center.y += payload_cart_half_height() + medallion_lift + world_medallion_radius;
    return center;
}

void payload_world_render()
{
    if (!payload_active()) {
        return;
    }
    const int64_t now_ms = world_hud_clock_ms();
    track_captures(now_ms);
    track_heal_ticks(now_ms);
    render_checkpoint_markers(now_ms);
    if (g_alpine_game_config.world_hud_payload_push_zone) {
        render_push_zone(now_ms);
    }
    if (g_alpine_game_config.world_hud_payload_track_pulse) {
        render_track_pulses(now_ms);
    }
    if (g_alpine_game_config.world_hud_payload_tethers) {
        render_tethers(now_ms);
    }
}

void payload_world_level_unload()
{
    g_payload_world = {};
}
