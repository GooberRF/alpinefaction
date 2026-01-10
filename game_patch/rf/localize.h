#pragma once

#include <patch_common/MemUtils.h>

namespace rf
{
    namespace strings {
        static const auto &array = addr_as_ref<char*[1000]>(0x007CBBF0);
        static const auto &player = array[675];
        static const auto &frags = array[676];
        static const auto &ping = array[677];
        static const auto &caps = array[681];
        static const auto &was_killed_by_her_own_hand = array[692];
        static const auto &was_killed_by_his_own_hand = array[693];
        static const auto &was_killed_by = array[694];
        static const auto &was_killed_mysteriously = array[695];
        static const auto &has_joined = array[708];
        static const auto &was_kicked = array[709];
        static const auto &score = array[720];
        static const auto &close_combat = array[779];
        static const auto &semi_auto = array[780];
        static const auto &heavy = array[781];
        static const auto &explosive = array[782];
        static const auto &team = array[833];
        static const auto &player_name = array[835];
        static const auto &exiting_game = array[884];
        static const auto &usage = array[886];
        static const auto &level_name = array[920];
        static const auto &level_time = array[922];
        static const auto &days = array[923];
        static const auto &you_killed_yourself = array[942];
        static const auto &you_just_got_beat_down_by = array[943];
        static const auto &you_were_killed_by = array[944];
        static const auto &you_killed = array[945];
        static const auto &got_beat_down_by = array[946];
        static const auto &kicking_player = array[958];
        static const auto &banning_player = array[959];
        static const auto &deathmatch = array[974];
        static const auto &capture_the_flag = array[975];
        static const auto &team_deathmatch = array[976];
        static const auto &dm = array[723];
        static const auto &ctf = array[724];
        static const auto &teamdm = array[725];

        // dedi config lookups
        static const auto& dedi_header = array[855];

        // control lookups
        static const auto &fire = array[807];
        static const auto &alt_fire = array[808];
        static const auto &use = array[147];
        static const auto &jump = array[148];
        static const auto &crouch = array[149];
        static const auto &holster = array[809];
        static const auto &reload = array[151];
        static const auto &next_weapon = array[152];
        static const auto &prev_weapon = array[810];
        static const auto &message_log = array[156];
        static const auto &quick_save = array[820];
        static const auto &quick_load = array[821];
    }
}
