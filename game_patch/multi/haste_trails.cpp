#include <algorithm>
#include <array>
#include <vector>
#include <common/utils/list-utils.h>
#include "haste_trails.h"
#include "multi.h"
#include "powerups.h"
#include "demo/demo.h"
#include "../graphics/gr.h"
#include "../hud/multi_spectate.h"
#include "../misc/alpine_settings.h"
#include "../rf/entity.h"
#include "../rf/gameseq.h"
#include "../rf/multi.h"
#include "../rf/object.h"
#include "../rf/player/player.h"
#include "../rf/gr/gr.h"

namespace
{

constexpr float trail_lifetime = 0.4f;
constexpr int trail_max_samples = 32;
constexpr float trail_sample_spacing = 0.15f;
constexpr float trail_teleport_dist = 3.0f;
constexpr float trail_ankle_height = 0.15f;
constexpr float trail_side_offset = 0.18f;
constexpr float trail_alpha = 0.5f;
constexpr float trail_min_half_width = 0.02f;
constexpr float trail_min_half_px = 1.0f;
constexpr float trail_band = 0.5f; // opaque share of the ribbon's half-width
// Of the multiplayer run speed: a hasted crouch run is barely faster than it.
constexpr float trail_min_speed_factor = 1.1f;

struct TrailSample
{
    std::array<rf::Vector3, 2> lines{};
    float age = 0.0f;
    bool joins_previous = false;
};

struct HasteTrail
{
    int entity_handle = -1;
    std::array<TrailSample, trail_max_samples> samples{};
    int first = 0; // the oldest sample's slot
    int count = 0;
    // Where the speed is measured from, and the time since.
    rf::Vector3 ref_pos{};
    float ref_time = 0.0f;
    bool has_ref = false;
    rf::Vector3 side_dir{}; // horizontal, right of the direction of travel
    bool live = false;      // the newest sample joins the entity's current position
    bool seen = false;

    TrailSample& sample(int i)
    {
        return samples[(first + i) % trail_max_samples];
    }

    const TrailSample& sample(int i) const
    {
        return samples[(first + i) % trail_max_samples];
    }

    void reset()
    {
        count = 0;
        live = false;
        has_ref = false;
    }
};

std::vector<HasteTrail> g_trails;
std::array<rf::Vector3, trail_max_samples + 1> g_stroke_pos;
std::array<float, trail_max_samples + 1> g_stroke_alpha;
std::array<rf::Vector3, trail_max_samples + 1> g_stroke_side;

bool trail_should_sample(rf::Entity* ep)
{
    return ep->info && !rf::entity_is_dying(ep) && !(ep->obj_flags & rf::OF_HIDDEN) && !rf::entity_in_vehicle(ep)
        && powerup_is_active(ep, AlpinePowerup::haste);
}

// Ankle height over the entity's lowest point. Interpolation moves remote players without updating
// p_data.bbox_min, so it comes from the object-space collision spheres, whose centres crouching swaps for the
// crouch set.
rf::Vector3 trail_anchor(const rf::Entity* ep)
{
    float lowest = -ep->radius;
    for (int i = 0; i < ep->p_data.cspheres.size(); ++i) {
        const rf::PCollisionSphere& s = ep->p_data.cspheres[i];
        const float y = ep->orient.transform_vector(s.center).y - s.radius;
        lowest = i == 0 ? y : std::min(lowest, y);
    }
    return {ep->pos.x, ep->pos.y + lowest + trail_ankle_height, ep->pos.z};
}

void trail_push(HasteTrail& trail, const rf::Vector3& anchor)
{
    if (trail.count == trail_max_samples) {
        trail.first = (trail.first + 1) % trail_max_samples;
        --trail.count;
    }
    TrailSample& s = trail.sample(trail.count++);
    s.lines = {anchor - trail.side_dir * trail_side_offset, anchor + trail.side_dir * trail_side_offset};
    s.age = 0.0f;
    s.joins_previous = trail.live;
    trail.live = true;
}

// A sample every trail_sample_spacing of travel covered faster than the multiplayer run speed, so a hasted run or a
// forward jump shows and a crouch run does not.
void trail_update(HasteTrail& trail, rf::Entity* ep, float dt)
{
    if (rf::entity_is_dying(ep)) {
        trail.reset();
        return;
    }
    if (!trail_should_sample(ep)) {
        trail.live = false;
        trail.has_ref = false;
        return;
    }
    const rf::Vector3 anchor = trail_anchor(ep);
    if (!trail.has_ref || (anchor - trail.ref_pos).len() > trail_teleport_dist) {
        if (trail.has_ref) {
            trail.reset();
        }
        trail.ref_pos = anchor;
        trail.ref_time = 0.0f;
        trail.has_ref = true;
        return;
    }
    trail.ref_time += dt;
    rf::Vector3 move = anchor - trail.ref_pos;
    move.y = 0.0f;
    const float dist = move.len();
    // entity_set_speed (0x00427450) gives every multiplayer class this standing run speed, whatever its info->max_vel.
    const float min_speed = rf::multiplayer_walk_speed * trail_min_speed_factor;
    if (dist < trail_sample_spacing) {
        if (trail.ref_time * min_speed > trail_sample_spacing) {
            trail.live = false;
            trail.ref_pos = anchor;
            trail.ref_time = 0.0f;
        }
        return;
    }
    if (dist >= min_speed * trail.ref_time) {
        trail.side_dir = rf::Vector3{move.z, 0.0f, -move.x} * (1.0f / dist);
        trail_push(trail, anchor);
    }
    else {
        trail.live = false;
    }
    trail.ref_pos = anchor;
    trail.ref_time = 0.0f;
}

// A camera-facing ribbon along the first n stroke points.
void trail_ribbon(int n, float half_width, const rf::gr::Color& color)
{
    gr_camera_facing_sides(n, g_stroke_pos.data(), half_width, g_stroke_side.data());
    gr_band_strip(n, g_stroke_pos.data(), g_stroke_side.data(), g_stroke_alpha.data(), trail_band, color,
                  additive_vertex_color_3d);
}

} // namespace

void haste_trails_clear()
{
    g_trails.clear();
}

void haste_trails_do_frame()
{
    if (g_trails.empty() && !powerup_any_active(AlpinePowerup::haste)) {
        return;
    }
    if (!rf::is_multi || rf::is_dedicated_server || is_headless_mode() || !g_alpine_game_config.rendering_enabled) {
        haste_trails_clear();
        return;
    }
    // Runs from rf_do_frame, outside the sim frame the demo pause freezes. A seek clears the trails as it lands.
    if (rf::game_paused || demo_playback_is_seeking()) {
        return;
    }
    const float dt = cosmetic_frame_dt();
    if (!(dt > 0.0f)) {
        return;
    }

    for (HasteTrail& trail : g_trails) {
        trail.seen = false;
        for (int i = 0; i < trail.count; ++i) {
            trail.sample(i).age += dt;
        }
        while (trail.count > 0 && trail.sample(0).age >= trail_lifetime) {
            trail.first = (trail.first + 1) % trail_max_samples;
            --trail.count;
        }
    }
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        rf::Entity* ep = rf::entity_from_handle(player.entity_handle);
        if (!ep) {
            continue;
        }
        auto it = std::ranges::find(g_trails, ep->handle, &HasteTrail::entity_handle);
        if (it == g_trails.end()) {
            if (!trail_should_sample(ep)) {
                continue;
            }
            it = g_trails.insert(g_trails.end(), HasteTrail{.entity_handle = ep->handle});
        }
        it->seen = true;
        trail_update(*it, ep, dt);
    }
    std::erase_if(g_trails, [](const HasteTrail& trail) {
        return !trail.seen || (trail.count == 0 && !trail.has_ref);
    });
}

void haste_trails_render()
{
    if (g_trails.empty() || !rf::is_multi) {
        return;
    }
    const rf::Vector3 eye = rf::gr::eye_pos;
    const float far_clip = level_projection_far();
    const rf::gr::Color& color = powerup_color(AlpinePowerup::haste);
    bool texture_cleared = false;
    for (const HasteTrail& trail : g_trails) {
        rf::Entity* ep = rf::entity_from_handle(trail.entity_handle);
        if (!ep || trail.count == 0 || (ep->obj_flags & rf::OF_HIDDEN) || multi_spectate_view_is_first_person_of(ep)) {
            continue;
        }
        const rf::Vector3 anchor = trail_anchor(ep);
        // Not fogged, so faded by hand, and dropped past the far plane, where it would depth-test against the
        // cleared sky.
        const float d = (anchor - eye).len();
        if (d >= far_clip) {
            continue;
        }
        const float fade = gr_fog_fade(d);
        if (!(fade > 0.0f)) {
            continue;
        }
        const float half_width =
            gr_floored_half_width(gr_pixels_per_meter(anchor), trail_min_half_width, trail_min_half_px);
        if (!texture_cleared) {
            rf::gr::set_texture(-1, -1);
            texture_cleared = true;
        }
        const bool head = trail.live && trail_should_sample(ep);
        for (int line = 0; line < 2; ++line) {
            int n = 0;
            for (int i = 0; i < trail.count; ++i) {
                const TrailSample& s = trail.sample(i);
                if (!s.joins_previous && n > 1) {
                    trail_ribbon(n, half_width, color);
                }
                if (!s.joins_previous) {
                    n = 0;
                }
                // Eased out with age, and clear at each stroke's tail.
                const float t = 1.0f - std::min(s.age / trail_lifetime, 1.0f);
                g_stroke_pos[n] = s.lines[line];
                g_stroke_alpha[n] = n == 0 ? 0.0f : trail_alpha * fade * t * t;
                ++n;
            }
            if (head) {
                const float offset = line == 0 ? -trail_side_offset : trail_side_offset;
                g_stroke_pos[n] = anchor + trail.side_dir * offset;
                g_stroke_alpha[n] = n == 0 ? 0.0f : trail_alpha * fade;
                ++n;
            }
            if (n > 1) {
                trail_ribbon(n, half_width, color);
            }
        }
    }
}
