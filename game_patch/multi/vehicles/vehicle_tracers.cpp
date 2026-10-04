#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <common/utils/string-utils.h>
#include "vehicle.h"
#include "vehicle_tracers.h"
#include "vehicle_view.h"
#include "../gametype.h"
#include "../multi.h"
#include "../demo/demo.h"
#include "../../hud/hud_world.h"
#include "../../graphics/gr.h"
#include "../../misc/alpine_settings.h"
#include "../../os/console.h"
#include "../../os/os.h"
#include "../../rf/collide.h"
#include "../../rf/entity.h"
#include "../../rf/gameseq.h"
#include "../../rf/gr/gr.h"
#include "../../rf/level.h"
#include "../../rf/multi.h"
#include "../../rf/object.h"
#include "../../rf/os/console.h"
#include "../../rf/os/frametime.h"
#include "../../rf/player/player.h"
#include "../../rf/weapon.h"

namespace
{
    constexpr int tracer_pool_size = 512;
    constexpr int tracer_counter_slots = 32;
    constexpr float tracer_skip_dist = 3.0f;
    constexpr float tracer_fade_in_dist = 3.0f;
    constexpr float tracer_muzzle_max_dist = 3.0f;
    constexpr float tracer_range = 2000.0f;
    constexpr float tracer_default_far_clip = 1700.0f; // D3D11's far plane when the level sets none
    constexpr float tracer_max_frame_delta = 0.1f;
    // Past twice the frame-delta clamp, tracers would crawl and pile up rather than fly.
    constexpr int64_t tracer_render_stale_ms = static_cast<int64_t>(2.0f * tracer_max_frame_delta * 1000.0f);
    int64_t g_tracer_last_render_ms = 0;
    constexpr int tracer_trace_passes = 6;
    constexpr float tracer_team_tint = 0.3f;
    // Screen-space floors keep a streak visible at any range; the world floors govern up close.
    // The pixel floors are for 1080 lines and scale with the render height.
    constexpr float tracer_core_min_half_width = 0.01f;
    constexpr float tracer_core_min_half_px = 0.75f;
    constexpr float tracer_glow_min_half_width = 0.03f;
    constexpr float tracer_glow_min_half_px = 2.25f;
    constexpr float tracer_px_reference_height = 1080.0f;
    constexpr float tracer_glow_alpha = 0.6f;
    constexpr float tracer_glow_darken = 0.85f;
    constexpr float tracer_core_whiten = 0.65f;
    constexpr float tracer_knee = 0.35f; // share of the length from the tail at which it is near full
    constexpr float tracer_knee_alpha = 0.9f;
    constexpr float tracer_band = 0.6f; // opaque share of each ribbon's half-width

    constexpr const char* tracer_weapon_names[] = {"Jeep Gun", "APC Minigun", "Fighter Minigun", "Vauss"};
    constexpr int tracer_weapon_count = static_cast<int>(std::size(tracer_weapon_names));

    // Alpha-blended glow reads against a pale sky, the additive core against sunlit ground.
    constexpr rf::gr::Mode tracer_glow_mode{
        rf::gr::TEXTURE_SOURCE_NONE,
        rf::gr::COLOR_SOURCE_VERTEX,
        rf::gr::ALPHA_SOURCE_VERTEX,
        rf::gr::ALPHA_BLEND_ALPHA,
        rf::gr::ZBUFFER_TYPE_READ,
        rf::gr::FOG_NOT_ALLOWED,
    };
    constexpr rf::gr::Mode tracer_core_mode{
        rf::gr::TEXTURE_SOURCE_NONE,
        rf::gr::COLOR_SOURCE_VERTEX,
        rf::gr::ALPHA_SOURCE_VERTEX,
        rf::gr::ALPHA_BLEND_ALPHA_ADDITIVE,
        rf::gr::ZBUFFER_TYPE_READ,
        rf::gr::FOG_NOT_ALLOWED,
    };

    struct VehicleTracer
    {
        rf::Vector3 origin{};
        rf::Vector3 dir{};
        float end_dist = 0.0f;
        float speed = 0.0f;
        float length = 0.0f;
        float age = 0.0f;
        rf::Color color{};
        bool active = false;
        bool eye_start = false; // restart from the first rendered frame's eye, offset by eye_offset
        rf::Vector3 eye_offset{};
    };

    // One frame's geometry for a tracer, so every glow can be drawn before every core.
    struct TracerDraw
    {
        rf::Vector3 row_pos[3];
        float row_alpha[3];
        rf::Vector3 side;
        rf::Vector3 axis;
        float glow_half;
        float core_half;
        rf::Color glow;
        rf::Color core;
    };

    struct ShotCounter
    {
        int handle = -1;
        unsigned shots = 0;
        unsigned last_serial = 0;
    };

    std::array<VehicleTracer, tracer_pool_size> g_tracers{};
    std::array<TracerDraw, tracer_pool_size> g_tracer_draws{};
    int g_tracer_next = 0;
    int g_tracer_live = 0;
    std::array<ShotCounter, tracer_counter_slots> g_shot_counters{};
    unsigned g_shot_serial = 0;
    std::array<int, tracer_weapon_count> g_tracer_weapon_types{-1, -1, -1, -1};

    void tracers_clear()
    {
        g_tracers.fill(VehicleTracer{});
        g_tracer_next = 0;
        g_tracer_live = 0;
        g_shot_counters.fill(ShotCounter{});
        g_shot_serial = 0;
    }

    bool tracer_weapon(int weapon_type)
    {
        return weapon_type >= 0
            && std::ranges::find(g_tracer_weapon_types, weapon_type) != g_tracer_weapon_types.end();
    }

    // The vehicle or turret a round came from and the rider who fired it: the round's parent is the
    // hull on every MP path, or its firing-seat rider. Anyone else is not vehicle fire.
    rf::Entity* tracer_shooting_hull(rf::Entity* parent, rf::Entity** out_shooter)
    {
        if (!parent) {
            return nullptr;
        }
        rf::Entity* hull = nullptr;
        rf::Entity* shooter = nullptr;
        if (vehicle_is_synced_entity_type(parent)) {
            hull = parent;
            shooter = vehicle_firing_seat_occupant(parent);
        }
        else {
            hull = vehicle_ridden_hull(parent);
            if (!hull || vehicle_firing_seat_occupant(hull) != parent) {
                return nullptr;
            }
            shooter = parent;
        }
        *out_shooter = shooter;
        return hull;
    }

    bool tracer_counter_take(int hull_handle)
    {
        const unsigned serial = ++g_shot_serial;
        ShotCounter* slot = nullptr;
        for (ShotCounter& c : g_shot_counters) {
            if (c.handle == hull_handle) {
                slot = &c;
                break;
            }
            if (!slot || c.last_serial < slot->last_serial) {
                slot = &c;
            }
        }
        if (slot->handle != hull_handle) {
            *slot = ShotCounter{hull_handle, 0, serial};
        }
        slot->last_serial = serial;
        const unsigned freq = static_cast<unsigned>(g_alpine_game_config.vehicle_tracer_frequency);
        return (slot->shots++ % std::max(freq, 1u)) == 0;
    }

    // Distance along dir to the first thing the round would stop on. Entities are tested by mesh and
    // the hull's own riders are stepped past. 0x0049C690 clips p1 to its own level hit before its
    // entity pass, so the first pass's p1 is the level distance that bounds the rest.
    float tracer_trace(const rf::Vector3& from, const rf::Vector3& dir, rf::Entity* hull,
                       rf::Entity* shooter)
    {
        float level_dist = tracer_range;
        rf::Vector3 start = from;
        for (int pass = 0; pass < tracer_trace_passes; ++pass) {
            rf::Vector3 p0 = start;
            rf::Vector3 p1 = from + dir * level_dist;
            rf::LevelCollisionOut col{};
            col.obj_handle = -1;
            col.face = nullptr;
            const bool any_hit =
                rf::collide_linesegment_level_for_multi(p0, p1, hull, shooter, &col, 0.0f, true, 1.0f);
            if (pass == 0) {
                level_dist = std::clamp((p1 - from).dot_prod(dir), 0.0f, tracer_range);
            }
            if (!any_hit) {
                return level_dist;
            }
            const float dist = (col.hit_point - from).dot_prod(dir);
            const rf::Object* hit = col.obj_handle >= 0 ? rf::obj_from_handle(col.obj_handle) : nullptr;
            if (!hit || hit->host_handle != hull->handle) {
                return std::clamp(dist, 0.0f, level_dist);
            }
            if (dist >= level_dist) {
                break;
            }
            start = col.hit_point + dir * 0.05f;
        }
        return level_dist;
    }

    // Restarts a tracer at from, still ending where the round stops.
    bool tracer_restart(rf::Vector3& origin, rf::Vector3& dir, float& end_dist, const rf::Vector3& from)
    {
        const rf::Vector3 to_end = origin + dir * end_dist - from;
        const float len = to_end.len();
        if (!std::isfinite(from.x + from.y + from.z) || !(len > 1e-3f)) {
            return false;
        }
        origin = from;
        dir = to_end * (1.0f / len);
        end_dist = len;
        return true;
    }

    rf::Color tracer_color(const rf::Entity* hull, const rf::Entity* shooter)
    {
        const auto [r, g, b, a] = extract_color_components(g_alpine_game_config.vehicle_tracer_color);
        float rgb[3] = {static_cast<float>(r), static_cast<float>(g), static_cast<float>(b)};
        if (multi_is_team_game_type()) {
            int team = -1;
            if (const rf::Player* pp = shooter ? rf::player_from_entity_handle(shooter->handle) : nullptr) {
                team = pp->team;
            }
            if (team != rf::TEAM_RED && team != rf::TEAM_BLUE) {
                team = vehicle_hull_team(hull->handle);
            }
            if (team == rf::TEAM_RED || team == rf::TEAM_BLUE) {
                const rf::Color tc = hud_team_color(team);
                const float target[3] = {static_cast<float>(tc.red), static_cast<float>(tc.green),
                                         static_cast<float>(tc.blue)};
                for (int i = 0; i < 3; ++i) {
                    rgb[i] += (target[i] - rgb[i]) * tracer_team_tint;
                }
            }
        }
        const auto to_byte = [](float v) {
            return static_cast<rf::ubyte>(std::clamp(v + 0.5f, 0.0f, 255.0f));
        };
        return {to_byte(rgb[0]), to_byte(rgb[1]), to_byte(rgb[2]), static_cast<rf::ubyte>(a)};
    }

    void tracer_set_vertex(rf::gr::Vertex& v, const rf::Vector3& pos, const rf::Color& color, float alpha)
    {
        rf::Vector3 p = pos;
        rf::gr::rotate_vertex(&v, &p);
        v.r = color.red;
        v.g = color.green;
        v.b = color.blue;
        v.a = static_cast<rf::ubyte>(std::clamp(alpha * color.alpha + 0.5f, 0.0f, 255.0f));
    }

    // Rows along the streak - tail, knee, head, then a transparent cap one half-width past the head
    // on screen, so a streak seen end-on reads as a dot rather than a flat dash. Four columns across:
    // transparent edges, an opaque middle band.
    void tracer_ribbon(const rf::Vector3 (&row_pos)[3], const float (&row_alpha)[3], const rf::Vector3& side,
                       const rf::Vector3& cap, const rf::Color& color, rf::gr::Mode mode)
    {
        constexpr int rows = 4;
        constexpr float col_offset[4] = {-1.0f, -tracer_band, tracer_band, 1.0f};
        const rf::Vector3 pos[rows] = {row_pos[0], row_pos[1], row_pos[2], row_pos[2] + cap};
        const float alpha[rows] = {row_alpha[0], row_alpha[1], row_alpha[2], 0.0f};
        rf::gr::Vertex verts[rows * 4]{};
        for (int row = 0; row < rows; ++row) {
            for (int col = 0; col < 4; ++col) {
                const bool edge = col == 0 || col == 3;
                tracer_set_vertex(verts[row * 4 + col], pos[row] + side * col_offset[col], color,
                                  edge ? 0.0f : alpha[row]);
            }
        }
        constexpr auto vertex_attributes =
            static_cast<rf::gr::TMapperFlags>(rf::gr::TMAP_FLAG_RGB | rf::gr::TMAP_FLAG_ALPHA);
        for (int row = 0; row + 1 < rows; ++row) {
            rf::gr::Vertex* r0 = &verts[row * 4];
            rf::gr::Vertex* r1 = &verts[(row + 1) * 4];
            for (int col = 0; col < 3; ++col) {
                // Same winding as the rain streaks: side = dir x (eye - p), tail row first.
                rf::gr::Vertex* quad[4] = {&r0[col], &r0[col + 1], &r1[col + 1], &r1[col]};
                rf::gr::poly(4, quad, vertex_attributes, mode, 0, 0.0f);
            }
        }
    }

    // A near-white core over a wider halo in the tracer colour: the halo reads against a pale sky, the
    // core against the ground. Alpha ramps up from the tail, so the streak dims into its end point.
    void tracer_build(const VehicleTracer& t, float tail, float head, float draw_tail, float draw_head,
                      const rf::Vector3& eye, float fade, float px_scale, TracerDraw& out)
    {
        const rf::Vector3 p_tail = t.origin + t.dir * draw_tail;
        const rf::Vector3 p_head = t.origin + t.dir * draw_head;
        const rf::Vector3 mid = (p_tail + p_head) * 0.5f;
        const rf::Vector3 to_eye = eye - mid;
        rf::Vector3 side = t.dir.cross(to_eye);
        const float side_len = side.len();
        rf::Vector3 axis;
        if (side_len > 1e-3f * to_eye.len()) {
            side *= 1.0f / side_len;
            // The streak's direction on screen: dir without its component along the view ray.
            rf::Vector3 view = mid - eye;
            view.normalize_safe();
            axis = t.dir - view * t.dir.dot_prod(view);
            const float axis_len = axis.len();
            axis = axis_len > 1e-3f ? axis * (1.0f / axis_len) : side.cross(view);
        }
        else {
            // Seen end-on: the head cap alone shows, as a round dot facing the camera.
            side = rf::gr::eye_matrix.rvec * -1.0f;
            axis = rf::gr::eye_matrix.uvec;
        }

        float px_per_m = 0.0f;
        float mx = 0.0f, my = 0.0f, ox = 0.0f, oy = 0.0f;
        if (gr_project_world_to_screen(mid, mx, my)
            && gr_project_world_to_screen(mid + rf::gr::eye_matrix.rvec, ox, oy)) {
            px_per_m = std::hypot(ox - mx, oy - my);
        }
        const auto half_width = [px_per_m, px_scale](float min_world, float min_px) {
            return px_per_m > 1e-3f ? std::max(min_world, min_px * px_scale / px_per_m) : min_world;
        };

        const float draw_mid = draw_tail + (draw_head - draw_tail) * tracer_knee;
        const auto along = [&](float d) {
            const float s = std::clamp((d - tail) / (head - tail), 0.0f, 1.0f);
            const float ramp = s < tracer_knee ? tracer_knee_alpha * s / tracer_knee
                                               : tracer_knee_alpha + (1.0f - tracer_knee_alpha)
                                                     * (s - tracer_knee) / (1.0f - tracer_knee);
            const float fade_in = std::clamp((d - tracer_skip_dist) / tracer_fade_in_dist, 0.0f, 1.0f);
            return ramp * fade_in * fade;
        };
        out.row_pos[0] = p_tail;
        out.row_pos[1] = t.origin + t.dir * draw_mid;
        out.row_pos[2] = p_head;
        out.row_alpha[0] = along(draw_tail);
        out.row_alpha[1] = along(draw_mid);
        out.row_alpha[2] = along(draw_head);
        out.side = side;
        out.axis = axis;
        out.glow_half = half_width(tracer_glow_min_half_width, tracer_glow_min_half_px);
        out.core_half = half_width(tracer_core_min_half_width, tracer_core_min_half_px);

        const auto whiten = [](rf::ubyte c) {
            return static_cast<rf::ubyte>(c + (255 - c) * tracer_core_whiten);
        };
        out.core = {whiten(t.color.red), whiten(t.color.green), whiten(t.color.blue), t.color.alpha};
        const auto darken = [](rf::ubyte c) {
            return static_cast<rf::ubyte>(c * tracer_glow_darken);
        };
        out.glow = {darken(t.color.red), darken(t.color.green), darken(t.color.blue), t.color.alpha};
    }

    // All glows, then all cores: the two layers differ in blend mode, so interleaving them would
    // break the batch at every tracer.
    void tracers_draw(int count)
    {
        for (int i = 0; i < count; ++i) {
            const TracerDraw& d = g_tracer_draws[i];
            const float glow_alpha[3] = {d.row_alpha[0] * tracer_glow_alpha, d.row_alpha[1] * tracer_glow_alpha,
                                         d.row_alpha[2] * tracer_glow_alpha};
            tracer_ribbon(d.row_pos, glow_alpha, d.side * d.glow_half, d.axis * d.glow_half, d.glow,
                          tracer_glow_mode);
        }
        for (int i = 0; i < count; ++i) {
            const TracerDraw& d = g_tracer_draws[i];
            tracer_ribbon(d.row_pos, d.row_alpha, d.side * d.core_half, d.axis * d.core_half, d.core,
                          tracer_core_mode);
        }
    }

    ConsoleCommand2 vehicletracers_cmd{
        "cl_vehicletracers",
        [](std::optional<bool> enabled) {
            g_alpine_game_config.vehicle_tracers = enabled.value_or(!g_alpine_game_config.vehicle_tracers);
            rf::console::print("Vehicle gun tracers are {}",
                               g_alpine_game_config.vehicle_tracers ? "enabled" : "disabled");
        },
        "Show tracer streaks for vehicle guns in multiplayer",
        "cl_vehicletracers [bool]",
    };

    ConsoleCommand2 vehicletracers_frequency_cmd{
        "cl_vehicletracers_frequency",
        [](std::optional<int> value) {
            if (value) {
                g_alpine_game_config.set_vehicle_tracer_frequency(*value);
            }
            rf::console::print("Vehicle tracer frequency is every {} round(s) (default {})",
                               g_alpine_game_config.vehicle_tracer_frequency,
                               AlpineGameSettings::default_vehicle_tracer_frequency);
        },
        "Set how often a vehicle gun round shows a tracer: every Nth round of each vehicle",
        "cl_vehicletracers_frequency [1-10]",
    };

    ConsoleCommand2 vehicletracers_color_cmd{
        "cl_vehicletracers_color",
        [](std::optional<std::string> value) {
            if (value) {
                const std::string lowered = string_to_lower(*value);
                if (lowered == "default" || lowered == "clear" || lowered == "-1") {
                    g_alpine_game_config.vehicle_tracer_color = AlpineGameSettings::default_vehicle_tracer_color;
                }
                else if (const auto c = parse_hex_color_string(*value)) {
                    g_alpine_game_config.vehicle_tracer_color = *c;
                }
                else {
                    rf::console::print("Invalid color. Specify hex RRGGBB / RRGGBBAA or comma-separated "
                                       "RGB[A] values enclosed in quotes.");
                    return;
                }
            }
            rf::console::print("Vehicle tracer color is {} (default {})",
                               format_hex_color_string(g_alpine_game_config.vehicle_tracer_color),
                               format_hex_color_string(AlpineGameSettings::default_vehicle_tracer_color));
        },
        "Set the vehicle gun tracer color; team games tint it toward the firing team's color",
        "cl_vehicletracers_color [RRGGBB|RRGGBBAA|default]",
    };

    ConsoleCommand2 vehicletracers_length_cmd{
        "cl_vehicletracers_length",
        [](std::optional<float> value) {
            if (value) {
                g_alpine_game_config.set_vehicle_tracer_length(*value);
            }
            rf::console::print("Vehicle tracer length is {:.2f} m (default {:.2f})",
                               g_alpine_game_config.vehicle_tracer_length,
                               AlpineGameSettings::default_vehicle_tracer_length);
        },
        "Set the length of a vehicle gun tracer streak, in meters",
        "cl_vehicletracers_length [0.5-10.0]",
    };
} // namespace

void vehicle_tracers_level_init()
{
    tracers_clear();
    for (int i = 0; i < tracer_weapon_count; ++i) {
        g_tracer_weapon_types[i] = rf::weapon_lookup_type(tracer_weapon_names[i]);
    }
}

void vehicle_tracers_clear()
{
    tracers_clear();
}

void vehicle_tracers_on_weapon_created(int weapon_type, int parent_handle, const rf::Vector3& pos,
                                       const rf::Matrix3& orient)
{
    // Nothing ages a tracer while no frame is rendered (menus, minimize, seek), so none may be created then.
    if (!rf::is_multi || rf::is_dedicated_server || !g_alpine_game_config.vehicle_tracers
        || !tracer_weapon(weapon_type) || is_headless_mode() || !g_alpine_game_config.rendering_enabled
        || demo_playback_is_seeking() || (g_alpine_game_config.vehicle_tracer_color & 0xFF) == 0
        || timer::get_i64(1000) - g_tracer_last_render_ms > tracer_render_stale_ms) {
        return;
    }
    rf::Entity* shooter = nullptr;
    rf::Entity* hull = tracer_shooting_hull(rf::entity_from_handle(parent_handle), &shooter);
    if (!hull) {
        return;
    }
    if (!tracer_counter_take(hull->handle)) {
        return;
    }
    // Not max_speed: multi_start scales that x100 for the lag-compensated weapons.
    const float speed = rf::weapon_types[weapon_type].max_speed_multi;
    rf::Vector3 dir = orient.fvec;
    const float dir_len = dir.len();
    if (!(speed > 1.0f) || !(dir_len > 1e-4f) || !std::isfinite(pos.x + pos.y + pos.z)) {
        return;
    }
    dir *= 1.0f / dir_len;
    float end_dist = tracer_trace(pos, dir, hull, shooter);
    rf::Vector3 origin = pos;
    // A from-eye round runs down its shooter's view ray, end-on to him; his own starts where he sees the
    // muzzle on screen, or with no muzzle drawn, at the eye he sees this frame from.
    bool eye_start = false;
    rf::Vector3 eye_offset{};
    if (rf::weapon_types[weapon_type].flags & rf::WTF_FROM_EYE) {
        rf::Vector3 start;
        switch (vehicle_fp_own_shot_start(hull, shooter, pos, tracer_muzzle_max_dist, &start)) {
        case VehicleFpShotStart::world:
            tracer_restart(origin, dir, end_dist, start);
            break;
        case VehicleFpShotStart::eye_frame:
            eye_start = true;
            eye_offset = start;
            break;
        case VehicleFpShotStart::stock:
            // Everyone else sees the APC minigun's rounds leave its barrel, converging on where they land.
            if (vehicle_apc_minigun_muzzle_pos(hull, &start)
                && (start - pos).len_sq() <= tracer_muzzle_max_dist * tracer_muzzle_max_dist) {
                tracer_restart(origin, dir, end_dist, start);
            }
            break;
        }
    }
    if (end_dist <= tracer_skip_dist) {
        return;
    }

    int slot = g_tracer_next;
    if (g_tracer_live == tracer_pool_size) {
        for (int i = 0; i < tracer_pool_size; ++i) {
            if (g_tracers[i].age > g_tracers[slot].age) {
                slot = i;
            }
        }
    }
    else {
        while (g_tracers[slot].active) {
            slot = (slot + 1) % tracer_pool_size;
        }
        ++g_tracer_live;
    }
    g_tracers[slot] = VehicleTracer{origin, dir, end_dist, speed, g_alpine_game_config.vehicle_tracer_length, 0.0f,
                                    tracer_color(hull, shooter), true, eye_start, eye_offset};
    g_tracer_next = (slot + 1) % tracer_pool_size;
}

void vehicle_tracers_render()
{
    g_tracer_last_render_ms = timer::get_i64(1000);
    if (g_tracer_live == 0) {
        return;
    }
    if (!rf::is_multi || !g_alpine_game_config.vehicle_tracers) {
        tracers_clear();
        return;
    }
    float dt = 0.0f;
    if (!rf::game_paused) {
        dt = std::clamp(rf::frametime, 0.0f, tracer_max_frame_delta) * demo_playback_sim_time_scale();
        if (!(dt >= 0.0f)) {
            dt = 0.0f;
        }
    }
    const rf::Vector3 eye = rf::gr::eye_pos;
    const bool fog = rf::gr::screen.fog_mode;
    const float fog_near = rf::gr::screen.fog_near;
    const float fog_far = rf::gr::screen.fog_far;
    const float far_clip =
        rf::level.distance_fog_far_clip > 0.0f ? rf::level.distance_fog_far_clip : tracer_default_far_clip;
    const float px_scale = rf::gr::screen.clip_height / tracer_px_reference_height;
    int draw_count = 0;
    for (VehicleTracer& t : g_tracers) {
        if (!t.active) {
            continue;
        }
        // Its fire pos is the rider's eye before this frame's attach pass moved him with the hull, and his
        // fpgun muzzle is last frame's pose, so the start is placed in the eye this frame is drawn from.
        if (t.eye_start) {
            t.eye_start = false;
            if ((eye - t.origin).len_sq() <= tracer_muzzle_max_dist * tracer_muzzle_max_dist) {
                const rf::Matrix3& m = rf::gr::eye_matrix;
                const rf::Vector3& o = t.eye_offset;
                tracer_restart(t.origin, t.dir, t.end_dist, eye + m.rvec * o.x + m.uvec * o.y + m.fvec * o.z);
            }
        }
        t.age += dt;
        const float head = t.speed * t.age;
        const float tail = head - t.length;
        if (tail >= t.end_dist) {
            t.active = false;
            --g_tracer_live;
            continue;
        }
        const float draw_head = std::min(head, t.end_dist);
        const float draw_tail = std::max(tail, tracer_skip_dist);
        if (!(draw_head - draw_tail > 0.01f)) {
            continue;
        }
        // The tracer is not fogged, and past the far plane it would depth-test against the cleared sky
        // behind the hills that are no longer drawn there.
        const float d =
            std::max((t.origin + t.dir * draw_head - eye).len(), (t.origin + t.dir * draw_tail - eye).len());
        if (d >= far_clip) {
            continue;
        }
        float fade = 1.0f;
        if (fog) {
            fade = fog_far > fog_near ? std::clamp((fog_far - d) / (fog_far - fog_near), 0.0f, 1.0f)
                                      : (d < fog_far ? 1.0f : 0.0f);
        }
        if (fade > 0.0f) {
            tracer_build(t, tail, head, draw_tail, draw_head, eye, fade, px_scale, g_tracer_draws[draw_count++]);
        }
    }
    if (draw_count > 0) {
        rf::gr::set_texture(-1, -1);
        tracers_draw(draw_count);
    }
}

void vehicle_tracers_install()
{
    vehicletracers_cmd.register_cmd();
    vehicletracers_frequency_cmd.register_cmd();
    vehicletracers_color_cmd.register_cmd();
    vehicletracers_length_cmd.register_cmd();
}
