#include <algorithm>
#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <patch_common/StaticBufferResizePatch.h>
#include <xlog/xlog.h>
#include "../graphics/d3d11/gr_d3d11_hooks.h"
#include "../hud/hud.h"
#include "../misc/alpine_settings.h"
#include "../multi/multi.h"
#include "../os/console.h"
#include "../os/os.h"
#include "../rf/gameseq.h"
#include "../rf/geometry.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_font.h"
#include "../rf/multi.h"
#include "../rf/particle_emitter.h"
#include "../rf/player/camera.h"
#include "../rf/player/player.h"
#include "particle_sim.h"

namespace
{

// An emitter is culled once its room has gone unrendered for more than this many frames.
constexpr int visible_frame_slack = 2;
// A wake-up only ramps back an emitter that was frozen for at least this long.
constexpr int64_t warmup_min_culled_ms = 1000;
constexpr float warmup_step = 1.0f / 30.0f;
constexpr int warmup_max_steps_per_emitter = 90;
constexpr int warmup_max_steps_per_frame = 300;
constexpr int warmup_max_spawns_per_step = 8;
constexpr int fixed_step_max_steps_per_frame = 2;
// Camera distance bands for the collision test interval k.
constexpr float collide_lod_near_dist = 15.0f;
constexpr float collide_lod_mid_dist = 40.0f;
// Stock capacities of the two engine pools, used when the configured pool size is 0.
constexpr int stock_pool_capacity[2] = {500, 1100};

// One frame's counters. A capture sums and maxes the same set, hence one width for all.
struct FrameCounters
{
    int64_t sim_us = 0;
    int64_t render_us = 0;
    int64_t simulated = 0;
    int64_t rendered = 0;
    int64_t emitters_rendered = 0;
    int64_t emitters_culled = 0;
    int64_t warmup_steps = 0;
    int64_t warmup_emitters = 0;
    int64_t eligible_steps = 0;
    int64_t collide_tested = 0;
    int64_t collide_skipped = 0;
    int64_t spawn_attempts = 0;
    int64_t spawn_failures = 0;
    int64_t gpu_drawn = 0;
    int64_t gpu_draws = 0;
};

constexpr int64_t FrameCounters::* frame_counter_fields[] = {
    &FrameCounters::sim_us, &FrameCounters::render_us, &FrameCounters::simulated, &FrameCounters::rendered,
    &FrameCounters::emitters_rendered, &FrameCounters::emitters_culled, &FrameCounters::warmup_steps,
    &FrameCounters::warmup_emitters, &FrameCounters::eligible_steps, &FrameCounters::collide_tested,
    &FrameCounters::collide_skipped, &FrameCounters::spawn_attempts, &FrameCounters::spawn_failures,
    &FrameCounters::gpu_drawn, &FrameCounters::gpu_draws,
};
static_assert(std::size(frame_counter_fields) * sizeof(int64_t) == sizeof(FrameCounters));

struct ParticleSimState
{
    bool overlay = false;
    bool warmup = true; // session only, not persisted

    FrameCounters cur;  // frame being built
    FrameCounters last; // last complete frame
    int64_t total_spawn_failures = 0;

    // one second rolling window
    int64_t window_start_ms = 0;
    int window_frames = 0;
    int64_t window_sim_sum = 0;
    int64_t window_sim_max = 0;
    int64_t window_render_sum = 0;
    int64_t window_render_max = 0;
    int64_t shown_sim_avg = 0;
    int64_t shown_sim_max = 0;
    int64_t shown_render_avg = 0;
    int64_t shown_render_max = 0;

    // dbg_particles_log capture
    bool capturing = false;
    int capture_secs = 0;
    int64_t capture_end_ms = 0;
    int capture_frames = 0;
    FrameCounters capture_sum;
    FrameCounters capture_max;
    int capture_pool_high_water[2] = {};
};

ParticleSimState g_ps;

// Only the overlay and a capture read the timings, so normal play skips the timer reads.
bool perf_timing_active()
{
    return g_ps.overlay || g_ps.capturing;
}

// last_seen_frame 0 means the emitter has not been stamped since the map was last cleared.
struct EmitterRecord
{
    int last_seen_frame = 0;
    bool culled = false;
    int64_t culled_since_ms = 0;
};

// Side state for one particle pool slot. last_age 0 marks a slot that has not been used yet.
struct CollideState
{
    rf::Vector3 last_tested;
    float acc_dt = 0.0f;
    float last_age = 0.0f;
    int frames = 0;
};

int g_frame = 0;
int g_map_cleared_frame = 0;
std::unordered_map<const rf::ParticleEmitter*, EmitterRecord> g_emitters;

float g_sim_acc = 0.0f;
float g_interp_dt = 0.0f;
std::vector<std::pair<rf::Vector3, rf::Vector3>> g_interp_scratch;

std::vector<CollideState> g_collide_state[2];
rf::Vector3 g_camera_pos;
bool g_camera_pos_valid = false;

std::unique_ptr<rf::Particle[]> g_pool_storage[2];
int g_pool_storage_capacity[2] = {};

bool cull_sim_enabled()
{
    return g_alpine_game_config.particle_sim_cull && !rf::is_dedicated_server && !is_headless_mode();
}

bool fixed_step_enabled()
{
    return g_alpine_game_config.particle_sim_fixed_step && !rf::is_dedicated_server;
}

bool collide_lod_enabled()
{
    return g_alpine_game_config.particle_sim_collide_lod && !rf::is_dedicated_server;
}

// Off only to debug against the stock drawing path, which every renderer but D3D11 always uses.
bool g_gpu_particles = true;

bool gpu_particles_enabled()
{
    return g_gpu_particles && is_d3d11();
}

// Level emitters that cannot affect gameplay are the only ones that may be frozen.
bool emitter_is_cullable(const rf::ParticleEmitter* emitter)
{
    return emitter
        && emitter->parent_handle == 0 && emitter->uid > 0
        && !(emitter->pci.flags2 & rf::PTF2_DAMAGES)
        && !(emitter->pci.flags & rf::PTF_HIT_CALLBACK);
}

// Attached emitters may be fixed stepped too, gameplay relevant ones never are.
bool emitter_is_fixed_step_eligible(const rf::ParticleEmitter* emitter)
{
    return emitter
        && !(emitter->pci.flags2 & rf::PTF2_DAMAGES)
        && !(emitter->pci.flags & rf::PTF_HIT_CALLBACK);
}

CollideState* collide_state_for(const rf::Particle* particle)
{
    const int pool_id = particle->pool_id;
    if (pool_id < 0 || pool_id > 1) {
        return nullptr;
    }
    const rf::ParticlePool& pool = rf::particle_pools[pool_id];
    if (!pool.particles || pool.capacity <= 0) {
        return nullptr;
    }
    std::vector<CollideState>& states = g_collide_state[pool_id];
    if (states.size() != static_cast<std::size_t>(pool.capacity)) {
        states.assign(static_cast<std::size_t>(pool.capacity), CollideState{});
    }
    const std::ptrdiff_t index = particle - pool.particles;
    if (index < 0 || index >= pool.capacity) {
        return nullptr;
    }
    return &states[static_cast<std::size_t>(index)];
}

// Points the engine pool records at Alpine owned arrays and rebuilds their free lists the way
// the stock rebuild does. Only ever called from the two wholesale rebuilds, which free every
// particle first, so no live particle is ever stranded by the swap. There is no eviction: a
// spawn on hit can call particle_create from inside the particle_list_move walk, and unlinking
// the node that walk saved as next would corrupt the iteration.
void relink_pools()
{
    const int configured = g_alpine_game_config.particle_pool_size;
    for (int i = 0; i < 2; ++i) {
        const int desired_cap = configured == 0 ? stock_pool_capacity[i] : configured;
        if (!g_pool_storage[i] || g_pool_storage_capacity[i] != desired_cap) {
            g_pool_storage[i] = std::make_unique<rf::Particle[]>(static_cast<std::size_t>(desired_cap));
            g_pool_storage_capacity[i] = desired_cap;
        }

        rf::ParticlePool& pool = rf::particle_pools[i];
        pool.particles = g_pool_storage[i].get();
        pool.capacity = desired_cap;
        pool.num_used = 0;
        pool.free_list.next = &pool.free_list;
        pool.free_list.prev = &pool.free_list;
        for (int j = 0; j < desired_cap; ++j) {
            rf::Particle& entry = pool.particles[j];
            entry.flags = 0;
            entry.flags2 = 0;
            entry.next = &pool.free_list;
            entry.prev = pool.free_list.prev;
            pool.free_list.prev->next = &entry;
            pool.free_list.prev = &entry;
        }
    }
}

struct ParticleCounts
{
    int collide = 0;
    int damaging = 0;
    int fade = 0;
};

void count_particle_list(const rf::Particle& head, ParticleCounts& counts)
{
    for (const rf::Particle* p = head.next; p && p != &head; p = p->next) {
        if (p->flags & rf::PTF_COLLIDE) {
            ++counts.collide;
        }
        if (p->flags2 & rf::PTF2_DAMAGES) {
            ++counts.damaging;
        }
        if (p->flags & rf::PTF_CLR_CHANGE) {
            ++counts.fade;
        }
    }
}

void report_line(const std::string& text)
{
    rf::console::print("{}", text);
    xlog::info("{}", text);
}

void finish_capture()
{
    g_ps.capturing = false;
    const int frames = std::max(g_ps.capture_frames, 1);
    const FrameCounters& sum = g_ps.capture_sum;
    const FrameCounters& peak = g_ps.capture_max;
    report_line(std::format("particle perf capture: {} s, {} frames", g_ps.capture_secs, g_ps.capture_frames));
    report_line(std::format("  sim us     avg {}  max {}", sum.sim_us / frames, peak.sim_us));
    report_line(std::format("  render us  avg {}  max {}", sum.render_us / frames, peak.render_us));
    report_line(std::format("  simulated  avg {}  max {}   rendered avg {}  max {}",
        sum.simulated / frames, peak.simulated, sum.rendered / frames, peak.rendered));
    report_line(std::format("  emitters culled avg {}  max {}   cull sim {}",
        sum.emitters_culled / frames, peak.emitters_culled, cull_sim_enabled() ? "on" : "off"));
    report_line(std::format("  warmup steps avg {}  max {}   emitters warmed {}   warmup {}",
        sum.warmup_steps / frames, peak.warmup_steps, sum.warmup_emitters, g_ps.warmup ? "on" : "off"));
    report_line(std::format("  eligible steps avg {}  max {}   fixed step {}  rate {}",
        sum.eligible_steps / frames, peak.eligible_steps,
        fixed_step_enabled() ? "on" : "off", g_alpine_game_config.particle_sim_rate));
    report_line(std::format("  collide tested avg {}  max {}   skipped avg {}  max {}   collide lod {}",
        sum.collide_tested / frames, peak.collide_tested, sum.collide_skipped / frames, peak.collide_skipped,
        collide_lod_enabled() ? "on" : "off"));
    report_line(std::format("  gpu particles {}  drawn avg {}  max {}   draws avg {}  max {}",
        gpu_particles_enabled() ? "on" : "off", sum.gpu_drawn / frames, peak.gpu_drawn,
        sum.gpu_draws / frames, peak.gpu_draws));
    report_line(std::format("  spawns {}  failures {}", sum.spawn_attempts, sum.spawn_failures));
    report_line(std::format("  pool high water {}/{}  {}/{}",
        g_ps.capture_pool_high_water[0], rf::particle_pools[0].capacity,
        g_ps.capture_pool_high_water[1], rf::particle_pools[1].capacity));
}

// Runs before the engine simulate pass: the previous frame is complete at this point because
// the emitter render callbacks for it already ran.
void frame_boundary()
{
    g_ps.cur.rendered = rf::num_particles_rendered_this_frame; // zeroed by the call that follows
    g_ps.last = g_ps.cur;
    g_ps.cur = {};

    if (!perf_timing_active()) {
        return;
    }
    const int64_t now_ms = timer::get_i64(1000);

    if (g_ps.window_start_ms == 0) {
        g_ps.window_start_ms = now_ms;
    }
    ++g_ps.window_frames;
    g_ps.window_sim_sum += g_ps.last.sim_us;
    g_ps.window_sim_max = std::max(g_ps.window_sim_max, g_ps.last.sim_us);
    g_ps.window_render_sum += g_ps.last.render_us;
    g_ps.window_render_max = std::max(g_ps.window_render_max, g_ps.last.render_us);

    if (now_ms - g_ps.window_start_ms >= 1000) {
        g_ps.shown_sim_avg = g_ps.window_sim_sum / g_ps.window_frames;
        g_ps.shown_sim_max = g_ps.window_sim_max;
        g_ps.shown_render_avg = g_ps.window_render_sum / g_ps.window_frames;
        g_ps.shown_render_max = g_ps.window_render_max;
        g_ps.window_start_ms = now_ms;
        g_ps.window_frames = 0;
        g_ps.window_sim_sum = 0;
        g_ps.window_sim_max = 0;
        g_ps.window_render_sum = 0;
        g_ps.window_render_max = 0;
    }

    if (g_ps.capturing) {
        ++g_ps.capture_frames;
        for (const auto field : frame_counter_fields) {
            g_ps.capture_sum.*field += g_ps.last.*field;
            g_ps.capture_max.*field = std::max(g_ps.capture_max.*field, g_ps.last.*field);
        }
        for (int i = 0; i < 2; ++i) {
            g_ps.capture_pool_high_water[i] = std::max(g_ps.capture_pool_high_water[i], rf::particle_pools[i].num_used);
        }
        if (now_ms >= g_ps.capture_end_ms) {
            finish_capture();
        }
    }
}

// Replays the spawns and ageing missed while an emitter was frozen, so a stream that needs time
// to reach the player is already established when its room comes back into view.
void warm_up_emitter(rf::ParticleEmitter* emitter, int64_t culled_ms, int& budget)
{
    if (!g_ps.warmup || budget <= 0 || !emitter->active || (emitter->emitter_flags & rf::PEF_ALTERNATE_STATES)) {
        return;
    }
    const float culled_secs = static_cast<float>(culled_ms) / 1000.0f;
    const float replay_secs = std::min(culled_secs, emitter->max_life_secs);
    int num_steps = std::min(static_cast<int>(replay_secs / warmup_step), warmup_max_steps_per_emitter);
    num_steps = std::min(num_steps, budget);
    if (num_steps <= 0) {
        return;
    }
    budget -= num_steps;

    const float mean_delay = std::max((emitter->min_spawn_delay + emitter->max_spawn_delay) * 0.5f, 1.0f / 60.0f);
    float acc = mean_delay; // so the first step spawns
    for (int i = 0; i < num_steps; ++i) {
        acc += warmup_step;
        int spawned = 0;
        while (acc >= mean_delay && spawned < warmup_max_spawns_per_step) {
            emitter->spawn_first_particle();
            acc -= mean_delay;
            ++spawned;
        }
        rf::particle_list_move(&emitter->particle_list, warmup_step);
    }

    g_ps.cur.warmup_steps += num_steps;
    ++g_ps.cur.warmup_emitters;
}

// Same sequence as the stock particle_move_all: the global list, every emitter list and the
// secondary list. Culled emitter lists are skipped and eligible ones run at the fixed rate.
void move_all_visible(float frametime)
{
    const int64_t now_ms = timer::get_i64(1000);
    int warmup_budget = warmup_max_steps_per_frame;

    const float step = 1.0f / static_cast<float>(g_alpine_game_config.particle_sim_rate);
    float eligible_dt = frametime;
    int eligible_steps = 1;
    if (!fixed_step_enabled() || frametime >= step) {
        // At or below the sim rate this matches stock exactly and slow frames never cost more
        g_sim_acc = 0.0f;
        g_interp_dt = 0.0f;
    }
    else {
        g_sim_acc += frametime;
        eligible_steps = std::min(static_cast<int>(g_sim_acc / step), fixed_step_max_steps_per_frame);
        g_sim_acc -= eligible_steps * step;
        eligible_dt = step;
        g_interp_dt = g_sim_acc;
    }
    g_ps.cur.eligible_steps = eligible_steps;

    rf::num_particles_simulated_this_frame = 0;
    rf::num_particles_rendered_this_frame = 0;
    rf::particle_list_move(&rf::particle_pools[0].active_list, frametime);
    rf::ParticleEmitter* emitter = rf::particle_emitter_list_head.next;
    while (emitter != &rf::particle_emitter_list_head) {
        const bool culled = particle_sim_emitter_culled(emitter);
        if (emitter_is_cullable(emitter)) {
            EmitterRecord& rec = g_emitters[emitter];
            if (culled != rec.culled) {
                rec.culled = culled;
                if (culled) {
                    rec.culled_since_ms = now_ms;
                }
                else if (now_ms - rec.culled_since_ms >= warmup_min_culled_ms) {
                    warm_up_emitter(emitter, now_ms - rec.culled_since_ms, warmup_budget);
                }
            }
        }
        if (culled) {
            ++g_ps.cur.emitters_culled;
        }
        else if (emitter_is_fixed_step_eligible(emitter)) {
            for (int i = 0; i < eligible_steps; ++i) {
                rf::particle_list_move(&emitter->particle_list, eligible_dt);
            }
        }
        else {
            rf::particle_list_move(&emitter->particle_list, frametime);
        }
        emitter = emitter->next; // the stock loop also reads next after the move call
    }
    rf::particle_list_move(&rf::particle_secondary_list, frametime);
}

FunHook<void(float)> particle_move_all_hook{
    0x00496480,
    [](float frametime) {
        frame_boundary();
        ++g_frame;

        g_camera_pos_valid = rf::local_player && rf::local_player->cam;
        if (g_camera_pos_valid) {
            g_camera_pos = rf::camera_get_pos(rf::local_player->cam);
        }

        const bool timed = perf_timing_active();
        const int64_t start_us = timed ? timer::get_i64(1000000) : 0;
        if (cull_sim_enabled() || fixed_step_enabled()) {
            move_all_visible(frametime);
        }
        else {
            particle_move_all_hook.call_target(frametime);
        }
        if (timed) {
            g_ps.cur.sim_us = timer::get_i64(1000000) - start_us;
        }
        g_ps.cur.simulated = rf::num_particles_simulated_this_frame;
    },
};

// Runs once per rendered room, before its emitters are handed to the portal renderer. Stamping
// here makes the visibility signal the room, not the particle derived portal sphere.
FunHook<void(rf::GRoom*)> particle_emitter_add_to_render_list_hook{
    0x00497C20,
    [](rf::GRoom* room) {
        for (rf::ParticleEmitter* e = rf::particle_emitter_list_head.next;
             e != &rf::particle_emitter_list_head; e = e->next) {
            if (e->room == room && emitter_is_cullable(e)) {
                g_emitters[e].last_seen_frame = g_frame;
            }
        }
        particle_emitter_add_to_render_list_hook.call_target(room);
    },
};

FunHook<void(rf::ParticleEmitter*, rf::GSolid*)> particle_emitter_render_hook{
    0x00497BF0,
    [](rf::ParticleEmitter* emitter, rf::GSolid* solid) {
        // The fixed step remainder is extrapolated forward for the draw only
        const float interp_dt = g_interp_dt > 0.0f && fixed_step_enabled()
            && emitter_is_fixed_step_eligible(emitter) ? g_interp_dt : 0.0f;

        const bool timed = perf_timing_active();
        const int64_t start_us = timed ? timer::get_i64(1000000) : 0;

        int gpu_drawn = 0;
        int gpu_draws = 0;
        const bool on_gpu = gpu_particles_enabled()
            && gr::d3d11::render_particle_emitter(emitter, interp_dt, &gpu_drawn, &gpu_draws);
        if (on_gpu) {
            // The batched path applies the extrapolation itself, without touching the particles
            g_ps.cur.gpu_drawn += gpu_drawn;
            g_ps.cur.gpu_draws += gpu_draws;
        }
        else {
            if (interp_dt > 0.0f) {
                g_interp_scratch.clear();
                for (rf::Particle* p = emitter->particle_list.next; p != &emitter->particle_list; p = p->next) {
                    g_interp_scratch.emplace_back(p->pos, p->last_pos);
                    const rf::Vector3 delta = p->vel * interp_dt;
                    p->pos += delta;
                    p->last_pos += delta; // the stretched quad draws last_pos to pos, keep the streak length
                }
            }

            particle_emitter_render_hook.call_target(emitter, solid);

            if (interp_dt > 0.0f) {
                std::size_t i = 0;
                for (rf::Particle* p = emitter->particle_list.next;
                     p != &emitter->particle_list && i < g_interp_scratch.size(); p = p->next, ++i) {
                    p->pos = g_interp_scratch[i].first;
                    p->last_pos = g_interp_scratch[i].second;
                }
            }
        }

        if (timed) {
            g_ps.cur.render_us += timer::get_i64(1000000) - start_us;
        }
        ++g_ps.cur.emitters_rendered;
    },
};

// Room render item of a particle no emitter owns, one item per particle. Consecutive items share one batch,
// which the D3D11 room render item hook flushes before any other item draws.
FunHook<void(rf::Particle*)> particle_render_item_hook{
    0x00496830,
    [](rf::Particle* particle) {
        if (gpu_particles_enabled() && gr::d3d11::batched_particle(*particle)) {
            ++g_ps.cur.gpu_drawn;
            return;
        }
        particle_render_item_hook.call_target(particle);
    },
};

// Reached only by fall through when the collide world flag is set. Skipping the raycast for a
// distant cosmetic particle costs a delayed hit reaction, never a tunnel: the deferred test
// rewinds to where the last test ended so the segment still covers the whole untested path.
CodeInjection particle_collide_lod_injection{
    0x004951E6,
    [](auto& regs) {
        rf::Particle* particle = regs.esi;
        if (!collide_lod_enabled() || !rf::particle_collide_world_enabled || (particle->flags2 & rf::PTF2_DAMAGES)
            || (particle->flags & rf::PTF_HIT_CALLBACK)) {
            return;
        }
        CollideState* state = collide_state_for(particle);
        if (!state) {
            return;
        }

        const float dt = addr_as_ref<float>(regs.esp + 0x90);
        // age was advanced by dt earlier in this step, so a first step particle has age == dt
        if (particle->age <= dt || state->last_age == 0.0f || particle->age < state->last_age) {
            // a fresh particle took over this pool slot
            state->last_tested = particle->last_pos;
            state->acc_dt = 0.0f;
            state->frames = 0;
        }
        state->last_age = particle->age;

        int k = 1;
        if (g_camera_pos_valid) {
            const float dist = (particle->pos - g_camera_pos).len();
            k = dist < collide_lod_near_dist ? 1 : (dist < collide_lod_mid_dist ? 2 : 4);
        }

        state->acc_dt += dt;
        ++state->frames;
        if (state->frames < k) {
            ++g_ps.cur.collide_skipped;
            regs.eip = 0x004951F9; // the simple move, which falls into the shared tail
            return;
        }

        particle->pos = state->last_tested;
        rf::particle_move_collide(particle, state->acc_dt);
        state->last_tested = particle->pos;
        state->acc_dt = 0.0f;
        state->frames = 0;
        ++g_ps.cur.collide_tested;
        regs.eip = 0x00495216; // the shared tail, skipping the stock collide call
    },
};

// The stock 128 emitter records are shared by every emitter in the game (level, entity, fire, flamethrower,
// weapon trail, explosion, Alpine effects) and silently run out, which also stops fire and flamethrower
// damage. Extra records join the free list once at game init and are kept until exit, so no emitter pointer can
// outlive its record. Free records cost nothing per frame: the engine only walks emitters in use, and the
// particle pools still bound what live emitters can spawn.
constexpr int emitter_capacity = 8192;
std::unique_ptr<std::byte[]> g_extra_emitter_storage;
int g_emitter_capacity = rf::particle_emitter_stock_capacity;

// Called once, from game init (0x004B22B5). Its prologue is a 10 byte non relative MOV.
FunHook<void()> particle_emitter_pool_init_hook{
    0x004973F0,
    []() {
        particle_emitter_pool_init_hook.call_target();
        const int num_extra = emitter_capacity - rf::particle_emitter_stock_capacity;
        g_extra_emitter_storage = std::make_unique<std::byte[]>(num_extra * sizeof(rf::ParticleEmitter));
        auto* extra = reinterpret_cast<rf::ParticleEmitter*>(g_extra_emitter_storage.get());
        rf::ParticleEmitter& head = rf::particle_emitter_free_list_head;
        for (int i = 0; i < num_extra; ++i) {
            rf::ParticleEmitter* emitter = rf::particle_emitter_construct(&extra[i]);
            emitter->next = &head;
            emitter->prev = head.prev;
            head.prev->next = emitter;
            head.prev = emitter;
        }
        g_emitter_capacity = emitter_capacity;
    },
};

// Game init pool build. Its prologue is 8 non relative bytes, so a FunHook is safe here.
FunHook<void()> particle_pools_init_hook{
    0x00494E70,
    []() {
        particle_pools_init_hook.call_target();
        relink_pools();
    },
};

// Level init pool build. Its own prologue holds a CALL at byte 3, so the call site is hooked instead.
CallHook<void(int)> particle_pools_level_init_hook{
    0x00460C71,
    [](int enable_collision) {
        particle_pools_level_init_hook.call_target(enable_collision);
        relink_pools();
    },
};

// The room pass render list (g_room_render_item_add 0x004D3560, drained by g_render_room_objects 0x004D3C40)
// holds 2048 items per room and refuses the rest. Every particle no emitter owns takes an item of its own, so with
// the larger pools they alone could fill it and stop the room's emitters from drawing. The list and the pointer
// lists its sortable items pass through are sized for the objects plus every particle and emitter. Alpha
// grouping, whose per group member arrays stay at 2048, is disabled in g_solid.cpp, so it never sees them.
constexpr int stock_room_render_items = 2048;
constexpr int room_render_items =
    stock_room_render_items + 2 * AlpineGameSettings::max_particle_pool_size + emitter_capacity;

StaticBufferResizePatch<rf::GRoomRenderItem> room_render_items_resize_patch{
    0x0088FD20,
    stock_room_render_items,
    room_render_items,
    {
        {0x004D31BC}, // static init (trivial constructor)
        {0x004D363D}, // g_room_render_item_add
        {0x004D3CA8}, // g_render_room_objects, at +0x10
    },
};

// Sortable items in queue order
StaticBufferResizePatch<rf::GRoomRenderItem*> room_render_sortable_resize_patch{
    0x00879B00,
    stock_room_render_items,
    room_render_items,
    {
        {0x004D3CA3},
        {0x004D3D24},
    },
};

// Sortable items 0x004D43E0 leaves ungrouped (all of them), then sorted back to front
StaticBufferResizePatch<rf::GRoomRenderItem*> room_render_ungrouped_resize_patch{
    0x009B9550,
    stock_room_render_items,
    room_render_items,
    {
        {0x004D3D64},
        {0x004D3D6B},
        {0x004D3F24},
        {0x004D4035},
        {0x004D44B9},
    },
};

// Sorted items drawn before the room's liquid surface (all of them in a room without one)
StaticBufferResizePatch<rf::GRoomRenderItem*> room_render_before_liquid_resize_patch{
    0x0088BB10,
    stock_room_render_items,
    room_render_items,
    {
        {0x004D3F1F},
        {0x004D403A},
        {0x004D40CC},
        {0x004D42C3},
    },
};

// Sorted items drawn after the room's liquid surface
StaticBufferResizePatch<rf::GRoomRenderItem*> room_render_after_liquid_resize_patch{
    0x0088DB10,
    stock_room_render_items,
    room_render_items,
    {
        {0x004D3F1A},
        {0x004D41D6},
        {0x004D43B1},
    },
};

ConsoleCommand2 dbg_particles_cmd{
    "dbg_particles",
    []() {
        g_ps.overlay = !g_ps.overlay;
        rf::console::print("Particle performance overlay is {}", g_ps.overlay ? "enabled" : "disabled");
    },
    "Toggle the particle performance overlay",
    "dbg_particles",
};

ConsoleCommand2 dbg_particles_log_cmd{
    "dbg_particles_log",
    [](std::optional<int> seconds) {
        const int secs = std::clamp(seconds.value_or(10), 1, 600);
        g_ps.capturing = true;
        g_ps.capture_secs = secs;
        g_ps.capture_end_ms = timer::get_i64(1000) + secs * 1000;
        g_ps.capture_frames = 0;
        g_ps.capture_sum = {};
        g_ps.capture_max = {};
        g_ps.capture_pool_high_water[0] = 0;
        g_ps.capture_pool_high_water[1] = 0;
        rf::console::print("Capturing particle performance for {} seconds...", secs);
    },
    "Log particle performance counters for a number of seconds",
    "dbg_particles_log [seconds]",
};

ConsoleCommand2 particle_cull_sim_cmd{
    "cl_particlecull",
    []() {
        g_alpine_game_config.particle_sim_cull = !g_alpine_game_config.particle_sim_cull;
        rf::console::print("Particle simulation culling is {}",
            g_alpine_game_config.particle_sim_cull ? "enabled" : "disabled");
    },
    "Toggle skipping simulation of level particle emitters that are not visible",
    "cl_particlecull",
};

ConsoleCommand2 particle_sim_warmup_cmd{
    "dbg_particlewarmup",
    []() {
        g_ps.warmup = !g_ps.warmup;
        rf::console::print("Particle emitter warm-up on wake is {}", g_ps.warmup ? "enabled" : "disabled");
    },
    "Toggle ramping a frozen level particle emitter back up when it becomes visible again",
    "dbg_particlewarmup",
};

ConsoleCommand2 particle_fixed_step_cmd{
    "cl_particlefixedstep",
    []() {
        g_alpine_game_config.particle_sim_fixed_step = !g_alpine_game_config.particle_sim_fixed_step;
        rf::console::print("Fixed timestep particle simulation is {}",
            g_alpine_game_config.particle_sim_fixed_step ? "enabled" : "disabled");
    },
    "Toggle stepping cosmetic particles at a fixed rate with render interpolation",
    "cl_particlefixedstep",
};

ConsoleCommand2 particle_sim_rate_cmd{
    "cl_particlerate",
    [](std::optional<int> hz) {
        if (hz) {
            g_alpine_game_config.set_particle_sim_rate(hz.value());
        }
        rf::console::print("Particle simulation rate is {} Hz (valid range {}-{})",
            g_alpine_game_config.particle_sim_rate, AlpineGameSettings::min_particle_sim_rate,
            AlpineGameSettings::max_particle_sim_rate);
    },
    "Set the fixed particle simulation rate in Hz",
    "cl_particlerate [hz]",
};

ConsoleCommand2 gpu_particles_cmd{
    "dbg_particlegpu",
    []() {
        g_gpu_particles = !g_gpu_particles;
        rf::console::print("GPU particle rendering: {}{}", g_gpu_particles ? "on" : "off",
            is_d3d11() ? "" : " (Direct3D 11 only, currently inactive)");
    },
    "Toggle GPU rendering of particles, plankton and weather (off = stock CPU path)",
    "dbg_particlegpu",
};

ConsoleCommand2 particle_collide_lod_cmd{
    "cl_particlecollidelod",
    []() {
        g_alpine_game_config.particle_sim_collide_lod = !g_alpine_game_config.particle_sim_collide_lod;
        rf::console::print("Amortised particle world collision is {}",
            g_alpine_game_config.particle_sim_collide_lod ? "enabled" : "disabled");
    },
    "Toggle testing cosmetic particle world collision less often with camera distance",
    "cl_particlecollidelod",
};

ConsoleCommand2 particle_pool_size_cmd{
    "cl_particlepoolsize",
    [](std::optional<int> size) {
        if (size) {
            g_alpine_game_config.set_particle_pool_size(size.value());
            rf::console::print("Applies at the next level load.");
        }
        if (g_alpine_game_config.particle_pool_size == 0) {
            rf::console::print("Particle pool size is stock ({} and {})", stock_pool_capacity[0],
                stock_pool_capacity[1]);
        }
        else {
            rf::console::print("Particle pool size is {} per pool (0 for stock, otherwise {}-{})",
                g_alpine_game_config.particle_pool_size, AlpineGameSettings::min_particle_pool_size,
                AlpineGameSettings::max_particle_pool_size);
        }
    },
    "Set the capacity of each particle pool, 0 for the stock capacities",
    "cl_particlepoolsize [n]",
};

} // namespace

int particle_sim_emitter_capacity()
{
    return g_emitter_capacity;
}

bool particle_sim_gpu_enabled()
{
    return gpu_particles_enabled();
}

bool particle_sim_emitter_culled(const rf::ParticleEmitter* emitter)
{
    if (!cull_sim_enabled() || !emitter_is_cullable(emitter)) {
        return false;
    }
    const auto it = g_emitters.find(emitter);
    const int stamp = it != g_emitters.end() && it->second.last_seen_frame != 0
        ? it->second.last_seen_frame
        : g_map_cleared_frame;
    return g_frame - stamp > visible_frame_slack;
}

void particle_sim_level_reset()
{
    g_emitters.clear();
    g_map_cleared_frame = g_frame;
    g_sim_acc = 0.0f;
    g_interp_dt = 0.0f;
    g_collide_state[0].clear();
    g_collide_state[1].clear();
    g_camera_pos_valid = false;
}

void particle_sim_note_create(int pool_id)
{
    if (pool_id < 0 || pool_id > 1) {
        return;
    }
    ++g_ps.cur.spawn_attempts;
    const rf::ParticlePool& pool = rf::particle_pools[pool_id];
    if (pool.free_list.next == &pool.free_list) {
        ++g_ps.cur.spawn_failures;
        ++g_ps.total_spawn_failures;
    }
}

void particle_sim_render_ui()
{
    if (!g_ps.overlay || !rf::gameseq_in_gameplay()) {
        return;
    }

    ParticleCounts counts;
    int emitters_total = 0;
    int emitters_active = 0;
    count_particle_list(rf::particle_pools[0].active_list, counts);
    count_particle_list(rf::particle_secondary_list, counts);
    for (const rf::ParticleEmitter* e = rf::particle_emitter_list_head.next;
         e && e != &rf::particle_emitter_list_head; e = e->next) {
        ++emitters_total;
        if (e->active) {
            ++emitters_active;
        }
        count_particle_list(e->particle_list, counts);
    }

    rf::gr::set_color(255, 255, 255, 255);
    const int font_id = hud_get_default_font();
    const int x = rf::gr::screen_width() - 340;
    int y = 120;
    auto line = [&](const std::string& text) {
        rf::gr::string(x, y, text.c_str(), font_id);
        y += 12;
    };

    line(std::format("particles sim {}  rend {}  (stock counters)", g_ps.last.simulated, g_ps.last.rendered));
    line(std::format("pool0 {}/{}  pool1 {}/{}", rf::particle_pools[0].num_used, rf::particle_pools[0].capacity,
        rf::particle_pools[1].num_used, rf::particle_pools[1].capacity));
    line(std::format("spawn fail {}/frame  total {}", g_ps.last.spawn_failures, g_ps.total_spawn_failures));
    line(std::format("emitters total {}  rendered {}  active {}  culled {}", emitters_total,
        g_ps.last.emitters_rendered, emitters_active, g_ps.last.emitters_culled));
    line(std::format("collide {}  damaging {}  fade {}", counts.collide, counts.damaging, counts.fade));
    line(std::format("cull sim {}  warmup {}  steps {}  warmed {}", cull_sim_enabled() ? "on" : "off",
        g_ps.warmup ? "on" : "off", g_ps.last.warmup_steps, g_ps.last.warmup_emitters));
    line(std::format("fixed step {}  rate {}  steps {}  interp ms {:.1f}", fixed_step_enabled() ? "on" : "off",
        g_alpine_game_config.particle_sim_rate, g_ps.cur.eligible_steps, g_interp_dt * 1000.0f));
    line(std::format("collide lod {}  tested {}  skipped {}", collide_lod_enabled() ? "on" : "off",
        g_ps.last.collide_tested, g_ps.last.collide_skipped));
    line(std::format("gpu particles {}  drawn {}  draws {}", gpu_particles_enabled() ? "on" : "off",
        g_ps.last.gpu_drawn, g_ps.last.gpu_draws));
    line(std::format("sim avg/max us  {}/{}   render avg/max us  {}/{}", g_ps.shown_sim_avg, g_ps.shown_sim_max,
        g_ps.shown_render_avg, g_ps.shown_render_max));
}

void particle_sim_do_patch()
{
    particle_move_all_hook.install();
    particle_emitter_add_to_render_list_hook.install();
    particle_emitter_render_hook.install();
    particle_render_item_hook.install();
    particle_collide_lod_injection.install();
    particle_pools_init_hook.install();
    particle_emitter_pool_init_hook.install();
    particle_pools_level_init_hook.install();

    room_render_items_resize_patch.install();
    room_render_sortable_resize_patch.install();
    room_render_ungrouped_resize_patch.install();
    room_render_before_liquid_resize_patch.install();
    room_render_after_liquid_resize_patch.install();
    write_mem<int>(0x004D3621 + 1, room_render_items); // g_room_render_item_add: cmp eax, 0x800

    dbg_particles_cmd.register_cmd();
    dbg_particles_log_cmd.register_cmd();
    particle_cull_sim_cmd.register_cmd();
    particle_sim_warmup_cmd.register_cmd();
    particle_fixed_step_cmd.register_cmd();
    particle_sim_rate_cmd.register_cmd();
    particle_collide_lod_cmd.register_cmd();
    gpu_particles_cmd.register_cmd();
    particle_pool_size_cmd.register_cmd();
}
