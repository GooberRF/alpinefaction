#pragma once

#include "object.h"
#include "entity.h"

namespace rf
{
    // Corpse::corpse_flags bit: the pose holds its last frame (corpse_create 0x00416940; stock save 0x004176A0 sets it)
    constexpr int corpse_flag_hold_last_frame = 0x8;

    struct Corpse : Object
    {
        Corpse *next;
        Corpse *prev;
        float create_time;
        float lifetime_seconds;
        int corpse_flags;
        int entity_type;
        String corpse_pose_name;
        Timestamp emitter_kill_timestamp;
        float body_temp;
        int corpse_state_vmesh_anim_index;
        int corpse_action_vmesh_anim_index;
        int corpse_drop_vmesh_anim_index;
        int corpse_carry_vmesh_anim_index;
        int corpse_pose;
        VMesh *helmet_v3d_handle;
        int item_handle;
        EntityFireInfo *fire_info;
        int body_drop_sound_handle;
        Color ambient_color;
        ShadowInfo shadow_info;
    };
    static_assert(sizeof(Corpse) == 0x318);

    static auto& corpse_from_handle = addr_as_ref<Corpse*(int handle)>(0x004174C0);
    static auto& corpse_is_cap_exempt = addr_as_ref<bool(Corpse* cp)>(0x00416F60);
    static auto& corpse_restore_mesh = addr_as_ref<void(Corpse *cp, const char *mesh_name)>(0x00417530);
    static auto& corpse_create = addr_as_ref<
        Corpse*(Entity* ep, const char* pose_name, const Vector3* pos, const Matrix3* orient, int unk1, int unk2)>(
        0x00416940);

    static auto& corpse_list = addr_as_ref<Corpse>(0x005CABB8);
    static auto& corpse_count = addr_as_ref<int>(0x005CAED0);
    static auto& corpse_update_collision_spheres = addr_as_ref<void(Corpse* cp)>(0x004164C0);
    // The ambient light at an object, as the stock corpse load sets Corpse::ambient_color (0x00417BD9)
    static auto& obj_get_ambient_color = addr_as_ref<void(Color* out, Object* obj)>(0x004605D0);

    // A pool of blood under a corpse. Pools move between a free and a used circular list (0x0042E6B0).
    struct EntityBloodPool
    {
        float pool_age;
        float last_known_size;
        float grow_time_seconds;
        float pool_size;
        float seconds_to_radians_scalar;
        Vector3 pool_pos;
        Matrix3 pool_orient;
        GRoom* pool_room;
        Color pool_color;
        EntityBloodPool* next;
        EntityBloodPool* prev;
    };
    static_assert(sizeof(EntityBloodPool) == 0x54);
    static_assert(offsetof(EntityBloodPool, pool_color) == 0x48);

    static auto& blood_pool_free_list = addr_as_ref<EntityBloodPool*>(0x0062F488);
    static auto& blood_pool_used_list = addr_as_ref<EntityBloodPool*>(0x0062F764);
}
