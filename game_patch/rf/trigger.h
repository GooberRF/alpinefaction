#pragma once

#include "object.h"

namespace rf
{
    enum TriggerShape
    {
        TRIGGER_SHAPE_SPHERE = 0,
        TRIGGER_SHAPE_BOX = 1,
    };

    enum TriggerFlags
    {
        TRIGGER_FLAG_USE_KEY = 0x1,
        // Fired by using an entity linked to the trigger, not by standing in its volume.
        TRIGGER_FLAG_LINKED_USE = 0x4,
        // Activated once as the level starts (0x004C01B0) unless disabled
        TRIGGER_FLAG_AUTO = 0x8,
        // Set by 0x004C0210, cleared by trigger_enable
        TRIGGER_FLAG_DISABLED = 0x10,
    };

    // Trigger::trigger_team for a trigger any team can set off.
    static constexpr ubyte trigger_team_any = 0xFF;

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

    static auto& trigger_inside_bounding_box = addr_as_ref<bool(Trigger*, Object*)>(0x004C0A80);
    static auto& trigger_inside_bounding_sphere = addr_as_ref<bool(Trigger*, Object*)>(0x004BF620);
    static auto& trigger_enable = addr_as_ref<void(Trigger*)>(0x004C0200);
    static auto& trigger_list = addr_as_ref<Trigger>(0x00856520);
    // Disables unlinked auto triggers; stock load runs it after restoring the level state (0x004B4C37)
    static auto& trigger_disable_unlinked = addr_as_ref<void()>(0x004C0960);
    // Read-only. `use_key`: whether Use is what would activate it; no team test.
    static auto& trigger_check_activate = addr_as_ref<bool(Trigger*, Object* activator, bool use_key)>(0x004C06D0);

}
