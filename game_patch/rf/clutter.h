#pragma once

#include "object.h"
#include "os/timestamp.h"

namespace rf
{

    struct ClutterInfo
    {
        String cls_name;
        String v3d_filename;
        String corpse_class_name;
        String debris_filename;
        String debris_sound_set;
        VArray<int> emitter_list;
        float emitter_life;
        int v3d_type;
        float life;
        float radius;
        ObjectUseFunction use;
        float use_radius;
        int material;
        int sound;
        int use_sound;
        int explode_anim_vclip;
        float explode_anim_radius;
        float explode_damage;
        Vector3 explode_offset;
        Timestamp explode_anim_timer;
        int flags;
        float debris_velocity;
        int glare;
        int rod_glare;
        FArray<int, 4> corona_props;
        int light_prop;
        float damage_type_factors[11];
        VArray<int> skins;
        VArray<int> skins_glare_effect;
        int screen_width;
        int screen_height;
    };
    static_assert(sizeof(ClutterInfo) == 0xE8);

    struct Clutter : Object
    {
        struct Clutter *next;
        struct Clutter *prev;
        ClutterInfo* info;
        int info_index;
        int corpse_index;
        int sound_handle;
        Timestamp delayed_kill_timestamp;
        int delayed_kill_sound;
        int dmg_type_that_killed_me;
        Timestamp emitter_kill_timestamp;
        Timestamp corpse_create_timestamp;
        VMesh *corpse_vmesh_handle;
        int current_skin_index;
        VArray<int> links;
        bool already_spawned_glass;
        int use_sound;
        uint16_t killable_index;
    };
    static_assert(sizeof(Clutter) == 0x2D8);

    enum MonitorState
    {
        MONITOR_STATE_ON = 0x0,
        MONITOR_STATE_OFF = 0x1,
        MONITOR_STATE_STATIC = 0x2,
    };


    struct Monitor
    {
        Monitor *next;
        Monitor *prev;
        int state;
        int clutter_handle;
        int camera_handle;
        int user_bitmap;
        VArray<int> camera_list;
        Timestamp next_cam_switch;
        float cycle_delay_seconds;
        int resolution_index;
        int bm_width;
        int bm_height;
        int flags;
    };
    static_assert(sizeof(Monitor) == 0x3C);

    // Monitor flags
    enum MonitorFlags
    {
        MF_OBJ_RENDERED = 0x2,
        MF_BM_RENDERED = 0x4,
        MF_CONTINOUSLY_UPDATE = 0x8,
    };

    static auto& clutter_list = addr_as_ref<Clutter>(0x005C9360);
    static auto& clutter_list_tail = addr_as_ref<Clutter*>(0x005C95F0);
    static auto& clutter_count = addr_as_ref<int>(0x005C9358);

    // Shared dummy ClutterInfo for alpine objects that don't need clutter behavior.
    inline ClutterInfo& get_dummy_clutter_info()
    {
        static ClutterInfo info = []() {
            ClutterInfo ci{};
            ci.life = -1.0f;
            ci.sound = -1;
            ci.use_sound = -1;
            ci.explode_anim_vclip = -1;
            ci.glare = -1;
            ci.rod_glare = -1;
            ci.light_prop = -1;
            return ci;
        }();
        return info;
    }
    static auto& monitor_list = addr_as_ref<Monitor>(0x005C98A8);

    static auto& clutter_restore_mesh = addr_as_ref<void(Clutter *clutter, const char *mesh_name)>(0x00410ED0);
}
