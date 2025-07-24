#pragma once

#include "object.h"

namespace rf
{
    struct Trigger : Object
    {
        Trigger *next;
        Trigger *prev;
        int type;
        Timestamp next_check;
        int reset_time_ms;
        int count;
        int max_count;
        float time_last_activated;
        int item_filter_type;
        int trigger_flags;
        String submesh_name;
        int mesh_index;
        int airlock_uid;
        int activate_method;
        Vector3 box_size;
        VArray<int> links;
        Timestamp activation_delay;
        int activator_handle;
        Timestamp airlock_beep_delay;
        bool one_way;
        float button_active_time;
        Timestamp button_active_timestamp;
        float inside_time_seconds;
        Timestamp inside_timestamp;
        int attached_to_uid;
        int use_clutter_uid;
        ubyte trigger_team;
    };
    static_assert(sizeof(Trigger) == 0x30C);

    static auto& trigger_list = addr_as_ref<Trigger>(0x00856520);

    static auto& trigger_inside_bounding_box = addr_as_ref<bool(Trigger*, Object*)>(0x004C0A80);
    static auto& trigger_inside_bounding_sphere = addr_as_ref<bool(Trigger*, Object*)>(0x004BF620);
    static auto& trigger_enable = addr_as_ref<void(Trigger*)>(0x004C0200);

}
