#pragma once

#include <cstddef>
#include "math/vector.h"
#include "os/timestamp.h"
#include "os/array.h"
#include "gr/gr.h"

namespace rf
{

struct GRoom;
struct ParticleEmitter;
struct Object;

enum ParticleEmitterFlags
{
    PEF_IMMEDIATE = 0x2,
    PEF_CONTINOUS = 0x4,
    PEF_DIRDEPEND = 0x8,
    PEF_INITIALLY_ON = 0x10,
    PEF_ALTERNATE_STATES = 0x20,
    PEF_DONT_MOVE_WITH_PARENT = 0x40,
    PEF_ACCEL_WITH_PARENT = 0x80,
};

enum ParticleFlags : unsigned
{
    PTF_GLOW = 0x2,
    PTF_CLR_CHANGE = 0x4,
    PTF_GRAVITY = 0x8,
    PTF_COLLIDE = 0x10,
    PTF_HIT_CALLBACK = 0x20,
    PTF_ACCELERATE = 0x40,
    PTF_EXPLODE = 0x80,
    PTF_LOOP = 0x100, // stock frame index frac(age) * 15 / n each second, not once over the lifetime
    PTF_RANDOM_ORIENT = 0x200,
    PTF_COLLIDE_LIQUID = 0x400,
    PTF_COLLIDE_AND_DIE = 0x800,
    PTF_NO_Z_CHECK = 0x2000,
    PTF_VEL_STRETCH = 0x4000,
    PTF_BOUNCINESS_MASK = 0x000F0000, // $bounciness 0-15 << 16
    PTF_STICKINESS_MASK = 0x00F00000, // $stickiness 0-15 << 20
    PTF_SWIRLINESS_MASK = 0x0F000000, // $swirliness 0-15 << 24
    PTF_WIND = 0xF0000000,
};

enum ParticleFlags2
{
    PTF2_DAMAGES = 0x1,
    PTF2_HOLD_LAST_FRAME = 0x4, // the frame set finishes at age_pct_to_finish_vbm of the lifetime
    PTF2_FIRE_DAMAGE = 0x8,
    PTF2_DAMAGE_FACTOR_MASK = 0xF000, // $damage_factor 0-15 << 12
};

struct ParticleEmitterType
{
    int uid;
    Vector3 pos;
    Vector3 dir;
    float dir_rand;
    float min_vel;
    float max_vel;
    float min_spawn_delay;
    float max_spawn_delay;
    float spawn_radius;
    int16_t flags;
    float min_life_secs;
    float max_life_secs;
    float min_pradius;
    float max_pradius;
    float growth_rate;
    float acceleration;
    float gravity_scale;
    float alt_stateon_time;
    float alt_stateon_time_variance;
    float alt_stateoff_time;
    float alt_stateoff_time_variance;
    int bitmap;
    int num_frames;
    Color particle_clr;
    Color particle_clr_dest;
    int particle_flags;
    int particle_flags2;
    float age_pct_to_finish_vbm;
    float active_distance;
};
static_assert(sizeof(ParticleEmitterType) == 0x84);

struct ParticleCreateInfo
{
    Vector3 pos;
    Vector3 vel;
    float radius;
    float growth_rate;
    float acceleration;
    float gravity_scale;
    float lifetime_seconds;
    int bitmap_handle;
    int num_frames;
    Color clr;
    Color clr_dest;
    int flags;
    int flags2;
    float age_pct_to_finish_vbm;
    int hit_callback;
};
static_assert(sizeof(ParticleCreateInfo) == 0x4C);

struct Particle
{
    Particle *next;
    Particle *prev;
    int parent_handle;
    Vector3 pos;
    Vector3 vel;
    float age;
    Color clr;
    Color clr_dest;
    Color clr_current;
    float max_life_seconds;
    float radius;
    float growth_rate;
    float acceleration;
    float gravity;
    int first_frame_bitmap;
    short num_frames;
    short flags2;
    char pool_id;
    float bitmap_orient;
    int flags;
    float age_pct_to_finish_vbm;
    void (*hit_callback)(Vector3 *hit_pos, const Vector3 *vel, Vector3 *normal, float *, int *is_liquid, int *hit_obj_uid);
    GRoom *room;
    ParticleEmitter *emitter;
    Vector3 last_pos;
};
static_assert(sizeof(Particle) == 0x78);

// Fixed size particle pool (particle_pools_init 0x00494E70). free_list and active_list are sentinel
// nodes: the free list is empty when free_list.next points back at free_list, and particle_create
// silently fails then. Only pool 0's active_list is simulated and drawn, so a pool 1 particle must be
// created into an emitter.
struct ParticlePool
{
    Particle* particles;  // +0x00
    int capacity;         // +0x04
    Particle free_list;   // +0x08
    Particle active_list; // +0x80 the particles no emitter owns
    int num_used;         // +0xF8
};
static_assert(offsetof(ParticlePool, free_list) == 0x08);
static_assert(offsetof(ParticlePool, active_list) == 0x80);
static_assert(offsetof(ParticlePool, num_used) == 0xF8);
static_assert(sizeof(ParticlePool) == 0xFC);

// Underwater plankton mote. Not emitter driven.
struct PlanktonParticle
{
    Vector3 pos;
    Vector3 vel;
    float size;
    ubyte r;
    ubyte g;
    ubyte b;
    ubyte a;
};
static_assert(sizeof(PlanktonParticle) == 0x20);

struct ParticleEmitter
{
    int uid;
    int parent_handle;
    Vector3 pos;
    Vector3 dir;
    float dir_rand;
    float min_vel;
    float max_vel;
    float spawn_radius;
    float min_spawn_delay;
    float max_spawn_delay;
    int emitter_flags;
    float min_life_secs;
    float max_life_secs;
    float min_pradius;
    float max_pradius;
    GRoom *room;
    ParticleCreateInfo pci;
    float cull_radius;
    float max_particle_dist_sq;
    Vector3 world_pos;
    Particle particle_list;
    float on_time;
    float on_time_variance;
    float off_time;
    float off_time_variance;
    float time_to_change;
    float current_state_time;
    bool active;
    float active_distance;
    ParticleEmitter *next;
    ParticleEmitter *prev;
    ParticleEmitter *next_entity_emitter;
    Timestamp spawn_timer;

    Object* get_pos_and_dir(Vector3 *pos, Vector3 *dir)
    {
        return AddrCaller{0x00496BC0}.this_call<Object*>(this, pos, dir);
    }

    void activate()
    {
        AddrCaller{0x004973B0}.this_call(this);
    }

    // Advances one emitter by a frame: alternate on/off states, a particle spawn
    // once the spawn timer is up and a room refresh from the parent object.
    void update()
    {
        AddrCaller{0x004972F0}.this_call(this);
    }

    void spawn_first_particle()
    {
        AddrCaller{0x00496C50}.this_call(this);
    }

    void destroy()
    {
        // Note: the engine function is __cdecl, not __thiscall.
        // Calling it as a this_call makes the engine unlink
        // stack garbage instead of this emitter and crash.
        AddrCaller{0x00497D80}.c_call(this);
    }
};
static_assert(offsetof(ParticleEmitter, particle_list) == 0xB0);
static_assert(offsetof(ParticleEmitter, next) == 0x148);
static_assert(offsetof(ParticleEmitter, prev) == 0x14C);
static_assert(sizeof(ParticleEmitter) == 0x158);

struct BoltInfo
{
    float life;
    void* bez;
    VArray<int> wander_points; // unsure of member type
    Vector3 control1;
    Vector3 control2;
    Vector3 wander1;
    Vector3 wander2;
};
static_assert(sizeof(BoltInfo) == 0x44);

struct BoltEmitter
{
    int uid;
    int parent_handle;
    int target_uid;
    int target_handle;
    Vector3 source_pos;
    Vector3 source_dir;
    Vector3 target_pos;
    Vector3 target_dir;
    float thickness;
    float min_spawn_delay;
    float max_spawn_delay;
    float min_life;
    float max_life;
    float jitter;
    bool active;
    char padding[3];
    int emitter_flags;
    int bitmap_handle;
    int num_frames;
    Color bolt_color;
    GRoom* room;
    BoltInfo bolts[4];
    Timestamp spawn_timer;
    float source_dir_mag;
    float target_dir_mag;

    // returns room && room->visited_this_frame
    bool should_render()
    {
        return AddrCaller{0x0048D620}.this_call<bool>(this);
    }

    void render(const Vector3* eye_pos)
    {
        AddrCaller{0x0048D4B0}.this_call(this, eye_pos);
    }
};
static_assert(sizeof(BoltEmitter) == 0x18C);

static auto& bolt_emitter_list = addr_as_ref<VArray<BoltEmitter*>>(0x0064608C);

static auto& level_get_particle_emitter_from_uid = addr_as_ref<ParticleEmitter*(int uid)>(0x0045D630);
static auto& level_get_bolt_emitter_from_uid = addr_as_ref<BoltEmitter*(int uid)>(0x0045D680);

static auto& particle_create = addr_as_ref<void(int pool_id, ParticleCreateInfo& pci,
    GRoom* room, Vector3* a4, int parent_obj, Particle** result,
    ParticleEmitter* emitter)>(0x00496840);

// Stock pool 0 holds 500 particles and pool 1 1100, until particle_sim's relink_pools resizes them.
// Particle::pool_id selects one.
static auto& particle_pools = addr_as_ref<ParticlePool[2]>(0x007A3B00);

// List sentinel: only next/prev are meaningful, the rest of the record is not an emitter.
static auto& particle_emitter_list_head = addr_as_ref<ParticleEmitter>(0x007BD6E8);

// Sentinel of the list destroyed emitters hand their live particles to; particle_move_all walks it
// after the pool 0 active list and the emitter lists.
static auto& particle_secondary_list = addr_as_ref<Particle>(0x007BD670);

// Both are zeroed by particle_move_all and filled during the simulate and render passes.
static auto& num_particles_simulated_this_frame = addr_as_ref<int>(0x007B2768);
static auto& num_particles_rendered_this_frame = addr_as_ref<int>(0x007B276C);

// Room pass render item callback of a particle no emitter owns (queued by 0x004967A0): calls particle_render.
static auto& particle_render_item = addr_as_ref<void(Particle* particle)>(0x00496830);

// Debug switch: particle_render also draws a bounding box per particle while this is set.
static auto& particle_draw_bbox = addr_as_ref<bool>(0x007B2759);

// Simulates one particle list (a sentinel node): the global list, an emitter list or the secondary list.
static auto& particle_list_move = addr_as_ref<void(Particle* list, float dt)>(0x00495120);

// Moves one particle by dt with world collision, leaving pos at the post move position.
static auto& particle_move_collide = addr_as_ref<void(Particle* particle, float dt)>(0x00495880);
// Set by the level's particle init; while 0, particle_move_collide only clears PTF_COLLIDE and moves nothing.
static auto& particle_collide_world_enabled = addr_as_ref<int>(0x007A3AE8);

// Array of particle emitter type template pointers loaded from emitters.tbl (64 slots)
static auto& g_particle_emitter_types = addr_as_ref<ParticleEmitterType*[64]>(0x007B2770);
static auto& g_num_particle_emitter_types = addr_as_ref<int>(0x007BD99C);
static auto& particle_emitter_type_lookup = addr_as_ref<int(const char* name)>(0x00497550);

// The stock emitter records: a static array of 128 (pool init 0x004973F0) on a circular free list whose
// sentinel is a full record. particle_emitter_create pops its head and returns null when it is empty;
// particle_emitter_destroy appends at its tail. Nothing indexes the array, so records may live anywhere.
constexpr int particle_emitter_stock_capacity = 128;
static auto& particle_emitter_free_list_head = addr_as_ref<ParticleEmitter>(0x007BD840);
static auto& particle_emitter_num_used = addr_as_ref<int>(0x007BD998);
// The constructor the stock array's static initialiser runs on every record.
static auto& particle_emitter_construct =
    addr_as_ref<ParticleEmitter* __fastcall(ParticleEmitter* emitter)>(0x00496FD0);

static auto& particle_emitter_create = addr_as_ref<ParticleEmitter*(
    int parent_handle, ParticleEmitterType& type, GRoom* room, Vector3& pos, bool is_on)>(0x00497CA0);

// spawn particle explosion from explosion.tbl by name.
static auto& particle_explosion_create = addr_as_ref<void(
    const char* name, Vector3* pos, Vector3* dir, float radius_scale, GRoom* room, unsigned int flags)>(0x0048e640);

// Plankton pool capacity; the max_plankton command clamps g_max_plankton to this.
constexpr int num_plankton_particles = 1024;
static auto& g_plankton_particles = addr_as_ref<PlanktonParticle[num_plankton_particles]>(0x007BD9E8);
static auto& g_max_plankton = addr_as_ref<int>(0x005A00B0);
// Half extent of the box a mote is respawned into when it leaves the camera box.
static auto& g_plankton_box_extent = addr_as_ref<float>(0x005A00C0);

// Returns every live particle (main list, per-emitter lists and the secondary list) to the
// free pool via particle_emitter_level_release. Emitter records themselves are NOT destroyed,
// so pointers held by entities/fires/explosions stay valid (verified in disassembly).
static auto& particle_level_release = addr_as_ref<void()>(0x004950A0);

// Destroys each live explosion's owned particle emitters, then zeroes the explosion slot pool
// and rebuilds the free list (supersets explosion_level_init 0x0048E150). Used by level_release;
// safe mid-level.
static auto& explosion_shut_down = addr_as_ref<void()>(0x0048E1C0);

}
