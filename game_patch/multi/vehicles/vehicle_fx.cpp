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
#include "vehicle_crash_tuning.h"
#include "vehicle_fx.h"
#include "vehicle_internal.h"
#include "vehicle_physics.h"
#include "../demo/demo.h"
#include "../../graphics/gr.h"
#include "../../graphics/weather.h"
#include "../../main/main.h"
#include "../../misc/alpine_settings.h"
#include "../../misc/level.h"
#include "../../object/particle_sim.h"
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
#include "../../rf/sound/sound.h"
#include "../../sound/sound.h"

namespace
{
    // Every emitter's particles live in pool 1; the effects leave most of it to the level's own.
    constexpr int fx_particle_pool = 1;
    constexpr int fx_max_live_particles = 400;
    constexpr float fx_pool_headroom = 0.8f;
    // Engine records are plentiful, but each emitter is a room render item and is walked every frame, so this
    // covers the busy layers of every hull in range and no more. At the cap, a hull near the camera takes one
    // from the farthest hull rather than going without.
    constexpr int fx_max_emitters = 96;
    // Kept free for everything else that shares the records: entity fire, the flamethrower, explosions.
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
    // How fast the remembered peak speeds fall away (u/s per second), so they cover the ~0.3 s an impact takes.
    constexpr float fx_peak_decay = 12.0f;
    // A landing needs the hull to have been off the ground this recently; the foot of a slope is not one.
    constexpr float fx_landing_airborne_window = 0.3f;
    constexpr float fx_ground_slack = 0.5f;

    constexpr float fx_downwash_height = 10.0f;
    // particle_create applies gravity as gravity_scale * 9.8 (0x00589858).
    constexpr float fx_gravity = 9.8f;

    constexpr float fx_heavy_impact = 0.5f;
    // Rock chips come from the engine's shared pool of 80 geomod rocks, and each burst also relaunches the
    // rocks around it and recycles the oldest once the pool is full, so bursts are rationed across all hulls.
    constexpr float fx_debris_min_strength = 0.4f;
    constexpr float fx_debris_cooldown = 1.0f;
    constexpr float fx_debris_global_cooldown = 0.5f;

    enum FxSound
    {
        fx_sound_impact_metal,
        fx_sound_impact_rock,
        fx_sound_impact_glass,
        fx_sound_splash_small,
        fx_sound_splash_medium,
        fx_sound_splash_large,
        fx_sound_splash_huge,
        fx_sound_spark,
        fx_sound_burning,
        fx_sound_count,
    };

    constexpr const char* fx_foley_names[fx_sound_count] = {
        "Riot Impact Metal",
        "Riot Impact Rock",
        "Glass Land",
        "Small Water Splash",
        "Medium Water Splash",
        "Large Water Splash",
        "Huge Water Splash",
        "Riot Spark",
        "Burning",
    };

    // Stock ambient loops for the fighter's downwash: a rush of wind over ground, spray over water.
    constexpr const char* fx_wash_sound_files[] = {
        "Amb_Wind_02.wav",
        "Amb_Water_Fall_02.wav",
    };

    // The fighter's scrape loop (alpinefaction.vpp).
    constexpr const char* fx_scrape_sound_file = "af_fighter_scrape_loop.wav";
    // How long a scrape probe's finding keeps the loop going; the last fx_scrape_fade_s of it fades it out.
    constexpr float fx_scrape_hold_s = 0.2f;
    constexpr float fx_scrape_fade_s = 0.1f;

    enum FxBitmap
    {
        fx_bitmap_smoke,
        fx_bitmap_dark_smoke,
        fx_bitmap_spark,
        fx_bitmap_fire,
        fx_bitmap_bubble,
        fx_bitmap_count,
    };

    // Stock bitmaps the stock emitters.tbl already uses.
    constexpr const char* fx_bitmap_filenames[fx_bitmap_count] = {
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
        fx_bitmap_spark, {255, 255, 255, 255}, {128, 128, 128, 0}, 0.08f, 0.13f, 0.0f, 0.25f, 0.5f, 2.0f, 0.0f,
        rf::PTF_GLOW | rf::PTF_VEL_STRETCH,
    };
    // The bubble bitmap's white specks read as droplets; the stock corona has an orange core.
    constexpr ParticleSpec spray_spec{
        fx_bitmap_bubble, {220, 235, 255, 220}, {220, 235, 255, 0}, 0.12f, 0.25f, -0.1f, 0.4f, 0.8f, 1.0f, 0.0f,
        rf::PTF_GLOW | rf::PTF_RANDOM_ORIENT,
    };
    constexpr ParticleSpec mist_spec{
        fx_bitmap_smoke, {225, 235, 245, 110}, {225, 235, 245, 0}, 0.3f, 0.6f, 1.5f, 0.8f, 1.4f, -0.05f, -1.5f,
        rf::PTF_GLOW | rf::PTF_RANDOM_ORIENT,
    };
    constexpr ParticleSpec smoke_spec{
        fx_bitmap_smoke, {120, 120, 120, 90}, {90, 90, 90, 0}, 0.3f, 0.5f, 0.4f, 1.2f, 2.0f, -0.12f, -1.0f,
        rf::PTF_RANDOM_ORIENT,
    };
    constexpr ParticleSpec dark_smoke_spec{
        fx_bitmap_dark_smoke, {255, 255, 255, 150}, {255, 255, 255, 0}, 0.4f, 0.6f, 0.9f, 2.4f, 3.4f, -0.3f, -1.0f,
        rf::PTF_RANDOM_ORIENT,
    };
    constexpr ParticleSpec fire_spec{
        fx_bitmap_fire, {255, 255, 255, 190}, {255, 255, 255, 0}, 0.35f, 0.6f, -0.3f, 0.45f, 0.8f, -0.6f, -3.0f,
        rf::PTF_GLOW | rf::PTF_RANDOM_ORIENT,
    };
    constexpr ParticleSpec bubble_spec{
        fx_bitmap_bubble, {255, 255, 255, 160}, {255, 255, 255, 0}, 0.05f, 0.12f, 0.0f, 1.0f, 2.0f, -0.5f, 0.0f, 0,
    };

    constexpr int fx_num_layers = fx_bitmap_count * 2;

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
        return {fx_bitmap_smoke, clr, dest, 0.25f * size, 0.45f * size, 0.8f, 0.6f, 1.1f, -0.05f, -2.0f, flags};
    }

    // A landing or crash throws a heavier, longer-lived cloud than a rolling wheel.
    ParticleSpec impact_dust_spec(const SurfaceFx& s, float size, bool haze)
    {
        ParticleSpec spec = dust_spec(s, size, haze);
        spec.growth_rate = 1.4f;
        spec.min_life = 1.2f;
        spec.max_life = 2.2f;
        spec.acceleration = -2.5f;
        return spec;
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
        float cam_dist = 0.0f;
        // Alternates spawn sites across frames: at these rates one frame rarely spawns more than one.
        uint32_t spawn_seq = 0;

        rf::Vector3 last_pos{};
        bool has_last = false;
        rf::Vector3 vel{};      // fast, what the hull is doing now
        rf::Vector3 vel_slow{}; // what it was doing a moment ago, for impacts
        // Recent peak fall and horizontal speeds: an impact is detected as soon as the hull starts to slow,
        // before the smoothed speeds show how hard it hit.
        float peak_fall = 0.0f;
        float peak_h = 0.0f;

        float probe_timer = 0.0f;
        GroundProbe ground;
        WashTarget wash;
        float airborne_age = fx_landing_airborne_window; // since the probe last saw the hull off the ground
        float scrape_timer = 0.0f;
        float impact_cooldown = 0.0f;
        float debris_cooldown = 0.0f;
        float scrape_snd_timer = 0.0f;
        float scrape_flash_timer = 0.0f;
        float scrape_burst_timer = 0.0f;
        float splash_snd_timer = 0.0f;
        float fire_light_timer = 0.0f;
        bool bottom_in_water = false;

        // Wanted afresh every frame: a hull can go out of range or hidden without anything ending them.
        float wash_level = 0.0f;
        bool burning = false;
        rf::Vector3 vent{};
        int wash_snd = -1;
        bool wash_snd_water = false;
        float wash_snd_pitch = 0.0f; // last ratio set on wash_snd; 0 for none yet
        int burn_snd = -1;
        float wash_snd_retry = 0.0f;
        float burn_snd_retry = 0.0f;
        // Held across the probes: a probe's slide, 0 to 1 over the damage span, and where it was.
        float scrape_hold = 0.0f;
        float scrape_level = 0.0f;
        rf::Vector3 scrape_pos{};
        int scrape_snd = -1;
        float scrape_snd_retry = 0.0f;
        float scrape_snd_pitch = 0.0f; // last ratio set on scrape_snd; 0 for none yet
        bool hold_loops = false; // a skipped frame decided nothing about them

        float dust_accum = 0.0f;
        float spray_accum = 0.0f;
        float wash_accum = 0.0f;
        float smoke_accum = 0.0f;
        float fire_accum = 0.0f;
        float bubble_accum = 0.0f;
    };

    template<std::size_t N>
    constexpr std::array<int, N> fx_unloaded()
    {
        std::array<int, N> handles{};
        handles.fill(-1);
        return handles;
    }

    std::unordered_map<int, HullFx> g_hull_fx;
    std::array<int, fx_bitmap_count> g_fx_bitmaps = fx_unloaded<fx_bitmap_count>();
    bool g_fx_bitmaps_loaded = false;
    int g_fx_live_particles = 0;
    int g_fx_num_emitters = 0;
    std::array<int, fx_sound_count> g_fx_foley = fx_unloaded<fx_sound_count>();
    std::array<int, std::size(fx_wash_sound_files)> g_fx_wash_sounds = fx_unloaded<std::size(fx_wash_sound_files)>();
    int g_fx_scrape_sound = -1;
    rf::ImpactSoundSet* g_fx_big_debris_iss = nullptr;
    rf::ImpactSoundSet* g_fx_small_debris_iss = nullptr;
    bool g_fx_sounds_loaded = false;
    float g_fx_debris_cooldown = 0.0f;
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

    void fx_load_bitmaps()
    {
        if (g_fx_bitmaps_loaded) {
            return;
        }
        g_fx_bitmaps_loaded = true;
        for (int i = 0; i < fx_bitmap_count; ++i) {
            g_fx_bitmaps[i] = rf::bm::load(fx_bitmap_filenames[i], -1, true);
            if (g_fx_bitmaps[i] < 0) {
                xlog::warn("[vehicle_fx] failed to load bitmap '{}'", fx_bitmap_filenames[i]);
            }
        }
    }

    rf::ImpactSoundSet* fx_find_iss(const char* name)
    {
        rf::ImpactSoundSet* iss = rf::material_find_impact_sound_set(name);
        if (!iss) {
            xlog::warn("[vehicle_fx] no impact sound set '{}'", name);
        }
        return iss;
    }

    void fx_load_sounds()
    {
        if (g_fx_sounds_loaded) {
            return;
        }
        g_fx_sounds_loaded = true;
        for (int i = 0; i < fx_sound_count; ++i) {
            g_fx_foley[i] = rf::foley_lookup_by_name(fx_foley_names[i]);
            if (g_fx_foley[i] < 0) {
                xlog::warn("[vehicle_fx] no foley '{}'", fx_foley_names[i]);
            }
        }
        for (std::size_t i = 0; i < std::size(fx_wash_sound_files); ++i) {
            g_fx_wash_sounds[i] = rf::snd_get_handle(fx_wash_sound_files[i], 8.0f, 0.9f, 1.0f);
            // With sound off the engine registers nothing, and that is no fault of the file.
            if (g_fx_wash_sounds[i] < 0 && rf::sound_enabled) {
                xlog::warn("[vehicle_fx] failed to load sound '{}'", fx_wash_sound_files[i]);
            }
        }
        // Full volume out to 10: loud in the cockpit, and plain to anyone near the hull.
        g_fx_scrape_sound = rf::snd_get_handle(fx_scrape_sound_file, 10.0f, 1.0f, 1.0f);
        if (g_fx_scrape_sound < 0 && rf::sound_enabled) {
            xlog::warn("[vehicle_fx] failed to load sound '{}'", fx_scrape_sound_file);
        }
        g_fx_big_debris_iss = fx_find_iss("big debris bounce");
        g_fx_small_debris_iss = fx_find_iss("small debris bounce");
    }

    // A fresh pick each time, so a foley's variants alternate.
    int fx_sound_handle(FxSound sound)
    {
        return g_fx_foley[sound] >= 0 ? rf::foley_get_sound_handle(g_fx_foley[sound]) : -1;
    }

    int fx_play_handle(int handle, const rf::Vector3& pos, float volume)
    {
        return handle >= 0 ? rf::snd_play_3d(handle, pos, std::min(volume, 1.0f), rf::zero_vector,
                                             rf::SOUND_GROUP_EFFECTS)
                           : -1;
    }

    void fx_play(FxSound sound, const rf::Vector3& pos, float volume)
    {
        fx_play_handle(fx_sound_handle(sound), pos, volume);
    }

    // Picks as a bouncing debris piece does.
    void fx_play_iss(const rf::ImpactSoundSet* iss, int material, const rf::Vector3& pos, float volume)
    {
        if (!iss) {
            return;
        }
        const int* sounds = nullptr;
        int count = 0;
        iss->get_material_sounds(material, sounds, count);
        if (!sounds) {
            return;
        }
        // The engine does not clamp the count.
        count = std::min(count, static_cast<int>(std::end(iss->sounds) - sounds));
        if (count <= 0) {
            return;
        }
        const int pick = std::uniform_int_distribution<int>{0, count - 1}(g_rng);
        fx_play_handle(sounds[pick], pos, volume);
    }

    // Kept going while wanted: `start` plays it afresh when it is not playing, as a one-shot foley ends.
    void fx_update_loop(int& instance, float& retry, const rf::Vector3& pos, const rf::Vector3& vel, float volume,
                        auto&& start)
    {
        if (snd_instance_is_playing(instance)) {
            rf::snd_change_3d(instance, pos, vel, std::min(volume, 1.0f));
            return;
        }
        retry = std::max(retry - g_fx_dt, 0.0f);
        if (retry > 0.0f) {
            return;
        }
        instance = start();
        if (instance < 0) {
            retry = 0.5f;
        }
    }

    void fx_stop_loop(int& instance)
    {
        if (instance >= 0) {
            rf::snd_stop(instance);
            instance = -1;
        }
    }

    // Only the D3D11 renderer draws (and retires) these lights.
    void fx_light(const rf::Vector3& pos, float radius, float intensity, int duration_ms)
    {
        if (is_d3d11()) {
            explosion_flash_light_create(pos, radius, intensity, 1.0f, 0.7f, 0.35f, duration_ms, false);
        }
    }

    // Sparks and impacts.
    void fx_flash(const rf::Vector3& pos, float radius, float intensity, int duration_ms)
    {
        if (g_alpine_game_config.explosion_env_flash_lights) {
            fx_light(pos, radius, intensity, duration_ms);
        }
    }

    void fx_tick(float& timer)
    {
        timer = std::max(timer - g_fx_dt, 0.0f);
    }

    bool fx_budget_left()
    {
        const rf::ParticlePool& pool = rf::particle_pools[fx_particle_pool];
        return g_fx_live_particles < fx_max_live_particles
            && pool.num_used < static_cast<int>(static_cast<float>(pool.capacity) * fx_pool_headroom);
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

    // Emitters go first come in entity order, so far hulls can hold them all while the one at the camera
    // goes without: a near hull takes the idlest layer of the farthest hull beyond fx_near_dist.
    bool fx_release_far_emitter(const HullFx& fx)
    {
        if (fx.cam_dist > fx_near_dist) {
            return false;
        }
        HullFx* farthest = nullptr;
        for (auto& [handle, other] : g_hull_fx) {
            const bool holds =
                std::ranges::any_of(other.emitters, [](const rf::ParticleEmitter* e) { return e != nullptr; });
            if (holds && other.cam_dist > fx_near_dist && (!farthest || other.cam_dist > farthest->cam_dist)) {
                farthest = &other;
            }
        }
        if (!farthest) {
            return false;
        }
        int idlest = -1;
        for (int layer = 0; layer < fx_num_layers; ++layer) {
            if (farthest->emitters[layer]
                && (idlest < 0 || farthest->emitter_idle[layer] > farthest->emitter_idle[idlest])) {
                idlest = layer;
            }
        }
        fx_destroy_emitter(*farthest, idlest);
        return true;
    }

    // Created on the first particle a layer actually spawns, so a quiet hull holds no engine record.
    rf::ParticleEmitter* fx_ensure_emitter(HullFx& fx, int layer)
    {
        rf::ParticleEmitter*& emitter = fx.emitters[layer];
        if (emitter) {
            return emitter;
        }
        if (!fx.room
            || rf::particle_emitter_num_used >= particle_sim_emitter_capacity() - fx_min_free_engine_emitters
            || (fx.emitter_retry.valid() && !fx.emitter_retry.elapsed())
            || (g_fx_num_emitters >= fx_max_emitters && !fx_release_far_emitter(fx))) {
            return nullptr;
        }
        // A container only: never switched on and never updated, so it spawns nothing itself. Its
        // particles come from particle_create, and the engine simulates, culls and draws them as its own.
        rf::ParticleEmitterType type{};
        type.pos = fx.last_pos;
        type.min_spawn_delay = 1.0f;
        type.max_spawn_delay = 1.0f;
        type.bitmap = g_fx_bitmaps[fx_bitmap_smoke];
        type.num_frames = 1;
        // Parent 0 is no object (handle signatures start at 1) but counts as parented, which stock needs to fit
        // the cull radius to the particles (0x00497E0A). Never ticked: particle_emitter_create does not link it.
        emitter = rf::particle_emitter_create(0, type, fx.room, fx.last_pos, false);
        if (!emitter) {
            fx.emitter_retry.set(1000);
            return nullptr;
        }
        fx.emitter_idle[layer] = 0.0f;
        ++g_fx_num_emitters;
        return emitter;
    }

    void fx_destroy_emitters(HullFx& fx)
    {
        for (int layer = 0; layer < fx_num_layers; ++layer) {
            fx_destroy_emitter(fx, layer);
        }
    }

    void fx_release(HullFx& fx)
    {
        fx_destroy_emitters(fx);
        fx_stop_loop(fx.wash_snd);
        fx_stop_loop(fx.burn_snd);
        fx_stop_loop(fx.scrape_snd);
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
        // Linked into the hull's emitter, so it is drawn whenever that emitter's room is. No parent object: a
        // particle's parent only names a damage source and ties it to a level emitter.
        rf::particle_create(fx_particle_pool, pci, emitter->room, &pci.pos, -1, nullptr, emitter);
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
            const float a = fx_rand(0.0f, vehicle_two_pi);
            const rf::Vector3 out{std::cos(a), 0.0f, std::sin(a)};
            const rf::Vector3 pos = center + out * (radius * fx_rand(0.3f, 1.0f)) + rf::Vector3{0.0f, 0.1f, 0.0f};
            const rf::Vector3 vel =
                out * (out_speed * fx_rand(0.5f, 1.0f)) + rf::Vector3{0.0f, up_speed * fx_rand(0.4f, 1.0f), 0.0f};
            fx_spawn(fx, s, pos, vel, size);
        }
    }

    // Water thrown off a hull cutting the surface at `pos`: to its right on even sequences, else its left.
    void fx_side_spray(HullFx& fx, const rf::Entity& ep, uint32_t seq, const rf::Vector3& pos,
                       const rf::Vector3& carry, float up)
    {
        const rf::Vector3 side = ep.orient.rvec * ((seq % 2) ? -1.0f : 1.0f);
        fx_spawn(fx, spray_spec, pos,
                 side * fx_rand(1.0f, 2.5f) + carry + rf::Vector3{0.0f, up + fx_rand(0.0f, 1.5f), 0.0f});
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

    FxSound fx_impact_sound(int material)
    {
        switch (material) {
            case rf::MATERIAL_METAL:
                return fx_sound_impact_metal;
            case rf::MATERIAL_GLASS:
                return fx_sound_impact_glass;
            default:
                return fx_sound_impact_rock;
        }
    }

    // Chips knocked off a hard surface, borrowed from the geomod rock pool.
    void fx_rock_debris(HullFx& fx, const rf::PCollisionOut& hit, float strength, float lod)
    {
        switch (hit.material) {
            case rf::MATERIAL_DEFAULT:
            case rf::MATERIAL_ROCK:
            case rf::MATERIAL_SOLID:
            case rf::MATERIAL_SAND:
            case rf::MATERIAL_ICE:
                break;
            default:
                return;
        }
        if (strength < fx_debris_min_strength || fx.debris_cooldown > 0.0f || g_fx_debris_cooldown > 0.0f
            || !hit.hit_face || !hit.hit_face->which_room || hit.hit_face->attributes.bitmap_id < 0) {
            return;
        }
        fx.debris_cooldown = fx_debris_cooldown;
        g_fx_debris_cooldown = fx_debris_global_cooldown;
        rf::Vector3 pos = hit.hit_point;
        rf::Vector3 dir = hit.hit_normal;
        // Off open ground a radius r throws about 8r rocks.
        rf::geomod_create_rock_debris(&pos, (0.6f + 1.2f * strength) * lod, &dir, hit.hit_face->attributes.bitmap_id,
                                      reinterpret_cast<int>(hit.hit_face->which_room));
    }

    // Spray thrown up and mist rolling out from `surface`. A bigger body (size > 1) throws a wider ring, more of
    // it, and is heard a splash size up.
    void fx_splash(HullFx& fx, const rf::Vector3& surface, float strength, float lod, float size)
    {
        const float amount = size * size * lod;
        fx_ring(fx, spray_spec, static_cast<int>((12.0f + 30.0f * strength) * amount), surface, 2.0f * size,
                3.0f + 4.0f * strength, (3.0f + 4.0f * strength) * size, size);
        fx_ring(fx, mist_spec, static_cast<int>((4.0f + 8.0f * strength) * amount), surface, 2.0f * size, 1.5f,
                1.0f * size, 1.5f * size);
        constexpr FxSound splashes[] = {fx_sound_splash_small, fx_sound_splash_medium, fx_sound_splash_large,
                                        fx_sound_splash_huge};
        const int tier = (strength < 0.35f ? 0 : strength < 0.7f ? 1 : 2) + (size > 1.0f ? 1 : 0);
        fx_play(splashes[tier], surface, 0.5f + 0.5f * strength);
    }

    void fx_impact(rf::Entity& ep, HullFx& fx, const rf::PCollisionOut& hit, float strength, float lod,
                   std::optional<float> water_y)
    {
        if (water_y) {
            fx_splash(fx, rf::Vector3{hit.hit_point.x, *water_y, hit.hit_point.z}, strength, lod, 1.0f);
        }
        else {
            const SurfaceFx s = surface_fx(hit.material);
            if (s.dust > 0.0f) {
                // Out past the hull's footprint, or the hull hides most of its own cloud.
                const float footprint = std::max(vehicle_collision_reach_along(&ep, ep.orient.rvec),
                                                 vehicle_collision_reach_along(&ep, ep.orient.fvec));
                const int count = static_cast<int>((6.0f + 22.0f * strength) * s.dust * lod);
                for (const bool haze : {false, true}) {
                    fx_ring(fx, impact_dust_spec(s, 1.5f + 1.5f * strength, haze), count, hit.hit_point,
                            footprint + 1.0f, 3.0f + 6.0f * strength, 1.0f + 2.5f * strength);
                }
            }
            if (s.sparks) {
                fx_burst_off(fx, spark_spec, static_cast<int>((6.0f + 18.0f * strength) * lod), hit.hit_point,
                             hit.hit_normal, 6.0f + 8.0f * strength);
                fx_flash(hit.hit_point + hit.hit_normal * 0.3f, 2.0f + 2.0f * strength, 0.6f + 0.6f * strength,
                         static_cast<int>(80.0f + 70.0f * strength));
            }
            fx_play(fx_impact_sound(hit.material), hit.hit_point, 0.4f + 0.6f * strength);
            if (strength >= fx_heavy_impact) {
                fx_play_iss(g_fx_big_debris_iss, hit.material, hit.hit_point, strength);
            }
            fx_rock_debris(fx, hit, strength, lod);
        }
        if (fx_local_player_rides(ep) && rf::local_player && rf::local_player->cam) {
            rf::camera_shake(rf::local_player->cam, 0.002f + 0.006f * strength, 0.35f);
        }
    }

    // A sudden loss of the speed the hull had a moment ago: a landing or a wall.
    void fx_detect_impacts(rf::Entity& ep, HullFx& fx, bool flyer, float lod, std::optional<float> liquid_y)
    {
        fx_tick(fx.impact_cooldown);
        fx_tick(fx.debris_cooldown);
        if (fx.impact_cooldown > 0.0f) {
            return;
        }
        // A fighter's effects start where its crash damage does, so a slam that shows costs hull and a soft
        // landing shows nothing.
        const float wall_min = flyer ? vehicle_crash_tuning[VDC_FIGHTER].wall_threshold : fx_impact_min_dv;
        const float land_min = flyer ? vehicle_crash_tuning[VDC_FIGHTER].ground_threshold : fx_impact_min_dv;
        const bool was_airborne = fx.airborne_age < fx_landing_airborne_window;
        const float land_dv = was_airborne && fx.vel_slow.y < 0.0f ? std::max(fx.vel.y - fx.vel_slow.y, 0.0f) : 0.0f;
        const rf::Vector3 slow_h{fx.vel_slow.x, 0.0f, fx.vel_slow.z};
        const float slow_h_speed = slow_h.len();
        float wall_dv = 0.0f;
        float wall_kept = 0.0f;
        rf::Vector3 wall_dir{};
        if (slow_h_speed > wall_min) {
            wall_dir = slow_h * (1.0f / slow_h_speed);
            wall_kept = std::max(rf::Vector3{fx.vel.x, 0.0f, fx.vel.z}.dot_prod(wall_dir), 0.0f);
            wall_dv = slow_h_speed - wall_kept;
        }
        const bool landing = land_dv >= land_min && land_dv >= wall_dv;
        if (!landing && wall_dv < wall_min) {
            return;
        }
        // A wall only takes the speed it stopped.
        const float impact_speed = landing ? fx.peak_fall : fx.peak_h - wall_kept;
        const float min_dv = landing ? land_min : wall_min;
        const float strength = std::clamp((impact_speed - min_dv) / (fx_impact_full_dv - min_dv), 0.15f, 1.0f);

        const rf::Vector3 dir = landing ? rf::Vector3{0.0f, -1.0f, 0.0f} : wall_dir;
        rf::PCollisionOut o{};
        // Generous reach: suspension and wheels start slowing a landing hull before its body is near the ground.
        if (!fx_cast(ep.pos, ep.pos + dir * (vehicle_collision_reach_along(&ep, dir) + 2.5f), o)) {
            fx.impact_cooldown = fx_impact_miss_cooldown;
            return;
        }
        const bool water = liquid_y && o.hit_point.y <= *liquid_y + 0.3f;
        fx.impact_cooldown = fx_impact_cooldown;
        fx.vel_slow = fx.vel;
        fx.peak_fall = std::max(-fx.vel.y, 0.0f);
        fx.peak_h = rf::Vector3{fx.vel.x, 0.0f, fx.vel.z}.len();
        // A ground hull's water entry is fx_plunge's.
        if (water && !flyer) {
            return;
        }
        fx_impact(ep, fx, o, strength, lod, water ? liquid_y : std::nullopt);
    }

    // Body contact while moving along a wall (and, for flyers, a ceiling or the ground, or nose or tail first).
    // A flyer runs the probe and the free speeds its scrape damage does, so what sparks costs hull, and looks and
    // sounds the same on any surface; a car's follows the material.
    void fx_scrape(rf::Entity& ep, HullFx& fx, bool flyer, float lod)
    {
        fx_tick(fx.scrape_snd_timer);
        fx_tick(fx.scrape_flash_timer);
        fx_tick(fx.scrape_burst_timer);
        fx.scrape_timer -= g_fx_dt;
        if (fx.scrape_timer > 0.0f) {
            return;
        }
        fx.scrape_timer = fx_probe_interval;
        if (fx.vel.len() < 3.0f) {
            return;
        }
        VehicleHullProbeHit hits[vehicle_hull_probe_max_hits];
        const int num_hits = vehicle_hull_probe(&ep, fx.vel, flyer, false, hits);
        float loop_level = -1.0f;
        rf::Vector3 loop_pos{};
        for (int i = 0; i < num_hits; ++i) {
            const VehicleHullProbeHit& o = hits[i];
            const rf::Vector3& along = o.along;
            const float along_speed = o.along_speed;
            float free_speed = 3.0f;
            if (flyer) {
                const VehicleScrapeTuning& scrape = vehicle_scrape_tuning[VDC_FIGHTER];
                free_speed = o.normal.y >= vehicle_crash_ground_ny ? scrape.ground_min_speed : scrape.min_speed;
                const float level =
                    std::clamp((along_speed - free_speed) / std::max(scrape.ref_speed - free_speed, 1.0f), 0.0f, 1.0f);
                if (along_speed >= free_speed && level > loop_level) {
                    loop_level = level;
                    loop_pos = o.point;
                }
            }
            else if (o.normal.dot_prod(ep.orient.uvec) > 0.5f) {
                continue; // a side ray tipped into the ground by a rolled hull finds the floor, not a wall
            }
            if (along_speed < free_speed) {
                continue;
            }
            const SurfaceFx s = flyer ? SurfaceFx{0.0f, {}, true} : surface_fx(o.material);
            const int count = flyer ? static_cast<int>(std::min(along_speed * 0.25f, 5.0f) * lod + 0.5f)
                                    : static_cast<int>(std::min(along_speed * 0.15f, 3.0f) * lod + 0.5f);
            if (s.sparks) {
                for (int k = 0; k < count; ++k) {
                    const rf::Vector3 vel = along * -fx_rand(0.2f, 0.5f) + o.normal * fx_rand(1.0f, 3.0f)
                                          + fx_rand_dir_off(o.normal) * 1.5f;
                    fx_spawn(fx, spark_spec, o.point, vel);
                }
                if (flyer && fx.scrape_burst_timer <= 0.0f) {
                    fx.scrape_burst_timer = fx_rand(0.2f, 0.35f);
                    fx_burst_off(fx, spark_spec, static_cast<int>((4.0f + 0.4f * along_speed) * lod), o.point,
                                 o.normal, 3.0f + 0.2f * along_speed);
                }
                // Under a flyer's scrape loop, a crackle rather than the whole sound.
                if (fx.scrape_snd_timer <= 0.0f) {
                    fx.scrape_snd_timer = fx_rand(0.12f, 0.25f);
                    fx_play(fx_sound_spark, o.point,
                            std::clamp(along_speed / 15.0f, 0.3f, 1.0f) * (flyer ? 0.5f : 1.0f));
                }
                if (fx.scrape_flash_timer <= 0.0f) {
                    fx.scrape_flash_timer = fx_rand(0.25f, 0.4f);
                    fx_flash(o.point + o.normal * 0.3f, 2.5f, 0.5f, 90);
                }
            }
            else if (s.dust > 0.0f) {
                fx_spawn(fx, dust_spec(s, 0.8f, fx.spawn_seq++ % 2 != 0), o.point, o.normal * 0.8f + along * -0.1f);
                if (fx.scrape_snd_timer <= 0.0f) {
                    fx.scrape_snd_timer = fx_rand(0.4f, 0.7f);
                    fx_play_iss(g_fx_small_debris_iss, o.material, o.point,
                                std::clamp(along_speed / 20.0f, 0.2f, 0.6f));
                }
            }
        }
        if (loop_level >= 0.0f) {
            fx.scrape_hold = fx_scrape_hold_s;
            fx.scrape_level = loop_level;
            fx.scrape_pos = loop_pos;
        }
    }

    // Wheels and tracks: dust behind them on dry ground, spray where they run through water.
    void fx_ground_contact(rf::Entity& ep, HullFx& fx, float lod, std::optional<float> liquid_y)
    {
        fx_tick(fx.splash_snd_timer);
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
                // Even sequences take the right-hand wheels, so each side throws outward.
                const uint32_t seq = fx.spawn_seq++;
                const rf::Vector3& c = contacts[seq % 4];
                fx_side_spray(fx, ep, seq, rf::Vector3{c.x, *liquid_y, c.z}, vel_h * 0.3f, 2.0f + speed * 0.1f);
            }
            if (speed > 1.5f && fx.splash_snd_timer <= 0.0f) {
                const float pace = std::clamp((speed - 1.5f) / 12.0f, 0.0f, 1.0f);
                fx.splash_snd_timer = 0.6f - 0.4f * pace;
                fx_play(fx_sound_splash_small, rf::Vector3{ep.pos.x, *liquid_y, ep.pos.z}, 0.3f + 0.4f * pace);
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
            const uint32_t seq = fx.spawn_seq++;
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
        const float intensity = closeness * closeness;
        // Heard over any surface, seen only over one that throws something up.
        fx.wash_level = intensity;
        if (closeness <= 0.0f || s.dust <= 0.0f) {
            fx.wash_accum = 0.0f;
            return;
        }
        const rf::Vector3 center{ep.pos.x, fx.wash.surface_y, ep.pos.z};
        const float radius = 1.5f + 1.5f * intensity;
        const int n = fx_take(fx.wash_accum, 45.0f * intensity * lod * s.dust);
        for (int i = 0; i < n; ++i) {
            if (!fx.wash.water) {
                fx_ring(fx, dust_spec(s, 1.2f, fx.spawn_seq++ % 2 != 0), 1, center, radius,
                        3.0f + 5.0f * intensity, 0.4f + 0.8f * intensity);
            }
            else if (fx.spawn_seq++ % 3 == 0) {
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
        fx_tick(fx.splash_snd_timer);
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
                fx_side_spray(fx, ep, fx.spawn_seq++, rf::Vector3{bow.x, *liquid_y, bow.z}, rf::Vector3{}, 1.5f);
            }
            if (speed > 3.0f && fx.splash_snd_timer <= 0.0f) {
                fx.splash_snd_timer = fx_rand(0.8f, 1.6f);
                fx_play(fx_sound_splash_small, rf::Vector3{bow.x, *liquid_y, bow.z}, 0.3f);
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
            fx_splash(fx, rf::Vector3{ep.pos.x, *liquid_y, ep.pos.z}, strength, lod, 1.25f);
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
        // A jeep burns under its hood; elsewhere aft of centre, so a first-person driver is not looking out through it.
        const float up = vehicle_collision_reach_along(&ep, ep.orient.uvec);
        const rf::Vector3 vent = vehicle_damage_class(&ep) == VDC_JEEP
            ? ep.pos + ep.orient.uvec * (up * 0.4f)
                  + ep.orient.fvec * (vehicle_collision_reach_along(&ep, ep.orient.fvec) * 0.6f)
            : ep.pos + ep.orient.uvec * (up * 0.7f)
                  - ep.orient.fvec * (vehicle_collision_reach_along(&ep, ep.orient.fvec * -1.0f) * 0.4f);
        const float severity = dying ? 1.0f : std::clamp((0.5f - frac) / 0.5f, 0.0f, 1.0f);
        // An APC's flames have to be read against a roof three times the jeep's.
        const float hull_scale = std::clamp(ep.radius / 2.0f, 1.0f, 1.6f);
        const bool burning = dying || frac < 0.25f;
        const int n_smoke = fx_take(fx.smoke_accum, (3.0f + 7.0f * severity) * lod);
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
        const int n_fire = fx_take(fx.fire_accum, 20.0f * lod);
        for (int i = 0; i < n_fire; ++i) {
            fx_spawn(fx, fire_spec, vent + fx_rand_dir() * (0.25f * hull_scale),
                     fx.vel * 0.7f + rf::Vector3{0.0f, 0.5f, 0.0f}, hull_scale);
        }
        fx.burning = true;
        fx.vent = vent;
        // Overlapping short flashes of random strength flicker; thinned-out distant smoke gets none.
        fx_tick(fx.fire_light_timer);
        if (g_alpine_game_config.burning_entity_lights && lod >= 1.0f && fx.fire_light_timer <= 0.0f) {
            fx.fire_light_timer = fx_rand(0.12f, 0.18f);
            const float life = fx.fire_light_timer + 0.16f;
            // A light stays where it was made: lead it by half its life so a moving hull carries it.
            fx_light(vent + fx.vel * (life * 0.5f), 3.0f * hull_scale, fx_rand(0.8f, 1.0f),
                     static_cast<int>(life * 1000.0f));
        }
    }

    void fx_update_loops(HullFx& fx)
    {
        if (fx.hold_loops) {
            fx.hold_loops = false;
            return;
        }
        if (fx.wash_level > 0.0f) {
            // Swapped when the fighter crosses between ground and water; swelling as it closes on either.
            if (fx.wash_snd_water != fx.wash.water) {
                fx_stop_loop(fx.wash_snd);
                fx.wash_snd_water = fx.wash.water;
            }
            // Linear in closeness from silence at the edge, so a fighter hovering there does not cut it in and out.
            const float volume = std::sqrt(fx.wash_level);
            fx_update_loop(fx.wash_snd, fx.wash_snd_retry, fx.last_pos, fx.vel, volume, [&] {
                fx.wash_snd_pitch = 0.0f;
                const int sound = g_fx_wash_sounds[fx.wash.water ? 1 : 0];
                return sound >= 0 ? snd_play_3d_looping(sound, fx.last_pos, volume, rf::SOUND_GROUP_EFFECTS) : -1;
            });
            const float pitch = 0.85f + 0.25f * fx.wash_level;
            if (pitch != fx.wash_snd_pitch) {
                snd_instance_set_pitch(fx.wash_snd, pitch);
                fx.wash_snd_pitch = pitch;
            }
        }
        else {
            fx_stop_loop(fx.wash_snd);
            fx.wash_snd_retry = 0.0f;
        }
        // Louder and higher the faster the slide; out over the hold's last stretch once the probes find none.
        if (fx.scrape_hold > 0.0f && g_fx_scrape_sound >= 0) {
            const float volume =
                (0.45f + 0.55f * fx.scrape_level) * std::min(fx.scrape_hold / fx_scrape_fade_s, 1.0f);
            fx_update_loop(fx.scrape_snd, fx.scrape_snd_retry, fx.scrape_pos, fx.vel, volume, [&] {
                fx.scrape_snd_pitch = 0.0f;
                return snd_play_3d_looping(g_fx_scrape_sound, fx.scrape_pos, volume, rf::SOUND_GROUP_EFFECTS);
            });
            const float pitch = 0.9f + 0.35f * fx.scrape_level;
            if (pitch != fx.scrape_snd_pitch) {
                snd_instance_set_pitch(fx.scrape_snd, pitch);
                fx.scrape_snd_pitch = pitch;
            }
        }
        else {
            fx_stop_loop(fx.scrape_snd);
            fx.scrape_snd_retry = 0.0f;
        }
        if (fx.burning) {
            fx_update_loop(fx.burn_snd, fx.burn_snd_retry, fx.vent, fx.vel, 1.0f,
                           [&] { return fx_play_handle(fx_sound_handle(fx_sound_burning), fx.vent, 1.0f); });
        }
        else {
            fx_stop_loop(fx.burn_snd);
            fx.burn_snd_retry = 0.0f;
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
        // A demo seek's fast-forward frames are culled when it ends, as the tracers are.
        return rf::is_multi && !rf::is_dedicated_server && g_alpine_game_config.vehicle_effects
            && vehicle_level_has_factories() && !demo_playback_is_seeking();
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
        "Show dust, sparks, spray, smoke and fire effects, with their sounds and lights, for vehicles in multiplayer",
        "cl_vehiclefx [bool]",
    };
} // namespace

void vehicle_fx_clear()
{
    for (auto& [handle, fx] : g_hull_fx) {
        fx_release(fx);
    }
    g_hull_fx.clear();
}

void vehicle_fx_level_init(bool load_assets)
{
    // Emitter records outlive the level; the bitmap and sound handles do not.
    vehicle_fx_clear();
    g_fx_bitmaps.fill(-1);
    g_fx_bitmaps_loaded = false;
    g_fx_sounds_loaded = false;
    g_fx_debris_cooldown = 0.0f;
    // Runs after the last level's unload frees its bitmaps and sounds, and ahead of this one's first frame.
    // The factories are not parsed yet, so every multiplayer level loads them.
    if (load_assets && rf::is_multi && !rf::is_dedicated_server && g_alpine_game_config.vehicle_effects) {
        fx_load_bitmaps();
        fx_load_sounds();
    }
}

void vehicle_fx_do_frame()
{
    if (!fx_active()) {
        if (!g_hull_fx.empty()) {
            vehicle_fx_clear();
        }
        return;
    }
    // Level init has loaded them unless cl_vehiclefx was switched on since.
    fx_load_bitmaps();
    fx_load_sounds();

    g_fx_dt = rf::frametime * demo_playback_sim_time_scale();
    const float dt = g_fx_dt;
    fx_tick(g_fx_debris_cooldown);
    g_fx_live_particles = 0;
    for (auto it = g_hull_fx.begin(); it != g_hull_fx.end();) {
        HullFx& fx = it->second;
        rf::Entity* ep = rf::entity_from_handle(it->first);
        if (!ep || vehicle_damage_class(ep) < 0) {
            fx_release(fx);
            it = g_hull_fx.erase(it);
            continue;
        }
        fx.wash_level = 0.0f;
        fx.burning = false;
        fx.scrape_hold = std::max(fx.scrape_hold - dt, 0.0f);
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
        fx.cam_dist = (entity.pos - cam_pos).len();
        if (entity.room) {
            fx.room = entity.room;
        }
        // The layers would tie in the back-to-front sort and swap from frame to frame; additive ones sort just nearer,
        // so fire always lands on its smoke.
        rf::Vector3 to_cam = cam_pos - entity.pos;
        to_cam.normalize_safe();
        for (int layer = 0; layer < fx_num_layers; ++layer) {
            if (rf::ParticleEmitter* emitter = fx.emitters[layer]) {
                emitter->pos = (layer & 1) ? entity.pos + to_cam * 0.1f : entity.pos;
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
            fx.peak_fall = 0.0f;
            fx.peak_h = 0.0f;
            fx.impact_cooldown = fx_impact_cooldown;
            fx.hold_loops = true;
            continue;
        }
        fx.vel += (raw_vel - fx.vel) * (1.0f - std::exp(-dt / 0.03f));
        fx.vel_slow += (fx.vel - fx.vel_slow) * (1.0f - std::exp(-dt / 0.2f));
        fx.peak_fall = std::max(-fx.vel.y, fx.peak_fall - fx_peak_decay * dt);
        fx.peak_h = std::max(rf::Vector3{fx.vel.x, 0.0f, fx.vel.z}.len(), fx.peak_h - fx_peak_decay * dt);
        fx.airborne_age += dt;

        const float dist = fx.cam_dist;
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

    // A paused demo has paused the loops with it.
    if (dt > 0.0f) {
        for (auto& [handle, fx] : g_hull_fx) {
            fx_update_loops(fx);
        }
    }
}

void vehicle_fx_install()
{
    vehiclefx_cmd.register_cmd();
}
