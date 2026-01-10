#pragma once

#include "object.h"
#include "entity.h"

namespace rf
{
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

    static auto& corpse_from_handle = addr_as_ref<Corpse*(int handle)>(0x004174C0);
    static auto& corpse_restore_mesh = addr_as_ref<void(Corpse *cp, const char *mesh_name)>(0x00417530);

    static auto& corpse_list = addr_as_ref<Corpse>(0x005CABB8);
    static auto& g_blood_used_list = addr_as_ref<EntityBloodPool*>(0x0062F764);
}
