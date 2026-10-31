#pragma once

#include <cstddef>
#include "math/vector.h"
#include "math/matrix.h"
#include "os/array.h"

namespace rf
{
    struct ObjectCreateInfo;
    struct GFace;
    struct Object;
    struct Entity;

    // Surface material (foley.tbl $Material, parser 0x00434960); PCollisionOut::material holds it,
    // resolved from the hit face's texture by 0x00468700.
    enum SurfaceMaterial
    {
        MATERIAL_DEFAULT = 0,
        MATERIAL_ROCK = 1,
        MATERIAL_METAL = 2,
        MATERIAL_FLESH = 3,
        MATERIAL_WATER = 4,
        MATERIAL_LAVA = 5,
        MATERIAL_SOLID = 6,
        MATERIAL_SAND = 7,
        MATERIAL_ICE = 8,
        MATERIAL_GLASS = 9,
    };

    struct PCollisionOut
    {
        Vector3 hit_point;
        Vector3 hit_normal;
        float hit_time;
        int material;
        float inv_mass;
        Vector3 vel;
        int obj_handle;
        int bitmap_handle;
        int is_liquid;
        GFace *hit_face;
        short *hit_face_v3d;
    };
    static_assert(sizeof(PCollisionOut) == 0x44);
    static_assert(offsetof(PCollisionOut, hit_point) == 0);
    static_assert(offsetof(PCollisionOut, obj_handle) == 0x30);

    struct PCollisionSphere
    {
        Vector3 center;
        float radius = 0.0f;
        float spring_const = 0.0f;
        int spring_length = 0;
    };
    static_assert(sizeof(PCollisionSphere) == 0x18);

    struct PhysicsData
    {
        float elasticity;
        float drag;
        float friction;
        int bouyancy;
        float mass;
        Matrix3 body_inv;
        Matrix3 tensor_inv;
        Vector3 pos;
        Vector3 next_pos;
        Matrix3 orient;
        Matrix3 next_orient;
        Vector3 vel;
        Vector3 rotvel;
        Vector3 ang_momentum;
        Vector3 force;
        Vector3 torque;
        float radius;
        VArray<PCollisionSphere> cspheres;
        Vector3 bbox_min;
        Vector3 bbox_max;
        int flags;
        int collision_flags;
        float frame_time_left;
        PCollisionOut collide_out;
    };
    static_assert(sizeof(PhysicsData) == 0x170);
    static_assert(offsetof(PhysicsData, pos) == 0x5C);
    static_assert(offsetof(PhysicsData, next_pos) == 0x68);
    static_assert(offsetof(PhysicsData, bbox_min) == 0x108);
    static_assert(offsetof(PhysicsData, bbox_max) == 0x114);
    static_assert(offsetof(PhysicsData, collide_out) == 0x12C);

    // PhysicsData.flags — controls physics simulation behavior.
    // Set via ObjectCreateInfo.physics_flags, copied to p_data.flags by physics_create_object.
    enum PhysicsFlags : int
    {
        PF_GRAVITY         = 0x1,        // apply gravity
        PF_UNK_02          = 0x2,
        PF_UNK_04          = 0x4,
        PF_UNK_08          = 0x8,
        PF_COLLIDE_WORLD   = 0x10,       // collide with world geometry
        PF_COLLIDE_OBJECTS = 0x20,       // participate in object-object collision pairs
        PF_UNK_40          = 0x40,
        PF_BOUNCE          = 0x100,      // bounce on impact (added when debris_flags & 0x04)
        PF_UNK_400         = 0x400,      // collide_object_object_mesh (0x0049AFE0): a pair with it hits at
                                         // time 0, no hit point
        PF_AUTOMOBILE      = 0x4000,     // entity_create sets it for jeep/apc/driller info flags; selects
                                         // physics_simulate_vehicle_rotation/translation (0x0049E180/0x0049E9F0)
        PF_NET_PLAYER      = 0x8000,     // multi_obj_interp_frame_start/end (0x00484560/0x004845E0) drive this
                                         // object from its ObjInterp keyframes instead of simulating it
        PF_USE_CUSTOM_MAX_VEL = 0x200000, // movement clamps use Entity::custom_max_vel instead of EntityInfo::max_vel; cleared by entity_land (0x00419830)
        PF_SKIP_SIM_ONCE = 0x800000, // set by camera_enter_freelook/deadlook; obj_move_all skips one physics frame, then clears it
        PF_ACCEL_APPLIED = 0x1000000, // dispatcher tail sets this; stock acceleration blocks bail when set (once-per-frame gate)
        PF_SIMULATED_THIS_FRAME = 0x40000000, // set before the physics tick in obj_move_all, cleared at the end of obj_process_physics
    };

    // Cached material, else from the name prefix (and caches it)
    static auto& bm_get_surface_material = addr_as_ref<int(int bm_handle)>(0x00468700);
    static auto& physics_frame_init = addr_as_ref<void(PhysicsData* pd)>(0x0049F1E0);
    // Full per-entity physics step: movemode input conversion (ai.ci), mouselook
    // rotation, acceleration and velocity integration
    static auto& physics_simulate_entity = addr_as_ref<void(Entity* ep)>(0x0049F3C0);
    static auto& physics_update_entity = addr_as_ref<void(Entity* ep)>(0x0049FE40);
    static auto& collide_object_world = addr_as_ref<char(Object* objp)>(0x0049BB70);

    static auto& gravity = addr_as_ref<float>(0x005A00DC);
    static auto& level_set_gravity = addr_as_ref<void(float value)>(0x004A0E20);
    static auto& mp_ground_acceleration = addr_as_ref<float>(0x007C7084);
    static auto& physics_create_object = addr_as_ref<void(PhysicsData *pd, ObjectCreateInfo *oci)>(0x0049EC90);
    static auto& physics_delete_object = addr_as_ref<void(PhysicsData *pd)>(0x0049F1D0);
}
