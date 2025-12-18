#pragma once

#include <patch_common/MemUtils.h>
#include "os/vtypes.h"
#include "os/timestamp.h"
#include "os/string.h"
#include "os/array.h"
#include "object.h"
#include "item.h"
#include "geometry.h"
#include "ai.h"
#include "gr/gr.h"

namespace rf
{
    // Forward declarations
    struct Player;
    struct Entity;

    // nw/psnet

    struct NetAddr
    {
        uint32_t ip_addr;
        uint32_t port;

        bool operator==(const NetAddr &other) const = default;
    };
    static_assert(sizeof(NetAddr) == 0x8);

    constexpr int NET_MAX_REL_SOCKETS = 40;

    static const auto& net_init_socket = addr_as_ref<void(unsigned short port)>(0x00528F10);
    static const auto& net_addr_to_string = addr_as_ref<void(char *buf, int buf_size, const NetAddr& addr)>(0x00529FE0);
    static const auto& net_send = addr_as_ref<void(const NetAddr &addr, const void *data, int len)>(0x0052A080);
    static const auto& net_same = addr_as_ref<int(const NetAddr &addr1, const NetAddr &addr2, bool check_port)>(0x0052A930);
    static const auto& net_rel_send = addr_as_ref<int(int, const uint8_t*, int)>(0x0052A310);
    static auto& net_udp_socket = addr_as_ref<int>(0x005A660C);
    static auto& net_port = addr_as_ref<unsigned short>(0x01B587D4);

    struct NetReliableSocket;
    static auto& net_rel_sockets = addr_as_ref<NetReliableSocket[NET_MAX_REL_SOCKETS]>(0x01B479E8);

    // multi

    struct MultiIoStats
    {
        int total_bytes_sent;
        int total_bytes_recvd;
        int bytes_sent_per_frame[30];
        int bytes_recvd_per_frame[30];
        int frame_slot_index;
        int bytes_sent_per_second[30];
        int bytes_recvd_per_second[30];
        int time_slot_index;
        TimestampRealtime packet_loss_update_timestamp;
        int obj_update_packets_sent;
        int obj_update_packets_recvd;
        int obj_update_repeated;
        int obj_update_too_slow;
        int obj_update_too_fast;
        int bytes_sent_per_type[55];
        int bytes_recvd_per_type[55];
        int packets_sent_per_type[55];
        int packets_recvd_per_type[55];
        TimestampRealtime time_slot_increment_timestamp;
        int ping_array[2];
        int current_ping_idx;
        TimestampRealtime send_ping_packet_timestamp;
        int last_ping_time;
    };
    static_assert(sizeof(MultiIoStats) == 0x590);

    struct PlayerNetData
    {
        NetAddr addr;
        int flags;
        int state;
        uint reliable_socket;
        ubyte player_id;
        int join_time_ms;
        MultiIoStats stats;
        int ping;
        float obj_update_packet_loss;
        ubyte unreliable_buffer[512];
        int unreliable_buffer_size;
        ubyte reliable_buffer[512];
        int reliable_buffer_size;
        int max_update_rate;
        int obj_update_interval;
        Timestamp obj_update_timestamp;
    };
    static_assert(sizeof(PlayerNetData) == 0x9C8);

    struct RespawnPoint
    {
        String name;
        uint8_t team;
        Vector3 position;
        Matrix3 orientation;
        bool red_team;
        bool blue_team;
        bool bot;
        float dist_other_player;

        bool operator==(const RespawnPoint& other) const
        {
        return (name == other.name &&
                team == other.team &&
                position == other.position &&
                orientation == other.orientation &&
                red_team == other.red_team &&
                blue_team == other.blue_team &&
                bot == other.bot);
        }
    };
    static_assert(sizeof(RespawnPoint) == 0x44);

    enum NetGameType {
        NG_TYPE_DM = 0,
        NG_TYPE_CTF = 1,
        NG_TYPE_TEAMDM = 2,
        NG_TYPE_KOTH = 3,   // King of the Hill, as of AF v1.2
        NG_TYPE_DC = 4,     // Damage Control, as of AF v1.2
        NG_TYPE_REV = 5,    // Revolt, as of AF v1.2
        NG_TYPE_RUN = 6,    // Run, as of AF v1.2
        NG_TYPE_ESC = 7,    // Escalation, as of AF v1.2
    };

    enum NetGameFlags
    {
        NG_FLAG_DEBUG_SCOREBOARD = 0x1,
        NG_FLAG_LEVEL_LOADED = 0x2,
        NG_FLAG_CHANGING_LEVEL = 0x4,
        NG_FLAG_RANDOM_MAP_ROTATION = 0x8,
        NG_FLAG_WEAPON_STAY = 0x10,
        NG_FLAG_FORCE_RESPAWN = 0x20,
        NG_FLAG_TEAM_DAMAGE_LOW = 0x40,
        NG_FLAG_FALL_DAMAGE = 0x80,
        NG_FLAG_REAL_FALL_DAMAGE = 0x100,
        NG_FLAG_TEAM_DAMAGE_HIGH = 0x200,
        NG_FLAG_TEAM_DAMAGE = 0x240, // unsure why this is split into two
        NG_FLAG_NOT_LAN_ONLY = 0x400,
        NG_FLAG_BALANCE_TEAMS = 0x2000,
    };

    struct NetGameInfo
    {
        String name;
        String password;
        int field_10;
        NetGameType type;
        int flags;
        int max_players;
        int current_total_kills;
        float max_time_seconds;
        int max_kills;
        int geomod_limit;
        int max_captures;
        NetAddr server_addr;
        int current_level_index;
        VArray_String<String> levels;
    };

    struct JoinRequest
    {
        String password;
        String name;
        int entity_type;
        int ac_info[4];
    };

    enum class ChatMsgColor
    {
        red_white = 0,
        blue_white = 1,
        red_red = 2,
        blue_blue = 3,
        white_white = 4,
        default_ = 5,
        gold_white = 6,
    };

    enum NetReliableSocketStatus: uint32_t {
        EMPTY = 0x0,
        CONNECTED = 0x1,
        TIMED_OUT = 0x2,
        UNK_3 = 0x3,
        CONNECTED_REMOTE = 0x4,
        CONNECTING = 0x5,
    };
    static_assert(sizeof(NetReliableSocketStatus) == 4);

    #pragma pack(push, 1)
    struct NetReliableSocket {
        void* sbuffers[75];
        uint16_t ssequence[75];
        int timesent[75];
        int16_t send_len[75];
        void* rbuffers[75];
        int recv_len[75];
        uint16_t rsequence[75];
        int last_packet_received;
        int last_packet_sent;
        NetReliableSocketStatus status;
        uint16_t oursequence;
        uint16_t theirsequence;
        rf::NetAddr net_addr;
        int pings[10];
        uint8_t ping_pos;
        uint32_t num_ping_samples;
        int ping_median;
        int retransmission_timeout;
    };
    #pragma pack(pop)
    static_assert(sizeof(NetReliableSocket) == 0x6BF);

    constexpr size_t max_packet_size = 512;

    static auto& multi_get_game_type = addr_as_ref<NetGameType()>(0x00470770);
    static auto& multi_io_send = addr_as_ref<void(Player *player, const void *packet, int len)>(0x00479370);
    static auto& multi_io_send_reliable =
        addr_as_ref<void(Player *player, const void *data, int len, int not_limbo)>(0x00479480);
    static auto& multi_io_send_reliable_to_all =
        addr_as_ref<void(const void *data, int len, int a4)>(0x004795A0);
    static auto& multi_io_send_buffered_reliable_packets = addr_as_ref<void(Player *pp)>(0x004796C0);
    static auto& multi_find_player_by_addr = addr_as_ref<Player*(const NetAddr& addr)>(0x00484850);
    static auto& multi_find_player_by_id = addr_as_ref<Player*(uint8_t id)>(0x00484890);
    static auto& multi_time_limit = addr_as_ref<float>(0x0064EC4C);
    static auto& multi_kill_limit = addr_as_ref<int>(0x0064EC50);
    static auto& multi_cap_limit = addr_as_ref<int>(0x0064EC58);
    static auto& multi_geo_limit = addr_as_ref<int>(0x0064EC54);
    static auto& multi_max_players = addr_as_ref<int>(0x0064EC44);
    static auto& multi_server_flags = addr_as_ref<NetGameFlags>(0x0064EC40);
    static auto& multi_game_type = addr_as_ref<int>(0x0064EC3C);
    static auto& multi_level_switch_queued = addr_as_ref<int>(0x0064EC64);
    static auto& ctf_flag_cooldown_timestamp = addr_as_ref<Timestamp>(0x006C74F4);
    static auto& multi_ctf_drop_flag = addr_as_ref<void(Player* pp)>(0x00473F40);
    static auto& multi_ctf_get_red_team_score = addr_as_ref<uint8_t()>(0x00475020);
    static auto& multi_ctf_get_blue_team_score = addr_as_ref<uint8_t()>(0x00475030);
    static auto& multi_ctf_get_red_flag_player = addr_as_ref<Player*()>(0x00474E60);
    static auto& multi_ctf_get_blue_flag_player = addr_as_ref<Player*()>(0x00474E70);
    static auto& multi_ctf_is_red_flag_in_base = addr_as_ref<bool()>(0x00474E80);
    static auto& multi_ctf_is_blue_flag_in_base = addr_as_ref<bool()>(0x00474EA0);
    static auto& multi_ctf_get_blue_flag_pos = addr_as_ref<Vector3*(Vector3*)>(0x00474F40);
    static auto& multi_ctf_get_red_flag_pos = addr_as_ref<Vector3*(Vector3*)>(0x00474EC0);
    static auto& multi_ctf_flag_blue_stolen_timestamp = addr_as_ref<Timestamp>(0x006C7544);
    static auto& multi_ctf_flag_red_stolen_timestamp = addr_as_ref<Timestamp>(0x006C754C);
    static auto& ctf_red_flag_item = addr_as_ref<Object*>(0x006C7560);
    static auto& ctf_blue_flag_item = addr_as_ref<Object*>(0x006C7564);
    static auto& ctf_red_flag_pos = addr_as_ref<Vector3>(0x006C7500);
    static auto& ctf_blue_flag_pos = addr_as_ref<Vector3>(0x006C7510);
    static auto& multi_tdm_get_red_team_score = addr_as_ref<int()>(0x004828F0); // returns ubyte in vanilla game
    static auto& multi_tdm_get_blue_team_score = addr_as_ref<int()>(0x00482900); // returns ubyte in vanilla game
    static auto& multi_num_players = addr_as_ref<int()>(0x00484830);
    static auto& multi_kick_player = addr_as_ref<void(Player *player)>(0x0047BF00);
    static auto& multi_ban_ip = addr_as_ref<void(const NetAddr& addr)>(0x0046D0F0);
    static auto& multi_set_next_weapon = addr_as_ref<void(int weapon_type)>(0x0047FCA0);
    static auto& multi_change_level = addr_as_ref<void(const char* filename)>(0x0047BF50);
    static auto& multi_ping_player = addr_as_ref<void(Player*)>(0x00484D00);
    static auto& send_entity_create_packet = addr_as_ref<void(Entity *entity, Player* player)>(0x00475160);
    static auto& send_entity_create_packet_to_all = addr_as_ref<void(Entity *entity)>(0x00475110);
    static auto& multi_find_character = addr_as_ref<int(const char *name)>(0x00476270);
    static auto& multi_chat_print = addr_as_ref<void(String::Pod text, ChatMsgColor color, String::Pod prefix)>(0x004785A0);
    static auto& multi_chat_say = addr_as_ref<void(const char *msg, bool is_team_msg)>(0x00444150);
    static auto& multi_chat_add_msg = addr_as_ref<void(Player* pp, const char* msg, bool is_team_msg)>(0x00443FB0);
    static auto& multi_is_connecting_to_server = addr_as_ref<uint8_t(const NetAddr& addr)>(0x0044AD80);
    using MultiIoProcessPackets_Type = void(const void* data, size_t len, const NetAddr& addr, Player* player);
    static auto& multi_io_process_packets = addr_as_ref<MultiIoProcessPackets_Type>(0x004790D0);
    static auto& multi_kill_local_player = addr_as_ref<void()>(0x004757A0);
    static auto& send_game_info_req_packet = addr_as_ref<void(const NetAddr& addr)>(0x0047B450);
    static auto& multi_entity_is_female = addr_as_ref<bool(int mp_character_idx)>(0x004762C0);
    static auto& multi_powerup_has_player = addr_as_ref<bool(Player* pp, int powerup_type)>(0x004802B0);
    static auto& multi_powerup_get_time_until = addr_as_ref<int(Player* pp, int powerup_type)>(0x004802D0);
    static auto& multi_powerup_add = addr_as_ref<void(Player* pp, int powerup_type, int time_ms)>(0x00480050);
    static auto& multi_powerup_remove = addr_as_ref<void(Player* pp, int powerup_type)>(0x004801F0);
    static auto& multi_powerup_remove_all_for_player = addr_as_ref<void(Player* pp)>(0x00480310);
    static auto& send_reload_packet = addr_as_ref<void(Entity* ep, int weapon_type, int clip_ammo, int ammo)>(0x00485B50);
    static auto& send_obj_kill_packet = addr_as_ref<void(Entity* killed_entity, Item* item, int* a3)>(0x0047E8C0);
    static auto& send_item_create_packet = addr_as_ref<void(Item* item, int16_t* index)>(0x00479A20); // send_item_create_packet3
    static auto& send_respawn_req_packet = addr_as_ref<void(uint32_t multi_character, uint8_t player_id)>(0x004809D0); // client -> server
    static auto& multi_spawn_player_server_side = addr_as_ref<void(Player* pp)>(0x00480820);
    static auto& multi_limbo_timer = addr_as_ref<Timestamp>(0x006D6138);
    static auto& local_spawn_attempt_timer = addr_as_ref<Timestamp>(0x007C718C);


    static auto& set_in_mp_flag = addr_as_ref<void()>(0x0046ED50);
    static auto& multi_start = addr_as_ref<void(int is_client, const NetAddr* serv_addr)>(0x0046D5B0);
    static auto& multi_stop = addr_as_ref<void()>(0x0046E2C0);
    static auto& multi_load_next_level = addr_as_ref<void()>(0x0046ED70);
    static auto& multi_init_server = addr_as_ref<void()>(0x00482060);
    static auto& multi_hud_clear_chat = addr_as_ref<void()>(0x00479000);

    static auto& netgame = addr_as_ref<NetGameInfo>(0x0064EC28);
    static auto& is_multi = addr_as_ref<bool>(0x0064ECB9);
    static auto& is_server = addr_as_ref<bool>(0x0064ECBA); // only refers to a listen server
    static auto& is_dedicated_server = addr_as_ref<bool>(0x0064ECBB);
    static auto& simultaneous_ping = addr_as_ref<uint32_t>(0x00599CD8);
    static auto& tracker_addr = addr_as_ref<NetAddr>(0x006FC550);
    static auto& rcon_password = addr_as_ref<char[20]>(0x0064ECD0);

    enum ChatSayType {
        CHAT_SAY_GLOBAL = 0,
        CHAT_SAY_TEAM = 1,
    };

    static auto& multi_chat_say_handle_key = addr_as_ref<void(int key)>(0x00444620);
    static auto& multi_chat_is_say_visible = addr_as_ref<bool()>(0x00444AC0);
    static auto& multi_chat_say_render = addr_as_ref<void()>(0x00444790);
    static auto& multi_chat_say_show = addr_as_ref<void(ChatSayType type)>(0x00444A80);
    static auto& multi_hud_render_chat = addr_as_ref<void()>(0x004773D0);
    static auto& game_poll = addr_as_ref<void(void(*key_callback)(int k))>(0x004353C0);
    static auto& scoreboard_render_internal = addr_as_ref<void(bool netgame_scoreboard)>(0x00470880);
    static auto& multiplayer_walk_speed = addr_as_ref<float>(0x0059458C);
    static auto& multiplayer_crouch_walk_speed = addr_as_ref<float>(0x00594590);

    constexpr int multi_max_player_id = 256;
}
