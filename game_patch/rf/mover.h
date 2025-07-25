#pragma once

#include "object.h"
#include "os/array.h"
#include "os/timestamp.h"

namespace rf
{
    struct GSolid;

    struct MoverBrush : Object
    {
        MoverBrush *next;
        MoverBrush *prev;
        GSolid *geometry;
    };

    struct Keyframe
    {
        int time;
    };
    static_assert(sizeof(Keyframe) == 0x4);

    enum class MoverKeyframeMoveType : int
    {
        MKMT_ONE_WAY = 0x1,
        MKMT_PING_PONG_ONCE = 0x2,
        MKMT_PING_PONG_INFINITE = 0x3,
        MKMT_LOOP_ONCE = 0x4,
        MKMT_LOOP_INFINITE = 0x5,
        MKMT_LIFT = 0x6,
    };

    struct Mover : Object
    {
        Mover* next;
        Mover* prev;
        char mover_index;
        char padding[3];
        Timestamp field_298;
        VArray<Keyframe> keyframes;
        VArray<int> object_uids;
        VArray<int> brush_uids;
        VArray<int> object_handles;
        VArray<int> brush_handles;
        int sounds[4];
        MoverKeyframeMoveType keyframe_move_type;
        void* door_room;
        float rot_cur_pos;
        float cur_vel;
        int start_at_keyframe;
        int stop_at_keyframe;
        float travel_time_seconds;
        float dist_travelled;
        float rotation_travel_time_seconds_unk;
        int stop_completely_at_keyframe;
        Timestamp wait_timestamp;
        int trigger_handle;
        int mover_flags;
        int sound_instances[4];
    };
    static_assert(sizeof(Mover) == 0x32C);

    static auto& mover_list = addr_as_ref<Mover>(0x0064E3B0);
    static auto& mover_brush_list = addr_as_ref<MoverBrush>(0x0064E6E0);
    static auto& mover_activate_from_trigger =
        addr_as_ref<void(int mover_handle, int trigger_handle, int activator_handle)>(0x0046ABA0);

}

