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
#include "../os/os.h"
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

constexpr rf::gr::Mode tether_mode{
    rf::gr::TEXTURE_SOURCE_NONE,
    rf::gr::COLOR_SOURCE_VERTEX,
    rf::gr::ALPHA_SOURCE_VERTEX,
    rf::gr::ALPHA_BLEND_ALPHA_ADDITIVE,
    rf::gr::ZBUFFER_TYPE_READ,
    rf::gr::FOG_NOT_ALLOWED,
};

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
    if (multi_spectate_view_is_player_eyes(pl)) {
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
    rf::Vector3 lo = from;
    rf::Vector3 hi = from;
    for (int i = 0; i < tether_rows; ++i) {
        const rf::Vector3 p = from_curve(rope_curve::sample(curve, static_cast<float>(i) / (tether_rows - 1)));
        points[i] = p;
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
    if (rf::gr::cull_bounding_box(lo, hi)) {
        return;
    }

    const rf::Vector3 eye = rf::gr::eye_pos;
    std::array<rf::gr::Vertex, tether_rows * 3> verts{};
    for (int i = 0; i < tether_rows; ++i) {
        const rf::Vector3& p = points[i];
        rf::Vector3 tangent = points[std::min(i + 1, tether_rows - 1)] - points[std::max(i - 1, 0)];
        tangent.normalize_safe();
        const rf::Vector3 to_point = p - eye;
        const float dist = to_point.len();
        rf::Vector3 side = tangent.cross(to_point);
        side.normalize_safe();
        side *= std::max(tether_half_width, tether_half_width_per_meter * dist);

        const float t = static_cast<float>(i) / (tether_rows - 1);
        float a = alpha;
        if (pulse_t) {
            a += heal_pulse_alpha * std::max(0.0f, 1.0f - std::abs(t - *pulse_t) / heal_pulse_half_length);
        }
        a *= std::clamp((dist - tether_fade_near) / tether_fade_length, 0.0f, 1.0f);

        gr_set_colored_vertex(verts[i * 3], p + side, color, 0.0f);
        gr_set_colored_vertex(verts[i * 3 + 1], p, color, a);
        gr_set_colored_vertex(verts[i * 3 + 2], p - side, color, 0.0f);
    }

    constexpr auto attributes = static_cast<rf::gr::TMapperFlags>(rf::gr::TMAP_FLAG_RGB | rf::gr::TMAP_FLAG_ALPHA);
    for (int i = 0; i + 1 < tether_rows; ++i) {
        rf::gr::Vertex* r0 = &verts[i * 3];
        rf::gr::Vertex* r1 = &verts[(i + 1) * 3];
        for (int col = 0; col < 2; ++col) {
            // The rope ribbon's winding: +side to -side on this row, back along the next
            rf::gr::Vertex* quad[4] = {&r0[col], &r0[col + 1], &r1[col + 1], &r1[col]};
            rf::gr::poly(4, quad, attributes, tether_mode, false, 0.0f);
        }
    }
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
        g_payload_world = {};
        return;
    }
    const int64_t now_ms = timer::get_i64(1000);
    track_captures(now_ms);
    track_heal_ticks(now_ms);
    render_checkpoint_markers(now_ms);
    if (g_alpine_game_config.world_hud_payload_push_zone) {
        render_push_zone(now_ms);
    }
    if (g_alpine_game_config.world_hud_payload_tethers) {
        render_tethers(now_ms);
    }
}

void payload_world_level_unload()
{
    g_payload_world = {};
}
