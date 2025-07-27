#pragma once

#include <patch_common/MemUtils.h>
#include <common/utils/enum-bitwise-operators.h>
#include "math/vector.h"
#include "math/matrix.h"
#include "os/array.h"
#include "os/string.h"
#include "os/timestamp.h"
#include "sound/sound.h"
#include "physics.h"
#include "vmesh.h"

namespace rf
{
    // Forward declarations
    struct GRoom;
    struct GSolid;
    struct VMesh;

    // Typedefs
    using ObjectUseFunction = int;

    // Object

    enum ObjectType
    {
        OT_ENTITY = 0x0,
        OT_ITEM = 0x1,
        OT_WEAPON = 0x2,
        OT_DEBRIS = 0x3,
        OT_CLUTTER = 0x4,
        OT_TRIGGER = 0x5,
        OT_EVENT = 0x6,
        OT_CORPSE = 0x7,
        OT_MOVER = 0x8,
        OT_MOVER_BRUSH = 0x9,
        OT_GLARE = 0xA,
    };

    enum ObjectFlags : int
    {
        OF_DELAYED_DELETE = 0x2,
        OF_INVULNERABLE = 0x4,
        OF_WAS_RENDERED = 0x10,
        OF_UNK_80 = 0x80,
        OF_HIDDEN = 0x4000,
        OF_START_HIDDEN = 0x8000,
        OF_IN_LIQUID = 0x80000,
        OF_HAS_ALPHA = 0x100000,
        OF_UNK_SAVEGAME_ENT = 0x8000000,
    };

    struct ObjInterp
    {
        char data_[0x900];

        void Clear()
        {
            AddrCaller{0x00483330}.this_call(this);
        }
    };

    enum ObjFriendliness
    {
        OBJ_UNFRIENDLY = 0x0,
        OBJ_NEUTRAL = 0x1,
        OBJ_FRIENDLY = 0x2,
        OBJ_OUTCAST = 0x3,
    };
#pragma pack(push, 1)
    struct Object
    {
        GRoom *room;
        Vector3 correct_pos;
        Object *next_obj;
        Object *prev_obj;
        String name;
        int uid;
        ObjectType type;
        ubyte team;
        char padding[3];
        int handle;
        int parent_handle;
        float life;
        float armor;
        Vector3 pos;
        Matrix3 orient;
        Vector3 last_pos;
        float radius;
        ObjectFlags obj_flags;
        VMesh *vmesh;
        int vmesh_submesh;
        PhysicsData p_data;
        ObjFriendliness friendliness;
        int material;
        int host_handle;
        int host_tag_handle;
        Vector3 host_offset;
        Matrix3 host_orient;
        Vector3 start_pos;
        Matrix3 start_orient;
        int *emitter_list_head;
        int root_bone_index;
        char killer_netid;
        char padding2[3];
        int server_handle;
        ObjInterp* obj_interp;
        void* mesh_lighting_data;
        Vector3 relative_transition_pos;

        void move(Vector3* new_pos)
        {
            AddrCaller{0x0048A230}.this_call(this, new_pos);
        }
    };
#pragma pack(pop)
    static_assert(sizeof(Object) == 0x28C);

    struct ObjectCreateInfo
    {
        const char* v3d_filename = nullptr;
        VMeshType v3d_type = MESH_TYPE_UNINITIALIZED;
        GSolid* solid = nullptr;
        float drag = 0.0f;
        int material = 0;
        float mass = 0.0f;
        Matrix3 body_inv;
        Vector3 pos;
        Matrix3 orient;
        Vector3 vel;
        Vector3 rotvel;
        float radius = 0.0f;
        VArray<PCollisionSphere> spheres;
        int physics_flags = 0;
    };
    static_assert(sizeof(ObjectCreateInfo) == 0x98);

    struct Debris : Object
    {
        Debris* next;
        Debris* prev;
        void* solid;
        int vmesh_submesh;
        int debris_flags;
        int explosion_index;
        Timestamp lifetime;
        int frame_num;
        int sound;
        String* custom_sound_set;
    };
    static_assert(sizeof(Debris) == 0x2B4);

    struct DebrisCreateStruct
    {
        Vector3 pos;
        Matrix3 orient;
        Vector3 vel;
        Vector3 spin;
        int lifetime_ms;
        int material;
        int explosion_index;
        int debris_flags;
        int obj_flags;
        void* room;
        ImpactSoundSet* iss;
    };
    static_assert(sizeof(DebrisCreateStruct) == 0x64);

    static auto& obj_lookup_from_uid = addr_as_ref<Object*(int uid)>(0x0048A4A0);
    static auto& obj_from_handle = addr_as_ref<Object*(int handle)>(0x0040A0E0);
    static auto& obj_from_remote_handle = addr_as_ref<Object*(int handle)>(0x00484B00); // from server handle
    static auto& obj_flag_dead = addr_as_ref<void(Object* obj)>(0x0048AB40);
    static auto& obj_find_root_bone_pos = addr_as_ref<void(Object*, Vector3&)>(0x0048AC70);
    static auto& obj_update_liquid_status = addr_as_ref<void(Object* obj)>(0x00486C30);
    static auto& obj_is_player = addr_as_ref<bool(Object* obj)>(0x004895D0);
    static auto& obj_is_hidden = addr_as_ref<bool(Object* obj)>(0x0040A110);
    static auto& obj_hide = addr_as_ref<void(Object* obj)>(0x0048A570);
    static auto& obj_unhide = addr_as_ref<void(Object* obj)>(0x0048A660);
    static auto& obj_emit_sound2 = addr_as_ref<int(
        Object* objp, Vector3 pos, int sound_handle, float vol_scale, float pan)>(0x0048A9C0);

    static auto& obj_light_free = addr_as_ref<void()>(0x0048B370);
    static auto& obj_light_alloc = addr_as_ref<void()>(0x0048B1D0);
    static auto& obj_light_calculate = addr_as_ref<void()>(0x0048B0E0);
    static auto& physics_force_to_ground = addr_as_ref<void(Object* obj)>(0x004A0770);
    static auto& obj_physics_activate = addr_as_ref<void(Object* objp)>(0x0040A420);

    static auto& obj_set_friendliness = addr_as_ref<void(Object* obj, int friendliness)>(0x00489F70);

    static auto& object_list = addr_as_ref<Object>(0x0073D880);

    static auto& debris_create = addr_as_ref<Debris*(int parent_handle, const char* vmesh_filename,
        float mass, DebrisCreateStruct* dcs, int mesh_num, float collision_radius)>(0x00412E70);
}

template<>
struct EnableEnumBitwiseOperators<rf::ObjectFlags> : std::true_type {};
