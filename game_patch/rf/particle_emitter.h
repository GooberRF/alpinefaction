#pragma once

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
    int age_pct_to_finish_vbm;
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
    int age_pct_to_finish_vbm;
    void (*hit_callback)(Vector3 *hit_pos, const Vector3 *vel, Vector3 *normal, float *, int *is_liquid, int *hit_obj_uid);
    GRoom *room;
    ParticleEmitter *emitter;
    Vector3 last_pos;
};
static_assert(sizeof(Particle) == 0x78);

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
};
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
};
static_assert(sizeof(BoltEmitter) == 0x18C);

static auto& level_get_particle_emitter_from_uid = addr_as_ref<ParticleEmitter*(int uid)>(0x0045D630);
static auto& level_get_bolt_emitter_from_uid = addr_as_ref<BoltEmitter*(int uid)>(0x0045D680);

static auto& bolt_emitter_list = addr_as_ref<VArray<BoltEmitter*>>(0x0064608C);
static auto& particle_emitter_list = addr_as_ref<VArray<ParticleEmitter*>>(0x00646080);

}
