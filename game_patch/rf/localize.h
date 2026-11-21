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
        static const auto &you_are_on_team = array[863];
        static const auto &red_caps = array[864];
        static const auto &blue_caps = array[865];
        static const auto &exiting_game = array[884];
        static const auto &usage = array[886];
        static const auto &incompatible_version = array[891];
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

        // extras menu
        static const auto &summoner_trailer = array[389];
        static const auto &back = array[772];
        static const auto &extras = array[982];
        static const auto &credits = array[983];
        static const auto &glass_house = array[985];

        // main menu
        static const auto &options = array[43];
        static const auto &main_menu_extras = array[44];
        static const auto &quit = array[46];
        static const auto &multi = array[47];
        static const auto &multiplayer = array[845];
        static const auto &main_menu_new = array[793];
        static const auto &main_menu_load = array[794];
        static const auto &main_menu_save = array[827];
        static const auto &main_menu_return = array[828];
        static const auto &main_menu = array[823];

        // new game
        static const auto &new_game = array[42];
        static const auto &cancel = array[202];
        static const auto &new_game_training_advice = array[830];
        static const auto &skip_training = array[847];
        static const auto &go_to_training = array[848];

        // save game and load game
        static const auto &yes = array[76];
        static const auto &no = array[77];
        static const auto &ok = array[543];
        static const auto &date_time_column = array[931];
        static const auto &load = array[933];
        static const auto &save_game = array[934];
        static const auto &game_saved = array[935];
        static const auto &save = array[936];
        static const auto &cannot_save_now = array[972];
        static const auto &load_game = array[986];
        static const auto &name_column = array[987];
        static const auto &game_time_column = array[988];
        static const auto &no_filename = array[989];
        static const auto &overwrite_save = array[990];
        // BACK on these screens and the server list; the other menus use `back`.
        static const auto &save_load_back = array[788];

        // message log
        static const auto &messages = array[210];
        static const auto &no_messages = array[211];
        static const auto &close = array[908];

        // multiplayer menu and server browser
        static const auto &join = array[732];
        static const auto &get_servers = array[733];
        static const auto &add_server = array[734];
        static const auto &refresh_all = array[740];
        static const auto &error = array[832];
        static const auto &enter_password = array[837];
        static const auto &enter_server_address = array[898];
        static const auto &server_list = array[902];
        static const auto &server_list_help = array[903];
        static const auto &mod_not_running = array[993];
        static const auto &mod_running = array[994];

        // create game
        static const auto &create_game_title = array[851];
        static const auto &start = array[770];
        static const auto &server_name = array[697];
        static const auto &game_type = array[698];
        static const auto &max_players = array[699];
        static const auto &password = array[449];
        static const auto &lan_only = array[992];
        static const auto &add_all = array[895];
        static const auto &unnamed_server = array[834];
        static const auto &tracker_init_failed = array[907];

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
