#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <optional>
#include <random>
#include <unordered_map>
#include <xlog/xlog.h>
#include <common/utils/list-utils.h>
#include "vehicle.h"
#include "vehicle_fx.h"
#include "vehicle_internal.h"
#include "vehicle_physics.h"
#include "../demo/demo.h"
#include "../../graphics/weather.h"
#include "../../main/main.h"
#include "../../misc/alpine_settings.h"
#include "../../misc/level.h"
#include "../../os/console.h"
#include "../../rf/bmpman.h"
#include "../../rf/collide.h"
#include "../../rf/entity.h"
#include "../../rf/geometry.h"
#include "../../rf/multi.h"
#include "../../rf/object.h"
#include "../../rf/os/console.h"
#include "../../rf/os/frametime.h"
#include "../../rf/particle_emitter.h"
#include "../../rf/physics.h"
#include "../../rf/player/camera.h"
#include "../../rf/player/player.h"

namespace
{
    // Every emitter's particles live in pool 1; the effects leave most of it to the level's own.
    constexpr int fx_particle_pool = 1;
    constexpr int fx_max_live_particles = 400;
    constexpr float fx_pool_headroom = 0.8f;
    // The engine's emitter records serve everything, entity fire (four per burning entity) and the
    // flamethrower included, and those silently go without when none is free.
    constexpr int fx_max_emitters = 40;
    constexpr int fx_min_free_engine_emitters = 32;
    // An emitter with nothing left to draw is handed back after this long.
    constexpr float fx_emitter_idle_s = 1.0f;

    // Beyond these camera distances a hull emits nothing (smoke and fire carry further).
    constexpr float fx_near_dist = 40.0f;
    constexpr float fx_far_dist = 120.0f;
    constexpr float fx_smoke_far_dist = 200.0f;

    constexpr float fx_probe_interval = 0.05f;
    // A step faster than any hull moves is a placement or a network correction, not driving.
    constexpr float fx_max_speed = 80.0f;

    constexpr float fx_impact_min_dv = 6.0f;
    constexpr float fx_impact_full_dv = 20.0f;
    constexpr float fx_impact_cooldown = 0.4f;
    constexpr float fx_impact_miss_cooldown = 0.1f;
    // A landing needs the hull to have been off the ground this recently; the foot of a slope is not one.
    constexpr float fx_landing_airborne_window = 0.3f;
    constexpr float fx_ground_slack = 0.5f;

    constexpr float fx_downwash_height = 10.0f;
    // particle_create applies gravity as gravity_scale * 9.8 (0x00589858).
    constexpr float fx_gravity = 9.8f;

    enum FxBitmap
    {
        FXB_SMOKE,
        FXB_DARK_SMOKE,
        FXB_SPARK,
        FXB_FIRE,
        FXB_BUBBLE,
        FXB_COUNT,
    };

    // Stock bitmaps the stock emitters.tbl already uses.
    constexpr const char* fx_bitmap_filenames[FXB_COUNT] = {
        "smoketest.tga",
        "smokedark.tga",
        "Flame01_02.tga",
        "Fire01.tga",
        "tinybubbles_A.tga",
    };

    struct ParticleSpec
    {
        FxBitmap bitmap;
        rf::Color clr;
        rf::Color clr_dest;
        float min_radius;
        float max_radius;
        float growth_rate;
        float min_life;
        float max_life;
        float gravity_scale; // 0: no gravity
        float acceleration;  // 0: speed kept
        unsigned flags;      // on top of colour fade, gravity and acceleration
    };

    constexpr ParticleSpec spark_spec{
        FXB_SPARK, {255, 255, 255, 255}, {128, 128, 128, 0}, 0.05f, 0.08f, 0.0f, 0.15f, 0.35f, 2.0f, 0.0f,
        rf::PTF_GLOW | rf::PTF_VEL_STRETCH,
    };
    // The bubble bitmap's white specks read as droplets; the stock corona has an orange core.
    constexpr ParticleSpec spray_spec{
        FXB_BUBBLE, {220, 235, 255, 220}, {220, 235, 255, 0}, 0.12f, 0.25f, -0.1f, 0.4f, 0.8f, 1.0f, 0.0f,
        rf::PTF_GLOW | rf::PTF_RANDOM_ORIENT,
    };
    constexpr ParticleSpec mist_spec{
        FXB_SMOKE, {225, 235, 245, 110}, {225, 235, 245, 0}, 0.3f, 0.6f, 1.5f, 0.8f, 1.4f, -0.05f, -1.5f,
        rf::PTF_GLOW | rf::PTF_RANDOM_ORIENT,
    };
    constexpr ParticleSpec smoke_spec{
        FXB_SMOKE, {120, 120, 120, 90}, {90, 90, 90, 0}, 0.3f, 0.5f, 0.4f, 1.2f, 2.0f, -0.12f, -1.0f,
        rf::PTF_RANDOM_ORIENT,
    };
    constexpr ParticleSpec dark_smoke_spec{
        FXB_DARK_SMOKE, {255, 255, 255, 120}, {255, 255, 255, 0}, 0.4f, 0.6f, 0.5f, 1.4f, 2.2f, -0.15f, -1.0f,
        rf::PTF_RANDOM_ORIENT,
    };
    constexpr ParticleSpec fire_spec{
        FXB_FIRE, {255, 255, 255, 170}, {255, 255, 255, 0}, 0.25f, 0.45f, -0.3f, 0.3f, 0.7f, -0.6f, -3.0f,
        rf::PTF_GLOW | rf::PTF_RANDOM_ORIENT,
    };
    constexpr ParticleSpec bubble_spec{
        FXB_BUBBLE, {255, 255, 255, 160}, {255, 255, 255, 0}, 0.05f, 0.12f, 0.0f, 1.0f, 2.0f, -0.5f, 0.0f, 0,
    };

    constexpr int fx_num_layers = FXB_COUNT * 2;

    int fx_layer(const ParticleSpec& s)
    {
        return s.bitmap * 2 + ((s.flags & rf::PTF_GLOW) ? 1 : 0);
    }

    // How a surface answers a wheel or a blow. Dust colour is the start colour; it fades to clear.
    struct SurfaceFx
    {
        float dust;  // 0 kicks up nothing
        rf::Color dust_clr;
        bool sparks; // hard enough to strike sparks
    };

    SurfaceFx surface_fx(int material)
    {
        switch (material) {
            case rf::MATERIAL_ROCK:
                return {0.8f, {200, 190, 180, 150}, true};
            case rf::MATERIAL_METAL:
                return {0.0f, {}, true};
            case rf::MATERIAL_SOLID:
                return {0.5f, {200, 200, 200, 130}, true};
            case rf::MATERIAL_SAND:
                return {1.3f, {225, 200, 160, 170}, false};
            case rf::MATERIAL_ICE:
                return {1.0f, {240, 245, 255, 170}, false};
            case rf::MATERIAL_FLESH:
            case rf::MATERIAL_WATER:
            case rf::MATERIAL_LAVA:
            case rf::MATERIAL_GLASS:
                return {0.0f, {}, false};
            default:
                return {1.0f, {210, 185, 150, 150}, false};
        }
    }

    // Dust alternates two layers. The stock smoke bitmaps are mid-grey at best, so a blended puff gives the
    // cloud its body but can only darken, and an additive one (like the stock fighter engine steam) lifts
    // it to the surface's colour.
    ParticleSpec dust_spec(const SurfaceFx& s, float size, bool haze)
    {
        rf::Color clr = haze ? s.dust_clr : rf::Color{255, 250, 240, static_cast<rf::ubyte>(s.dust_clr.alpha / 3)};
        rf::Color dest = clr;
        dest.alpha = 0;
        const unsigned flags = haze ? rf::PTF_GLOW | rf::PTF_RANDOM_ORIENT : rf::PTF_RANDOM_ORIENT;
        return {FXB_SMOKE, clr, dest, 0.25f * size, 0.45f * size, 0.8f, 0.6f, 1.1f, -0.05f, -2.0f, flags};
    }

    struct GroundProbe
    {
        bool hit = false;
        float dist = 0.0f; // from the hull origin along hull-down
        int material = rf::MATERIAL_DEFAULT;
        rf::GRoom* room = nullptr;
    };

    // What a fighter's thrust is blowing on, if anything is close enough.
    struct WashTarget
    {
        bool valid = false;
        bool water = false;
        float surface_y = 0.0f;
        int material = rf::MATERIAL_DEFAULT;
    };

    struct HullFx
    {
        // One per bitmap and blend, so each draws as a single batch: the renderer flushes on every
        // texture or mode change, and one mixed list changes on nearly every particle.
        std::array<rf::ParticleEmitter*, fx_num_layers> emitters{};
        std::array<float, fx_num_layers> emitter_idle{};
        rf::Timestamp emitter_retry;
        rf::GRoom* room = nullptr;
        uint32_t spawn_seq = 0;

        rf::Vector3 last_pos{};
        bool has_last = false;
        rf::Vector3 vel{};      // fast, what the hull is doing now
        rf::Vector3 vel_slow{}; // what it was doing a moment ago, for impacts

        float probe_timer = 0.0f;
        GroundProbe ground;
        WashTarget wash;
        float airborne_age = fx_landing_airborne_window; // since the probe last saw the hull off the ground
        float scrape_timer = 0.0f;
        float impact_cooldown = 0.0f;
        bool bottom_in_water = false;

        float dust_accum = 0.0f;
        float spray_accum = 0.0f;
        float wash_accum = 0.0f;
        float smoke_accum = 0.0f;
        float fire_accum = 0.0f;
        float bubble_accum = 0.0f;
    };

    std::unordered_map<int, HullFx> g_hull_fx;
    int g_fx_bitmaps[FXB_COUNT] = {-1, -1, -1, -1, -1};
    bool g_fx_bitmaps_loaded = false;
    int g_fx_live_particles = 0;
    int g_fx_num_emitters = 0;
    // This frame's simulated time: a paused demo stops the particles, and a demo timescale moves the hulls.
    float g_fx_dt = 0.0f;

    float fx_rand(float lo, float hi)
    {
        return std::uniform_real_distribution<float>{lo, hi}(g_rng);
    }

    rf::Vector3 fx_rand_dir()
    {
        rf::Vector3 v{fx_rand(-1.0f, 1.0f), fx_rand(-1.0f, 1.0f), fx_rand(-1.0f, 1.0f)};
        v.normalize_safe();
        return v;
    }

    // Random, but never into the surface it came off.
    rf::Vector3 fx_rand_dir_off(const rf::Vector3& normal)
    {
        rf::Vector3 v = fx_rand_dir();
        const float d = v.dot_prod(normal);
        if (d < 0.0f) {
            v -= normal * (2.0f * d);
        }
        return v;
    }

    // Whole spawns this frame from a rate; the remainder carries to the next frame.
    int fx_take(float& accum, float rate)
    {
        accum += std::max(rate, 0.0f) * g_fx_dt;
        const int n = static_cast<int>(accum);
        accum -= static_cast<float>(n);
        return n;
    }

    // Alternates spawn sites across frames: at these rates one frame rarely spawns more than one.
    uint32_t fx_next(HullFx& fx)
    {
        return fx.spawn_seq++;
    }

    void fx_load_bitmaps()
    {
        if (g_fx_bitmaps_loaded) {
            return;
        }
        g_fx_bitmaps_loaded = true;
        for (int i = 0; i < FXB_COUNT; ++i) {
            g_fx_bitmaps[i] = rf::bm::load(fx_bitmap_filenames[i], -1, true);
            if (g_fx_bitmaps[i] < 0) {
                xlog::warn("[vehicle_fx] failed to load bitmap '{}'", fx_bitmap_filenames[i]);
            }
        }
    }

    bool fx_budget_left()
    {
        const rf::ParticlePool& pool = rf::particle_pools[fx_particle_pool];
        return g_fx_live_particles < fx_max_live_particles
            && pool.num_live < static_cast<int>(static_cast<float>(pool.capacity) * fx_pool_headroom);
    }

    // Created on the first particle a layer actually spawns, so a quiet hull holds no engine record.
    rf::ParticleEmitter* fx_ensure_emitter(HullFx& fx, int layer)
    {
        rf::ParticleEmitter*& emitter = fx.emitters[layer];
        if (emitter) {
            return emitter;
        }
        if (!fx.room || g_fx_num_emitters >= fx_max_emitters
            || rf::particle_emitter_num_used >= rf::particle_emitter_capacity - fx_min_free_engine_emitters
            || (fx.emitter_retry.valid() && !fx.emitter_retry.elapsed())) {
            return nullptr;
        }
        // A container only: never switched on and never updated, so it spawns nothing itself. Its
        // particles come from particle_create, and the engine simulates, culls and draws them as its own.
        rf::ParticleEmitterType type{};
        type.min_spawn_delay = 1.0f;
        type.max_spawn_delay = 1.0f;
        type.bitmap = g_fx_bitmaps[FXB_SMOKE];
        type.num_frames = 1;
        rf::Vector3 pos = fx.last_pos;
        // Unparented and carried by hand: particle_emitter_create never links an emitter for ticking.
        emitter = rf::particle_emitter_create(0, type, fx.room, pos, false);
        if (!emitter) {
            fx.emitter_retry.set(1000);
            return nullptr;
        }
        emitter->pos = fx.last_pos;
        emitter->room = fx.room;
        fx.emitter_idle[layer] = 0.0f;
        ++g_fx_num_emitters;
        return emitter;
    }

    // Destroying hands the live particles to the engine's orphan list, which still simulates and
    // draws them, so nothing on screen is cut short.
    void fx_destroy_emitter(HullFx& fx, int layer)
    {
        if (rf::ParticleEmitter*& emitter = fx.emitters[layer]) {
            emitter->destroy();
            emitter = nullptr;
            --g_fx_num_emitters;
        }
    }

    void fx_destroy_emitters(HullFx& fx)
    {
        for (int layer = 0; layer < fx_num_layers; ++layer) {
            fx_destroy_emitter(fx, layer);
        }
    }

    void fx_spawn(HullFx& fx, const ParticleSpec& s, const rf::Vector3& pos, const rf::Vector3& vel,
                  float size = 1.0f, float max_life = 0.0f)
    {
        if (g_fx_bitmaps[s.bitmap] < 0 || !fx_budget_left()) {
            return;
        }
        rf::ParticleEmitter* const emitter = fx_ensure_emitter(fx, fx_layer(s));
        if (!emitter || !emitter->room) {
            return;
        }
        rf::ParticleCreateInfo pci{};
        pci.pos = pos;
        pci.vel = vel;
        pci.radius = fx_rand(s.min_radius, s.max_radius) * size;
        pci.growth_rate = s.growth_rate * size;
        pci.acceleration = s.acceleration;
        pci.gravity_scale = s.gravity_scale;
        pci.lifetime_seconds = fx_rand(s.min_life, s.max_life);
        if (max_life > 0.0f) {
            pci.lifetime_seconds = std::min(pci.lifetime_seconds, max_life);
        }
        pci.bitmap_handle = g_fx_bitmaps[s.bitmap];
        pci.num_frames = 1;
        pci.clr = s.clr;
        pci.clr_dest = s.clr_dest;
        pci.flags = s.flags | rf::PTF_CLR_CHANGE;
        if (s.gravity_scale != 0.0f) {
            pci.flags |= rf::PTF_GRAVITY;
        }
        if (s.acceleration != 0.0f) {
            pci.flags |= rf::PTF_ACCELERATE;
        }
        // Decoration only: never a damaging particle.
        pci.flags2 = 0;
        // Linked into the hull's emitter, so it is drawn whenever that emitter's room is.
        rf::particle_create(fx_particle_pool, pci, emitter->room, &pci.pos, 0, nullptr, emitter);
        ++g_fx_live_particles;
    }

    // A bubble that pops at the surface instead of rising out of it.
    void fx_bubble(HullFx& fx, const rf::Vector3& pos, const rf::Vector3& vel, float liquid_y)
    {
        const float depth = liquid_y - pos.y;
        if (depth <= 0.05f) {
            return;
        }
        const float rise_accel = -bubble_spec.gravity_scale * fx_gravity;
        const float time_to_surface = (std::sqrt(vel.y * vel.y + 2.0f * rise_accel * depth) - vel.y) / rise_accel;
        fx_spawn(fx, bubble_spec, pos, vel, 1.0f, 0.9f * time_to_surface);
    }

    void fx_burst_off(HullFx& fx, const ParticleSpec& s, int count, const rf::Vector3& pos, const rf::Vector3& normal,
                      float speed)
    {
        for (int i = 0; i < count; ++i) {
            fx_spawn(fx, s, pos, fx_rand_dir_off(normal) * (speed * fx_rand(0.4f, 1.0f)));
        }
    }

    // A ring on the surface at `center`, thrown outward and a little up.
    void fx_ring(HullFx& fx, const ParticleSpec& s, int count, const rf::Vector3& center, float radius,
                 float out_speed, float up_speed, float size = 1.0f)
    {
        for (int i = 0; i < count; ++i) {
            const float a = fx_rand(0.0f, 6.2831853f);
            const rf::Vector3 out{std::cos(a), 0.0f, std::sin(a)};
            const rf::Vector3 pos = center + out * (radius * fx_rand(0.3f, 1.0f)) + rf::Vector3{0.0f, 0.1f, 0.0f};
            const rf::Vector3 vel =
                out * (out_speed * fx_rand(0.5f, 1.0f)) + rf::Vector3{0.0f, up_speed * fx_rand(0.4f, 1.0f), 0.0f};
            fx_spawn(fx, s, pos, vel, size);
        }
    }

    bool fx_cast(const rf::Vector3& from, const rf::Vector3& to, rf::PCollisionOut& out)
    {
        return vphys_collide_solid_segment(from, to, rf::CF_PROCESS_INVISIBLE_FACES, out);
    }

    rf::GRoom* fx_hit_room(const rf::PCollisionOut& o)
    {
        return o.hit_face ? o.hit_face->which_room : nullptr;
    }

    // The liquid surface around the hull, from its room or the room the ground probe hit.
    std::optional<float> fx_liquid_surface(const rf::Entity& ep, const GroundProbe& ground)
    {
        float y = 0.0f;
        if (room_liquid_surface_y(ep.room, y)) {
            return y;
        }
        if (ground.hit && room_liquid_surface_y(ground.room, y)) {
            return y;
        }
        return std::nullopt;
    }

    void fx_probe_ground(rf::Entity& ep, HullFx& fx)
    {
        const rf::Vector3 down = ep.orient.uvec * -1.0f;
        const float reach_down = vehicle_collision_reach_along(&ep, down);
        rf::PCollisionOut o{};
        GroundProbe g;
        if (fx_cast(ep.pos, ep.pos + down * (reach_down + 1.5f), o)) {
            g.hit = true;
            g.dist = (o.hit_point - ep.pos).dot_prod(down);
            g.material = o.material;
            g.room = fx_hit_room(o);
        }
        fx.ground = g;
        if (!g.hit || g.dist > reach_down + fx_ground_slack) {
            fx.airborne_age = 0.0f;
        }
    }

    bool fx_grounded(rf::Entity& ep, const HullFx& fx)
    {
        return fx.ground.hit
            && fx.ground.dist <= vehicle_collision_reach_along(&ep, ep.orient.uvec * -1.0f) + fx_ground_slack;
    }

    void fx_probe_wash(rf::Entity& ep, HullFx& fx)
    {
        fx.wash = {};
        const float reach_down = vehicle_collision_reach_along(&ep, rf::Vector3{0.0f, -1.0f, 0.0f});
        rf::PCollisionOut o{};
        const bool hit = fx_cast(ep.pos, ep.pos - rf::Vector3{0.0f, reach_down + fx_downwash_height, 0.0f}, o);
        // The ray passes through water and stops on the bed, whose room carries the surface.
        float liquid_y = 0.0f;
        if ((room_liquid_surface_y(ep.room, liquid_y) || (hit && room_liquid_surface_y(fx_hit_room(o), liquid_y)))
            && liquid_y < ep.pos.y && (!hit || liquid_y > o.hit_point.y)) {
            fx.wash = {true, true, liquid_y, rf::MATERIAL_WATER};
        }
        else if (hit) {
            fx.wash = {true, false, o.hit_point.y, o.material};
        }
    }

    bool fx_local_player_rides(const rf::Entity& ep)
    {
        return rf::local_player_entity && rf::local_player_entity->host_handle == ep.handle;
    }

    void fx_impact(rf::Entity& ep, HullFx& fx, const rf::PCollisionOut& hit, float strength, float lod,
                   std::optional<float> water_y)
    {
        if (water_y) {
            const rf::Vector3 surface{hit.hit_point.x, *water_y, hit.hit_point.z};
            fx_ring(fx, spray_spec, static_cast<int>((12.0f + 30.0f * strength) * lod), surface, 2.0f,
                    3.0f + 4.0f * strength, 3.0f + 4.0f * strength);
            fx_ring(fx, mist_spec, static_cast<int>((4.0f + 8.0f * strength) * lod), surface, 2.0f, 1.5f, 1.0f, 1.5f);
        }
        else {
            const SurfaceFx s = surface_fx(hit.material);
            if (s.dust > 0.0f) {
                const int count = static_cast<int>((3.0f + 9.0f * strength) * s.dust * lod);
                for (const bool haze : {false, true}) {
                    fx_ring(fx, dust_spec(s, 1.0f + strength, haze), count, hit.hit_point, 2.0f,
                            2.0f + 4.0f * strength, 0.5f + 1.0f * strength);
                }
            }
            if (s.sparks) {
                fx_burst_off(fx, spark_spec, static_cast<int>((6.0f + 18.0f * strength) * lod), hit.hit_point,
                             hit.hit_normal, 6.0f + 8.0f * strength);
            }
        }
        if (fx_local_player_rides(ep) && rf::local_player && rf::local_player->cam) {
            rf::camera_shake(rf::local_player->cam, 0.002f + 0.006f * strength, 0.35f);
        }
    }

    // A sudden loss of the speed the hull had a moment ago: a landing or a wall.
    void fx_detect_impacts(rf::Entity& ep, HullFx& fx, bool flyer, float lod, std::optional<float> liquid_y)
    {
        fx.impact_cooldown = std::max(fx.impact_cooldown - g_fx_dt, 0.0f);
        if (fx.impact_cooldown > 0.0f) {
            return;
        }
        const bool was_airborne = fx.airborne_age < fx_landing_airborne_window;
        const float land_dv = was_airborne && fx.vel_slow.y < 0.0f ? std::max(fx.vel.y - fx.vel_slow.y, 0.0f) : 0.0f;
        const rf::Vector3 slow_h{fx.vel_slow.x, 0.0f, fx.vel_slow.z};
        const float slow_h_speed = slow_h.len();
        float wall_dv = 0.0f;
        rf::Vector3 wall_dir{};
        if (slow_h_speed > fx_impact_min_dv) {
            wall_dir = slow_h * (1.0f / slow_h_speed);
            wall_dv = slow_h_speed - std::max(rf::Vector3{fx.vel.x, 0.0f, fx.vel.z}.dot_prod(wall_dir), 0.0f);
        }
        const float dv = std::max(land_dv, wall_dv);
        if (dv < fx_impact_min_dv) {
            return;
        }
        const float strength =
            std::clamp((dv - fx_impact_min_dv) / (fx_impact_full_dv - fx_impact_min_dv), 0.15f, 1.0f);

        const rf::Vector3 dir = land_dv >= wall_dv ? rf::Vector3{0.0f, -1.0f, 0.0f} : wall_dir;
        rf::PCollisionOut o{};
        if (!fx_cast(ep.pos, ep.pos + dir * (vehicle_collision_reach_along(&ep, dir) + 1.5f), o)) {
            fx.impact_cooldown = fx_impact_miss_cooldown;
            return;
        }
        const bool water = liquid_y && o.hit_point.y <= *liquid_y + 0.3f;
        fx.impact_cooldown = fx_impact_cooldown;
        fx.vel_slow = fx.vel;
        // A ground hull's water entry is fx_plunge's.
        if (water && !flyer) {
            return;
        }
        fx_impact(ep, fx, o, strength, lod, water ? liquid_y : std::nullopt);
    }

    // Body contact while moving along a wall (and, for flyers, a ceiling, or a floor while airborne).
    void fx_scrape(rf::Entity& ep, HullFx& fx, bool flyer, float lod)
    {
        fx.scrape_timer -= g_fx_dt;
        if (fx.scrape_timer > 0.0f) {
            return;
        }
        fx.scrape_timer = fx_probe_interval;
        if (fx.vel.len() < 3.0f) {
            return;
        }
        const rf::Vector3 dirs[] = {ep.orient.rvec, ep.orient.rvec * -1.0f, ep.orient.uvec, ep.orient.uvec * -1.0f};
        // A fighter resting or taxiing on its belly is not scraping it.
        const int num_dirs = !flyer ? 2 : fx_grounded(ep, fx) ? 3 : 4;
        for (int i = 0; i < num_dirs; ++i) {
            const rf::Vector3& dir = dirs[i];
            rf::PCollisionOut o{};
            if (!fx_cast(ep.pos, ep.pos + dir * (vehicle_collision_reach_along(&ep, dir) + 0.15f), o)) {
                continue;
            }
            // A side ray tipped into the ground by a rolled hull finds the floor, not a wall.
            if (i < 2 && o.hit_normal.dot_prod(ep.orient.uvec) > 0.5f) {
                continue;
            }
            const rf::Vector3 along = fx.vel - o.hit_normal * fx.vel.dot_prod(o.hit_normal);
            const float along_speed = along.len();
            if (along_speed < 3.0f) {
                continue;
            }
            const SurfaceFx s = surface_fx(o.material);
            const int count = static_cast<int>(std::min(along_speed * 0.15f, 3.0f) * lod + 0.5f);
            if (s.sparks) {
                for (int k = 0; k < count; ++k) {
                    const rf::Vector3 vel = along * -fx_rand(0.2f, 0.5f) + o.hit_normal * fx_rand(1.0f, 3.0f)
                                          + fx_rand_dir_off(o.hit_normal) * 1.5f;
                    fx_spawn(fx, spark_spec, o.hit_point, vel);
                }
            }
            else if (s.dust > 0.0f) {
                fx_spawn(fx, dust_spec(s, 0.8f, fx_next(fx) % 2 != 0), o.hit_point,
                         o.hit_normal * 0.8f + along * -0.1f);
            }
        }
    }

    // Wheels and tracks: dust behind them on dry ground, spray where they run through water.
    void fx_ground_contact(rf::Entity& ep, HullFx& fx, float lod, std::optional<float> liquid_y)
    {
        const float reach_down = vehicle_collision_reach_along(&ep, ep.orient.uvec * -1.0f);
        const rf::Vector3 vel_h{fx.vel.x, 0.0f, fx.vel.z};
        const float speed = vel_h.len();
        const float side_vel = fx.vel.dot_prod(ep.orient.rvec);

        const float reach_r = vehicle_collision_reach_along(&ep, ep.orient.rvec) * 0.75f;
        const float reach_l = vehicle_collision_reach_along(&ep, ep.orient.rvec * -1.0f) * 0.75f;
        const float reach_b = vehicle_collision_reach_along(&ep, ep.orient.fvec * -1.0f) * 0.8f;
        const float reach_f = vehicle_collision_reach_along(&ep, ep.orient.fvec) * 0.8f;
        const rf::Vector3 bottom = ep.pos - ep.orient.uvec * (reach_down * 0.9f);
        // Rear right, rear left, front right, front left.
        const rf::Vector3 contacts[] = {
            bottom + ep.orient.rvec * reach_r - ep.orient.fvec * reach_b,
            bottom - ep.orient.rvec * reach_l - ep.orient.fvec * reach_b,
            bottom + ep.orient.rvec * reach_r + ep.orient.fvec * reach_f,
            bottom - ep.orient.rvec * reach_l + ep.orient.fvec * reach_f,
        };

        if (liquid_y && bottom.y < *liquid_y) {
            fx.dust_accum = 0.0f;
            // Fording: the wheels are under the surface but the hull is not. Deeper, nothing shows.
            if (ep.pos.y <= *liquid_y - 0.5f) {
                fx.spray_accum = 0.0f;
                return;
            }
            const int n = fx_take(fx.spray_accum, std::clamp((speed - 1.5f) * 4.0f, 0.0f, 50.0f) * lod);
            for (int i = 0; i < n; ++i) {
                const uint32_t seq = fx_next(fx);
                const rf::Vector3& c = contacts[seq % 4];
                const rf::Vector3 side = ep.orient.rvec * ((seq % 2) ? -1.0f : 1.0f);
                fx_spawn(fx, spray_spec, rf::Vector3{c.x, *liquid_y, c.z},
                         side * fx_rand(1.0f, 2.5f) + vel_h * 0.3f
                             + rf::Vector3{0.0f, fx_rand(2.0f, 3.5f) + speed * 0.1f, 0.0f});
            }
            return;
        }
        fx.spray_accum = 0.0f;
        if (!fx_grounded(ep, fx)) {
            fx.dust_accum = 0.0f;
            return;
        }
        const SurfaceFx s = surface_fx(fx.ground.material);
        if (s.dust <= 0.0f) {
            return;
        }
        const float rate = (std::clamp((speed - 2.0f) * 1.2f, 0.0f, 16.0f) + std::min(std::abs(side_vel) * 1.5f, 10.0f))
                         * s.dust * lod;
        const int n = fx_take(fx.dust_accum, rate);
        const float size = 1.0f + std::min(speed / 40.0f, 0.4f);
        for (int i = 0; i < n; ++i) {
            // Both layers at every wheel; rear wheels three times as often as the front, left and right alike.
            const uint32_t seq = fx_next(fx);
            const uint32_t k = seq >> 1;
            const int w = static_cast<int>(k % 8 < 6 ? k % 2 : 2 + k % 2);
            const rf::Vector3 vel = vel_h * -0.12f + rf::Vector3{0.0f, fx_rand(0.4f, 1.4f), 0.0f}
                                  + fx_rand_dir() * 0.8f + ep.orient.rvec * (side_vel * -0.2f);
            fx_spawn(fx, dust_spec(s, size, (seq & 1) != 0), contacts[w] + ep.orient.uvec * 0.3f, vel);
        }
    }

    // The fighter's thrust flattening water or blowing dust off the ground below it.
    void fx_downwash(rf::Entity& ep, HullFx& fx, float lod)
    {
        // A fighter sitting on its belly with a pilot aboard is not holding itself up on thrust.
        if (!vehicle_driver_entity(&ep) || !fx.wash.valid || fx_grounded(ep, fx)) {
            fx.wash_accum = 0.0f;
            return;
        }
        const float reach_down = vehicle_collision_reach_along(&ep, rf::Vector3{0.0f, -1.0f, 0.0f});
        const float h = ep.pos.y - reach_down - fx.wash.surface_y;
        const float closeness = std::clamp(1.0f - h / fx_downwash_height, 0.0f, 1.0f);
        const SurfaceFx s = fx.wash.water ? SurfaceFx{1.0f, {}, false} : surface_fx(fx.wash.material);
        if (closeness <= 0.0f || s.dust <= 0.0f) {
            fx.wash_accum = 0.0f;
            return;
        }
        const float intensity = closeness * closeness;
        const rf::Vector3 center{ep.pos.x, fx.wash.surface_y, ep.pos.z};
        const float radius = 1.5f + 1.5f * intensity;
        const int n = fx_take(fx.wash_accum, 45.0f * intensity * lod * s.dust);
        for (int i = 0; i < n; ++i) {
            if (!fx.wash.water) {
                fx_ring(fx, dust_spec(s, 1.2f, fx_next(fx) % 2 != 0), 1, center, radius, 3.0f + 5.0f * intensity,
                        0.4f + 0.8f * intensity);
            }
            else if (fx_next(fx) % 3 == 0) {
                fx_ring(fx, mist_spec, 1, center, radius, 3.0f + 6.0f * intensity, 1.0f + 2.0f * intensity, 1.3f);
            }
            else {
                fx_ring(fx, spray_spec, 1, center, radius, 3.0f + 6.0f * intensity, 1.0f + 2.0f * intensity);
            }
        }
    }

    // The sub: bubbles off the screws when submerged, bow spray when running on the surface.
    void fx_sub(rf::Entity& ep, HullFx& fx, float lod, std::optional<float> liquid_y)
    {
        const float speed = fx.vel.len();
        if (!liquid_y || speed < 1.0f) {
            fx.bubble_accum = 0.0f;
            fx.spray_accum = 0.0f;
            return;
        }
        const float reach_up = vehicle_collision_reach_along(&ep, ep.orient.uvec);
        if (ep.pos.y + reach_up < *liquid_y) {
            const int n = fx_take(fx.bubble_accum, (6.0f + speed * 2.0f) * lod);
            const rf::Vector3 stern =
                ep.pos - ep.orient.fvec * vehicle_collision_reach_along(&ep, ep.orient.fvec * -1.0f);
            for (int i = 0; i < n; ++i) {
                fx_bubble(fx, stern + fx_rand_dir() * 0.3f, fx.vel * 0.2f + fx_rand_dir() * 0.6f, *liquid_y);
            }
        }
        else if (ep.pos.y - reach_up < *liquid_y) {
            const int n = fx_take(fx.spray_accum, std::clamp((speed - 1.5f) * 3.0f, 0.0f, 30.0f) * lod);
            const rf::Vector3 bow = ep.pos + ep.orient.fvec * vehicle_collision_reach_along(&ep, ep.orient.fvec);
            for (int i = 0; i < n; ++i) {
                const rf::Vector3 side = ep.orient.rvec * ((fx_next(fx) % 2) ? -1.0f : 1.0f);
                fx_spawn(fx, spray_spec, rf::Vector3{bow.x, *liquid_y, bow.z},
                         side * fx_rand(1.0f, 2.5f) + rf::Vector3{0.0f, fx_rand(1.5f, 3.0f), 0.0f});
            }
        }
    }

    // A ground hull dropping into water hard enough to throw it.
    void fx_plunge(rf::Entity& ep, HullFx& fx, float lod, std::optional<float> liquid_y)
    {
        const float reach_down = vehicle_collision_reach_along(&ep, rf::Vector3{0.0f, -1.0f, 0.0f});
        const bool in = liquid_y && ep.pos.y - reach_down < *liquid_y;
        if (in && !fx.bottom_in_water && fx.vel_slow.y < -3.0f) {
            const float strength = std::clamp((-fx.vel_slow.y - 3.0f) / 12.0f, 0.2f, 1.0f);
            const rf::Vector3 surface{ep.pos.x, *liquid_y, ep.pos.z};
            fx_ring(fx, spray_spec, static_cast<int>((20.0f + 40.0f * strength) * lod), surface, 2.5f,
                    3.0f + 4.0f * strength, 4.0f + 5.0f * strength, 1.2f);
            fx_ring(fx, mist_spec, static_cast<int>((6.0f + 10.0f * strength) * lod), surface, 2.5f, 1.5f, 1.5f, 1.6f);
        }
        fx.bottom_in_water = in;
    }

    // Smoke from half health down, thick dark smoke and fire under a quarter; bubbles under water.
    void fx_damage(rf::Entity& ep, HullFx& fx, float lod, std::optional<float> liquid_y)
    {
        const float max_life = vehicle_hud_max_life(&ep);
        const float frac = max_life > 0.0f ? vehicle_hud_life(&ep) / max_life : 1.0f;
        const bool dying = rf::entity_is_dying(&ep);
        if (frac >= 0.5f && !dying) {
            fx.smoke_accum = 0.0f;
            fx.fire_accum = 0.0f;
            return;
        }
        // Aft of centre, so a first-person driver is not looking out through it.
        const rf::Vector3 vent = ep.pos + ep.orient.uvec * (vehicle_collision_reach_along(&ep, ep.orient.uvec) * 0.7f)
                               - ep.orient.fvec * (vehicle_collision_reach_along(&ep, ep.orient.fvec * -1.0f) * 0.4f);
        const float severity = dying ? 1.0f : std::clamp((0.5f - frac) / 0.5f, 0.0f, 1.0f);
        // An APC's flames have to be read against a roof three times the jeep's.
        const float hull_scale = std::clamp(ep.radius / 2.0f, 1.0f, 1.6f);
        const bool burning = dying || frac < 0.25f;
        const int n_smoke = fx_take(fx.smoke_accum, (3.0f + 5.0f * severity) * lod);
        if (liquid_y && vent.y < *liquid_y) {
            fx.fire_accum = 0.0f;
            for (int i = 0; i < n_smoke; ++i) {
                fx_bubble(fx, vent + fx_rand_dir() * 0.3f, fx_rand_dir() * 0.4f, *liquid_y);
            }
            return;
        }
        const ParticleSpec& smoke = burning ? dark_smoke_spec : smoke_spec;
        for (int i = 0; i < n_smoke; ++i) {
            fx_spawn(fx, smoke, vent + fx_rand_dir() * (0.3f * hull_scale), fx.vel * 0.5f + fx_rand_dir() * 0.4f,
                     (1.0f + 0.3f * severity) * hull_scale);
        }
        if (!burning) {
            fx.fire_accum = 0.0f;
            return;
        }
        const int n_fire = fx_take(fx.fire_accum, 10.0f * lod);
        for (int i = 0; i < n_fire; ++i) {
            fx_spawn(fx, fire_spec, vent + fx_rand_dir() * (0.35f * hull_scale),
                     fx.vel * 0.7f + rf::Vector3{0.0f, 0.5f, 0.0f}, hull_scale);
        }
    }

    int fx_count_particles(const rf::ParticleEmitter* emitter)
    {
        int n = 0;
        for (const rf::Particle* p = emitter->particle_list.next; p != &emitter->particle_list; p = p->next) {
            ++n;
        }
        return n;
    }

    bool fx_active()
    {
        return rf::is_multi && !rf::is_dedicated_server && g_alpine_game_config.vehicle_effects
            && vehicle_level_has_factories();
    }

    ConsoleCommand2 vehiclefx_cmd{
        "cl_vehiclefx",
        [](std::optional<bool> enabled) {
            g_alpine_game_config.vehicle_effects = enabled.value_or(!g_alpine_game_config.vehicle_effects);
            if (!g_alpine_game_config.vehicle_effects) {
                vehicle_fx_clear();
            }
            rf::console::print("Vehicle effects are {}", g_alpine_game_config.vehicle_effects ? "enabled" : "disabled");
        },
        "Show dust, sparks, spray, smoke and fire effects for vehicles in multiplayer",
        "cl_vehiclefx [bool]",
    };
} // namespace

void vehicle_fx_clear()
{
    for (auto& [handle, fx] : g_hull_fx) {
        fx_destroy_emitters(fx);
    }
    g_hull_fx.clear();
}

void vehicle_fx_level_init()
{
    // Emitter records outlive the level; the bitmap handles do not.
    vehicle_fx_clear();
    std::fill(std::begin(g_fx_bitmaps), std::end(g_fx_bitmaps), -1);
    g_fx_bitmaps_loaded = false;
}

void vehicle_fx_do_frame()
{
    if (!fx_active()) {
        if (!g_hull_fx.empty()) {
            vehicle_fx_clear();
        }
        return;
    }
    fx_load_bitmaps();

    g_fx_dt = rf::frametime * demo_playback_sim_time_scale();
    const float dt = g_fx_dt;
    g_fx_live_particles = 0;
    for (auto it = g_hull_fx.begin(); it != g_hull_fx.end();) {
        HullFx& fx = it->second;
        rf::Entity* ep = rf::entity_from_handle(it->first);
        if (!ep || vehicle_damage_class(ep) < 0) {
            fx_destroy_emitters(fx);
            it = g_hull_fx.erase(it);
            continue;
        }
        for (int layer = 0; layer < fx_num_layers; ++layer) {
            if (const rf::ParticleEmitter* emitter = fx.emitters[layer]) {
                const int n = fx_count_particles(emitter);
                fx.emitter_idle[layer] = n > 0 ? 0.0f : fx.emitter_idle[layer] + dt;
                if (fx.emitter_idle[layer] > fx_emitter_idle_s) {
                    fx_destroy_emitter(fx, layer);
                }
                g_fx_live_particles += n;
            }
        }
        ++it;
    }

    const rf::Vector3 cam_pos =
        rf::local_player && rf::local_player->cam ? rf::camera_get_pos(rf::local_player->cam) : rf::Vector3{};

    for (rf::Entity& entity : DoublyLinkedList{rf::entity_list}) {
        const int vdc = vehicle_damage_class(&entity);
        if (vdc < 0 || (entity.obj_flags & rf::OF_HIDDEN)) {
            continue;
        }
        HullFx& fx = g_hull_fx[entity.handle];

        const rf::Vector3 delta = entity.pos - fx.last_pos;
        const bool had_last = fx.has_last;
        fx.last_pos = entity.pos;
        fx.has_last = true;
        if (entity.room) {
            fx.room = entity.room;
        }
        for (rf::ParticleEmitter* emitter : fx.emitters) {
            if (emitter) {
                emitter->pos = entity.pos;
                emitter->room = fx.room;
            }
        }
        if (dt <= 0.0f) {
            continue;
        }
        // From the position delta, not a velocity: a coasting driverless hull reports none on the wire.
        const rf::Vector3 raw_vel = delta * (1.0f / dt);
        if (!had_last || raw_vel.len_sq() > fx_max_speed * fx_max_speed) {
            fx.vel = {};
            fx.vel_slow = {};
            fx.impact_cooldown = fx_impact_cooldown;
            continue;
        }
        fx.vel += (raw_vel - fx.vel) * (1.0f - std::exp(-dt / 0.03f));
        fx.vel_slow += (fx.vel - fx.vel_slow) * (1.0f - std::exp(-dt / 0.2f));
        fx.airborne_age += dt;

        const float dist = (entity.pos - cam_pos).len();
        if (dist > fx_smoke_far_dist) {
            continue;
        }
        const bool flyer = vdc == VDC_FIGHTER;
        const bool in_range = dist <= fx_far_dist && vdc != VDC_TURRET;
        fx.probe_timer -= dt;
        if (in_range && fx.probe_timer <= 0.0f) {
            fx.probe_timer = fx_probe_interval;
            fx_probe_ground(entity, fx);
            if (flyer) {
                fx_probe_wash(entity, fx);
            }
        }
        const std::optional<float> liquid_y = fx_liquid_surface(entity, fx.ground);

        const float lod_smoke = std::clamp(1.0f - (dist - fx_far_dist) / (fx_smoke_far_dist - fx_far_dist), 0.0f, 1.0f);
        fx_damage(entity, fx, lod_smoke, liquid_y);
        if (!in_range) {
            continue;
        }
        const float lod = std::clamp(1.0f - (dist - fx_near_dist) / (fx_far_dist - fx_near_dist), 0.3f, 1.0f);
        if (vdc == VDC_SUB) {
            fx_sub(entity, fx, lod, liquid_y);
            continue;
        }
        if (!flyer) {
            // Ahead of the impact test, which leaves a ground hull's water entry to it.
            fx_plunge(entity, fx, lod, liquid_y);
        }
        fx_detect_impacts(entity, fx, flyer, lod, liquid_y);
        fx_scrape(entity, fx, flyer, lod);
        if (flyer) {
            fx_downwash(entity, fx, lod);
        }
        else {
            fx_ground_contact(entity, fx, lod, liquid_y);
        }
    }
}

void vehicle_fx_install()
{
    vehiclefx_cmd.register_cmd();
}
