#pragma once

#include <patch_common/MemUtils.h>
#include "os/string.h"

namespace rf
{
    /* Other */

    static auto& default_player_weapon = addr_as_ref<String>(0x007C7600);

    static auto& get_file_checksum = addr_as_ref<unsigned(const char* filename)>(0x00436630);
    static auto& geomod_shape_init = addr_as_ref<void()>(0x004374C0);
    static auto& geomod_shape_create = addr_as_ref<int(const char*)>(0x00437500);
    static auto& geomod_shape_shutdown = addr_as_ref<void()>(0x00437460);
    static auto& bink_play = addr_as_ref<char*(const char* filename)>(0x00520A90);
    static auto& g_multi_damage_modifier = addr_as_ref<float>(0x0059F7E0);
    static auto& gr_set_near_clip = addr_as_ref<void(float dist)>(0x005180A0);
    static auto& multi_powerup_destroy_all = addr_as_ref<void()>(0x0047FE00); // used in multi_init

    static auto& gr_use_far_clip = addr_as_ref<bool>(0x01818B65);
    static auto& gr_far_clip_dist = addr_as_ref<float>(0x01818B68);
    static auto& gr_setup_frustum = addr_as_ref<void()>(0x00517E70);

    static auto& bomb_defuse_time_left = addr_as_ref<float>(0x006391B4);

    static auto& endgame_launch = addr_as_ref<void(const char* endgame_name)>(0x0043E9B0);

}
