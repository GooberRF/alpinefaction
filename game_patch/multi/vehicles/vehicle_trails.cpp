#include <algorithm>
#include <array>
#include <cmath>
#include <vector>
#include "vehicle.h"
#include "vehicle_trails.h"
#include "../multi.h"
#include "../demo/demo.h"
#include "../../graphics/ribbon_trail.h"
#include "../../misc/alpine_settings.h"
#include "../../misc/level.h"
#include "../../rf/entity.h"
#include "../../rf/multi.h"
#include "../../rf/vmesh.h"
#include "../../rf/os/frametime.h"

namespace
{
    constexpr RibbonTrailStyle trail_style{
        .lifetime = 1.0f,
        .alpha = 0.35f,
        .band = 0.5f,
        .min_half_width = 0.03f,
        .min_half_px = 1.0f,
    };
    constexpr rf::gr::Color trail_color{255, 255, 255, 255};
    constexpr int trail_tips = 4;
    constexpr int trail_max_samples = 48;
    constexpr float trail_sample_spacing = 0.75f;
    // A step faster than any hull moves, and longer than an interpolation catch-up, is a placement or a network
    // correction, not flight.
    constexpr float trail_max_speed = 80.0f;
    constexpr float trail_teleport_dist = 3.0f;
    // Remote hulls move in interpolated steps, so speed and turn rate are smoothed over about this long.
    constexpr float trail_smoothing_s = 0.1f;
    // Turning this fast or faster gives the full boost, on top of the speed's strength.
    constexpr float trail_turn_full_rad_s = 1.5f;
    constexpr float trail_turn_boost = 1.0f;

    struct TrailClass
    {
        const char* class_name;
        std::array<const char*, trail_tips> tip_props;
        float min_speed; // trails start above this speed
        float full_speed; // and are at full strength from this one
    };

    // The flight model holds Fighter01 at 25 u/s and starts dragging it at 16.25.
    constexpr TrailClass trail_classes[] = {
        {"Fighter01", {"thruster_3", "thruster_4", "thruster_5", "thruster_6"}, 15.0f, 25.0f},
    };

    struct WingTrail
    {
        int entity_handle = -1;
        const TrailClass* cls = nullptr;
        const rf::VMesh* vmesh = nullptr; // the mesh `props` index
        std::array<int, trail_tips> props{};
        RibbonTrail<trail_tips, trail_max_samples> ribbon;
        rf::Vector3 last_pos{};
        rf::Vector3 sample_pos{}; // the hull's position at the newest sample
        rf::Vector3 vel{};
        float turn_rate = 0.0f;
        float strength = 0.0f;
        bool has_pos = false;
        bool seen = false;
    };

    std::vector<WingTrail> g_trails;

    bool trails_active()
    {
        return rf::is_multi && !rf::is_dedicated_server && !is_headless_mode()
            && g_alpine_game_config.rendering_enabled && g_alpine_game_config.vehicle_effects
            && vehicle_level_has_factories() && !demo_playback_is_seeking();
    }

    const TrailClass* trail_class(const rf::Entity* ep)
    {
        if (vehicle_damage_class(ep) != VDC_FIGHTER) {
            return nullptr;
        }
        for (const TrailClass& cls : trail_classes) {
            if (ep->info->name == cls.class_name) {
                return &cls;
            }
        }
        return nullptr;
    }

    // Looks up every tip's prop index whenever the hull's mesh changes.
    void trail_bind_mesh(WingTrail& trail, rf::Entity* ep)
    {
        if (trail.vmesh == ep->vmesh) {
            return;
        }
        trail.vmesh = ep->vmesh;
        for (int i = 0; i < trail_tips; ++i) {
            trail.props[i] = ep->vmesh ? rf::vmesh_lookup_prop_point(ep->vmesh, trail.cls->tip_props[i]) : -1;
        }
    }

    // False when the mesh lacks a tip, so a class's other meshes draw nothing.
    bool trail_tips_ready(const WingTrail& trail)
    {
        return trail.vmesh && std::ranges::none_of(trail.props, [](int prop) { return prop < 0; });
    }

    std::array<rf::Vector3, trail_tips> trail_tip_points(const WingTrail& trail, rf::Entity* ep)
    {
        std::array<rf::Vector3, trail_tips> points{};
        for (int i = 0; i < trail_tips; ++i) {
            rf::Matrix3 tip_orient;
            rf::vmesh_get_prop_point_transform(ep->vmesh, trail.props[i], &ep->orient, &ep->pos, &tip_orient,
                                               &points[i]);
        }
        return points;
    }

    void trail_update(WingTrail& trail, rf::Entity* ep, float dt)
    {
        trail_bind_mesh(trail, ep);
        if (rf::entity_is_dying(ep) || (ep->obj_flags & rf::OF_HIDDEN) || !trail_tips_ready(trail)) {
            trail.ribbon.live = false;
            trail.has_pos = false;
            return;
        }
        if (!trail.has_pos || (ep->pos - trail.last_pos).len() > std::max(trail_max_speed * dt, trail_teleport_dist)) {
            trail.ribbon.live = false;
            trail.has_pos = true;
            trail.last_pos = ep->pos;
            trail.sample_pos = ep->pos;
            trail.vel = {};
            trail.turn_rate = 0.0f;
            trail.strength = 0.0f;
            return;
        }
        const rf::Vector3 frame_vel = (ep->pos - trail.last_pos) * (1.0f / dt);
        trail.last_pos = ep->pos;
        const float k = std::min(dt / trail_smoothing_s, 1.0f);
        const rf::Vector3 prev_vel = trail.vel;
        trail.vel += (frame_vel - trail.vel) * k;
        const float speed = trail.vel.len();
        const float prev_speed = prev_vel.len();
        float turn = 0.0f;
        if (speed > 1.0f && prev_speed > 1.0f) {
            const float cos_turn = std::clamp(trail.vel.dot_prod(prev_vel) / (speed * prev_speed), -1.0f, 1.0f);
            turn = std::acos(cos_turn) / dt;
        }
        trail.turn_rate += (turn - trail.turn_rate) * k;

        const TrailClass& cls = *trail.cls;
        const float s = std::clamp((speed - cls.min_speed) / (cls.full_speed - cls.min_speed), 0.0f, 1.0f);
        const float boost = trail_turn_boost * std::min(trail.turn_rate / trail_turn_full_rad_s, 1.0f);
        trail.strength = s * s * (3.0f - 2.0f * s) * (1.0f + boost);
        if (!(trail.strength > 0.0f)) {
            trail.ribbon.live = false;
            return;
        }
        if (trail.ribbon.live && (ep->pos - trail.sample_pos).len() < trail_sample_spacing) {
            return;
        }
        trail.sample_pos = ep->pos;
        trail.ribbon.push(trail_tip_points(trail, ep), trail.strength);
    }
} // namespace

void vehicle_trails_clear()
{
    g_trails.clear();
}

void vehicle_trails_do_frame()
{
    if (!trails_active()) {
        vehicle_trails_clear();
        return;
    }
    const float dt = rf::frametime * demo_playback_sim_time_scale();
    if (!(dt > 0.0f)) {
        return;
    }
    for (WingTrail& trail : g_trails) {
        trail.seen = false;
        trail.ribbon.age(dt, trail_style.lifetime);
    }
    for (const int handle : vehicle_hull_handles()) {
        rf::Entity* ep = rf::entity_from_handle(handle);
        if (!ep || !ep->info) {
            continue;
        }
        auto it = std::ranges::find(g_trails, handle, &WingTrail::entity_handle);
        if (it == g_trails.end()) {
            const TrailClass* cls = trail_class(ep);
            if (!cls) {
                continue;
            }
            it = g_trails.insert(g_trails.end(), WingTrail{.entity_handle = handle, .cls = cls});
        }
        it->seen = true;
        trail_update(*it, ep, dt);
    }
    // A dead hull's trail stays until it has faded.
    std::erase_if(g_trails, [](const WingTrail& trail) { return !trail.seen && trail.ribbon.count == 0; });
}

void vehicle_trails_render()
{
    if (g_trails.empty() || !rf::is_multi) {
        return;
    }
    bool texture_cleared = false;
    for (const WingTrail& trail : g_trails) {
        if (trail.ribbon.count == 0) {
            continue;
        }
        rf::Entity* ep = trail.seen ? rf::entity_from_handle(trail.entity_handle) : nullptr;
        const bool head = ep && trail.ribbon.live && trail_tips_ready(trail) && trail.vmesh == ep->vmesh;
        const rf::Vector3 anchor = ep ? ep->pos : trail.ribbon.sample(trail.ribbon.count - 1).points[0];
        // From inside the hull, its cockpit view, the tips would start beside the eye.
        if (ep && (anchor - rf::gr::eye_pos).len() < ep->radius) {
            continue;
        }
        float fade = 0.0f;
        float half_width = 0.0f;
        if (!ribbon_trail_view(anchor, trail_style, fade, half_width)) {
            continue;
        }
        if (!texture_cleared) {
            rf::gr::set_texture(-1, -1);
            texture_cleared = true;
        }
        std::array<rf::Vector3, trail_tips> tips{};
        if (head) {
            tips = trail_tip_points(trail, ep);
        }
        ribbon_trail_render(trail.ribbon, head ? &tips : nullptr, trail.strength, trail_style, fade, half_width,
                            trail_color);
    }
}
