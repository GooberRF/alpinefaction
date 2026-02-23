#pragma once

#include <cstdint>
#include <variant>
#include <vector>
#include <common/rfproto.h>
#include "gametype.h"
#include "../multi/server_internal.h"
#include "../rf/multi.h"

#pragma pack(push, 1)

// Forward declarations
namespace rf
{
    struct Object;
    struct Player;
    struct Vector3;
    struct Matrix3;
    struct Entity;
}

enum class af_packet_type : uint8_t
{
    af_ping_location_req = 0x50,        // Alpine 1.1
    af_ping_location = 0x51,            // Alpine 1.1
    af_damage_notify = 0x52,            // Alpine 1.1
    af_obj_update = 0x53,               // Alpine 1.1
    af_client_req = 0x55,               // Alpine 1.2
    af_just_spawned_info = 0x56,        // Alpine 1.2
    af_koth_hill_state = 0x57,          // Alpine 1.2
    af_koth_hill_captured = 0x58,       // Alpine 1.2
    af_just_died_info = 0x59,           // Alpine 1.2
    af_server_info = 0x5A,              // Alpine 1.2
    af_spectate_start = 0x5B,           // Alpine 1.2
    af_spectate_notify = 0x5C,          // Alpine 1.2
    af_server_msg = 0x5D,               // Alpine 1.2
    af_server_req = 0x5E,               // Alpine 1.2.1
};

struct af_ping_location_req_packet
{
    RF_GamePacketHeader header;
    RF_Vector pos;
};

struct af_ping_location_packet
{
    RF_GamePacketHeader header;
    uint8_t player_id;
    RF_Vector pos;
};

struct af_damage_notify_packet
{
    RF_GamePacketHeader header;
    uint8_t player_id;
    uint16_t damage;
    uint8_t flags;
};

struct af_obj_update // members of af_obj_update_packet
{
    uint32_t obj_handle;
    uint8_t current_primary_weapon;
    uint8_t ammo_type;
    uint16_t clip_ammo;
    uint16_t reserve_ammo;
};

struct af_obj_update_packet
{
    RF_GamePacketHeader header;
    af_obj_update objects[];
};

enum class af_client_req_type : uint8_t
{
    af_req_handicap = 0x0,
    af_req_server_cfg = 0x1,
};

struct HandicapPayload
{
    uint8_t amount = 0;
};

using af_client_payload = std::variant<HandicapPayload, std::monostate>;

struct af_client_req_packet
{
    RF_GamePacketHeader header;
    af_client_req_type req_type;
    af_client_payload payload;
};

enum class af_server_req_type : uint8_t
{
    af_sreq_should_gib = 0x0,
};

struct ShouldGibPayload
{
    uint32_t obj_handle = 0;
};

using af_server_req_payload = std::variant<ShouldGibPayload>;

struct af_server_req_packet
{
    RF_GamePacketHeader header;
    af_server_req_type req_type;
    af_server_req_payload payload;
};

enum class af_just_spawned_info_type : uint8_t
{
    af_loadout = 0x00,
};

struct LoadoutEntry
{
    uint8_t weapon_index;
    uint32_t ammo;
};

struct af_just_spawned_info_packet
{
    RF_GamePacketHeader header;
    uint8_t info_type;  // af_just_spawned_info_type
    uint8_t data[];     // type-specific payload
};

struct af_koth_hill_state_packet
{
    RF_GamePacketHeader header;
    uint8_t hill_uid;
    uint8_t ownership;
    uint8_t steal_dir;
    uint8_t state;
    uint8_t lock_status;
    uint8_t capture_progress;
    uint8_t num_red_players;
    uint8_t num_blue_players;
    uint16_t red_score;
    uint16_t blue_score;
};

struct af_koth_hill_captured_packet
{
    RF_GamePacketHeader header;
    uint8_t hill_uid;
    uint8_t ownership;
    uint8_t num_new_owner_players;
    //uint8_t new_owner_player_ids[]; // appended on the wire
};

enum af_just_died_info_flags
{
    JDI_RESPAWN_ALLOWED = 0x1,
    JDI_FORCE_RESPAWN = 0x2
};

struct af_just_died_info_packet
{
    RF_GamePacketHeader header;
    uint8_t flags;
    uint16_t spawn_delay;
};

enum af_server_info_flags : uint32_t {
    SIF_NONE = 0,
    SIF_POSITION_SAVING = 1u << 0,
    SIF_UNUSED = 1u << 1,
    SIF_ALLOW_FULLBRIGHT_MESHES = 1u << 2,
    SIF_ALLOW_LIGHTMAPS_ONLY = 1u << 3,
    SIF_ALLOW_NO_SCREENSHAKE = 1u << 4,
    SIF_NO_PLAYER_COLLIDE = 1u << 5,
    SIF_ALLOW_NO_MUZZLE_FLASH_LIGHT = 1u << 6,
    SIF_CLICK_LIMITER = 1u << 7,
    SIF_ALLOW_UNLIMITED_FPS = 1u << 8,
    SIF_GAUSSIAN_SPREAD = 1u << 9,
    SIF_LOCATION_PINGING = 1u << 10,
    SIF_DELAYED_SPAWNS = 1u << 11,
    SIF_SERVER_CFG_CHANGED = 1u << 12,
    SIF_ALLOW_FOOTSTEPS = 1u << 13,
};

// Subset of `rf::NetGameFlags`.
enum rf_server_info_flags : uint8_t {
    RFSIF_NONE = 0,
    RFSIF_WEAPON_STAY = 1u << 0,
    RFSIF_FORCE_RESPAWN = 1u << 1,
    RFSIF_TEAM_DAMAGE = 1u << 2,
    RFSIF_FALL_DAMAGE = 1u << 3,
    RFSIF_BALANCE_TEAMS = 1u << 4,
};

struct af_server_info_packet
{
    RF_GamePacketHeader header;
    uint8_t rf_flags = 0;  // subset of rf::NetGameFlags
    uint8_t game_type = 0; // rf::NetGameType
    uint32_t af_flags = 0;
    uint32_t win_condition = 0; // gametype-dependent
    uint16_t semi_auto_cooldown = 0;
};

struct af_spectate_start_packet {
    RF_GamePacketHeader header;
    uint8_t spectatee_id;
};

struct af_spectate_notify_packet {
    RF_GamePacketHeader header;
    uint8_t spectator_id;
    bool does_spectate;
};

enum af_server_msg_type : uint8_t {
    AF_SERVER_MSG_TYPE_REMOTE_SERVER_CFG = 0x1,
    AF_SERVER_MSG_TYPE_AUTOMATED_CHAT = 0x2,
    AF_SERVER_MSG_TYPE_REMOTE_SERVER_CFG_EOF = 0x3,
    AF_SERVER_MSG_TYPE_CONSOLE = 0x4,
};

struct af_server_msg_packet {
    RF_GamePacketHeader header;
    uint8_t type;
    char data[];
};

#pragma pack(pop)

bool af_process_packet(const void* data, int len, const rf::NetAddr& addr, rf::Player* player);
void af_send_packet(rf::Player* player, const void* data, int len, bool is_reliable);

void af_send_ping_location_req_packet(rf::Vector3* pos);
static void af_process_ping_location_req_packet(const void* data, size_t len, const rf::NetAddr& addr);
void af_send_ping_location_packet_to_team(rf::Vector3* pos, uint8_t player_id, rf::ubyte team);
void af_send_ping_location_packet_to_all(rf::Vector3* pos, uint8_t player_id);
static void af_process_ping_location_packet(const void* data, size_t len, const rf::NetAddr& addr);
void af_send_damage_notify_packet(uint8_t player_id, float damage, bool died, rf::Player* player);
static void af_process_damage_notify_packet(const void* data, size_t len, const rf::NetAddr& addr);
void af_send_obj_update_packet(rf::Player* player);
static void af_process_obj_update_packet(const void* data, size_t len, const rf::NetAddr& addr);
void af_send_client_req_packet(const af_client_req_packet& packet);
static void af_process_client_req_packet(const void* data, size_t len, const rf::NetAddr& addr);
void af_send_server_req_packet(const af_server_req_packet& packet, rf::Player* player);
void af_send_should_gib_req(uint32_t obj_handle);
static void af_process_server_req_packet(const void* data, size_t len, const rf::NetAddr& addr);
void af_send_just_spawned_loadout(rf::Player* to_player, std::vector<WeaponLoadoutEntry> loadout);
static void af_process_just_spawned_info_packet(const void* data, size_t len, const rf::NetAddr& addr);
void af_send_koth_hill_state_packet(rf::Player* player, const HillInfo& h, const Presence& pres); // sent to new joiners
void af_send_koth_hill_state_packet_to_all(const HillInfo& h, const Presence& pres);
static void af_process_koth_hill_state_packet(const void* data, size_t len, const rf::NetAddr&);
void af_send_koth_hill_captured_packet_to_all(uint8_t hill_uid, HillOwner owner, const std::vector<uint8_t>& new_owner_player_ids);
static void af_process_koth_hill_captured_packet(const void* data, size_t len, const rf::NetAddr&);
void af_send_just_died_info_packet(rf::Player* to_player, bool respawn_allowed, bool force_respawn, uint16_t spawn_delay);
static void af_process_just_died_info_packet(const void* data, size_t len, const rf::NetAddr& addr);
void af_send_server_info_packet(rf::Player* player);
void af_send_server_info_packet_to_all();
static void af_process_server_info_packet(const void* data, size_t len, const rf::NetAddr&);
void af_send_spectate_start_packet(const rf::Player* spectatee);
void af_process_spectate_start_packet(const void* data, size_t len, const rf::NetAddr&);
void af_send_spectate_notify_packet(rf::Player* player, const rf::Player* spectator, bool does_spectate);
void af_process_spectate_notify_packet(const void* data, size_t len, const rf::NetAddr&);
void af_send_server_cfg(rf::Player* player);
void af_process_server_msg_packet(const void* data, size_t len, const rf::NetAddr&);
void af_broadcast_automated_chat_msg(std::string_view msg);
void af_send_automated_chat_msg(std::string_view msg, rf::Player* player, bool tell_server = false);
void af_send_server_console_msg(std::string_view msg, rf::Player* player, bool tell_server = false);

// client requests
void af_send_handicap_request(uint8_t amount);
void af_send_server_cfg_request();
