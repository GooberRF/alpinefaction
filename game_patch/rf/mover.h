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

    struct MoverKeyframe
    {
        int uid;
        Vector3 pos;
        Matrix3 orient;
        float pause_time_seconds;
        float forward_time_seconds;
        float reverse_time_seconds;
        float ramp_up_time_seconds;
        float ramp_down_time_seconds;
        int event_uid;
        int item_uids[2];
        float rotation_angle;
        Vector3 rotation_axis;
    };
    static_assert(sizeof(MoverKeyframe) == 0x64);

    enum class MoverKeyframeMoveType : int
    {
        MKMT_ONE_WAY = 0x1,
        MKMT_PING_PONG_ONCE = 0x2,
        MKMT_PING_PONG_INFINITE = 0x3,
        MKMT_LOOP_ONCE = 0x4,
        MKMT_LOOP_INFINITE = 0x5,
        MKMT_LIFT = 0x6,
    };

    #pragma pack(push, 1)
    struct MoverCreateInfo
    {
        int unused;
        VArray<MoverKeyframe*> keyframes;
        VArray<int> object_uids;
        VArray<int> brush_uids;
        bool is_door;
        bool rotate_in_place;
        bool is_moving_backwards;
        bool time_is_velocity;
        bool force_orient;
        bool no_player_collide;
        char padding[2];
        MoverKeyframeMoveType keyframe_move_type;
        int start_at_keyframe;
        String sound_names[4];
        float sound_volumes[4];
    };
#pragma pack(pop)
    static_assert(sizeof(MoverCreateInfo) == 0x68);

    enum MoverFlags
    {
        MF_PAUSED_AT_KEYFRAME = 0x1,
        MF_DOOR = 0x2,
        MF_ROTATE_IN_PLACE = 0x4,
        MF_UNK_8 = 0x8,
        MF_UNK_10 = 0x10,
        MF_UNK_20 = 0x20,
        MF_ACCEL_DECEL = 0x40,
        MF_PAUSED = 0x80,
        MF_STARTS_BACKWARDS = 0x100,
        MF_USE_TRAV_TIME_AS_SPD = 0x400,
        MF_FORCE_ORIENT = 0x800,
        MF_NO_PLAYER_COLLIDE = 0x1000,
        MF_DIR_FORWARD = 0x2000,
        MF_UNK_4000 = 0x4000,
        MF_UNK_80000000 = 0x80000000,
    };

    struct Mover : Object
    {
        Mover* next;
        Mover* prev;
        char mover_index;
        char padding[3];
        Timestamp field_298;
        VArray<MoverKeyframe*> keyframes;
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
        MoverFlags mover_flags;
        int sound_instances[4];
    };
    static_assert(sizeof(Mover) == 0x32C);

    static auto& mover_list = addr_as_ref<Mover>(0x0064E3B0);
    static auto& mover_brush_list = addr_as_ref<MoverBrush>(0x0064E6E0);
    static auto& mover_activate_from_trigger =
        addr_as_ref<void(int mover_handle, int trigger_handle, int activator_handle)>(0x0046ABA0);
    static auto& mover_is_obstructed = addr_as_ref<bool(Mover* mp)>(0x0046A280);
    static auto& mover_is_obstructed_by_entity = addr_as_ref<bool(Mover* mp)>(0x0046A1E0);
    static auto& mover_reverse_direction = addr_as_ref<void(Mover* mp)>(0x0046BAE0);
    static auto& mover_is_door = addr_as_ref<bool(Mover* mp)>(0x0046B2E0);
    static auto& mover_rotating_process_pre = addr_as_ref<void(Mover* mp)>(0x0046A3D0);
    static auto& mover_rotates_in_place = addr_as_ref<bool(Mover* mp)>(0x0046B320);
    static auto& mover_update_item_status = addr_as_ref<void(Mover* mp)>(0x0046A060);
    static auto& mover_from_handle = addr_as_ref<Mover*(int handle)>(0x0046AFA0);
    static auto& mover_play_stop_sound = addr_as_ref<void(Mover* mp)>(0x0046A0D0);
    static auto& mover_play_start_sound = addr_as_ref<void(Mover* mp)>(0x0046A120);

}

