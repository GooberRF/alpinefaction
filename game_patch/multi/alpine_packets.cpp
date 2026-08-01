#include <cstddef>
#include <cstring>
#include <cassert>
#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <unordered_map>
#include <common/utils/bool-utils.h>
#include <common/utils/list-utils.h>
#include <common/rfproto.h>
#include <xlog/xlog.h>
#include "../rf/multi.h"
#include "../rf/level.h"
#include "../rf/player/player.h"
#include "../rf/weapon.h"
#include "../rf/os/frametime.h"
#include "multi.h"
#include "mutators.h"
#include "network.h"
#include "server_internal.h"
#include "server.h"
#include "../hud/hud_world.h"
#include "alpine_packets.h"
#include "sprays.h"
#include "kill.h"
#include "kill_attribution.h"
#include "bagman.h"
#include "pit.h"
#include "vote_client.h"
#include "../misc/player.h"
#include "../hud/hud.h"
#include "../hud/multi_spectate.h"
#include "rounds.h"
#include "../sound/sound.h"
#include "../misc/alpine_settings.h"
#include "../misc/waypoints.h"
#include "../object/object.h"
#include "bots/bot_personality.h"
#include "bots/bot_state.h"
#include "../object/object_private.h"
#include "../misc/misc.h"
#include "../purefaction/pf_packets.h"
#include "../os/os.h"

void af_send_packet(rf::Player* player, const void* data, int len, bool is_reliable)
{
    if (!player || !player->net_data) {
        if (!player) { // expected for af_spectate_notify packet when using freecam spec
            xlog::debug("af_send_packet: Attempted to send to null player");
        }
        else if (data && len >= static_cast<int>(sizeof(RF_GamePacketHeader))) {
            RF_GamePacketHeader header{};
            std::memcpy(&header, data, sizeof(header));
            xlog::error("af_send_packet: Attempted to send to invalid player (type 0x{:x}, size {}, buf_len {})",
                header.type,
                header.size,
                len);
        }
        else {
            xlog::error("af_send_packet: Attempted to send to invalid player (unknown type)");
        }
        return;
    }
    if (len <= 0 || len > rf::max_packet_size) {
        xlog::error("af_send_packet: Packet size {} exceeds max {}", len, rf::max_packet_size);
        return;
    }

    //xlog::info("Sending packet: player={}, size={}, reliable={}", player->name, len, is_reliable);

    if (is_reliable) {
        rf::multi_io_send_reliable(player, data, len, 0);
    }
    else {
        //rf::net_send(player->net_data->addr, data, len);
        rf::multi_io_send(player, data, len);
    }
}

bool af_process_packet(
    const void* const data,
    const int len,
    const rf::NetAddr& addr,
    [[maybe_unused]] rf::Player* const player
) {
    RF_GamePacketHeader header{};
    if (len < static_cast<int>(sizeof(header))) {
        return false;
    }

    std::memcpy(&header, data, sizeof(header));
    const af_packet_type packet_type = static_cast<af_packet_type>(header.type);

    switch (packet_type)
    {
        case af_packet_type::af_ping_location_req:
            af_process_ping_location_req_packet(data, len, addr);
            break;
        case af_packet_type::af_ping_location:
            af_process_ping_location_packet(data, len, addr);
            break;
        case af_packet_type::af_damage_notify:
            af_process_damage_notify_packet(data, len, addr);
            break;
        case af_packet_type::af_obj_update:
            af_process_obj_update_packet(data, len, addr);
            break;
        case af_packet_type::af_client_req:
            af_process_client_req_packet(data, len, addr);
            break;
        case af_packet_type::af_just_spawned_info:
            af_process_just_spawned_info_packet(data, len, addr);
            break;
        case af_packet_type::af_koth_hill_state:
            af_process_koth_hill_state_packet(data, static_cast<size_t>(len), addr);
            return true;
        case af_packet_type::af_koth_hill_captured:
            af_process_koth_hill_captured_packet(data, static_cast<size_t>(len), addr);
            return true;
        case af_packet_type::af_just_died_info:
            af_process_just_died_info_packet(data, static_cast<size_t>(len), addr);
            return true;
        case af_packet_type::af_server_info:
            af_process_server_info_packet(data, static_cast<size_t>(len), addr);
            return true;
        case af_packet_type::af_spectate_start: {
            af_process_spectate_start_packet(data, static_cast<size_t>(len), addr);
            return true;
        }
        case af_packet_type::af_spectate_notify: {
            af_process_spectate_notify_packet(data, static_cast<size_t>(len), addr);
            return true;
        }
        case af_packet_type::af_server_msg: {
            af_process_server_msg_packet(data, static_cast<size_t>(len), addr);
            return true;
        }
        case af_packet_type::af_server_req: {
            af_process_server_req_packet(data, static_cast<size_t>(len), addr);
            return true;
        }
        case af_packet_type::af_server_bot_control: {
            af_process_bot_control_packet(data, static_cast<size_t>(len), addr);
            return true;
        }
        case af_packet_type::af_bagman_state: {
            af_process_bagman_state_packet(data, static_cast<size_t>(len), addr);
            return true;
        }
        case af_packet_type::af_pit_roster: {
            af_process_pit_roster_packet(data, static_cast<size_t>(len), addr);
            return true;
        }
        case af_packet_type::af_gungame_order: {
            af_process_gungame_order_packet(data, static_cast<size_t>(len), addr);
            return true;
        }
        default:
            return false; // ignore if unrecognized
    }
    return true;
}

void af_send_ping_location_req_packet(rf::Vector3* pos)
{
    // Send: client -> server
    if (!rf::is_multi || rf::is_server) {
        return;
    }

    std::byte packet_buf[rf::max_packet_size];
    af_ping_location_req_packet ping_location_req_packet{};
    ping_location_req_packet.header.type = static_cast<uint8_t>(af_packet_type::af_ping_location_req);
    ping_location_req_packet.header.size = sizeof(ping_location_req_packet) - sizeof(ping_location_req_packet.header);
    ping_location_req_packet.pos.x = pos->x;
    ping_location_req_packet.pos.y = pos->y;
    ping_location_req_packet.pos.z = pos->z;
    std::memcpy(packet_buf, &ping_location_req_packet, sizeof(ping_location_req_packet));
    af_send_packet(rf::local_player, packet_buf, sizeof(ping_location_req_packet), false);
}

static void af_process_ping_location_req_packet(const void* data, size_t len, const rf::NetAddr& addr)
{
    // Receive: server <- client
    if (!rf::is_dedicated_server) {
        return;
    }

    // ignore ping_location_req packets if location pinging is configured off
    if (!server_location_pinging()) {
        return;
    }

    // ignore ping_location_req packets unless we're in a team mode or RUN
    if (!multi_is_team_game_type() && !gt_is_run()) {
        return;
    }

    if (len < sizeof(RF_GamePacketHeader)) {
        xlog::warn("af_process_ping_location_req_packet: packet too short for header (len={})", len);
        return;
    }

    const auto* header = static_cast<const RF_GamePacketHeader*>(data);
    if (header->type != static_cast<uint8_t>(af_packet_type::af_ping_location_req)) {
        xlog::warn("af_process_ping_location_req_packet: unexpected type {}", header->type);
        return;
    }

    const size_t expected_payload_size = sizeof(af_ping_location_req_packet) - sizeof(RF_GamePacketHeader);
    const size_t expected_wire_size = sizeof(RF_GamePacketHeader) + expected_payload_size;
    if (header->size != expected_payload_size) {
        xlog::warn("af_process_ping_location_req_packet: payload size mismatch ({} != {})", header->size, expected_payload_size);
        return;
    }

    if (len < expected_wire_size) {
        xlog::warn("af_process_ping_location_req_packet: truncated packet ({} < {})", len, expected_wire_size);
        return;
    }

    af_ping_location_req_packet ping_location_req_packet{};
    ping_location_req_packet.header.type = static_cast<uint8_t>(af_packet_type::af_ping_location_req);
    ping_location_req_packet.header.size = sizeof(ping_location_req_packet) - sizeof(ping_location_req_packet.header);

    rf::Player* player = rf::multi_find_player_by_addr(addr);

    if (!player || !player->net_data) {
        xlog::warn("af_process_ping_location_req_packet: no valid player for addr");
        return;
    }

    std::memcpy(&ping_location_req_packet, data, sizeof(ping_location_req_packet));

    rf::Vector3 pos{
        ping_location_req_packet.pos.x,
        ping_location_req_packet.pos.y,
        ping_location_req_packet.pos.z
    };

    gt_is_run() ? af_send_ping_location_packet_to_all(&pos, player->net_data->player_id)
                : af_send_ping_location_packet_to_team(&pos, player->net_data->player_id, player->team);
}

void af_send_ping_location_packet(rf::Vector3* pos, uint8_t player_id, rf::Player* player)
{
    // Send: server -> client
    if (!rf::is_server) {
        return;
    }

    std::byte packet_buf[rf::max_packet_size];
    af_ping_location_packet ping_location_packet{};
    ping_location_packet.header.type = static_cast<uint8_t>(af_packet_type::af_ping_location);
    ping_location_packet.header.size = sizeof(ping_location_packet) - sizeof(ping_location_packet.header);
    ping_location_packet.pos.x = pos->x;
    ping_location_packet.pos.y = pos->y;
    ping_location_packet.pos.z = pos->z;
    ping_location_packet.player_id = player_id;

    std::memcpy(packet_buf, &ping_location_packet, sizeof(ping_location_packet));

    if (!player) {
        xlog::error("af_ping_location_packet: Attempted to send to a null player");
        return;
    }
    af_send_packet(player, packet_buf, sizeof(ping_location_packet), false);
}

void af_send_ping_location_packet_to_team(rf::Vector3* pos, uint8_t player_id, rf::ubyte team)
{
    SinglyLinkedList<rf::Player> linked_player_list{rf::player_list};

    for (auto& player : linked_player_list) {
        if (!&player) continue;  // Skip invalid player
        if (&player == rf::local_player) continue;  // Skip the local player
        if (player.net_data->player_id == player_id) continue; // Skip the sender
        if (player.team != team) continue; // Skip if not on the correct team

        af_send_ping_location_packet(pos, player_id, &player);
    }
}

void af_send_ping_location_packet_to_all(rf::Vector3* pos, uint8_t player_id)
{
    SinglyLinkedList<rf::Player> linked_player_list{rf::player_list};

    for (auto& player : linked_player_list) {
        if (!&player) continue;  // Skip invalid player
        if (&player == rf::local_player) continue;  // Skip the local player
        if (player.net_data->player_id == player_id) continue; // Skip the sender

        af_send_ping_location_packet(pos, player_id, &player);
    }
}

static void af_process_ping_location_packet(const void* data, size_t len, const rf::NetAddr& addr)
{
    // Receive: client <- server
    if (!rf::is_multi || rf::is_server) {
        return;
    }

    af_ping_location_packet ping_location_packet{};
    if (len < sizeof(ping_location_packet)) {
        return;
    }

    std::memcpy(&ping_location_packet, data, sizeof(ping_location_packet));

    rf::Player* player = rf::multi_find_player_by_id(ping_location_packet.player_id);
    if (!player) {
        return;
    }

    rf::Vector3 pos{
        ping_location_packet.pos.x,
        ping_location_packet.pos.y,
        ping_location_packet.pos.z
    };

    add_location_ping_world_hud_sprite(pos, player->name, ping_location_packet.player_id);
}

void af_send_damage_notify_packet(uint8_t player_id, float damage, bool died, rf::Player* player)
{
    // Send: server -> client
    if (!rf::is_server) {
        return;
    }

    std::byte packet_buf[rf::max_packet_size];
    af_damage_notify_packet damage_notify_packet{};
    damage_notify_packet.header.type = static_cast<uint8_t>(af_packet_type::af_damage_notify);
    damage_notify_packet.header.size = sizeof(damage_notify_packet) - sizeof(damage_notify_packet.header);
    damage_notify_packet.player_id = player_id;
    int rounded_damage = static_cast<int>(std::round(damage));
    if (rounded_damage <= 0) {
        return; // skip negligible damage
    }
    damage_notify_packet.damage = static_cast<uint16_t>(rounded_damage);

    damage_notify_packet.flags =
        (static_cast<uint8_t>(died)       << 0);

    std::memcpy(packet_buf, &damage_notify_packet, sizeof(damage_notify_packet));

    if (!player) {
        xlog::error("af_damage_notify_packet: Attempted to send to an invalid player");
        return;
    }
    af_send_packet(player, packet_buf, sizeof(damage_notify_packet), false);
}

static void af_process_damage_notify_packet(const void* data, size_t len, const rf::NetAddr& addr)
{
    // Receive: client <- server
    if (!rf::is_multi || rf::is_server) {
        return;
    }

    af_damage_notify_packet damage_notify_packet{};
    if (len < sizeof(damage_notify_packet)) {
        return;
    }

    std::memcpy(&damage_notify_packet, data, sizeof(damage_notify_packet));

    rf::Player* player = rf::multi_find_player_by_id(damage_notify_packet.player_id);
    if (!player) {
        return;
    }

    rf::Entity* entity = rf::entity_from_handle(player->entity_handle);
    if (!entity) {
        return;
    }

    bool died = static_cast<bool>(damage_notify_packet.flags & 0x01);
    add_damage_notify_world_hud_string(entity->pos, damage_notify_packet.player_id, damage_notify_packet.damage, died);
    play_local_hit_sound(died);
}

void af_send_obj_update_packet(rf::Player* player)
{
    // Send: server -> client
    if (!rf::is_server) {
        return;
    }

    // This is called once per recipient from the obj_update send loop, and the gathered state is
    // identical for every recipient within a frame apart from excluding the recipient's own entry,
    // so gather it only once per frame.
    struct GatheredObjUpdate
    {
        rf::Player* owner;
        af_obj_update update;
    };
    static std::vector<GatheredObjUpdate> gathered_updates;
    static std::vector<af_obj_update> obj_updates;
    static int gathered_frame = -1;

    if (gathered_frame != rf::frame_count) {
        gathered_frame = rf::frame_count;
        gathered_updates.clear();
        auto player_list = SinglyLinkedList{rf::player_list};

        // loop through players to gather info
        for (auto& other_player : player_list) {
            //xlog::info("starting payer list loop");
            if (!&other_player) {
                continue; // player not valid
            }

            if (rf::player_is_dead(&other_player)) {
                continue; // player is dead
            }

            rf::Entity* entity = rf::entity_from_handle(other_player.entity_handle);
            if (!entity) {
                continue; // player entity is invalid or not spawned
            }

            if (rf::entity_is_dying(entity)) {
                continue; // player entity is dying (dying entities have invalid info in ai)
            }

            af_obj_update obj_update{};
            obj_update.obj_handle = entity->handle;

            uint8_t current_primary_weapon = static_cast<uint8_t>(entity->ai.current_primary_weapon);
            if (current_primary_weapon > 63) {
                xlog::debug("obj_update packet tried to process an invalid weapon type: {}", current_primary_weapon);
                continue; // reported weapon type is out of valid range
            }
            obj_update.current_primary_weapon = current_primary_weapon;

            uint8_t ammo_type = static_cast<uint8_t>(rf::weapon_types[current_primary_weapon].ammo_type);
            if (ammo_type > 31) {
                xlog::debug("obj_update packet tried to process an invalid ammo type: {}", ammo_type); // todo: figure out why this happens sometimes on specific maps???
                continue; // reported ammo type is out of valid range
            }
            obj_update.ammo_type = ammo_type;

            obj_update.clip_ammo = static_cast<uint16_t>(entity->ai.clip_ammo[current_primary_weapon]);
            obj_update.reserve_ammo = static_cast<uint16_t>(entity->ai.ammo[ammo_type]);

            /*xlog::warn("Adding player {}, weap {}, ammo {}, clip {}, reserve {}",
                       entity->name, obj_update.current_primary_weapon, obj_update.ammo_type,
                       obj_update.clip_ammo, obj_update.reserve_ammo);*/
            gathered_updates.push_back({&other_player, obj_update});
        }
    }

    // exclude the recipient's own entry
    obj_updates.clear();
    for (const GatheredObjUpdate& gathered_update : gathered_updates) {
        if (gathered_update.owner != player) {
            obj_updates.push_back(gathered_update.update);
        }
    }

    if (obj_updates.empty()) {
        return;  // No updates to send
    }

    // Calculate packet size
    size_t object_data_size = obj_updates.size() * sizeof(af_obj_update);
    size_t total_packet_size = sizeof(RF_GamePacketHeader) + object_data_size;

    if (total_packet_size > rf::max_packet_size) {
        xlog::error("af_send_obj_update_packet: Packet too large! Size: {}", total_packet_size);
        return;
    }

    static std::byte packet_buf[rf::max_packet_size];

    // Fill packet header
    RF_GamePacketHeader header{};
    header.type = static_cast<uint8_t>(af_packet_type::af_obj_update);
    header.size = static_cast<uint16_t>(object_data_size);

    // Copy data to packet buffer
    std::memcpy(packet_buf, &header, sizeof(header));
    std::memcpy(packet_buf + sizeof(header), obj_updates.data(), object_data_size);

    if (!player) {
        xlog::error("af_obj_update: Attempted to send to an invalid player");
        return;
    }
    af_send_packet(player, packet_buf, total_packet_size, false);
}

static void af_process_obj_update_packet(const void* data, size_t len, const rf::NetAddr& addr)
{
    // Receive: client <- server
    if (!rf::is_multi || rf::is_server) {
        return;
    }

    if (len < sizeof(RF_GamePacketHeader)) {
        xlog::warn("Received malformed object update packet (too small)");
        return;
    }

    // Read the packet header
    RF_GamePacketHeader header;
    std::memcpy(&header, data, sizeof(header));

    // Calculate the expected object data size
    size_t object_data_size = header.size;
    if (len != sizeof(RF_GamePacketHeader) + object_data_size) {
        xlog::warn("Object update packet has unexpected size! Expected {}, got {}", 
                   sizeof(RF_GamePacketHeader) + object_data_size, len);
        return;
    }

    // Calculate number of objects being updated
    size_t num_objects = object_data_size / sizeof(af_obj_update);
    if (num_objects == 0) {
        return; // Nothing to process
    }

    // Copy object updates to a vector to avoid misaligned access
    std::vector<af_obj_update> obj_updates(num_objects);
    std::memcpy(obj_updates.data(), 
                static_cast<const std::byte*>(data) + sizeof(RF_GamePacketHeader), 
                object_data_size);

    // Process each object update safely
    for (const auto& obj_update : obj_updates) {
        // Validate the remote object
        rf::Object* remote_object = rf::obj_from_remote_handle(obj_update.obj_handle);
        if (!remote_object) {
            //xlog::warn("Received obj update for invalid remote handle: {:x}", obj_update.obj_handle);
            continue;
        }

        rf::Entity* entity = rf::entity_from_handle(remote_object->handle);
        if (!entity) {
            //xlog::warn("Received obj update for invalid entity handle: {:x}", obj_update.obj_handle);
            continue;
        }

        // Do not update the local player (shouldn't even be in the packet, but just to make sure
        if (entity == rf::local_player_entity) {
            continue;
        }

        // Only update ammo if the player's weapon matches the packet
        if (entity->ai.current_primary_weapon == obj_update.current_primary_weapon) {
            entity->ai.clip_ammo[obj_update.current_primary_weapon] = obj_update.clip_ammo;
            entity->ai.ammo[obj_update.ammo_type] = obj_update.reserve_ammo;

            /* xlog::warn("Updated player {}, weapon {}, ammo type {}, clip {}, reserve {}", 
                        entity->name, obj_update.current_primary_weapon, obj_update.ammo_type, 
                        obj_update.clip_ammo, obj_update.reserve_ammo);*/
        } else {
            //xlog::warn("Did not update player {} because packet weapon {}, their weapon {}", entity->name, obj_update.current_primary_weapon, entity->ai.current_primary_weapon);
        }
    }
}

// client requests

// send handicap request
void af_send_handicap_request(uint8_t amount)
{
    if (!rf::is_multi || rf::is_server) {
        return; // Only clients should send this
    }

    af_client_req_packet packet{};
    packet.header.type = static_cast<uint8_t>(af_packet_type::af_client_req);
    packet.header.size = sizeof(uint8_t) + sizeof(uint8_t); // req_type + payload (amount)
    packet.req_type = af_client_req_type::af_req_handicap;
    packet.payload = HandicapPayload{amount};

    af_send_client_req_packet(packet);
}

// af_req_handicap
void serialize_payload(const HandicapPayload& payload, std::byte* buf, size_t& offset)
{
    buf[offset++] = static_cast<std::byte>(payload.amount);
}

// af_req_spray
void serialize_payload(const SprayReqPayload& payload, std::byte* buf, size_t& offset)
{
    std::memcpy(buf + offset, &payload.texture_id, sizeof(payload.texture_id));
    offset += sizeof(payload.texture_id);
    std::memcpy(buf + offset, &payload.pos, sizeof(payload.pos));
    offset += sizeof(payload.pos);
    std::memcpy(buf + offset, &payload.normal, sizeof(payload.normal));
    offset += sizeof(payload.normal);
}

// af_req_character
void serialize_payload(const CharacterPayload& payload, std::byte* buf, size_t& offset)
{
    buf[offset++] = static_cast<std::byte>(payload.character_index);
}

// af_req_ready
void serialize_payload(const ReadyReqPayload& payload, std::byte* buf, size_t& offset)
{
    buf[offset++] = static_cast<std::byte>(payload.action);
}

// af_req_pit_queue
void serialize_payload(const PitQueueReqPayload& payload, std::byte* buf, size_t& offset)
{
    buf[offset++] = static_cast<std::byte>(payload.action);
}

// af_req_vote_cast
void serialize_payload(const VoteCastReqPayload& payload, std::byte* buf, size_t& offset)
{
    buf[offset++] = static_cast<std::byte>(payload.is_yes);
}

// af_req_vote_options
void serialize_payload(const VoteOptionsReqPayload& payload, std::byte* buf, size_t& offset)
{
    buf[offset++] = static_cast<std::byte>(payload.flags);
    std::memcpy(buf + offset, &payload.known_generation, sizeof(payload.known_generation));
    offset += sizeof(payload.known_generation);
}

// af_sreq_ready_prompt
void serialize_payload(const ReadyPromptPayload& payload, std::byte* buf, size_t& offset)
{
    buf[offset++] = static_cast<std::byte>(payload.state);
}

// af_sreq_pit_queue_state
void serialize_payload(const PitQueueStatePayload& payload, std::byte* buf, size_t& offset)
{
    buf[offset++] = static_cast<std::byte>(payload.flags);
    buf[offset++] = static_cast<std::byte>(payload.position);
    buf[offset++] = static_cast<std::byte>(payload.total);
}

// af_req_server_cfg
void serialize_payload(const std::monostate& payload, const std::byte* const buf, const size_t& offset)
{
    // nothing to do
}

// af_sreq_should_gib
void serialize_payload(const ShouldGibPayload& payload, std::byte* buf, size_t& offset)
{
    std::memcpy(buf + offset, &payload.obj_handle, sizeof(payload.obj_handle));
    offset += sizeof(payload.obj_handle);
}

// af_sreq_teleport_entity
void serialize_payload(const TeleportEntityPayload& payload, std::byte* buf, size_t& offset)
{
    std::memcpy(buf + offset, &payload.obj_handle, sizeof(payload.obj_handle));
    offset += sizeof(payload.obj_handle);
    std::memcpy(buf + offset, &payload.pos, sizeof(payload.pos));
    offset += sizeof(payload.pos);
    std::memcpy(buf + offset, &payload.orient, sizeof(payload.orient));
    offset += sizeof(payload.orient);
    std::memcpy(buf + offset, &payload.vel, sizeof(payload.vel));
    offset += sizeof(payload.vel);
}

// af_sreq_spray
void serialize_payload(const SprayPayload& payload, std::byte* buf, size_t& offset)
{
    std::memcpy(buf + offset, &payload.player_id, sizeof(payload.player_id));
    offset += sizeof(payload.player_id);
    std::memcpy(buf + offset, &payload.texture_id, sizeof(payload.texture_id));
    offset += sizeof(payload.texture_id);
    std::memcpy(buf + offset, &payload.pos, sizeof(payload.pos));
    offset += sizeof(payload.pos);
    std::memcpy(buf + offset, &payload.normal, sizeof(payload.normal));
    offset += sizeof(payload.normal);
    std::memcpy(buf + offset, &payload.flags, sizeof(payload.flags));
    offset += sizeof(payload.flags);
}

void af_send_server_cfg_request() {
    if (!rf::is_multi || rf::is_server) {
        return;
    }

    af_client_req_packet client_req_packet{};
    client_req_packet.header.type = static_cast<uint8_t>(af_packet_type::af_client_req);
    client_req_packet.header.size = sizeof(uint8_t);
    client_req_packet.req_type = af_client_req_type::af_req_server_cfg;
    client_req_packet.payload = std::monostate{};

    af_send_client_req_packet(client_req_packet);
}

void af_send_spray_request(uint16_t texture_id, const rf::Vector3& pos, const rf::Vector3& normal)
{
    // Send: client -> server
    if (!rf::is_multi || rf::is_server) {
        return;
    }

    SprayReqPayload payload{};
    payload.texture_id = texture_id;
    static_assert(sizeof(payload.pos) == sizeof(rf::Vector3), "RF_Vector / rf::Vector3 layout mismatch");
    std::memcpy(&payload.pos, &pos, sizeof(payload.pos));
    std::memcpy(&payload.normal, &normal, sizeof(payload.normal));

    af_client_req_packet packet{};
    packet.header.type = static_cast<uint8_t>(af_packet_type::af_client_req);
    packet.header.size = sizeof(uint8_t) + sizeof(payload.texture_id) + sizeof(payload.pos) + sizeof(payload.normal);
    packet.req_type = af_client_req_type::af_req_spray;
    packet.payload = payload;

    //xlog::info("sprays: sending af_req_spray to server (id={})", texture_id);
    af_send_client_req_packet(packet);
}

// send client request packet
void af_send_client_req_packet(const af_client_req_packet& packet, bool is_reliable)
{
    // Send: client -> server
    if (!rf::is_multi || rf::is_server) {
        return;
    }

    std::byte buf[rf::max_packet_size];
    size_t offset = 0;

    // Write header
    std::memcpy(buf + offset, &packet.header, sizeof(packet.header));
    offset += sizeof(packet.header);

    // Write req_type
    buf[offset++] = static_cast<std::byte>(packet.req_type);

    // Write payload based on type
    std::visit([&](const auto& payload) { serialize_payload(payload, buf, offset); }, packet.payload);

    int total_len = static_cast<int>(offset);
    af_send_packet(rf::local_player, buf, total_len, is_reliable);
}

// Reliably report the locally-selected mp character to the server.
void af_send_character_request(int character_index)
{
    if (!rf::is_multi || rf::is_server) {
        return;
    }
    if (character_index < 0 || character_index >= rf::num_multi_characters) {
        return; // never report a selection outside the valid range
    }

    af_client_req_packet packet{};
    packet.header.type = static_cast<uint8_t>(af_packet_type::af_client_req);
    packet.header.size = sizeof(uint8_t) + sizeof(uint8_t); // req_type + character index
    packet.req_type = af_client_req_type::af_req_character;
    packet.payload = CharacterPayload{static_cast<uint8_t>(character_index)};

    af_send_client_req_packet(packet, true); // reliable
}

// Report a match ready-up action to the server.
void af_send_ready_request(uint8_t action)
{
    if (!rf::is_multi || rf::is_server) {
        return;
    }

    af_client_req_packet packet{};
    packet.header.type = static_cast<uint8_t>(af_packet_type::af_client_req);
    packet.header.size = sizeof(uint8_t) + sizeof(uint8_t); // req_type + action
    packet.req_type = af_client_req_type::af_req_ready;
    packet.payload = ReadyReqPayload{action};

    af_send_client_req_packet(packet, true); // reliable
}

// Request a Pit duel-queue action (join/leave/toggle) from the server.
void af_send_pit_queue_request(uint8_t action)
{
    if (!rf::is_multi || rf::is_server) {
        return;
    }

    af_client_req_packet packet{};
    packet.header.type = static_cast<uint8_t>(af_packet_type::af_client_req);
    packet.header.size = sizeof(uint8_t) + sizeof(uint8_t); // req_type + action
    packet.req_type = af_client_req_type::af_req_pit_queue;
    packet.payload = PitQueueReqPayload{action};

    af_send_client_req_packet(packet, true); // reliable
}

// ============================================================================
// Vote system packets
// ============================================================================

namespace
{

// Bounds-checked little-endian writers/readers for the variable-length vote
// payloads. Everything is written into a fixed packet buffer, so a payload that
// would overflow simply marks the writer failed and the caller drops the packet.
struct VoteWriter
{
    std::byte* buf;
    size_t cap;
    size_t off;
    bool ok = true;

    void u8(uint8_t v)
    {
        if (!ok || 1 > cap - off) {
            ok = false;
            return;
        }
        buf[off++] = static_cast<std::byte>(v);
    }

    void u16(uint16_t v)
    {
        if (!ok || sizeof(v) > cap - off) {
            ok = false;
            return;
        }
        std::memcpy(buf + off, &v, sizeof(v));
        off += sizeof(v);
    }

    void u32(uint32_t v)
    {
        if (!ok || sizeof(v) > cap - off) {
            ok = false;
            return;
        }
        std::memcpy(buf + off, &v, sizeof(v));
        off += sizeof(v);
    }

    void i32(int32_t v)
    {
        if (!ok || sizeof(v) > cap - off) {
            ok = false;
            return;
        }
        std::memcpy(buf + off, &v, sizeof(v));
        off += sizeof(v);
    }

    void f32(float v)
    {
        if (!ok || sizeof(v) > cap - off) {
            ok = false;
            return;
        }
        std::memcpy(buf + off, &v, sizeof(v));
        off += sizeof(v);
    }

    void str(std::string_view s)
    {
        const size_t n = std::min<size_t>(s.size(), 255);
        u8(static_cast<uint8_t>(n));
        if (!ok || n > cap - off) {
            ok = false;
            return;
        }
        if (n) {
            std::memcpy(buf + off, s.data(), n); // an empty string_view's data() may be null
        }
        off += n;
    }

    void bytes(const void* src, size_t n)
    {
        if (!ok || n > cap - off) {
            ok = false;
            return;
        }
        std::memcpy(buf + off, src, n);
        off += n;
    }
};

struct VoteReader
{
    const uint8_t* data;
    size_t len;
    size_t pos = 0;
    bool ok = true;

    uint8_t u8()
    {
        if (!ok || 1 > len - pos) {
            ok = false;
            return 0;
        }
        return data[pos++];
    }

    uint16_t u16()
    {
        uint16_t v = 0;
        if (!ok || sizeof(v) > len - pos) {
            ok = false;
            return 0;
        }
        std::memcpy(&v, data + pos, sizeof(v));
        pos += sizeof(v);
        return v;
    }

    uint32_t u32()
    {
        uint32_t v = 0;
        if (!ok || sizeof(v) > len - pos) {
            ok = false;
            return 0;
        }
        std::memcpy(&v, data + pos, sizeof(v));
        pos += sizeof(v);
        return v;
    }

    int32_t i32()
    {
        int32_t v = 0;
        if (!ok || sizeof(v) > len - pos) {
            ok = false;
            return 0;
        }
        std::memcpy(&v, data + pos, sizeof(v));
        pos += sizeof(v);
        return v;
    }

    float f32()
    {
        float v = 0.0f;
        if (!ok || sizeof(v) > len - pos) {
            ok = false;
            return 0.0f;
        }
        std::memcpy(&v, data + pos, sizeof(v));
        pos += sizeof(v);
        return v;
    }

    std::string str()
    {
        const uint8_t n = u8();
        if (!ok || n > len - pos) {
            ok = false;
            return {};
        }
        std::string v(reinterpret_cast<const char*>(data + pos), n);
        pos += n;
        return v;
    }
};

// The protocol's only limits on a vote call are the u8 count encoding and
// rf::max_packet_size (enforced by VoteWriter) — there is no separate policy cap.
//
// Returns false if the selection cannot be encoded. Never truncates or wraps:
// silently dropping part of a player's mutator selection would start a vote that
// isn't the one they asked for.
bool write_vote_mutators(VoteWriter& w, const std::vector<VoteMutatorInput>& mutators)
{
    constexpr size_t u8_max = std::numeric_limits<uint8_t>::max();

    const size_t count = mutators.size();
    if (count > u8_max) {
        xlog::warn("af_send_vote_call: {} mutators does not fit the u8 count encoding", count);
        return false;
    }

    w.u8(static_cast<uint8_t>(count));
    for (size_t i = 0; i < count; ++i) {
        const VoteMutatorInput& mutator = mutators[i];
        w.u8(mutator.mutator_id);
        const size_t option_count = mutator.options.size();
        if (option_count > u8_max) {
            xlog::warn("af_send_vote_call: mutator {} has {} options, which does not fit the u8 count encoding",
                       mutator.mutator_id, option_count);
            return false;
        }
        w.u8(static_cast<uint8_t>(option_count));
        for (size_t o = 0; o < option_count; ++o) {
            const VoteMutatorOptionInput& opt = mutator.options[o];
            w.u8(opt.option_id);
            w.u8(static_cast<uint8_t>(opt.type));
            switch (opt.type) {
                case MutatorOptionType::Bool:
                    w.u8(opt.bool_value ? 1 : 0);
                    break;
                case MutatorOptionType::Choice:
                    w.u8(opt.choice_index);
                    break;
                case MutatorOptionType::Int:
                    w.i32(opt.int_value);
                    break;
                case MutatorOptionType::Float:
                    w.f32(opt.float_value);
                    break;
                case MutatorOptionType::String:
                    w.str(opt.string_value);
                    break;
                default:
                    xlog::warn("af_send_vote_call: mutator {} option {} has unknown type {}",
                               mutator.mutator_id, opt.option_id, static_cast<int>(opt.type));
                    w.ok = false;
                    return false;
            }
        }
    }
    return true;
}

bool read_vote_mutators(VoteReader& r, std::vector<VoteMutatorInput>& out)
{
    // Both counts are u8 reads, so they are inherently <= 255 and every reserve()
    // below is bounded by the encoding itself. The real bound on a hostile
    // payload is the reader's length check against the declared packet size.
    const uint8_t count = r.u8();
    if (!r.ok) {
        return false;
    }
    out.reserve(count);
    for (uint8_t i = 0; i < count; ++i) {
        VoteMutatorInput mutator;
        mutator.mutator_id = r.u8();
        const uint8_t option_count = r.u8();
        if (!r.ok) {
            return false;
        }
        mutator.options.reserve(option_count);
        for (uint8_t o = 0; o < option_count; ++o) {
            VoteMutatorOptionInput opt;
            opt.option_id = r.u8();
            const uint8_t type_raw = r.u8();
            if (!r.ok) {
                return false;
            }
            opt.type = static_cast<MutatorOptionType>(type_raw);
            switch (opt.type) {
                case MutatorOptionType::Bool:
                    opt.bool_value = r.u8() != 0;
                    break;
                case MutatorOptionType::Choice:
                    opt.choice_index = r.u8();
                    break;
                case MutatorOptionType::Int:
                    opt.int_value = r.i32();
                    break;
                case MutatorOptionType::Float:
                    opt.float_value = r.f32();
                    break;
                case MutatorOptionType::String:
                    opt.string_value = r.str();
                    break;
                default:
                    // A real client/server schema mismatch lands here, so make it
                    // diagnosable rather than a silent rejection.
                    xlog::warn("read_vote_mutators: mutator {} option {} has unknown type {}",
                               mutator.mutator_id, opt.option_id, type_raw);
                    return false;
            }
            if (!r.ok) {
                return false;
            }
            mutator.options.push_back(std::move(opt));
        }
        out.push_back(std::move(mutator));
    }
    return r.ok;
}

} // namespace

// Call a vote through the structured path (AF 1.4+ servers only).
void af_send_vote_call(const AfVoteCallParams& params)
{
    if (!rf::is_multi || rf::is_server) {
        return;
    }

    std::byte buf[rf::max_packet_size];
    RF_GamePacketHeader header{};
    header.type = static_cast<uint8_t>(af_packet_type::af_client_req);

    VoteWriter w{buf, sizeof(buf), 0};
    w.bytes(&header, sizeof(header));
    w.u8(static_cast<uint8_t>(af_client_req_type::af_req_vote_call));
    w.u8(static_cast<uint8_t>(params.type));

    bool encoded = true;
    switch (params.type) {
        case AfVoteType::Kick:
            w.u8(params.target_player_id);
            break;
        case AfVoteType::Level:
            w.str(params.level);
            w.u8(params.gametype);
            encoded = write_vote_mutators(w, params.mutators);
            break;
        case AfVoteType::Match:
            w.u8(params.team_size);
            w.str(params.level);
            w.u8(params.gametype);
            encoded = write_vote_mutators(w, params.mutators);
            break;
        case AfVoteType::Extend:
            w.u8(params.extend_minutes);
            break;
        default:
            break; // parameterless vote types
    }

    if (!encoded) {
        return; // write_vote_mutators already logged why
    }

    if (!w.ok) {
        // Would have overflowed the packet buffer. Dropping the send is the only
        // safe option, but never do it silently.
        xlog::warn("af_send_vote_call: vote type {} payload exceeds the {} byte packet buffer; not sent",
                   static_cast<int>(params.type), rf::max_packet_size);
        return;
    }

    header.size = static_cast<uint16_t>(w.off - sizeof(header));
    std::memcpy(buf, &header, sizeof(header));
    af_send_packet(rf::local_player, buf, static_cast<int>(w.off), true);
}

void af_send_vote_cast(bool is_yes_vote)
{
    if (!rf::is_multi || rf::is_server) {
        return;
    }

    af_client_req_packet packet{};
    packet.header.type = static_cast<uint8_t>(af_packet_type::af_client_req);
    packet.header.size = sizeof(uint8_t) + sizeof(uint8_t); // req_type + yes/no
    packet.req_type = af_client_req_type::af_req_vote_cast;
    packet.payload = VoteCastReqPayload{static_cast<uint8_t>(is_yes_vote ? 1 : 0)};

    af_send_client_req_packet(packet, true); // reliable
}

void af_send_vote_cancel()
{
    if (!rf::is_multi || rf::is_server) {
        return;
    }

    af_client_req_packet packet{};
    packet.header.type = static_cast<uint8_t>(af_packet_type::af_client_req);
    packet.header.size = sizeof(uint8_t); // req_type only
    packet.req_type = af_client_req_type::af_req_vote_cancel;
    packet.payload = std::monostate{};

    af_send_client_req_packet(packet, true); // reliable
}

void af_send_vote_options_request(bool has_cache, uint32_t known_generation)
{
    if (!rf::is_multi || rf::is_server) {
        return;
    }

    VoteOptionsReqPayload payload{};
    payload.flags = has_cache ? AF_VOTE_OPTIONS_REQ_HAS_CACHE : uint8_t{0};
    payload.known_generation = has_cache ? known_generation : 0;

    af_client_req_packet packet{};
    packet.header.type = static_cast<uint8_t>(af_packet_type::af_client_req);
    packet.header.size = sizeof(uint8_t) + sizeof(payload.flags) + sizeof(payload.known_generation);
    packet.req_type = af_client_req_type::af_req_vote_options;
    packet.payload = payload;

    af_send_client_req_packet(packet, true); // reliable
}

// Shared tail for the three af_sreq_vote_state events: `w` already holds the
// header, req_type and event byte plus the event-specific fields.
static void af_finish_vote_state_packet(rf::Player* player, std::byte* buf, VoteWriter& w)
{
    if (!w.ok) {
        xlog::warn("af_send_vote_state: payload too large");
        return;
    }

    RF_GamePacketHeader header{};
    header.type = static_cast<uint8_t>(af_packet_type::af_server_req);
    header.size = static_cast<uint16_t>(w.off - sizeof(header));
    std::memcpy(buf, &header, sizeof(header));
    af_send_packet(player, buf, static_cast<int>(w.off), true);
}

// Can this recipient receive the structured vote events at all?
static bool af_vote_recipient_is_structured(rf::Player* player)
{
    return player && player != rf::local_player && player->net_data
        && is_player_minimum_af_client_version(player, 1, 4, 0);
}

void af_send_vote_state_start(rf::Player* player, AfVoteType type, uint16_t time_remaining_sec,
                              uint8_t yes, uint8_t no, uint8_t remaining, bool is_owner, bool is_sync,
                              std::string_view initiator_name, std::string_view title)
{
    if (!rf::is_server || !af_vote_recipient_is_structured(player)) {
        return;
    }

    // Fixed part: header + req_type + event + vote_type + u16 time + yes/no/
    // remaining + flags + the two string length bytes.
    constexpr size_t fixed_len = sizeof(RF_GamePacketHeader) + 9 + 2;
    // Guards str_budget against underflowing if fields are ever added here.
    static_assert(fixed_len < rf::max_packet_size,
                  "af_sreq_vote_state start fixed fields no longer fit a packet");
    constexpr size_t str_budget = rf::max_packet_size - fixed_len;

    // A long title (many mutators) must never cost the client its START event,
    // so both strings are truncated to fit rather than overflowing the packet.
    const size_t name_len = std::min<size_t>({initiator_name.size(), 255, str_budget});
    const size_t title_len = std::min<size_t>({title.size(), 255, str_budget - name_len});

    uint8_t flags = 0;
    if (is_owner) {
        flags |= AF_VOTE_STATE_FLAG_OWNER;
    }
    if (is_sync) {
        flags |= AF_VOTE_STATE_FLAG_SYNC;
    }

    std::byte buf[rf::max_packet_size];
    VoteWriter w{buf, sizeof(buf), sizeof(RF_GamePacketHeader)};
    w.u8(static_cast<uint8_t>(af_server_req_type::af_sreq_vote_state));
    w.u8(static_cast<uint8_t>(AfVoteStateEvent::Start));
    w.u8(static_cast<uint8_t>(type));
    w.u16(time_remaining_sec);
    w.u8(yes);
    w.u8(no);
    w.u8(remaining);
    w.u8(flags);
    w.str(initiator_name.substr(0, name_len));
    w.str(title.substr(0, title_len));
    af_finish_vote_state_packet(player, buf, w);
}

void af_send_vote_state_update(rf::Player* player, uint8_t yes, uint8_t no, uint8_t remaining)
{
    if (!rf::is_server || !af_vote_recipient_is_structured(player)) {
        return;
    }

    std::byte buf[rf::max_packet_size];
    VoteWriter w{buf, sizeof(buf), sizeof(RF_GamePacketHeader)};
    w.u8(static_cast<uint8_t>(af_server_req_type::af_sreq_vote_state));
    w.u8(static_cast<uint8_t>(AfVoteStateEvent::Update));
    w.u8(yes);
    w.u8(no);
    w.u8(remaining);
    af_finish_vote_state_packet(player, buf, w);
}

void af_send_vote_state_end(rf::Player* player, AfVoteResult result, bool passed,
                            std::string_view detail)
{
    if (!rf::is_server || !af_vote_recipient_is_structured(player)) {
        return;
    }

    // header + req_type + event + result + flags + the detail length byte.
    constexpr size_t fixed_len = sizeof(RF_GamePacketHeader) + 4 + 1;
    static_assert(fixed_len < rf::max_packet_size,
                  "af_sreq_vote_state end fixed fields no longer fit a packet");
    // The detail line is truncated rather than dropped: losing the end event
    // entirely would strand the client's HUD notification.
    const size_t detail_len = std::min<size_t>({detail.size(), 255, rf::max_packet_size - fixed_len});

    std::byte buf[rf::max_packet_size];
    VoteWriter w{buf, sizeof(buf), sizeof(RF_GamePacketHeader)};
    w.u8(static_cast<uint8_t>(af_server_req_type::af_sreq_vote_state));
    w.u8(static_cast<uint8_t>(AfVoteStateEvent::End));
    w.u8(static_cast<uint8_t>(result));
    w.u8(passed ? AF_VOTE_END_FLAG_PASSED : uint8_t{0});
    w.str(detail.substr(0, detail_len));
    af_finish_vote_state_packet(player, buf, w);
}

// Stream the vote-options blob as Begin -> Data* -> End on the deferred reliable
// queue. Everything on that queue is drained FIFO into rf::net_rel_send, which is
// an ordered reliable channel, so the client sees the three event kinds in the
// order they were queued and no chunk index or chunk count is needed. No size
// ceiling, and the per-packet payload length comes from the packet header rather
// than a u8, so a chunk carries ~500 bytes instead of 255.
//
// Note the End sentinel is QUEUED, not sent with rf::multi_io_send_reliable:
// mixing an immediate send with queued packets would let the sentinel overtake
// the data it terminates.
void af_send_vote_options_data(rf::Player* player)
{
    if (!rf::is_server) {
        return;
    }

    // Gate on the recipient BEFORE touching the blob: building it bumps the
    // generation, so an ineligible client must not be able to force a rebuild.
    if (!af_vote_recipient_is_structured(player)) {
        return;
    }

    uint32_t generation = 0;
    const std::vector<uint8_t>& blob = server_vote_get_options_blob(generation);

    const int socket = player->net_data->reliable_socket;

    // req_type + stream event + generation
    constexpr size_t frame_prefix = sizeof(uint8_t) + sizeof(uint8_t) + sizeof(uint32_t);
    constexpr size_t max_chunk_len = rf::max_packet_size - sizeof(RF_GamePacketHeader) - frame_prefix;
    static_assert(max_chunk_len > 0, "vote options chunk payload no longer fits a packet");

    const auto queue_frame = [&](AfVoteOptionsStream event, const uint8_t* data, size_t len,
                                 std::optional<uint32_t> extra) {
        std::byte buf[rf::max_packet_size];
        VoteWriter w{buf, sizeof(buf), sizeof(RF_GamePacketHeader)};
        w.u8(static_cast<uint8_t>(af_server_req_type::af_sreq_vote_options_data));
        w.u8(static_cast<uint8_t>(event));
        w.u32(generation);
        if (extra) {
            w.u32(*extra);
        }
        if (len) {
            w.bytes(data, len);
        }
        if (!w.ok) {
            xlog::error("af_send_vote_options_data: frame overflow (event {})", static_cast<int>(event));
            return false;
        }

        RF_GamePacketHeader header{};
        header.type = static_cast<uint8_t>(af_packet_type::af_server_req);
        header.size = static_cast<uint16_t>(w.off - sizeof(header));
        std::memcpy(buf, &header, sizeof(header));

        send_queues_rel_add_packet(socket, reinterpret_cast<const uint8_t*>(buf), w.off);
        return true;
    };

    if (blob.size() > af_vote_options_max_blob_size) {
        // Would be rejected by the client's accumulation cap anyway, so don't
        // spend the bandwidth. This is unreachable in practice.
        xlog::error("af_send_vote_options_data: blob of {} bytes exceeds the {} byte transport cap",
                    blob.size(), af_vote_options_max_blob_size);
        return;
    }

    if (!queue_frame(AfVoteOptionsStream::Begin, nullptr, 0, static_cast<uint32_t>(blob.size()))) {
        return;
    }
    for (size_t sent = 0; sent < blob.size(); sent += max_chunk_len) {
        const size_t chunk_len = std::min(max_chunk_len, blob.size() - sent);
        if (!queue_frame(AfVoteOptionsStream::Data, blob.data() + sent, chunk_len, std::nullopt)) {
            return; // the client discards the unterminated stream
        }
    }
    queue_frame(AfVoteOptionsStream::End, nullptr, 0, std::nullopt);

    xlog::debug("vote options: streamed {} bytes (generation {}) to {}", blob.size(), generation,
                player->name);
}

// process client request packet
static void af_process_client_req_packet(const void* data, size_t len, const rf::NetAddr& addr)
{
    // Receive: server <- client
    if (!rf::is_server) {
        return;
    }

    if (len < sizeof(RF_GamePacketHeader)) {
        xlog::warn("af_process_client_req_packet: packet too short for header (len={})", len);
        return;
    }

    RF_GamePacketHeader header{};
    std::memcpy(&header, data, sizeof(header));

    if (header.type != static_cast<uint8_t>(af_packet_type::af_client_req)) {
        xlog::warn("af_process_client_req_packet: unexpected type {}", header.type);
        return;
    }

    const size_t expected_wire_size = sizeof(RF_GamePacketHeader) + static_cast<size_t>(header.size);
    if (expected_wire_size > len) {
        xlog::warn("af_process_client_req_packet: truncated packet ({} > {})", expected_wire_size, len);
        return;
    }

    if (header.size < sizeof(uint8_t)) {
        xlog::warn("af_process_client_req_packet: payload too small ({})", header.size);
        return;
    }

    rf::Player* player = rf::multi_find_player_by_addr(addr);
    if (!player || !player->net_data) {
        xlog::warn("af_process_client_req_packet: no valid player for addr");
        return;
    }

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);

    const size_t payload_begin = sizeof(RF_GamePacketHeader);
    const size_t payload_end = sizeof(RF_GamePacketHeader) + static_cast<size_t>(header.size);

    size_t offset = payload_begin;
    const auto req_type = static_cast<af_client_req_type>(bytes[offset++]);

    // Remaining payload types after req_type
    const size_t remaining = (offset <= payload_end) ? (payload_end - offset) : 0;

    switch (req_type) {
        case af_client_req_type::af_req_handicap: {
            if (remaining < 1) {
                xlog::warn("af_process_client_req_packet: Handicap payload too short");
                return;
            }

            uint8_t amount = bytes[offset];

            handle_player_set_handicap(player, amount);
            break;
        }
        case af_client_req_type::af_req_server_cfg: {
            if (!player->remote_server_cfg_sent) {
                af_send_server_cfg(player);
                player->remote_server_cfg_sent = true;
            }
            break;
        }
        case af_client_req_type::af_req_character: {
            if (remaining < sizeof(uint8_t)) {
                xlog::warn("af_process_client_req_packet: Character payload too short");
                return;
            }
            const uint8_t idx = bytes[offset];
            if (idx < rf::num_multi_characters) {
                // Store the client's real selection separately from
                // settings.multi_character, which force_character overwrites at
                // spawn time. The spawn hook picks force vs. this reported value.
                player->reported_multi_character = static_cast<int>(idx);
            }
            else {
                xlog::warn("af_process_client_req_packet: character index {} out of range (max {})",
                    idx, rf::num_multi_characters - 1);
            }
            break;
        }
        case af_client_req_type::af_req_spray: {
            constexpr size_t expected = sizeof(uint16_t) + sizeof(RF_Vector) + sizeof(RF_Vector);
            if (remaining < expected) {
                xlog::warn("af_process_client_req_packet: Spray payload too short ({} < {})", remaining, expected);
                return;
            }

            uint16_t texture_id = 0;
            RF_Vector pos{};
            RF_Vector normal{};
            std::memcpy(&texture_id, bytes + offset, sizeof(texture_id));
            offset += sizeof(texture_id);
            std::memcpy(&pos, bytes + offset, sizeof(pos));
            offset += sizeof(pos);
            std::memcpy(&normal, bytes + offset, sizeof(normal));
            offset += sizeof(normal);

            rf::Vector3 spray_pos;
            rf::Vector3 spray_normal;
            std::memcpy(&spray_pos, &pos, sizeof(spray_pos));
            std::memcpy(&spray_normal, &normal, sizeof(spray_normal));

            sprays_handle_spray_request(player, texture_id, spray_pos, spray_normal);
            break;
        }
        case af_client_req_type::af_req_ready: {
            if (remaining < sizeof(uint8_t)) {
                xlog::warn("af_process_client_req_packet: Ready payload too short");
                return;
            }
            const uint8_t action = bytes[offset];
            // toggle_ready_status / set_ready_status both guard pre-match state
            // and Alpine-client status internally.
            if (action == 2) {
                toggle_ready_status(player);
            } else {
                set_ready_status(player, action == 1);
            }
            break;
        }
        case af_client_req_type::af_req_pit_queue: {
            if (remaining < sizeof(uint8_t)) {
                xlog::warn("af_process_client_req_packet: Pit queue payload too short");
                return;
            }
            const uint8_t action = bytes[offset];
            if (gt_is_pit()) {
                pit_handle_queue_request(player, action);
            }
            break;
        }
        case af_client_req_type::af_req_vote_call: {
            VoteReader r{bytes + offset, remaining};

            AfVoteCallParams params{};
            const uint8_t type_raw = r.u8();
            if (!r.ok || type_raw >= af_vote_type_count) {
                xlog::warn("af_process_client_req_packet: bad vote type {}", type_raw);
                return;
            }
            params.type = static_cast<AfVoteType>(type_raw);

            switch (params.type) {
                case AfVoteType::Kick:
                    params.target_player_id = r.u8();
                    break;
                case AfVoteType::Level:
                    params.level = r.str();
                    params.gametype = r.u8();
                    if (!read_vote_mutators(r, params.mutators)) {
                        xlog::warn("af_process_client_req_packet: bad vote level mutators");
                        return;
                    }
                    break;
                case AfVoteType::Match:
                    params.team_size = r.u8();
                    params.level = r.str();
                    params.gametype = r.u8();
                    if (!read_vote_mutators(r, params.mutators)) {
                        xlog::warn("af_process_client_req_packet: bad vote match mutators");
                        return;
                    }
                    break;
                case AfVoteType::Extend:
                    params.extend_minutes = r.u8();
                    break;
                default:
                    break; // parameterless vote types
            }

            if (!r.ok) {
                xlog::warn("af_process_client_req_packet: truncated vote call payload");
                return;
            }

            handle_vote_call_packet(player, std::move(params));
            break;
        }
        case af_client_req_type::af_req_vote_cast: {
            if (remaining < sizeof(uint8_t)) {
                xlog::warn("af_process_client_req_packet: vote cast payload too short");
                return;
            }
            handle_vote_cast_packet(player, bytes[offset] != 0);
            break;
        }
        case af_client_req_type::af_req_vote_cancel: {
            handle_vote_cancel_packet(player);
            break;
        }
        case af_client_req_type::af_req_vote_options: {
            // The payload is optional: a request without it is treated as "I have
            // no cached blob", so the server always has something to send.
            bool has_cache = false;
            uint32_t known_generation = 0;
            if (remaining >= sizeof(uint8_t)) {
                has_cache = (bytes[offset] & AF_VOTE_OPTIONS_REQ_HAS_CACHE) != 0;
                if (has_cache && remaining >= sizeof(uint8_t) + sizeof(uint32_t)) {
                    std::memcpy(&known_generation, bytes + offset + sizeof(uint8_t),
                                sizeof(known_generation));
                }
                else {
                    has_cache = false;
                }
            }
            server_vote_handle_options_request(player, has_cache, known_generation);
            break;
        }
        default: {
            xlog::debug("af_process_client_req_packet: unknown req_type {}", static_cast<int>(req_type));
            return;
        }
    }
}

void af_send_server_req_packet(const af_server_req_packet& packet, rf::Player* player)
{
    // Send: server -> client
    if (!rf::is_server || !player || !player->net_data) {
        return;
    }

    std::byte buf[rf::max_packet_size];
    size_t offset = 0;

    std::memcpy(buf + offset, &packet.header, sizeof(packet.header));
    offset += sizeof(packet.header);

    buf[offset++] = static_cast<std::byte>(packet.req_type);

    std::visit([&](const auto& payload) { serialize_payload(payload, buf, offset); }, packet.payload);

    int total_len = static_cast<int>(offset);
    af_send_packet(player, buf, total_len, true);
}

void af_send_should_gib_req(uint32_t obj_handle)
{
    if (!rf::is_server) {
        return;
    }

    af_server_req_packet packet{};
    packet.header.type = static_cast<uint8_t>(af_packet_type::af_server_req);
    packet.header.size = sizeof(uint8_t) + sizeof(uint32_t);
    packet.req_type = af_server_req_type::af_sreq_should_gib;
    packet.payload = ShouldGibPayload{obj_handle};

    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        // 1.2.1 through 1.3 only. Alpine 1.4+ receives the same decision as
        // AF_KILL_FLAG_GIBBED inside the kill-info packet it is already being sent.
        if (is_player_minimum_af_client_version(&player, 1, 2, 1)
            && !is_player_minimum_af_client_version(&player, 1, 4, 0)) {
            af_send_server_req_packet(packet, &player);
        }
    }
}

// Tell capable clients what actually landed the killing blow.
void af_send_kill_info(rf::Player* killed_player)
{
    if (!rf::is_server || !killed_player || !killed_player->net_data) {
        return;
    }

    KillInfoPayload payload{};
    payload.killed_player_id = killed_player->net_data->player_id;
    std::vector<uint8_t> assists;
    if (auto attr = kill_attribution_lookup_for_send(payload.killed_player_id)) {
        payload.killer_player_id = attr->killer_player_id;
        payload.weapon_type = attr->weapon_type;
        payload.flags = attr->flags;
        payload.damage_type = attr->damage_type;
        assists = attr->assist_player_ids;
    }
    const uint8_t assist_count =
        static_cast<uint8_t>(std::min<size_t>(assists.size(), af_kill_info_max_assists));

    // Nothing here the client can act on - no weapon, no flags, no assists - so sending it
    // would cost a reliable packet per client for nothing.
    if (payload.weapon_type == 0xFF && payload.flags == 0 && assist_count == 0) {
        return;
    }

    std::byte buf[rf::max_packet_size];
    size_t off = 0;

    RF_GamePacketHeader header{};
    header.type = static_cast<uint8_t>(af_packet_type::af_server_req);
    header.size = static_cast<uint16_t>(sizeof(uint8_t) + sizeof(KillInfoPayload)
                                        + sizeof(uint8_t) + assist_count);
    std::memcpy(buf + off, &header, sizeof(header));
    off += sizeof(header);

    buf[off++] = static_cast<std::byte>(af_server_req_type::af_sreq_kill_info);
    std::memcpy(buf + off, &payload, sizeof(payload));
    off += sizeof(payload);
    buf[off++] = static_cast<std::byte>(assist_count);
    for (uint8_t i = 0; i < assist_count; ++i) {
        buf[off++] = static_cast<std::byte>(assists[i]);
    }

    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        // Bots and browsers have no purpose for presentation metadata,
        // save the packets.
        if (!player.net_data || player.is_bot || player.is_browser) {
            continue;
        }
        // state 2 is "in game"; stock send_obj_kill_packet gates on it too (0x0047ED9B).
        // Recipients must stay a subset of obj_kill's, or a still-joining client is left
        // holding a pending attribution whose obj_kill never comes.
        if (player.net_data->state != 2) {
            continue;
        }
        if (is_player_minimum_af_client_version(&player, 1, 4, 0)) {
            af_send_packet(&player, buf, static_cast<int>(off), true); // reliable
        }
    }
}

void af_send_teleport_entity_req(
    uint32_t obj_handle,
    const rf::Vector3& pos,
    const rf::Matrix3& orient,
    const rf::Vector3& vel)
{
    if (!rf::is_server) {
        return;
    }

    TeleportEntityPayload payload{};
    payload.obj_handle = obj_handle;
    static_assert(sizeof(payload.pos) == sizeof(rf::Vector3), "RF_Vector / rf::Vector3 layout mismatch");
    static_assert(sizeof(payload.orient) == sizeof(rf::Matrix3), "RF_Matrix / rf::Matrix3 layout mismatch");
    std::memcpy(&payload.pos, &pos, sizeof(payload.pos));
    std::memcpy(&payload.orient, &orient, sizeof(payload.orient));
    std::memcpy(&payload.vel, &vel, sizeof(payload.vel));

    af_server_req_packet packet{};
    packet.header.type = static_cast<uint8_t>(af_packet_type::af_server_req);
    packet.header.size = sizeof(uint8_t) + sizeof(payload.obj_handle) + sizeof(payload.pos) + sizeof(payload.orient) + sizeof(payload.vel);
    packet.req_type = af_server_req_type::af_sreq_teleport_entity;
    packet.payload = payload;

    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (is_player_minimum_af_client_version(&player, 1, 4, 0)) {
            af_send_server_req_packet(packet, &player);
        }
    }
}

void af_send_spray_to_player(uint8_t player_id, uint16_t texture_id, const rf::Vector3& pos,
    const rf::Vector3& normal, uint8_t flags, rf::Player* player)
{
    if (!rf::is_server || !player || !player->net_data) {
        return;
    }
    if (!is_player_minimum_af_client_version(player, 1, 4, 0)) {
        return;
    }

    SprayPayload payload{};
    payload.player_id = player_id;
    payload.texture_id = texture_id;
    static_assert(sizeof(payload.pos) == sizeof(rf::Vector3), "RF_Vector / rf::Vector3 layout mismatch");
    std::memcpy(&payload.pos, &pos, sizeof(payload.pos));
    std::memcpy(&payload.normal, &normal, sizeof(payload.normal));
    payload.flags = flags;

    af_server_req_packet packet{};
    packet.header.type = static_cast<uint8_t>(af_packet_type::af_server_req);
    packet.header.size = sizeof(uint8_t) + sizeof(payload.player_id) + sizeof(payload.texture_id)
        + sizeof(payload.pos) + sizeof(payload.normal) + sizeof(payload.flags);
    packet.req_type = af_server_req_type::af_sreq_spray;
    packet.payload = payload;

    af_send_server_req_packet(packet, player);
}

void af_broadcast_spray(uint8_t player_id, uint16_t texture_id, const rf::Vector3& pos, const rf::Vector3& normal)
{
    if (!rf::is_server) {
        return;
    }

    int sent = 0;
    int skipped = 0;
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (&player == rf::local_player) {
            continue; // listen-server host renders locally instead
        }
        // Recipients (including the requesting player) are gated on AF 1.4 inside the sender.
        if (is_player_minimum_af_client_version(&player, 1, 4, 0)) {
            af_send_spray_to_player(player_id, texture_id, pos, normal, 0, &player);
            ++sent;
        }
        else {
            ++skipped;
        }
    }
    //xlog::info("sprays: broadcast spray for player_id {} to {} clients ({} pre-1.4 skipped)", player_id, sent, skipped);
}

// Show/hide the match ready-up prompt on a specific client.
void af_send_ready_prompt(rf::Player* player, uint8_t state)
{
    if (!rf::is_server || !player) {
        return;
    }
    if (player == rf::local_player) {
        // Listen-server host: apply locally instead of routing through the net
        // (same 0/1/2 mapping as the remote client handler).
        apply_ready_prompt_state(state);
        return;
    }
    if (!player->net_data) {
        return;
    }
    if (!is_player_minimum_af_client_version(player, 1, 4, 0)) {
        return;
    }

    af_server_req_packet packet{};
    packet.header.type = static_cast<uint8_t>(af_packet_type::af_server_req);
    packet.header.size = sizeof(uint8_t) + sizeof(uint8_t); // req_type + state
    packet.req_type = af_server_req_type::af_sreq_ready_prompt;
    packet.payload = ReadyPromptPayload{state};

    af_send_server_req_packet(packet, player);
}

// Apply a Pit queue-state update on the local client.
static void apply_local_pit_queue_state(uint8_t flags, uint8_t pos, uint8_t total)
{
    const bool queued = (flags & 0x1) != 0;
    const bool is_dueler = (flags & 0x2) != 0;
    const bool spectate = (flags & 0x4) != 0;
    set_local_pit_queue_state(queued, is_dueler, static_cast<int>(pos), static_cast<int>(total), spectate);

    // A promoted dueler still locally spectating must leave spectate so the
    // server's auto-spawn round-trips (af_spectate_start(self)).
    if (is_dueler && multi_spectate_is_spectating()) {
        multi_spectate_leave();
    }
}

// Push a client's Pit queue state (dueler / queued position).
// Listen-server host applies locally.
void af_send_pit_queue_state(rf::Player* player, uint8_t flags, uint8_t pos, uint8_t total)
{
    if (!rf::is_server || !player) {
        return;
    }
    if (player == rf::local_player) {
        // Listen-server host: apply locally instead of sending on the wire.
        apply_local_pit_queue_state(flags, pos, total);
        return;
    }
    if (!player->net_data) {
        return;
    }
    if (!is_player_minimum_af_client_version(player, 1, 4, 0)) {
        return;
    }

    af_server_req_packet packet{};
    packet.header.type = static_cast<uint8_t>(af_packet_type::af_server_req);
    packet.header.size = sizeof(uint8_t) + 3; // req_type + flags + position + total
    packet.req_type = af_server_req_type::af_sreq_pit_queue_state;
    packet.payload = PitQueueStatePayload{flags, pos, total};

    af_send_server_req_packet(packet, player);
}

// Apply a Pit roster on the local client: clear the stored roles, then set each
// (player_id -> role) entry.
static void apply_local_pit_roster(const af_pit_roster_entry* entries, uint8_t count)
{
    reset_local_pit_roster();
    for (uint8_t i = 0; i < count; ++i) {
        set_local_pit_roster_entry(entries[i].player_id, entries[i].role, entries[i].order);
    }
}

// Send the full Pit roster to one player. Listen-server host applies locally..
void af_send_pit_roster(rf::Player* player, const std::vector<af_pit_roster_entry>& roster)
{
    if (!rf::is_server || !player) {
        return;
    }

    std::byte buf[rf::max_packet_size];
    const size_t max_entries =
        (sizeof(buf) - sizeof(RF_GamePacketHeader) - sizeof(uint8_t)) / sizeof(af_pit_roster_entry);
    const uint8_t count =
        static_cast<uint8_t>(std::min<size_t>({roster.size(), max_entries, size_t{255}}));

    if (player == rf::local_player) {
        apply_local_pit_roster(roster.data(), count); // listen host applies locally
        return;
    }
    if (!player->net_data) {
        return;
    }
    if (!is_player_minimum_af_client_version(player, 1, 4, 0)) {
        return;
    }

    size_t off = 0;
    RF_GamePacketHeader header{};
    header.type = static_cast<uint8_t>(af_packet_type::af_pit_roster);
    header.size = static_cast<uint16_t>(sizeof(uint8_t) + count * sizeof(af_pit_roster_entry));
    std::memcpy(buf + off, &header, sizeof(header));
    off += sizeof(header);
    buf[off++] = static_cast<std::byte>(count);
    for (uint8_t i = 0; i < count; ++i) {
        std::memcpy(buf + off, &roster[i], sizeof(af_pit_roster_entry));
        off += sizeof(af_pit_roster_entry);
    }

    af_send_packet(player, buf, static_cast<int>(off), true); // reliable
}

void af_broadcast_pit_roster(const std::vector<af_pit_roster_entry>& roster)
{
    if (!rf::is_server) {
        return;
    }
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (player.is_browser) continue;
        af_send_pit_roster(&player, roster);
    }
}

void af_process_pit_roster_packet(const void* data, size_t len, const rf::NetAddr&)
{
    // Receive: client <- server
    if (!rf::is_multi || rf::is_server) {
        return;
    }
    if (len < sizeof(RF_GamePacketHeader)) {
        return;
    }

    RF_GamePacketHeader hdr{};
    std::memcpy(&hdr, data, sizeof(hdr));
    if (sizeof(RF_GamePacketHeader) + static_cast<size_t>(hdr.size) > len) {
        xlog::warn("pit_roster: truncated (declared={}, len={})", hdr.size, len);
        return;
    }
    if (hdr.size < sizeof(uint8_t)) {
        xlog::warn("pit_roster: missing count byte");
        return;
    }

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
    size_t off = sizeof(RF_GamePacketHeader);
    const uint8_t count = bytes[off];
    off += sizeof(uint8_t);

    // Validate the entry count against BOTH the declared payload and actual len
    // before indexing.
    const size_t entries_bytes = static_cast<size_t>(count) * sizeof(af_pit_roster_entry);
    if (static_cast<size_t>(hdr.size) < sizeof(uint8_t) + entries_bytes) {
        xlog::warn("pit_roster: count {} exceeds declared payload", count);
        return;
    }
    if (off + entries_bytes > len) {
        xlog::warn("pit_roster: truncated entries (need {}, have {})", off + entries_bytes, len);
        return;
    }

    apply_local_pit_roster(reinterpret_cast<const af_pit_roster_entry*>(bytes + off), count);
}

// Send a player their own Gun Game weapon order.
void af_send_gungame_order(rf::Player* player, const std::vector<af_gungame_order_entry>& order)
{
    if (!rf::is_server || !player) {
        return;
    }

    std::byte buf[rf::max_packet_size];
    const size_t max_entries =
        (sizeof(buf) - sizeof(RF_GamePacketHeader) - sizeof(uint8_t)) / sizeof(af_gungame_order_entry);
    const uint8_t count =
        static_cast<uint8_t>(std::min<size_t>({order.size(), max_entries, size_t{255}}));

    if (player == rf::local_player) {
        set_local_gungame_order(order.data(), count); // listen host applies locally
        return;
    }
    if (!player->net_data) {
        return;
    }
    if (!is_player_minimum_af_client_version(player, 1, 4, 0)) {
        return;
    }

    size_t off = 0;
    RF_GamePacketHeader header{};
    header.type = static_cast<uint8_t>(af_packet_type::af_gungame_order);
    header.size = static_cast<uint16_t>(sizeof(uint8_t) + count * sizeof(af_gungame_order_entry));
    std::memcpy(buf + off, &header, sizeof(header));
    off += sizeof(header);
    buf[off++] = static_cast<std::byte>(count);
    if (count > 0) {
        // entries are packed (3 bytes) and the vector is contiguous.
        std::memcpy(buf + off, order.data(), count * sizeof(af_gungame_order_entry));
        off += count * sizeof(af_gungame_order_entry);
    }

    af_send_packet(player, buf, static_cast<int>(off), true); // reliable
}

void af_process_gungame_order_packet(const void* data, size_t len, const rf::NetAddr&)
{
    // Receive: client <- server
    if (!rf::is_multi || rf::is_server) {
        return;
    }
    if (len < sizeof(RF_GamePacketHeader)) {
        return;
    }

    RF_GamePacketHeader hdr{};
    std::memcpy(&hdr, data, sizeof(hdr));
    if (sizeof(RF_GamePacketHeader) + static_cast<size_t>(hdr.size) > len) {
        //xlog::warn("gungame_order: truncated (declared={}, len={})", hdr.size, len);
        return;
    }
    if (hdr.size < sizeof(uint8_t)) {
        //xlog::warn("gungame_order: missing count byte");
        return;
    }

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
    size_t off = sizeof(RF_GamePacketHeader);
    const uint8_t count = bytes[off];
    off += sizeof(uint8_t);

    // Validate the entry count against BOTH the declared payload and actual len.
    const size_t entries_bytes = static_cast<size_t>(count) * sizeof(af_gungame_order_entry);
    if (static_cast<size_t>(hdr.size) < sizeof(uint8_t) + entries_bytes) {
        //xlog::warn("gungame_order: count {} exceeds declared payload", count);
        return;
    }
    if (off + entries_bytes > len) {
        //xlog::warn("gungame_order: truncated entries (need {}, have {})", off + entries_bytes, len);
        return;
    }

    set_local_gungame_order(reinterpret_cast<const af_gungame_order_entry*>(bytes + off), count);
}

static void af_process_server_req_packet(const void* data, size_t len, const rf::NetAddr&)
{
    // Receive: client <- server
    if (!rf::is_multi || rf::is_server) {
        return;
    }

    if (len < sizeof(RF_GamePacketHeader)) {
        xlog::warn("af_process_server_req_packet: packet too short for header (len={})", len);
        return;
    }

    RF_GamePacketHeader header{};
    std::memcpy(&header, data, sizeof(header));

    if (header.type != static_cast<uint8_t>(af_packet_type::af_server_req)) {
        xlog::warn("af_process_server_req_packet: unexpected type {}", header.type);
        return;
    }

    const size_t expected_wire_size = sizeof(RF_GamePacketHeader) + static_cast<size_t>(header.size);
    if (expected_wire_size > len) {
        xlog::warn("af_process_server_req_packet: truncated packet ({} > {})", expected_wire_size, len);
        return;
    }

    if (header.size < sizeof(uint8_t)) {
        xlog::warn("af_process_server_req_packet: payload too small ({})", header.size);
        return;
    }

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);

    const size_t payload_begin = sizeof(RF_GamePacketHeader);
    const size_t payload_end = sizeof(RF_GamePacketHeader) + static_cast<size_t>(header.size);

    size_t offset = payload_begin;
    const auto req_type = static_cast<af_server_req_type>(bytes[offset++]);

    // remaining payload bytes after req_type
    const size_t remaining = (offset <= payload_end) ? (payload_end - offset) : 0;

    switch (req_type) {
        case af_server_req_type::af_sreq_should_gib: {
            if (remaining < sizeof(uint32_t)) {
                xlog::warn("af_process_server_req_packet: ShouldGib payload too short");
                return;
            }

            uint32_t obj_handle = 0;
            std::memcpy(&obj_handle, bytes + offset, sizeof(obj_handle));

            rf::Object* remote_object = rf::obj_from_remote_handle(obj_handle);
            if (!remote_object) {
                xlog::warn("af_process_server_req_packet: invalid remote handle {:x}", obj_handle);
                return;
            }

            rf::Entity* entity = rf::entity_from_handle(remote_object->handle);
            if (!entity) {
                xlog::warn("af_process_server_req_packet: invalid entity handle {:x}", obj_handle);
                return;
            }

            entity_set_gib_flag(entity);
            break;
        }
        case af_server_req_type::af_sreq_teleport_entity: {
            constexpr size_t expected = sizeof(uint32_t) + sizeof(RF_Vector) + sizeof(RF_Matrix) + sizeof(RF_Vector);
            if (remaining < expected) {
                xlog::warn("af_process_server_req_packet: TeleportEntity payload too short ({} < {})", remaining, expected);
                return;
            }

            TeleportEntityPayload payload{};
            std::memcpy(&payload.obj_handle, bytes + offset, sizeof(payload.obj_handle));
            offset += sizeof(payload.obj_handle);
            std::memcpy(&payload.pos, bytes + offset, sizeof(payload.pos));
            offset += sizeof(payload.pos);
            std::memcpy(&payload.orient, bytes + offset, sizeof(payload.orient));
            offset += sizeof(payload.orient);
            std::memcpy(&payload.vel, bytes + offset, sizeof(payload.vel));
            offset += sizeof(payload.vel);

            rf::Object* remote_object = rf::obj_from_remote_handle(payload.obj_handle);
            if (!remote_object) {
                xlog::warn("af_process_server_req_packet: TeleportEntity invalid remote handle {:x}", payload.obj_handle);
                return;
            }

            rf::Entity* entity = rf::entity_from_handle(remote_object->handle);
            if (!entity) {
                xlog::warn("af_process_server_req_packet: TeleportEntity invalid entity handle {:x}", payload.obj_handle);
                return;
            }

            rf::Vector3 new_pos;
            rf::Matrix3 new_orient;
            rf::Vector3 new_vel;
            std::memcpy(&new_pos, &payload.pos, sizeof(new_pos));
            std::memcpy(&new_orient, &payload.orient, sizeof(new_orient));
            std::memcpy(&new_vel, &payload.vel, sizeof(new_vel));

            // Snap physics state. move() updates pos, bbox, and room.
            entity->p_data.next_pos = new_pos;
            entity->move(&new_pos);
            entity->orient = new_orient;
            entity->p_data.orient = new_orient;
            entity->p_data.next_orient = new_orient;
            entity->eye_orient = new_orient;
            entity->p_data.vel = new_vel;

            // Drop the interp buffer so we don't render a slide from old pos to new pos.
            if (entity->obj_interp) {
                entity->obj_interp->Clear();
            }
            break;
        }
        case af_server_req_type::af_sreq_spray: {
            constexpr size_t expected =
                sizeof(uint8_t) + sizeof(uint16_t) + sizeof(RF_Vector) + sizeof(RF_Vector) + sizeof(uint8_t);
            if (remaining < expected) {
                xlog::warn("af_process_server_req_packet: Spray payload too short ({} < {})", remaining, expected);
                return;
            }

            uint8_t player_id = 0;
            uint16_t texture_id = 0;
            RF_Vector pos{};
            RF_Vector normal{};
            uint8_t flags = 0;
            std::memcpy(&player_id, bytes + offset, sizeof(player_id));
            offset += sizeof(player_id);
            std::memcpy(&texture_id, bytes + offset, sizeof(texture_id));
            offset += sizeof(texture_id);
            std::memcpy(&pos, bytes + offset, sizeof(pos));
            offset += sizeof(pos);
            std::memcpy(&normal, bytes + offset, sizeof(normal));
            offset += sizeof(normal);
            std::memcpy(&flags, bytes + offset, sizeof(flags));
            offset += sizeof(flags);

            rf::Vector3 spray_pos;
            rf::Vector3 spray_normal;
            std::memcpy(&spray_pos, &pos, sizeof(spray_pos));
            std::memcpy(&spray_normal, &normal, sizeof(spray_normal));

            //xlog::info("sprays: received af_sreq_spray (player_id={}, id={}, flags={:#x})", player_id, texture_id, flags);

            // Unknown/reserved flag bits are ignored.
            const bool play_sound = (flags & AF_SPRAY_FLAG_SILENT) == 0;
            sprays_apply_client_state(player_id, texture_id, spray_pos, spray_normal, play_sound);
            break;
        }
        case af_server_req_type::af_sreq_ready_prompt: {
            if (remaining < sizeof(uint8_t)) {
                xlog::warn("af_process_server_req_packet: ReadyPrompt payload too short");
                return;
            }
            const uint8_t state = bytes[offset];
            apply_ready_prompt_state(state);
            break;
        }
        case af_server_req_type::af_sreq_pit_queue_state: {
            constexpr size_t expected = 3 * sizeof(uint8_t);
            if (remaining < expected) {
                xlog::warn("af_process_server_req_packet: PitQueueState payload too short");
                return;
            }
            const uint8_t flags = bytes[offset];
            const uint8_t position = bytes[offset + 1];
            const uint8_t total = bytes[offset + 2];
            // The spectate flag (bit2) is acted on per-frame by
            // hud_pit_queue_auto_spectate (the packet can arrive before the
            // local entity finishes dying).
            apply_local_pit_queue_state(flags, position, total);
            break;
        }
        case af_server_req_type::af_sreq_kill_info: {
            constexpr size_t fixed_size = sizeof(KillInfoPayload) + sizeof(uint8_t); // + assist_count
            if (remaining < fixed_size) {
                xlog::warn("af_process_server_req_packet: KillInfo payload too short");
                return;
            }
            KillInfoPayload payload{};
            std::memcpy(&payload, bytes + offset, sizeof(payload));
            const uint8_t assist_count = bytes[offset + sizeof(payload)];
            if (remaining < fixed_size + assist_count) {
                xlog::warn("af_process_server_req_packet: KillInfo assist list truncated ({} ids)",
                    assist_count);
                return;
            }
            multi_kill_set_pending_attribution(payload,
                std::span{bytes + offset + fixed_size, assist_count});

            if (payload.flags & AF_KILL_FLAG_GIBBED) {
                rf::Player* gibbed_player = rf::multi_find_player_by_id(payload.killed_player_id);
                rf::Entity* gibbed_entity = gibbed_player
                    ? rf::entity_from_handle(gibbed_player->entity_handle) : nullptr;
                if (gibbed_entity) {
                    entity_set_gib_flag(gibbed_entity);
                }
                else {
                    xlog::debug("af_process_server_req_packet: KillInfo gib target {} unresolved",
                        payload.killed_player_id);
                }
            }
            break;
        }
        case af_server_req_type::af_sreq_vote_state: {
            VoteReader r{bytes + offset, remaining};
            const uint8_t event = r.u8();
            if (!r.ok) {
                xlog::warn("af_process_server_req_packet: VoteState payload too short");
                return;
            }

            switch (static_cast<AfVoteStateEvent>(event)) {
                case AfVoteStateEvent::Start: {
                    const uint8_t type_raw = r.u8();
                    const uint16_t time_remaining = r.u16();
                    const uint8_t yes = r.u8();
                    const uint8_t no = r.u8();
                    const uint8_t voters_left = r.u8();
                    const uint8_t flags = r.u8();
                    std::string initiator_name = r.str();
                    std::string title = r.str();
                    // `type_raw` is deliberately NOT bounds-checked: a newer
                    // server may run a vote type this build predates, and
                    // dropping the event would leave this client with no HUD, no
                    // tally and no idea a vote is running while the server still
                    // counts it as an eligible voter. Only a malformed packet is
                    // rejected.
                    if (!r.ok) {
                        xlog::warn("af_process_server_req_packet: bad VoteState start");
                        return;
                    }
                    vote_state_on_start(type_raw, time_remaining, yes, no,
                                        voters_left, (flags & AF_VOTE_STATE_FLAG_OWNER) != 0,
                                        (flags & AF_VOTE_STATE_FLAG_SYNC) != 0,
                                        std::move(initiator_name), std::move(title));
                    break;
                }
                case AfVoteStateEvent::Update: {
                    const uint8_t yes = r.u8();
                    const uint8_t no = r.u8();
                    const uint8_t voters_left = r.u8();
                    if (!r.ok) {
                        xlog::warn("af_process_server_req_packet: bad VoteState update");
                        return;
                    }
                    vote_state_on_update(yes, no, voters_left);
                    break;
                }
                case AfVoteStateEvent::End: {
                    const uint8_t result = r.u8();
                    const uint8_t flags = r.u8();
                    std::string detail = r.str();
                    if (!r.ok) {
                        xlog::warn("af_process_server_req_packet: bad VoteState end");
                        return;
                    }
                    vote_state_on_end(static_cast<AfVoteResult>(result),
                                      (flags & AF_VOTE_END_FLAG_PASSED) != 0, std::move(detail));
                    break;
                }
                default:
                    xlog::debug("af_process_server_req_packet: unknown VoteState event {}", event);
                    break;
            }
            break;
        }
        case af_server_req_type::af_sreq_vote_options_data: {
            VoteReader r{bytes + offset, remaining};
            const uint8_t event = r.u8();
            const uint32_t generation = r.u32();
            if (!r.ok) {
                xlog::warn("af_process_server_req_packet: truncated VoteOptionsData frame");
                return;
            }
            switch (static_cast<AfVoteOptionsStream>(event)) {
                case AfVoteOptionsStream::Begin: {
                    const uint32_t total_bytes = r.u32();
                    if (!r.ok) {
                        xlog::warn("af_process_server_req_packet: truncated VoteOptionsData begin");
                        return;
                    }
                    vote_options_stream_begin(generation, total_bytes);
                    break;
                }
                case AfVoteOptionsStream::Data:
                    // The chunk is whatever is left in the packet: its length comes
                    // from the packet header, not from a length byte.
                    vote_options_stream_data(generation, r.data + r.pos, r.len - r.pos);
                    break;
                case AfVoteOptionsStream::End:
                    vote_options_stream_end(generation);
                    break;
                default:
                    xlog::debug("af_process_server_req_packet: unknown VoteOptionsData event {}", event);
                    break;
            }
            break;
        }
        default:
            xlog::debug("af_process_server_req_packet: unknown req_type {}", static_cast<int>(req_type));
            break;
    }
}

static inline bool af_encode_loadout_payload_to_bytes(const std::vector<WeaponLoadoutEntry>& loadout, std::byte* out, size_t out_cap, size_t& out_written)
{
    out_written = 0;

    LoadoutEntry tmp[64];
    uint8_t count = 0;

    // collect valid and enabled loadout entries first
    for (const auto& src : loadout) {
        if (!src.enabled)
            continue;
        if (src.index < 0 || src.index >= 64)
            continue;
        if (count == 64)
            break;

        tmp[count].weapon_index = static_cast<uint8_t>(src.index);
        tmp[count].ammo = static_cast<uint32_t>(src.reserve_ammo);
        ++count;
    }

    const size_t need = sizeof(uint8_t) + static_cast<size_t>(count) * sizeof(LoadoutEntry);
    if (out_cap < need)
        return false;

    // write count
    out[0] = static_cast<std::byte>(count);

    // write entries
    if (count) {
        std::memcpy(out + sizeof(uint8_t), tmp, static_cast<size_t>(count) * sizeof(LoadoutEntry));
    }

    out_written = need;
    return true;
}

void af_send_just_spawned_loadout(rf::Player* to_player, std::vector<WeaponLoadoutEntry> loadout)
{
    if (!rf::is_server || !to_player)
        return;

    std::byte buf[rf::max_packet_size];
    if (sizeof(buf) < sizeof(RF_GamePacketHeader) + 1)
        return;

    // create header
    auto* hdr = reinterpret_cast<RF_GamePacketHeader*>(buf);
    std::byte* p = buf + sizeof(RF_GamePacketHeader);
    const std::byte* end = buf + sizeof(buf);

    // info_type
    *p++ = static_cast<std::byte>(af_just_spawned_info_type::af_loadout);

    // payload
    size_t payload_written = 0;
    if (!af_encode_loadout_payload_to_bytes(loadout, p, static_cast<size_t>(end - p), payload_written)) {
        xlog::warn("af_send_just_spawned_loadout: payload too large");
        return;
    }
    p += payload_written;

    // fill header
    hdr->type = static_cast<uint8_t>(af_packet_type::af_just_spawned_info); // 0x56
    hdr->size = static_cast<uint16_t>(1 + payload_written);

    const size_t total_len = sizeof(RF_GamePacketHeader) + hdr->size;
    if (total_len > rf::max_packet_size)
        return;

    af_send_packet(to_player, buf, static_cast<int>(total_len), true);
}

static void af_process_just_spawned_info_packet(const void* data, size_t len, const rf::NetAddr&)
{
    // Receive: client <- server
    if (!rf::is_multi || rf::is_server)
        return;
    if (len < sizeof(RF_GamePacketHeader) + 1)
        return;

    const auto* hdr = reinterpret_cast<const RF_GamePacketHeader*>(data);
    if (hdr->type != static_cast<uint8_t>(af_packet_type::af_just_spawned_info))
        return;

    const size_t bytes_after_header = hdr->size;
    if (sizeof(RF_GamePacketHeader) + bytes_after_header != len) {
        xlog::warn("af_just_spawned_info: size mismatch");
        return;
    }

    const uint8_t* p = reinterpret_cast<const uint8_t*>(data) + sizeof(RF_GamePacketHeader);
    const uint8_t* end = reinterpret_cast<const uint8_t*>(data) + len;

    if (p >= end)
        return;
    const auto info_type = static_cast<af_just_spawned_info_type>(*p++);
    const size_t payload_len = static_cast<size_t>(end - p);

    switch (info_type) {
        case af_just_spawned_info_type::af_loadout: {
            if (payload_len < 1)
                return;
            const uint8_t num = *p++;
            const size_t need = sizeof(uint8_t) + static_cast<size_t>(num) * sizeof(LoadoutEntry);
            if (payload_len != need) {
                xlog::warn("af_just_spawned_info: loadout payload_len mismatch");
                return;
            }

            const auto* entries = reinterpret_cast<const LoadoutEntry*>(p);
            if (!rf::local_player)
                return;

            for (uint8_t i = 0; i < num; ++i) {
                const int weapon_idx = static_cast<int>(entries[i].weapon_index);
                const int ammo = static_cast<int>(entries[i].ammo);

                // ignore invalid indices
                if (weapon_idx < 0 || weapon_idx >= 64) {
                    xlog::warn("af_just_spawned_info: invalid weapon index {} (ignored)", weapon_idx);
                    continue;
                }

                // add weapon locally
                rf::player_add_weapon(rf::local_player, weapon_idx, ammo);

                // if remote charge, we also need to add the detonator
                if (weapon_idx == rf::remote_charge_weapon_type && rf::local_player_entity) {
                    rf::ai_add_weapon(&rf::local_player_entity->ai, rf::remote_charge_det_weapon_type, 0);
                }
            }
        } break;

        default: // ignore unknown info types
            break;
    }
}

void af_send_koth_hill_state_packet(rf::Player* player, const HillInfo& h, const Presence& pres)
{
    // Send: server -> client
    if (!rf::is_server) {
        return;
    }

    if (!player) {
        xlog::error("af_koth_state_packet: Attempted to send to an invalid player");
        return;
    }

    af_koth_hill_state_packet pkt{};
    pkt.header.type = static_cast<uint8_t>(af_packet_type::af_koth_hill_state);
    pkt.header.size = static_cast<uint16_t>(sizeof(af_koth_hill_state_packet) - sizeof(RF_GamePacketHeader));

    pkt.hill_uid = static_cast<uint8_t>(std::clamp(h.hill_uid, 0, 255));
    pkt.ownership = static_cast<uint8_t>(h.ownership);
    pkt.steal_dir = static_cast<uint8_t>(h.steal_dir);
    pkt.state = static_cast<uint8_t>(h.state);
    pkt.lock_status = static_cast<uint8_t>(h.lock_status);
    pkt.capture_progress = h.capture_progress;
    pkt.num_red_players = static_cast<uint8_t>(std::clamp(pres.red, 0, 255));
    pkt.num_blue_players = static_cast<uint8_t>(std::clamp(pres.blue, 0, 255));
    pkt.red_score = static_cast<uint16_t>(std::clamp(g_koth_info.red_team_score, 0, 0xFFFF));
    pkt.blue_score = static_cast<uint16_t>(std::clamp(g_koth_info.blue_team_score, 0, 0xFFFF));

    std::byte buf[rf::max_packet_size];
    const size_t wire_sz = sizeof(pkt);
    if (wire_sz > rf::max_packet_size) {
        xlog::error("af_koth_state: packet too large ({}>{})", wire_sz, rf::max_packet_size);
        return;
    }
    std::memcpy(buf, &pkt, wire_sz);
    af_send_packet(player, buf, wire_sz, true);
}

void af_send_koth_hill_state_packet_to_all(const HillInfo& h, const Presence& pres)
{
    auto plist = SinglyLinkedList{rf::player_list};
    for (auto& pl : plist) {
        if (!pl.net_data)
            continue; // Skip invalid player

        af_send_koth_hill_state_packet(&pl, h, pres);
    }
}

static void af_process_koth_hill_state_packet(const void* data, size_t len, const rf::NetAddr&)
{
    // Receive: client <- server
    if (!rf::is_multi || rf::is_server)
        return;
    if (len < sizeof(RF_GamePacketHeader))
        return;

    RF_GamePacketHeader hdr{};
    std::memcpy(&hdr, data, sizeof(hdr));
    if (sizeof(RF_GamePacketHeader) + hdr.size > len) {
        xlog::warn("koth_state: truncated (declared={}, len={})", hdr.size, len);
        return;
    }

    if (len < sizeof(af_koth_hill_state_packet)) {
        xlog::warn("koth_state: short packet ({}<{})", len, sizeof(af_koth_hill_state_packet));
        return;
    }

    af_koth_hill_state_packet pkt{};
    std::memcpy(&pkt, data, sizeof(pkt));

    const size_t expected_payload = sizeof(af_koth_hill_state_packet) - sizeof(RF_GamePacketHeader);
    if (pkt.header.size != expected_payload) {
        xlog::warn("koth_state: bad payload size {} (expected {})", pkt.header.size, expected_payload);
        return;
    }

    HillInfo* h = koth_find_hill_by_uid(pkt.hill_uid);
    if (!h) {
        //xlog::warn("koth_state: unknown hill uid {}", pkt.hill_uid);
        return;
    }

    // Apply authoritative state
    h->ownership = static_cast<HillOwner>(pkt.ownership);
    h->steal_dir = static_cast<HillOwner>(pkt.steal_dir);
    h->state = static_cast<HillState>(pkt.state);
    h->lock_status = static_cast<HillLockStatus>(pkt.lock_status);
    h->capture_progress = pkt.capture_progress;
    h->capture_milli = std::clamp<int>(h->capture_progress, 0, 100) * 1000;

    // Snapshot presence for clientside prediction
    h->net_last_red = pkt.num_red_players;
    h->net_last_blue = pkt.num_blue_players;
    h->net_last_state = h->state;
    h->net_last_dir = h->steal_dir;
    h->net_last_prog_bucket = static_cast<uint8_t>(h->capture_progress / 5);
    h->net_last_lock_status = h->lock_status;
    h->client_hold_ms_accum = 0; // reset prediction accumulator

    // Scores are authoritative
    multi_koth_set_red_team_score(pkt.red_score);
    multi_koth_set_blue_team_score(pkt.blue_score);
}

// Build the wire packet once from the current bagman state. Used by both the
// single-player and broadcast send paths so we don't reconstruct per-player.
static void build_af_bagman_state_packet(af_bagman_state_packet& pkt)
{
    pkt.header.type = static_cast<uint8_t>(af_packet_type::af_bagman_state);
    pkt.header.size = static_cast<uint16_t>(sizeof(af_bagman_state_packet) - sizeof(RF_GamePacketHeader));

    pkt.carrier_player_id = (g_bagman_info.carrier && g_bagman_info.carrier->net_data)
        ? g_bagman_info.carrier->net_data->player_id
        : 0xFF;
    pkt.state = static_cast<uint8_t>(g_bagman_info.state);

    int return_left = 0;
    if (g_bagman_info.state == BagState::BS_Dropped && g_bagman_info.return_timer.valid()) {
        return_left = g_bagman_info.return_timer.time_until();
    }
    pkt.return_time_left_ms = static_cast<uint16_t>(std::clamp(return_left, 0, 0xFFFF));
    pkt.red_team_score = static_cast<uint16_t>(std::clamp(g_bagman_info.red_team_score, 0, 0xFFFF));
    pkt.blue_team_score = static_cast<uint16_t>(std::clamp(g_bagman_info.blue_team_score, 0, 0xFFFF));
    pkt.carrier_score = (g_bagman_info.carrier && g_bagman_info.carrier->stats)
        ? g_bagman_info.carrier->stats->score
        : 0;
}

void af_send_bagman_state_packet(rf::Player* player)
{
    // server -> single client
    if (!rf::is_server) {
        return;
    }
    if (!player) {
        xlog::error("af_bagman_state_packet: Attempted to send to an invalid player");
        return;
    }

    af_bagman_state_packet pkt{};
    build_af_bagman_state_packet(pkt);

    std::byte buf[sizeof(pkt)];
    std::memcpy(buf, &pkt, sizeof(pkt));
    af_send_packet(player, buf, static_cast<int>(sizeof(pkt)), true);
}

void af_send_bagman_state_packet_to_all()
{
    // server -> all clients
    if (!rf::is_server)
        return;

    af_bagman_state_packet pkt{};
    build_af_bagman_state_packet(pkt);

    std::byte buf[sizeof(pkt)];
    std::memcpy(buf, &pkt, sizeof(pkt));

    SinglyLinkedList<rf::Player> players{rf::player_list};
    for (auto& p : players) {
        if (!p.net_data)
            continue;
        af_send_packet(&p, buf, static_cast<int>(sizeof(pkt)), true);
    }
}

void af_process_bagman_state_packet(const void* data, size_t len, const rf::NetAddr&)
{
    // Receive: client <- server
    if (!rf::is_multi || rf::is_server)
        return;
    if (len < sizeof(RF_GamePacketHeader))
        return;

    RF_GamePacketHeader hdr{};
    std::memcpy(&hdr, data, sizeof(hdr));
    if (sizeof(RF_GamePacketHeader) + hdr.size > len) {
        xlog::warn("bagman_state: truncated (declared={}, len={})", hdr.size, len);
        return;
    }
    if (len < sizeof(af_bagman_state_packet)) {
        xlog::warn("bagman_state: short packet ({}<{})", len, sizeof(af_bagman_state_packet));
        return;
    }

    af_bagman_state_packet pkt{};
    std::memcpy(&pkt, data, sizeof(pkt));

    const size_t expected_payload = sizeof(af_bagman_state_packet) - sizeof(RF_GamePacketHeader);
    if (pkt.header.size != expected_payload) {
        xlog::warn("bagman_state: bad payload size {} (expected {})", pkt.header.size, expected_payload);
        return;
    }

    const BagState prev_state = g_bagman_info.state;
    g_bagman_info.state = static_cast<BagState>(pkt.state);

    if (prev_state == BagState::BS_Dropped &&
        g_bagman_info.state == BagState::BS_At_Spawn) {
        bagman_play_return_sound();
    }

    if (pkt.carrier_player_id == 0xFF) {
        g_bagman_info.carrier = nullptr;
    } else {
        g_bagman_info.carrier = rf::multi_find_player_by_id(pkt.carrier_player_id);
    }

    g_bagman_info.red_team_score = pkt.red_team_score;
    g_bagman_info.blue_team_score = pkt.blue_team_score;

    // Keep return_timer in sync so the client can render a smooth countdown
    // between packet broadcasts.
    if (g_bagman_info.state == BagState::BS_Dropped) {
        g_bagman_info.return_timer.set(pkt.return_time_left_ms);
    } else {
        g_bagman_info.return_timer.invalidate();
    }

    // Keep score in sync
    if (g_bagman_info.carrier && g_bagman_info.carrier->stats) {
        g_bagman_info.carrier->stats->score = pkt.carrier_score;
    }

    // Handle bag waypoints for bots.
    if (g_bagman_info.state == BagState::BS_Carried || g_bagman_info.state == BagState::BS_Delayed) {
        waypoints_on_bag_carried();
    } else {
        // On-ground bag: position comes from the replicated item object.
        rf::Vector3 bag_world_pos;
        if (bagman_get_client_pickup_pos(&bag_world_pos)) {
            waypoints_on_bag_world_pos(bag_world_pos);
        }
    }
}

void af_send_koth_hill_captured_packet(rf::Player* player, uint8_t hill_uid, HillOwner owner, const std::vector<uint8_t>& new_owner_player_ids)
{
    // Send: server -> client
    if (!rf::is_server) {
        return;
    }

    if (!player) {
        //xlog::error("af_koth_hill_captured_packet: Attempted to send to an invalid player");
        return;
    }

    const size_t id_count = std::min<size_t>(new_owner_player_ids.size(), 255);
    const size_t payload_size = (sizeof(af_koth_hill_captured_packet) - sizeof(RF_GamePacketHeader)) + id_count;
    const size_t wire_size = sizeof(RF_GamePacketHeader) + payload_size;

    if (wire_size > rf::max_packet_size) {
        xlog::error("af_koth_hill_captured: packet too large ({} > {})", wire_size, rf::max_packet_size);
        return;
    }

    std::byte packet_buf[rf::max_packet_size];

    af_koth_hill_captured_packet af_koth_hill_captured_packet{};
    af_koth_hill_captured_packet.header.type = static_cast<uint8_t>(af_packet_type::af_koth_hill_captured);
    af_koth_hill_captured_packet.header.size = static_cast<uint16_t>(payload_size);
    af_koth_hill_captured_packet.hill_uid = hill_uid;
    af_koth_hill_captured_packet.ownership = static_cast<uint8_t>(owner);
    af_koth_hill_captured_packet.num_new_owner_players = static_cast<uint8_t>(id_count);

    size_t off = 0;
    std::memcpy(packet_buf + off, &af_koth_hill_captured_packet, sizeof(af_koth_hill_captured_packet));
    off += sizeof(af_koth_hill_captured_packet);

    // append variable-length id array
    if (id_count) {
        std::memcpy(packet_buf + off, new_owner_player_ids.data(), id_count);
        off += id_count;
    }

    af_send_packet(player, packet_buf, off, true);
}

void af_send_koth_hill_captured_packet_to_all(uint8_t hill_uid, HillOwner owner, const std::vector<uint8_t>& new_owner_player_ids)
{
    if (rf::is_server && !rf::is_dedicated_server) {
        if (HillInfo* h = koth_find_hill_by_uid(hill_uid)) {
            koth_local_announce_hill_captured_vector(h, owner, new_owner_player_ids);
        }
    }

    SinglyLinkedList<rf::Player> linked_player_list{rf::player_list};
    for (auto& player : linked_player_list) {
        if (!&player)
            continue; // Skip invalid player

        af_send_koth_hill_captured_packet(&player, hill_uid, owner, new_owner_player_ids);
    }
}

static void af_process_koth_hill_captured_packet(const void* data, size_t len, const rf::NetAddr&)
{
    // Receive: client <- server
    if (!rf::is_multi || rf::is_server)
        return;
    if (len < sizeof(RF_GamePacketHeader))
        return;

    RF_GamePacketHeader hdr{};
    std::memcpy(&hdr, data, sizeof(hdr));
    if (sizeof(RF_GamePacketHeader) + hdr.size > len) {
        xlog::warn("koth_captured: truncated (declared={}, len={})", hdr.size, len);
        return;
    }

    if (len < sizeof(af_koth_hill_captured_packet)) {
        xlog::warn("koth_captured: short packet ({}<{})", len, sizeof(af_koth_hill_captured_packet));
        return;
    }

    af_koth_hill_captured_packet pkt{};
    std::memcpy(&pkt, data, sizeof(pkt));

    const size_t base_payload = sizeof(af_koth_hill_captured_packet) - sizeof(RF_GamePacketHeader);
    if (pkt.header.size < base_payload) {
        xlog::warn("koth_captured: bad payload size {}", pkt.header.size);
        return;
    }

    const size_t ids_len = pkt.header.size - base_payload; // number of ID bytes
    const size_t expected_wire = sizeof(RF_GamePacketHeader) + base_payload + ids_len;
    if (expected_wire > len) {
        xlog::warn("koth_captured: truncated tail ({}>{})", expected_wire, len);
        return;
    }

    HillInfo* h = koth_find_hill_by_uid(pkt.hill_uid);
    if (!h) {
        //xlog::warn("koth_captured: unknown hill uid {}", pkt.hill_uid);
        return;
    }

    const HillOwner new_owner = static_cast<HillOwner>(pkt.ownership);

    // Apply capture locally
    h->ownership = new_owner;
    h->steal_dir = HillOwner::HO_Neutral;
    h->state = HillState::HS_Idle;
    h->capture_milli = 0;
    h->capture_progress = 0;
    h->hold_ms_accum = 0;
    h->client_hold_ms_accum = 0;

    const uint8_t* ids = reinterpret_cast<const uint8_t*>(data) + sizeof(af_koth_hill_captured_packet);

    // Local announce
    koth_local_announce_hill_captured(h, new_owner, ids, ids_len);

    // Seed presence snapshot for prediction
    h->net_last_red = (new_owner == HillOwner::HO_Red) ? static_cast<uint8_t>(ids_len) : 0;
    h->net_last_blue = (new_owner == HillOwner::HO_Blue) ? static_cast<uint8_t>(ids_len) : 0;
}

void af_send_just_died_info_packet(rf::Player* to_player, bool respawn_allowed, bool force_respawn, uint16_t spawn_delay_ms)
{
    // Send: server -> client
    if (!rf::is_server) {
        return;
    }

    if (!to_player || !to_player->net_data) {
        xlog::error("af_just_died_info: Attempted to send to an invalid player");
        return;
    }

    af_just_died_info_packet pkt{};
    pkt.header.type = static_cast<uint8_t>(af_packet_type::af_just_died_info);
    pkt.header.size = static_cast<uint16_t>(sizeof(af_just_died_info_packet) - sizeof(RF_GamePacketHeader));

    uint8_t flags = 0;
    if (respawn_allowed) flags |= JDI_RESPAWN_ALLOWED;
    if (force_respawn)   flags |= JDI_FORCE_RESPAWN;

    pkt.flags = flags;
    pkt.spawn_delay = spawn_delay_ms;

    // send reliable
    std::byte buf[sizeof(pkt)];
    std::memcpy(buf, &pkt, sizeof(pkt));
    af_send_packet(to_player, buf, static_cast<int>(sizeof(pkt)), true);
}

static void af_process_just_died_info_packet(const void* data, size_t len, const rf::NetAddr&)
{
    // Receive: client <- server
    if (!rf::is_multi || rf::is_server)
        return;

    if (len < sizeof(af_just_died_info_packet)) {
        xlog::warn("just_died_info: short packet ({}<{})", len, sizeof(af_just_died_info_packet));
        return;
    }

    af_just_died_info_packet pkt{};
    std::memcpy(&pkt, data, sizeof(pkt));

    const size_t expected_payload = sizeof(af_just_died_info_packet) - sizeof(RF_GamePacketHeader);
    if (pkt.header.size != expected_payload) {
        xlog::warn("just_died_info: bad payload size {} (expected {})", pkt.header.size, expected_payload);
        return;
    }

    if (!get_af_server_info().has_value() || !get_af_server_info()->delayed_spawns) {
        xlog::warn("just_died_info: delayed spawns are not enabled in this server");
        return; // delayed spawns are disabled in this server, how did you get this packet?
    }

    const bool respawn_allowed = (pkt.flags & JDI_RESPAWN_ALLOWED) != 0;
    const bool force_respawn = (pkt.flags & JDI_FORCE_RESPAWN) != 0;
    const uint16_t spawn_delay = pkt.spawn_delay;

    //xlog::warn("just_died_info: allowed={}, force={}, delay_ms={}", respawn_allowed, force_respawn, static_cast<int>(spawn_delay));

    set_local_spawn_delay(respawn_allowed, force_respawn, static_cast<int>(spawn_delay));
}

static void build_af_server_info_packet(af_server_info_packet& pkt)
{
    pkt = {};
    pkt.header.type = static_cast<uint8_t>(af_packet_type::af_server_info);
    pkt.header.size = static_cast<uint16_t>(sizeof(af_server_info_packet) - sizeof(RF_GamePacketHeader));

    // build rf_flags
    uint32_t rf32 = 0;
    if (rf::netgame.flags & rf::NetGameFlags::NG_FLAG_WEAPON_STAY)
        rf32 |= static_cast<uint32_t>(rf_server_info_flags::RFSIF_WEAPON_STAY);
    if (rf::netgame.flags & rf::NetGameFlags::NG_FLAG_FORCE_RESPAWN)
        rf32 |= static_cast<uint32_t>(rf_server_info_flags::RFSIF_FORCE_RESPAWN);
    if (rf::netgame.flags & rf::NetGameFlags::NG_FLAG_TEAM_DAMAGE)
        rf32 |= static_cast<uint32_t>(rf_server_info_flags::RFSIF_TEAM_DAMAGE);
    if (rf::netgame.flags & rf::NetGameFlags::NG_FLAG_FALL_DAMAGE)
        rf32 |= static_cast<uint32_t>(rf_server_info_flags::RFSIF_FALL_DAMAGE);
    if (rf::netgame.flags & rf::NetGameFlags::NG_FLAG_BALANCE_TEAMS)
        rf32 |= static_cast<uint32_t>(rf_server_info_flags::RFSIF_BALANCE_TEAMS);
    pkt.rf_flags = static_cast<uint8_t>(rf32);

    // build af_flags
    uint32_t af = 0;
    if (g_alpine_server_config_active_rules.saving_enabled)
        af |= af_server_info_flags::SIF_POSITION_SAVING;
    if (g_alpine_server_config.allow_fullbright_meshes)
        af |= af_server_info_flags::SIF_ALLOW_FULLBRIGHT_MESHES;
    if (g_alpine_server_config.allow_lightmaps_only)
        af |= af_server_info_flags::SIF_ALLOW_LIGHTMAPS_ONLY;
    if (g_alpine_server_config.allow_disable_screenshake)
        af |= af_server_info_flags::SIF_ALLOW_NO_SCREENSHAKE;
    if (g_alpine_server_config_active_rules.no_player_collide)
        af |= af_server_info_flags::SIF_NO_PLAYER_COLLIDE;
    if (g_alpine_server_config.allow_disable_muzzle_flash)
        af |= af_server_info_flags::SIF_ALLOW_NO_MUZZLE_FLASH_LIGHT;
    if (g_alpine_server_config.click_limiter_config.enabled)
        af |= af_server_info_flags::SIF_CLICK_LIMITER;
    if (g_alpine_server_config.allow_unlimited_fps)
        af |= af_server_info_flags::SIF_ALLOW_UNLIMITED_FPS;
    if (g_alpine_server_config.gaussian_spread)
        af |= af_server_info_flags::SIF_GAUSSIAN_SPREAD;
    if (g_alpine_server_config_active_rules.geo_chunk_physics)
        af |= af_server_info_flags::SIF_GEO_CHUNK_PHYSICS;
    if (g_alpine_server_config_active_rules.location_pinging)
        af |= af_server_info_flags::SIF_LOCATION_PINGING;
    if (g_alpine_server_config_active_rules.spawn_delay.enabled)
        af |= af_server_info_flags::SIF_DELAYED_SPAWNS;
    if (g_alpine_server_config.allow_footsteps)
        af |= af_server_info_flags::SIF_ALLOW_FOOTSTEPS;
    if (g_alpine_server_config.allow_outlines)
        af |= af_server_info_flags::SIF_ALLOW_OUTLINES;
    if (g_alpine_server_config.allow_outlines_xray)
        af |= af_server_info_flags::SIF_ALLOW_OUTLINES_XRAY;
    if (g_alpine_server_config_active_rules.clear_stale_movement_input)
        af |= af_server_info_flags::SIF_CLEAR_STALE_MOVEMENT_INPUT;
    if (was_level_loaded_manually())
        af |= af_server_info_flags::SIF_MANUAL_LEVEL_LOAD;
    if (server_sprays_enabled())
        af |= af_server_info_flags::SIF_ALLOW_SPRAYS;
    if (g_alpine_server_config_active_rules.mutators.no_featured_reload &&
        g_alpine_server_config_active_rules.mutators.featured_weapon_index == rf::rail_gun_weapon_type)
        af |= af_server_info_flags::SIF_FEATURED_NO_CLIP;
    if (g_alpine_server_config_active_rules.mutators.reload_weapon_on_kill)
        af |= af_server_info_flags::SIF_RELOAD_ON_KILL;
    if (g_alpine_server_config.signal_cfg_changed) {
        af |= af_server_info_flags::SIF_SERVER_CFG_CHANGED;
        for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
            player.remote_server_cfg_sent = false;
        }
        g_alpine_server_config.signal_cfg_changed = false;
    }
    pkt.af_flags = af;

    // game_type
    pkt.game_type = static_cast<uint8_t>(get_upcoming_game_type());

    // build win_condition
    switch (get_upcoming_game_type()) {
        case rf::NetGameType::NG_TYPE_CTF:
            pkt.win_condition = static_cast<uint32_t>(rf::netgame.max_captures);
            break;
        case rf::NetGameType::NG_TYPE_KOTH:
            pkt.win_condition = static_cast<uint32_t>(g_alpine_server_config_active_rules.koth_score_limit);
            break;
        case rf::NetGameType::NG_TYPE_DC:
            pkt.win_condition = static_cast<uint32_t>(g_alpine_server_config_active_rules.dc_score_limit);
            break;
        case rf::NetGameType::NG_TYPE_PIT:
            pkt.win_condition = static_cast<uint32_t>(g_alpine_server_config_active_rules.pit_score_limit);
            break;
        case rf::NetGameType::NG_TYPE_GG:
            pkt.win_condition = static_cast<uint32_t>(g_alpine_server_config_active_rules.gungame_score_limit);
            break;
        case rf::NetGameType::NG_TYPE_RUN:
        case rf::NetGameType::NG_TYPE_REV:
        case rf::NetGameType::NG_TYPE_ESC:
            pkt.win_condition = static_cast<uint32_t>(0); // no wincon necessary
            break;
        default:
            pkt.win_condition = static_cast<uint32_t>(rf::netgame.max_kills);
            break;
    }

    // semi_auto_cooldown
    pkt.semi_auto_cooldown =
        static_cast<uint16_t>(std::clamp(g_alpine_server_config.click_limiter_config.cooldown, 0, 0xFFFF));
}

void af_send_server_info_packet(rf::Player* player)
{
    // Send: server -> client
    if (!rf::is_server) {
        return;
    }

    if (!player || !player->net_data) {
        xlog::error("af_server_info: Attempted to send to an invalid player");
        return;
    }

    af_server_info_packet pkt{};
    build_af_server_info_packet(pkt);

    xlog::trace("af_server_info SENDING to player '{}': af_flags=0x{:08X}", player->name, pkt.af_flags);

    std::byte buf[sizeof(pkt)];
    std::memcpy(buf, &pkt, sizeof(pkt));
    af_send_packet(player, buf, static_cast<int>(sizeof(pkt)), true);
}

// Decode af_flags and semi_auto_cooldown from a server info packet into the
// AlpineFactionServerInfo struct. Shared between the client packet handler
// and the listen-server local application path.
static void decode_af_server_info_flags(const af_server_info_packet& pkt, AlpineFactionServerInfo& server_info)
{
    server_info.saving_enabled = (pkt.af_flags & af_server_info_flags::SIF_POSITION_SAVING) != 0;
    server_info.allow_fb_mesh = (pkt.af_flags & af_server_info_flags::SIF_ALLOW_FULLBRIGHT_MESHES) != 0;
    server_info.allow_lmap = (pkt.af_flags & af_server_info_flags::SIF_ALLOW_LIGHTMAPS_ONLY) != 0;
    server_info.allow_no_ss = (pkt.af_flags & af_server_info_flags::SIF_ALLOW_NO_SCREENSHAKE) != 0;
    server_info.no_player_collide = (pkt.af_flags & af_server_info_flags::SIF_NO_PLAYER_COLLIDE) != 0;
    server_info.allow_no_mf = (pkt.af_flags & af_server_info_flags::SIF_ALLOW_NO_MUZZLE_FLASH_LIGHT) != 0;
    server_info.click_limit = (pkt.af_flags & af_server_info_flags::SIF_CLICK_LIMITER) != 0;
    server_info.semi_auto_cooldown = static_cast<int>(pkt.semi_auto_cooldown);
    server_info.unlimited_fps = (pkt.af_flags & af_server_info_flags::SIF_ALLOW_UNLIMITED_FPS) != 0;
    server_info.gaussian_spread = (pkt.af_flags & af_server_info_flags::SIF_GAUSSIAN_SPREAD) != 0;
    server_info.location_pinging = (pkt.af_flags & af_server_info_flags::SIF_LOCATION_PINGING) != 0;
    server_info.delayed_spawns = (pkt.af_flags & af_server_info_flags::SIF_DELAYED_SPAWNS) != 0;
    server_info.geo_chunk_physics = (pkt.af_flags & af_server_info_flags::SIF_GEO_CHUNK_PHYSICS) != 0;
    server_info.allow_footsteps = (pkt.af_flags & af_server_info_flags::SIF_ALLOW_FOOTSTEPS) != 0;
    server_info.allow_outlines = (pkt.af_flags & af_server_info_flags::SIF_ALLOW_OUTLINES) != 0;
    server_info.allow_outlines_xray = (pkt.af_flags & af_server_info_flags::SIF_ALLOW_OUTLINES_XRAY) != 0;
    server_info.clear_stale_movement_input = (pkt.af_flags & af_server_info_flags::SIF_CLEAR_STALE_MOVEMENT_INPUT) != 0;
    server_info.was_manual_level_load = (pkt.af_flags & af_server_info_flags::SIF_MANUAL_LEVEL_LOAD) != 0;
    server_info.allow_sprays = (pkt.af_flags & af_server_info_flags::SIF_ALLOW_SPRAYS) != 0;
    server_info.reload_on_kill = (pkt.af_flags & af_server_info_flags::SIF_RELOAD_ON_KILL) != 0;
}

// Apply af_server_info_packet flags to the local server info (for listen server host)
static void apply_server_info_packet_locally(const af_server_info_packet& pkt)
{
    auto& opt = get_af_server_info_mutable();
    if (!opt.has_value())
        return;

    decode_af_server_info_flags(pkt, opt.value());
}

// todo: on join, level init, relevant svar change, sv_loadconfig
void af_send_server_info_packet_to_all()
{
    // Send: server -> all clients
    if (!rf::is_server)
        return;

    af_server_info_packet pkt{};
    build_af_server_info_packet(pkt);

    std::byte buf[sizeof(pkt)];
    std::memcpy(buf, &pkt, sizeof(pkt));

    SinglyLinkedList<rf::Player> players{rf::player_list};
    for (auto& p : players) {
        if (!p.net_data)
            continue;
        af_send_packet(&p, buf, static_cast<int>(sizeof(pkt)), true);
    }

    // On a listen server, the local player has no net_data so the packet is never
    // sent/received via the network. Apply the flags directly to keep local state in sync.
    apply_server_info_packet_locally(pkt);
}

static void af_process_server_info_packet(const void* data, size_t len, const rf::NetAddr&)
{
    xlog::trace("af_server_info_packet RECEIVED: is_multi={}, is_server={}, len={}", rf::is_multi, rf::is_server, len);

    // Receive: client <- server
    if (!rf::is_multi || rf::is_server)
        return;

    if (len < sizeof(af_server_info_packet)) {
        xlog::warn("af_server_info: short packet ({}<{})", len, sizeof(af_server_info_packet));
        return;
    }

    af_server_info_packet pkt{};
    std::memcpy(&pkt, data, sizeof(pkt));

    const size_t expected_payload = sizeof(af_server_info_packet) - sizeof(RF_GamePacketHeader);
    if (pkt.header.size != expected_payload) {
        xlog::warn("af_server_info: bad payload size {} (expected {})", pkt.header.size, expected_payload);
        return;
    }

    if (!get_af_server_info_mutable().has_value()) {
        xlog::warn("af_server_info: missing initial server info from join");
        return; // server info is missing, how did you get this packet?
    }

    xlog::trace("af_server_info: processing af_flags=0x{:08X}", pkt.af_flags);

    auto& server_info = get_af_server_info_mutable().value();

    auto game_type = static_cast<rf::NetGameType>(pkt.game_type);

    if (game_type != rf::netgame.type) {
        set_local_pending_game_type(game_type, static_cast<int>(pkt.win_condition));
    }
    else {
        switch (game_type) {
            case rf::NetGameType::NG_TYPE_CTF:
                rf::netgame.max_captures = static_cast<int>(pkt.win_condition);
                break;
            case rf::NetGameType::NG_TYPE_KOTH:
                server_info.koth_score_limit = static_cast<int>(pkt.win_condition);
                break;
            case rf::NetGameType::NG_TYPE_DC:
                server_info.dc_score_limit = static_cast<int>(pkt.win_condition);
                break;
            case rf::NetGameType::NG_TYPE_RUN:
            case rf::NetGameType::NG_TYPE_REV:
            case rf::NetGameType::NG_TYPE_ESC:
                break; // no wincon necessary
            default:
                rf::netgame.max_kills = static_cast<int>(pkt.win_condition);
                break;
        }
    }

    // rf_flags
    if (pkt.rf_flags & rf_server_info_flags::RFSIF_WEAPON_STAY)
        rf::netgame.flags |= rf::NetGameFlags::NG_FLAG_WEAPON_STAY;
    else
        rf::netgame.flags &= ~rf::NetGameFlags::NG_FLAG_WEAPON_STAY;

    if (pkt.rf_flags & rf_server_info_flags::RFSIF_FORCE_RESPAWN)
        rf::netgame.flags |= rf::NetGameFlags::NG_FLAG_FORCE_RESPAWN;
    else
        rf::netgame.flags &= ~rf::NetGameFlags::NG_FLAG_FORCE_RESPAWN;

    if (pkt.rf_flags & rf_server_info_flags::RFSIF_TEAM_DAMAGE)
        rf::netgame.flags |= rf::NetGameFlags::NG_FLAG_TEAM_DAMAGE;
    else
        rf::netgame.flags &= ~rf::NetGameFlags::NG_FLAG_TEAM_DAMAGE;

    if (pkt.rf_flags & rf_server_info_flags::RFSIF_FALL_DAMAGE)
        rf::netgame.flags |= rf::NetGameFlags::NG_FLAG_FALL_DAMAGE;
    else
        rf::netgame.flags &= ~rf::NetGameFlags::NG_FLAG_FALL_DAMAGE;

    if (pkt.rf_flags & rf_server_info_flags::RFSIF_BALANCE_TEAMS)
        rf::netgame.flags |= rf::NetGameFlags::NG_FLAG_BALANCE_TEAMS;
    else
        rf::netgame.flags &= ~rf::NetGameFlags::NG_FLAG_BALANCE_TEAMS;

    // af_flags
    decode_af_server_info_flags(pkt, server_info);

    // Mirror the server's no-clip featured weapon (Instagib rail) locally so the
    // client's fire/reload prediction matches. Cleared (restored) when unset.
    mutators_set_no_clip_weapon((pkt.af_flags & af_server_info_flags::SIF_FEATURED_NO_CLIP)
                                    ? rf::rail_gun_weapon_type
                                    : -1);

    if ((pkt.af_flags & af_server_info_flags::SIF_SERVER_CFG_CHANGED) != 0) {
        g_remote_server_cfg_popup.set_cfg_changed();
        vote_options_mark_stale(); // votable levels / vote toggles may have changed
    }

    // Update footstep activation based on new server permissions
    evaluate_footsteps();

    //xlog::warn("af_server_info processed - gt {}, cooldown {}", pkt.game_type, server_info.semi_auto_cooldown.value());
}

constexpr int FREE_SPEC_ID = 255;

void af_send_spectate_start_packet(const rf::Player* const spectatee) {
    // Are we a client?
    if (!rf::is_multi || rf::is_server) {
        return;
    }

    af_spectate_start_packet spectate_start_packet{};
    spectate_start_packet.header.type =
        static_cast<uint8_t>(af_packet_type::af_spectate_start);
    spectate_start_packet.header.size = sizeof(spectate_start_packet)
        - sizeof(spectate_start_packet.header);

    if (!spectatee) {
        spectate_start_packet.spectatee_id = FREE_SPEC_ID;
    } else if (spectatee->net_data) {
        spectate_start_packet.spectatee_id = spectatee->net_data->player_id;
    } else {
        return;
    }

    af_send_packet(
        rf::local_player,
        &spectate_start_packet,
        sizeof(spectate_start_packet),
        true
    );
}

void af_process_spectate_start_packet(
    const void* const data,
    const size_t len,
    const rf::NetAddr& addr) {
    // Are we a server?
    if (!rf::is_server) {
        return;
    }

    rf::Player* const spectator = rf::multi_find_player_by_addr(addr);
    if (!spectator || spectator->is_bot) {
        // A bot can never enter spectate.
        return;
    }

    af_spectate_start_packet spectate_start_packet{};
    if (len < sizeof(spectate_start_packet )) {
        return;
    }

    std::memcpy(&spectate_start_packet, data, sizeof(spectate_start_packet));

    rf::Player* const new_target = rf::multi_find_player_by_id(
        spectate_start_packet.spectatee_id
    );
    if (!new_target && spectate_start_packet.spectatee_id != FREE_SPEC_ID) {
        return;
    }

    update_player_active_status(spectator);

    const bool in_spectate = spectator != new_target;
    rf::Player* const old_target = spectator->spectatee.value_or(nullptr);
    const bool target_changed = old_target != new_target;

    if (old_target && (!in_spectate || target_changed)) {
        af_send_spectate_notify_packet(old_target, spectator, false);
    }

    if (in_spectate && target_changed && new_target) {
        af_send_spectate_notify_packet(new_target, spectator, true);
    }

    spectator->spectatee = then_some(in_spectate, new_target);
    if (!spectator->is_spectator && in_spectate) {
        spectator->spectate_start_time = std::chrono::steady_clock::now();
    }
    else if (!in_spectate) {
        spectator->spectate_start_time = std::nullopt;
    }
    spectator->is_spectator = in_spectate;
}

void af_send_spectate_notify_packet(
    rf::Player* const spectatee,
    const rf::Player* const spectator,
    const bool does_spectate
) {
    if (!rf::is_server || !spectator || !spectator->net_data) {
        return;
    }

    af_spectate_notify_packet spectate_notify_packet{};
    spectate_notify_packet.header.type =
        static_cast<uint8_t>(af_packet_type::af_spectate_notify);
    spectate_notify_packet.header.size = sizeof(spectate_notify_packet)
        - sizeof(spectate_notify_packet.header);
    spectate_notify_packet.spectator_id = spectator->net_data->player_id;
    spectate_notify_packet.does_spectate = does_spectate;

    af_send_packet(
        spectatee,
        &spectate_notify_packet,
        sizeof(spectate_notify_packet),
        true
    );
}

void af_process_spectate_notify_packet(
    const void* const data,
    const size_t len,
    const rf::NetAddr&
) {
    // Are we a client?
    if (!rf::is_multi || rf::is_server) {
        return;
    }

    af_spectate_notify_packet spectate_notify_packet{};
    if (len < sizeof(spectate_notify_packet )) {
        return;
    }

    std::memcpy(&spectate_notify_packet, data, sizeof(spectate_notify_packet));

    rf::Player* const spectator = rf::multi_find_player_by_id(
        spectate_notify_packet.spectator_id
    );
    if (!spectator) {
        return;
    }

    if (spectate_notify_packet.does_spectate) {
        g_local_player_spectators.emplace(spectator);
    } else {
        g_local_player_spectators.erase(spectator);
    }

    build_local_player_spectators_strings();
}

void af_send_server_cfg(rf::Player* player) {
    // Are we a server?
    if (!rf::is_server) {
        return;
    }

    if (g_alpine_server_config.printed_cfg.empty()) {
        print_alpine_dedicated_server_config_info(
            g_alpine_server_config.printed_cfg,
            true,
            true
        );
    }

    const auto send_msg = [player] (const std::string_view msg) {
        constexpr size_t max_chunk_len = rf::max_packet_size - sizeof(af_server_msg_packet);
        const size_t len = std::clamp(msg.size(), 0uz, max_chunk_len);

        af_server_msg_packet server_msg_packet;
        server_msg_packet.header.type = static_cast<uint8_t>(af_packet_type::af_server_msg);
        server_msg_packet.header.size = static_cast<uint16_t>(
            sizeof(server_msg_packet)
                - sizeof(server_msg_packet.header)
                + len
        );
        server_msg_packet.type = static_cast<uint8_t>(AF_SERVER_MSG_TYPE_REMOTE_SERVER_CFG);

        std::array<uint8_t, sizeof(server_msg_packet) + max_chunk_len> buf{}; 
        std::memcpy(buf.data(), &server_msg_packet, sizeof(server_msg_packet));
        uint8_t* ptr = buf.data() + sizeof(server_msg_packet);
        std::memcpy(ptr, msg.data(), len);

        send_queues_rel_add_packet(
            player->net_data->reliable_socket,
            buf.data(),
            server_msg_packet.header.size + sizeof(server_msg_packet.header)
        );

        return len;
    };

    // We cannot send multiple server configs at once.
    //
    // Hazard: this clear is not scoped to config packets, so it discards
    // everything queued for this player, including an unrelated stream that
    // happens to be in flight (today that means an af_sreq_vote_options_data
    // blob, which the client recovers from by re-requesting the generation it
    // never finished receiving). Too rare to be worth solving here.
    // A future streamed feature should tag its queued packets and clear by
    // tag rather than widening this clear.
    send_queues_rel_clear_packets(player->net_data->reliable_socket);

    constexpr int chunk_size = rf::max_packet_size - sizeof(af_server_msg_packet);
    for (const auto chunk : g_alpine_server_config.printed_cfg
        | std::views::chunk(chunk_size)) {
        send_msg(std::string_view{chunk.begin(), chunk.end()});
    }

    af_server_msg_packet server_msg_packet;
    server_msg_packet.header.type = static_cast<uint8_t>(af_packet_type::af_server_msg);
    server_msg_packet.header.size = static_cast<uint16_t>(
        sizeof(server_msg_packet) - sizeof(server_msg_packet.header)
    );
    server_msg_packet.type = static_cast<uint8_t>(AF_SERVER_MSG_TYPE_REMOTE_SERVER_CFG_EOF);

    // The EOF sentinel is QUEUED, not sent with rf::multi_io_send_reliable: the
    // chunks above sit in the deferred reliable queue and are drained a few per
    // frame, so an immediate send would overtake the content it terminates and
    // the client would finalize a config that has barely started arriving. The
    // queue is FIFO per socket, so queuing puts the sentinel behind the chunks.
    // No payload follows the sentinel, so the packet struct is the whole wire
    // buffer and the chunk lambda's separate buffer is unnecessary here.
    send_queues_rel_add_packet(
        player->net_data->reliable_socket,
        reinterpret_cast<const uint8_t*>(&server_msg_packet),
        server_msg_packet.header.size + sizeof(server_msg_packet.header)
    );
}

union af_server_msg_packet_buf {
    af_server_msg_packet packet;
    std::array<uint8_t, rf::max_packet_size> buf;
};

af_server_msg_packet_buf build_automated_chat_msg_packet(
    const std::string_view msg
) {
    constexpr size_t max_len = rf::max_packet_size - sizeof(af_server_msg_packet);
    const size_t len = std::clamp(msg.size(), 0uz, max_len);

    af_server_msg_packet_buf buf{};
    buf.packet.header.type = static_cast<uint8_t>(af_packet_type::af_server_msg);
    buf.packet.header.size = static_cast<uint16_t>(
        sizeof(buf.packet)
            - sizeof(buf.packet.header)
            + len
    );
    buf.packet.type = static_cast<uint8_t>(AF_SERVER_MSG_TYPE_AUTOMATED_CHAT);
    std::memcpy(buf.packet.data, msg.data(), len);

    return buf;
}

af_server_msg_packet_buf build_hud_notification_packet(
    std::string_view text, int8_t duration_seconds,
    uint8_t notification_type, bool fade_on_expire
) {
    constexpr size_t max_len = rf::max_packet_size
        - sizeof(af_server_msg_packet)
        - sizeof(af_hud_notification_prefix);
    const size_t len = std::clamp(text.size(), 0uz, max_len);

    af_server_msg_packet_buf buf{};
    buf.packet.header.type = static_cast<uint8_t>(af_packet_type::af_server_msg);
    buf.packet.header.size = static_cast<uint16_t>(
        sizeof(buf.packet)
            - sizeof(buf.packet.header)
            + sizeof(af_hud_notification_prefix)
            + len
    );
    buf.packet.type = static_cast<uint8_t>(AF_SERVER_MSG_TYPE_HUD_NOTIFICATION);

    af_hud_notification_prefix prefix{};
    prefix.duration_seconds = duration_seconds;
    prefix.notification_type = notification_type;
    prefix.fade_on_expire = fade_on_expire ? 1 : 0;
    std::memcpy(buf.packet.data, &prefix, sizeof(prefix));
    std::memcpy(buf.packet.data + sizeof(prefix), text.data(), len);

    return buf;
}

af_server_msg_packet_buf build_server_console_msg_packet(
    const std::string_view msg
) {
    constexpr size_t max_len = rf::max_packet_size - sizeof(af_server_msg_packet);
    const size_t len = std::clamp(msg.size(), 0uz, max_len);

    af_server_msg_packet_buf buf{};
    buf.packet.header.type = static_cast<uint8_t>(af_packet_type::af_server_msg);
    buf.packet.header.size = static_cast<uint16_t>(
        sizeof(buf.packet)
            - sizeof(buf.packet.header)
            + len
    );
    buf.packet.type = static_cast<uint8_t>(AF_SERVER_MSG_TYPE_CONSOLE);
    std::memcpy(buf.packet.data, msg.data(), len);

    return buf;
}

void af_broadcast_automated_chat_msg(const std::string_view msg) {
    if (!rf::is_server) {
        return;
    }

    rf::console::print("Server: {}", msg);

    const af_server_msg_packet_buf buf = build_automated_chat_msg_packet(msg);

    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (&player == rf::local_player) {
            continue;
        }

        if (is_player_minimum_af_client_version(&player, 1, 2, 0)) {
            rf::multi_io_send_reliable(
                &player,
                &buf.packet,
                buf.packet.header.size + sizeof(buf.packet.header),
                0
            );
        } else {
            send_chat_line_packet(std::format("\xA6 {}", msg), &player);
        }
    }
}

void af_broadcast_vote_legacy_chat_msg(const std::string_view msg) {
    if (!rf::is_server) {
        return;
    }

    // Matches af_broadcast_automated_chat_msg's console line, so dedicated server
    // output is identical to what the old per-recipient loop produced.
    rf::console::print("Server: {}", msg);

    // Built once for the whole broadcast rather than per recipient.
    const af_server_msg_packet_buf buf = build_automated_chat_msg_packet(msg);
    std::optional<std::string> pre_1_2_msg;

    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (&player == rf::local_player) {
            continue; // the listen-server host is not part of the vote system
        }
        if (is_player_minimum_af_client_version(&player, 1, 4, 0)) {
            continue; // receives the structured af_sreq_vote_state events instead
        }

        if (is_player_minimum_af_client_version(&player, 1, 2, 0)) {
            rf::multi_io_send_reliable(
                &player,
                &buf.packet,
                buf.packet.header.size + sizeof(buf.packet.header),
                0
            );
        } else {
            if (!pre_1_2_msg) {
                pre_1_2_msg = std::string("\xA6 ") + std::string(msg);
            }
            send_chat_line_packet(*pre_1_2_msg, &player);
        }
    }
}

void af_send_server_console_msg(const std::string_view msg, rf::Player* player, bool tell_server) {
    if (!rf::is_server || !player) {
        return;
    }

    if (tell_server && rf::is_dedicated_server) {
        rf::console::print("Console message to {}: {}", player->name, msg);
    }

    if (!is_player_minimum_af_client_version(player, 1, 3, 0)) {
        return;
    }

    const af_server_msg_packet_buf buf = build_server_console_msg_packet(msg);

    rf::multi_io_send_reliable(
        player,
        &buf.packet,
        buf.packet.header.size + sizeof(buf.packet.header),
        0
    );
}

void af_send_automated_chat_msg(const std::string_view msg, rf::Player* player, bool tell_server) {
    if (!rf::is_server || !player) {
        return;
    }

    if (tell_server && rf::is_dedicated_server) {
        rf::console::print("Server (to {}): {}", player->name, msg);
    }

    if (is_player_minimum_af_client_version(player, 1, 2, 0)) {
        const af_server_msg_packet_buf buf = build_automated_chat_msg_packet(msg);

        rf::multi_io_send_reliable(
            player,
            &buf.packet,
            buf.packet.header.size + sizeof(buf.packet.header),
            0
        );
    } else {
        std::string legacy_msg = std::string("\xA6 ") + std::string(msg);
        send_chat_line_packet(legacy_msg, player);
    }
}

void af_broadcast_hud_notification(const std::string_view text, int duration_seconds, int notification_type, bool fade_on_expire) {
    if (!rf::is_server) {
        return;
    }

    const int8_t clamped_seconds = static_cast<int8_t>(std::clamp(duration_seconds, -1, 127));
    const af_server_msg_packet_buf buf = build_hud_notification_packet(
        text, clamped_seconds,
        static_cast<uint8_t>(notification_type), fade_on_expire);

    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (&player == rf::local_player) {
            // Listen-server host: render locally; we don't go through the network path.
            hud_notification_show(std::string{text}, clamped_seconds,
                                  static_cast<HudNotificationType>(notification_type),
                                  fade_on_expire);
            continue;
        }
        if (!is_player_minimum_af_client_version(&player, 1, 4, 0)) {
            continue;
        }
        rf::multi_io_send_reliable(
            &player,
            &buf.packet,
            buf.packet.header.size + sizeof(buf.packet.header),
            0
        );
    }
}

void af_send_hud_notification(const std::string_view text, int duration_seconds, int notification_type, bool fade_on_expire, rf::Player* player) {
    if (!rf::is_server || !player) {
        return;
    }

    const int8_t clamped_seconds = static_cast<int8_t>(std::clamp(duration_seconds, -1, 127));

    if (player == rf::local_player) {
        // Listen-server host: render locally instead of routing through the network path.
        hud_notification_show(std::string{text}, clamped_seconds,
                              static_cast<HudNotificationType>(notification_type),
                              fade_on_expire);
        return;
    }
    if (!is_player_minimum_af_client_version(player, 1, 4, 0)) {
        return;
    }

    const af_server_msg_packet_buf buf = build_hud_notification_packet(
        text, clamped_seconds,
        static_cast<uint8_t>(notification_type), fade_on_expire);

    rf::multi_io_send_reliable(
        player,
        &buf.packet,
        buf.packet.header.size + sizeof(buf.packet.header),
        0
    );
}

af_server_msg_packet_buf build_round_countdown_packet(uint8_t duration_seconds)
{
    af_server_msg_packet_buf buf{};
    buf.packet.header.type = static_cast<uint8_t>(af_packet_type::af_server_msg);
    buf.packet.header.size = static_cast<uint16_t>(
        sizeof(buf.packet)
            - sizeof(buf.packet.header)
            + sizeof(af_round_countdown_payload)
    );
    buf.packet.type = static_cast<uint8_t>(AF_SERVER_MSG_TYPE_ROUND_COUNTDOWN);

    af_round_countdown_payload payload{};
    payload.duration_seconds = duration_seconds;
    std::memcpy(buf.packet.data, &payload, sizeof(payload));

    return buf;
}

af_server_msg_packet_buf build_play_custom_sound_packet(uint16_t custom_sound_id)
{
    af_server_msg_packet_buf buf{};
    buf.packet.header.type = static_cast<uint8_t>(af_packet_type::af_server_msg);
    buf.packet.header.size = static_cast<uint16_t>(
        sizeof(buf.packet)
            - sizeof(buf.packet.header)
            + sizeof(af_play_custom_sound_payload)
    );
    buf.packet.type = static_cast<uint8_t>(AF_SERVER_MSG_TYPE_PLAY_CUSTOM_SOUND);

    af_play_custom_sound_payload payload{};
    payload.custom_sound_id = custom_sound_id;
    std::memcpy(buf.packet.data, &payload, sizeof(payload));

    return buf;
}

void af_broadcast_play_custom_sound(int custom_sound_id)
{
    if (!rf::is_server) return;

    const af_server_msg_packet_buf buf = build_play_custom_sound_packet(
        static_cast<uint16_t>(custom_sound_id));

    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (&player == rf::local_player) {
            // Listen-server host: play locally (doesn't go through the network path).
            play_local_sound_2d(static_cast<uint16_t>(get_custom_sound_id(custom_sound_id)), 0, 1.0f);
            continue;
        }
        if (!is_player_minimum_af_client_version(&player, 1, 4, 0)) continue;
        rf::multi_io_send_reliable(&player, &buf.packet,
            buf.packet.header.size + sizeof(buf.packet.header), 0);
    }
}

void af_send_play_custom_sound(int custom_sound_id, rf::Player* player)
{
    if (!rf::is_server || !player) return;
    if (player == rf::local_player) {
        play_local_sound_2d(static_cast<uint16_t>(get_custom_sound_id(custom_sound_id)), 0, 1.0f);
        return;
    }
    if (!is_player_minimum_af_client_version(player, 1, 4, 0)) return;

    const af_server_msg_packet_buf buf = build_play_custom_sound_packet(
        static_cast<uint16_t>(custom_sound_id));
    rf::multi_io_send_reliable(player, &buf.packet,
        buf.packet.header.size + sizeof(buf.packet.header), 0);
}

void af_broadcast_round_countdown(int duration_seconds)
{
    if (!rf::is_server) {
        return;
    }

    const uint8_t clamped = static_cast<uint8_t>(std::clamp(duration_seconds, 0, 10));
    const af_server_msg_packet_buf buf = build_round_countdown_packet(clamped);

    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (&player == rf::local_player) {
            // Server's listen-server host renders via the same client-side
            // hook by setting the local state directly.
            rounds_client_set_countdown(clamped);
            continue;
        }
        if (!is_player_minimum_af_client_version(&player, 1, 4, 0)) {
            continue;
        }
        rf::multi_io_send_reliable(
            &player,
            &buf.packet,
            buf.packet.header.size + sizeof(buf.packet.header),
            0
        );
    }
}

void af_process_server_msg_packet(
    const void* const data,
    const size_t len,
    const rf::NetAddr&
) {
    // Are we a client?
    if (!rf::is_multi || rf::is_server) {
        return;
    }

    af_server_msg_packet msg_packet;
    if (len < sizeof(msg_packet )) {
        return;
    }

    std::memcpy(&msg_packet, data, sizeof(msg_packet));

    if (msg_packet.type == static_cast<uint8_t>(AF_SERVER_MSG_TYPE_REMOTE_SERVER_CFG)) {
        const char* ptr = static_cast<const char*>(data) + sizeof(msg_packet);
        g_remote_server_cfg_popup.add_content(
            std::string_view{ptr, len - sizeof(msg_packet)}
        );
    } else if (msg_packet.type == static_cast<uint8_t>(AF_SERVER_MSG_TYPE_REMOTE_SERVER_CFG_EOF)) {
        g_remote_server_cfg_popup.finalize();
    } else if (msg_packet.type == static_cast<uint8_t>(AF_SERVER_MSG_TYPE_AUTOMATED_CHAT)) {
        const char* ptr = static_cast<const char*>(data) + sizeof(msg_packet);
        const rf::String msg{std::string_view{ptr, len - sizeof(msg_packet)}};
        handle_vote_or_ready_up_msg(msg);
        rf::multi_chat_print(msg, rf::ChatMsgColor::gold_white, rf::String{"Server: "});
        if (!g_alpine_game_config.simple_server_chat_msgs) {
            rf::snd_play(stock_sound_id::end_voice, 0, 0.f, 1.f);
        }
    } else if (msg_packet.type == static_cast<uint8_t>(AF_SERVER_MSG_TYPE_CONSOLE)) {
        const char* ptr = static_cast<const char*>(data) + sizeof(msg_packet);
        const std::string msg{ptr, len - sizeof(msg_packet)};
        rf::console::print("{}", msg);
    } else if (msg_packet.type == static_cast<uint8_t>(AF_SERVER_MSG_TYPE_HUD_NOTIFICATION)) {
        const size_t header_len = sizeof(msg_packet) + sizeof(af_hud_notification_prefix);
        if (len < header_len) {
            return;
        }
        af_hud_notification_prefix prefix{};
        std::memcpy(&prefix, static_cast<const char*>(data) + sizeof(msg_packet), sizeof(prefix));
        // notification_type is wire data cast to an enum; only accept the types
        // the server legitimately sends through this path.
        const auto notification_type = static_cast<HudNotificationType>(prefix.notification_type);
        if (notification_type != HudNotificationType::Round
            && notification_type != HudNotificationType::GunGame
            && notification_type != HudNotificationType::Rampage
            && notification_type != HudNotificationType::Generic
            && notification_type != HudNotificationType::GenericBig) {
            return;
        }
        const char* text_ptr = static_cast<const char*>(data) + header_len;
        const size_t text_len = len - header_len;
        std::string text{text_ptr, text_len};
        hud_notification_show(std::move(text),
                              prefix.duration_seconds,
                              static_cast<HudNotificationType>(prefix.notification_type),
                              prefix.fade_on_expire != 0);
    } else if (msg_packet.type == static_cast<uint8_t>(AF_SERVER_MSG_TYPE_ROUND_COUNTDOWN)) {
        if (len < sizeof(msg_packet) + sizeof(af_round_countdown_payload)) {
            return;
        }
        af_round_countdown_payload payload{};
        std::memcpy(&payload, static_cast<const char*>(data) + sizeof(msg_packet), sizeof(payload));
        // Re-enforce the sender's 0-10 contract on receive (sender is untrusted).
        rounds_client_set_countdown(std::clamp<int>(payload.duration_seconds, 0, 10));
    } else if (msg_packet.type == static_cast<uint8_t>(AF_SERVER_MSG_TYPE_PLAY_CUSTOM_SOUND)) {
        if (len < sizeof(msg_packet) + sizeof(af_play_custom_sound_payload)) {
            return;
        }
        af_play_custom_sound_payload payload{};
        std::memcpy(&payload, static_cast<const char*>(data) + sizeof(msg_packet), sizeof(payload));
        // reject anything that doesn't resolve to a loaded custom-sound entry
        // before it reaches the unchecked engine sound-table index.
        if (!is_valid_custom_sound_id(payload.custom_sound_id)) {
            return;
        }
        play_local_sound_2d(static_cast<uint16_t>(get_custom_sound_id(payload.custom_sound_id)), 0, 1.0f);
    }
}

// ---------------------------------------------------------------------------
// af_server_bot_control (0x5F)
// ---------------------------------------------------------------------------

static bool can_send_bot_control(rf::Player* player)
{
    return rf::is_server && player && player->net_data && player->is_bot;
}

void af_send_bot_control_simple(rf::Player* player, af_bot_control_type subtype)
{
    if (!can_send_bot_control(player)) {
        return;
    }

    std::byte buf[rf::max_packet_size];
    size_t offset = 0;

    RF_GamePacketHeader header{};
    header.type = static_cast<uint8_t>(af_packet_type::af_server_bot_control);
    header.size = 2; // version + subtype
    std::memcpy(buf + offset, &header, sizeof(header));
    offset += sizeof(header);

    buf[offset++] = static_cast<std::byte>(kBotControlPacketVersion);
    buf[offset++] = static_cast<std::byte>(subtype);

    af_send_packet(player, buf, static_cast<int>(offset), true);
}

static size_t write_profile_update_payload(
    std::byte* buf,
    size_t buf_cap,
    af_bot_control_type subtype,
    const std::string& preset_name,
    const std::vector<BotConfigOverride>& overrides)
{
    // Validate that the payload fits in the buffer.
    const size_t num_overrides_unclamped = std::min<size_t>(overrides.size(), 255);
    const size_t name_len_unclamped = std::min<size_t>(preset_name.size(), kMaxPresetNameLen);
    const size_t max_needed = sizeof(RF_GamePacketHeader) + 2 + 1 + name_len_unclamped
        + 1 + num_overrides_unclamped * (1 + sizeof(float));
    if (max_needed > buf_cap) {
        xlog::warn("write_profile_update_payload: payload size {} exceeds buffer capacity {}", max_needed, buf_cap);
        return 0;
    }

    size_t offset = 0;

    RF_GamePacketHeader header{};
    header.type = static_cast<uint8_t>(af_packet_type::af_server_bot_control);
    // size will be filled in after
    std::memcpy(buf + offset, &header, sizeof(header));
    offset += sizeof(header);

    const size_t payload_start = offset;

    buf[offset++] = static_cast<std::byte>(kBotControlPacketVersion);
    buf[offset++] = static_cast<std::byte>(subtype);

    // preset name
    const uint8_t name_len = static_cast<uint8_t>(name_len_unclamped);
    buf[offset++] = static_cast<std::byte>(name_len);
    std::memcpy(buf + offset, preset_name.data(), name_len);
    offset += name_len;

    // overrides
    const uint8_t num_overrides = static_cast<uint8_t>(num_overrides_unclamped);
    buf[offset++] = static_cast<std::byte>(num_overrides);
    for (uint8_t i = 0; i < num_overrides; ++i) {
        buf[offset++] = static_cast<std::byte>(overrides[i].field_id);
        std::memcpy(buf + offset, &overrides[i].value, sizeof(float));
        offset += sizeof(float);
    }

    // patch header size
    const uint16_t payload_size = static_cast<uint16_t>(offset - payload_start);
    std::memcpy(buf + offsetof(RF_GamePacketHeader, size), &payload_size, sizeof(payload_size));

    return offset;
}

void af_send_bot_control_update_personality(rf::Player* player, const ServerBotConfig& config)
{
    if (!can_send_bot_control(player)) {
        return;
    }

    std::byte buf[rf::max_packet_size];
    const size_t len = write_profile_update_payload(
        buf, sizeof(buf),
        af_bot_control_type::update_personality,
        config.personality_preset,
        config.personality_overrides);
    if (len == 0) {
        return;
    }

    af_send_packet(player, buf, static_cast<int>(len), true);
}

void af_send_bot_control_update_skill(rf::Player* player, const ServerBotConfig& config)
{
    if (!can_send_bot_control(player)) {
        return;
    }

    std::byte buf[rf::max_packet_size];
    const size_t len = write_profile_update_payload(
        buf, sizeof(buf),
        af_bot_control_type::update_skill,
        config.skill_preset,
        config.skill_overrides);
    if (len == 0) {
        return;
    }

    af_send_packet(player, buf, static_cast<int>(len), true);
}

void af_send_bot_control_update_identity(rf::Player* player, const std::string& name, int32_t character_index)
{
    if (!can_send_bot_control(player)) {
        return;
    }

    const uint8_t name_len = static_cast<uint8_t>(
        std::min<size_t>(name.size(), kMaxPresetNameLen));

    std::byte buf[rf::max_packet_size];
    size_t offset = 0;

    RF_GamePacketHeader header{};
    header.type = static_cast<uint8_t>(af_packet_type::af_server_bot_control);
    // size will be filled in after
    std::memcpy(buf + offset, &header, sizeof(header));
    offset += sizeof(header);

    const size_t payload_start = offset;

    buf[offset++] = static_cast<std::byte>(kBotControlPacketVersion);
    buf[offset++] = static_cast<std::byte>(af_bot_control_type::update_identity);

    // player name
    buf[offset++] = static_cast<std::byte>(name_len);
    std::memcpy(buf + offset, name.data(), name_len);
    offset += name_len;

    // character index
    std::memcpy(buf + offset, &character_index, sizeof(character_index));
    offset += sizeof(character_index);

    // patch header size
    const uint16_t payload_size = static_cast<uint16_t>(offset - payload_start);
    std::memcpy(buf + offsetof(RF_GamePacketHeader, size), &payload_size, sizeof(payload_size));

    af_send_packet(player, buf, static_cast<int>(offset), true);
}

void af_send_bot_config(rf::Player* player, const ServerBotConfig& config,
                        const std::string& resolved_name, int32_t resolved_character)
{
    if (!can_send_bot_control(player)) {
        return;
    }
    if (!is_player_minimum_af_client_version(player, 1, 3, 0)) {
        return;
    }

    af_send_bot_control_update_personality(player, config);
    af_send_bot_control_update_skill(player, config);
    af_send_bot_control_update_identity(player, resolved_name, resolved_character);
    af_send_bot_control_simple(player, af_bot_control_type::go_active);
}

void af_process_bot_control_packet(const void* data, size_t len, const rf::NetAddr&)
{
    // Receive: client <- server (bot clients only)
    if (!rf::is_multi || rf::is_server || !client_bot_launch_enabled()) {
        return;
    }

    if (len < sizeof(RF_GamePacketHeader)) {
        xlog::warn("af_process_bot_control_packet: packet too short for header (len={})", len);
        return;
    }

    RF_GamePacketHeader header{};
    std::memcpy(&header, data, sizeof(header));

    const size_t expected_wire_size = sizeof(RF_GamePacketHeader) + static_cast<size_t>(header.size);
    if (expected_wire_size > len) {
        xlog::warn("af_process_bot_control_packet: truncated packet ({} > {})", expected_wire_size, len);
        return;
    }

    if (header.size < 2) {
        xlog::warn("af_process_bot_control_packet: payload too small ({})", header.size);
        return;
    }

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
    const size_t payload_begin = sizeof(RF_GamePacketHeader);
    const size_t payload_end = sizeof(RF_GamePacketHeader) + static_cast<size_t>(header.size);

    size_t offset = payload_begin;

    const uint8_t version = bytes[offset++];
    if (version > kBotControlPacketVersion) {
        xlog::error("af_process_bot_control_packet: unsupported version {} (max {})",
            version, kBotControlPacketVersion);
        return;
    }

    const auto subtype = static_cast<af_bot_control_type>(bytes[offset++]);
    const size_t remaining = (offset <= payload_end) ? (payload_end - offset) : 0;

    switch (subtype) {
        case af_bot_control_type::go_inactive:
            xlog::info("Bot control: go_inactive");
            // Deactivate the bot: stop decision-making but preserve config.
            // The server can reactivate with go_active without resending config.
            g_client_bot_state.server_deactivated = true;
            bot_state_clear_goal();
            bot_state_clear_waypoint_route(true, true, true);
            bot_state_reset_fsm(BotFsmState::inactive);
            break;

        case af_bot_control_type::go_active:
            xlog::info("Bot control: go_active");
            g_client_bot_state.server_config_received = true;
            g_client_bot_state.server_deactivated = false;
            g_client_bot_state.server_config_timeout_timer.invalidate();
            g_client_bot_state.connection_watchdog_timer.set(kBotConnectionWatchdogMs);
            break;

        case af_bot_control_type::disconnect_bot:
            xlog::info("Bot control: disconnect_bot");
            multi_disconnect_from_server();
            break;

        case af_bot_control_type::update_personality: {
            if (remaining < 1) {
                xlog::warn("af_process_bot_control_packet: update_personality too short");
                return;
            }
            const uint8_t name_len = bytes[offset++];
            if (name_len > kMaxPresetNameLen || offset + name_len > payload_end) {
                xlog::warn("af_process_bot_control_packet: preset name too long or truncated");
                return;
            }
            std::string preset_name(reinterpret_cast<const char*>(bytes + offset), name_len);
            offset += name_len;

            if (offset >= payload_end) {
                xlog::warn("af_process_bot_control_packet: missing override count");
                return;
            }
            const uint8_t num_overrides = bytes[offset++];

            // Apply preset first
            bot_personality_set_presets(preset_name.c_str(), nullptr);
            xlog::info("Bot control: update_personality preset='{}' overrides={}", preset_name, num_overrides);

            // Apply field overrides
            for (uint8_t i = 0; i < num_overrides; ++i) {
                if (offset + 5 > payload_end) {
                    xlog::warn("af_process_bot_control_packet: override {} truncated", i);
                    break;
                }
                const uint8_t field_id = bytes[offset++];
                float value = 0.0f;
                std::memcpy(&value, bytes + offset, sizeof(float));
                offset += sizeof(float);

                if (!bot_personality_apply_field_override(field_id, value)) {
                    xlog::debug("af_process_bot_control_packet: unknown personality field_id 0x{:02x}", field_id);
                }
            }
            break;
        }

        case af_bot_control_type::update_skill: {
            if (remaining < 1) {
                xlog::warn("af_process_bot_control_packet: update_skill too short");
                return;
            }
            const uint8_t name_len = bytes[offset++];
            if (name_len > kMaxPresetNameLen || offset + name_len > payload_end) {
                xlog::warn("af_process_bot_control_packet: skill preset name too long or truncated");
                return;
            }
            std::string preset_name(reinterpret_cast<const char*>(bytes + offset), name_len);
            offset += name_len;

            if (offset >= payload_end) {
                xlog::warn("af_process_bot_control_packet: missing skill override count");
                return;
            }
            const uint8_t num_overrides = bytes[offset++];

            // Apply preset first
            bot_personality_set_presets(nullptr, preset_name.c_str());
            xlog::info("Bot control: update_skill preset='{}' overrides={}", preset_name, num_overrides);

            // Apply field overrides
            for (uint8_t i = 0; i < num_overrides; ++i) {
                if (offset + 5 > payload_end) {
                    xlog::warn("af_process_bot_control_packet: skill override {} truncated", i);
                    break;
                }
                const uint8_t field_id = bytes[offset++];
                float value = 0.0f;
                std::memcpy(&value, bytes + offset, sizeof(float));
                offset += sizeof(float);

                if (!bot_skill_apply_field_override(field_id, value)) {
                    xlog::debug("af_process_bot_control_packet: unknown skill field_id 0x{:02x}", field_id);
                }
            }
            break;
        }

        case af_bot_control_type::update_identity: {
            if (remaining < 1) {
                xlog::warn("af_process_bot_control_packet: update_identity too short");
                return;
            }
            const uint8_t name_len = bytes[offset++];
            if (name_len > kMaxPresetNameLen || offset + name_len > payload_end) {
                xlog::warn("af_process_bot_control_packet: identity name too long or truncated");
                return;
            }
            std::string new_name(reinterpret_cast<const char*>(bytes + offset), name_len);
            offset += name_len;

            if (offset + sizeof(int32_t) > payload_end) {
                xlog::warn("af_process_bot_control_packet: identity missing character index");
                return;
            }
            int32_t character_index = 0;
            std::memcpy(&character_index, bytes + offset, sizeof(character_index));
            offset += sizeof(character_index);

            xlog::info("Bot control: update_identity name='{}' character={}", new_name, character_index);

            if (rf::local_player) {
                if (!new_name.empty()) {
                    rf::local_player->name = new_name.c_str();
                }
                if (character_index >= 0 && character_index < rf::num_multi_characters) {
                    rf::local_player->settings.multi_character = character_index;
                }
                else if (character_index != -1) {
                    xlog::warn("Bot control: character_index {} out of range (0-{})",
                        character_index, rf::num_multi_characters - 1);
                }
            }
            break;
        }

        default:
            xlog::debug("af_process_bot_control_packet: unknown subtype {}", static_cast<int>(subtype));
            break;
    }
}

static uint8_t build_player_info_flags(const rf::Player& player)
{
    uint8_t flags = 0;
    if (player.is_bot)
        flags |= AF_PLAYER_FLAG_BOT;
    if (player.is_browser)
        flags |= AF_PLAYER_FLAG_BROWSER;
    if (player.is_spectator)
        flags |= AF_PLAYER_FLAG_SPECTATOR;
    if (player_is_idle(&player))
        flags |= AF_PLAYER_FLAG_IDLE;
    if (player.team == rf::TEAM_BLUE)
        flags |= AF_PLAYER_FLAG_TEAM_BLUE;
    return flags;
}

void af_send_player_info_response(const rf::NetAddr& addr)
{
    if (!rf::is_server) {
        return;
    }

    // Placeholder filename used when no level is loaded
    static constexpr const char* kNoLevelFilename = "No level loaded";

    // Rate limit: one response per IP per second to reduce UDP amplification exposure
    static constexpr size_t kMaxRecentResponses = 16384;
    static std::unordered_map<uint32_t, HighResTimer> recent_responses;

    // Sweep expired entries first
    std::erase_if(recent_responses, [](const auto& entry) {
        return entry.second.elapsed();
    });

    if (recent_responses.size() >= kMaxRecentResponses) {
        return;
    }

    auto [it, inserted] = recent_responses.try_emplace(addr.ip_addr.inner);
    if (!inserted) {
        // Already responded to this IP within the last second
        return;
    }
    it->second.set(std::chrono::seconds(1));

    static uint8_t next_response_id = 0;
    uint8_t response_id = next_response_id++;

    uint32_t time_left_seconds;
    if (rf::multi_time_limit <= 0.0f) {
        time_left_seconds = UINT32_MAX; // no time limit
    }
    else {
        float remaining = rf::multi_time_limit - rf::level.time;
        time_left_seconds = remaining > 0.0f
            ? static_cast<uint32_t>(std::min(remaining, static_cast<float>(UINT32_MAX)))
            : 0;
    }

    uint16_t red_score = 0;
    uint16_t blue_score = 0;
    if (multi_is_team_game_type()) {
        switch (rf::netgame.type) {
            case rf::NetGameType::NG_TYPE_CTF:
                red_score = static_cast<uint16_t>(rf::multi_ctf_get_red_team_score());
                blue_score = static_cast<uint16_t>(rf::multi_ctf_get_blue_team_score());
                break;
            case rf::NetGameType::NG_TYPE_TEAMDM:
                red_score = static_cast<uint16_t>(rf::multi_tdm_get_red_team_score());
                blue_score = static_cast<uint16_t>(rf::multi_tdm_get_blue_team_score());
                break;
            case rf::NetGameType::NG_TYPE_KOTH:
            case rf::NetGameType::NG_TYPE_DC:
                red_score = static_cast<uint16_t>(multi_koth_get_red_team_score());
                blue_score = static_cast<uint16_t>(multi_koth_get_blue_team_score());
                break;
            default:
                break;
        }
    }

    // Serialize the logical payload into a flat buffer:
    //   [payload_header][level_filename\0][player entry 0][player entry 1]...
    // which is then sliced into segments. The receiver reassembles by
    // response_id and parses the header + filename + entries.
    // Per player: flags (1) + score (2) + kills (2) + deaths (2) + caps (2) + name (len+1)
    // Name cannot realistically be longer than 20 characters,
    // but wire protocol allows 31, so account for it
    constexpr size_t preamble_size = sizeof(af_player_info_payload_header);
    constexpr size_t max_filename_size = RF_MAX_LEVEL_NAME_LEN + 1;
    constexpr size_t max_players_data = preamble_size + max_filename_size + 32 * (1 + 2 + 2 + 2 + 2 + 32);
    std::byte players_data[max_players_data];
    int players_data_len = 0;

    // Chunk 0 is the payload header + level filename; chunks 1..entry_count are
    // player entries. The chunker keeps chunks intact per segment, so chunk 0
    // always lands entirely in segment 0.
    int entry_offsets[34]; // preamble+filename + max 32 players + sentinel
    entry_offsets[0] = 0;
    int entry_count = 1;

    af_player_info_payload_header preamble{};
    preamble.red_score = red_score;
    preamble.blue_score = blue_score;
    preamble.time_left_seconds = time_left_seconds;
    preamble.af_flags = server_get_game_info_flags().game_info_flags_to_uint32();
    preamble.game_type = static_cast<uint8_t>(rf::netgame.type);
    std::memcpy(players_data, &preamble, preamble_size);
    players_data_len = static_cast<int>(preamble_size);

    // level filename (null-terminated), clamped to max_filename_size - 1 bytes.
    // Falls back to a placeholder when no level has been loaded yet.
    const char* filename_src = rf::level.filename.c_str();
    int filename_len = rf::level.filename.size();
    if (filename_len <= 0) {
        filename_src = kNoLevelFilename;
        filename_len = static_cast<int>(std::strlen(kNoLevelFilename));
    }
    if (filename_len >= static_cast<int>(max_filename_size)) {
        filename_len = static_cast<int>(max_filename_size) - 1;
    }
    std::memcpy(players_data + players_data_len, filename_src, filename_len);
    players_data_len += filename_len;
    players_data[players_data_len++] = std::byte{0};

    constexpr int max_players = 32;
    constexpr int max_name_len = 31; // wire protocol cap
    auto player_list = SinglyLinkedList(rf::player_list);
    for (auto& player : player_list) {
        int name_len = std::min<int>(player.name.size(), max_name_len);
        int entry_size = 1 + 2 + 2 + 2 + 2 + name_len + 1; // flags + score + kills + deaths + caps + name + null

        if (entry_count - 1 >= max_players ||
            players_data_len + entry_size > static_cast<int>(sizeof(players_data))) {
                break;
        }

        entry_offsets[entry_count++] = players_data_len;

        const auto* stats = static_cast<const PlayerStatsNew*>(player.stats);

        // flags
        players_data[players_data_len] = static_cast<std::byte>(build_player_info_flags(player));
        players_data_len += 1;

        // score
        int16_t score = stats ? stats->score : 0;
        std::memcpy(players_data + players_data_len, &score, sizeof(score));
        players_data_len += sizeof(score);

        // kills
        uint16_t kills = stats ? stats->num_kills : 0;
        std::memcpy(players_data + players_data_len, &kills, sizeof(kills));
        players_data_len += sizeof(kills);

        // deaths
        uint16_t deaths = stats ? stats->num_deaths : 0;
        std::memcpy(players_data + players_data_len, &deaths, sizeof(deaths));
        players_data_len += sizeof(deaths);

        // caps
        uint16_t caps = stats ? static_cast<uint16_t>(std::max<int16_t>(stats->caps, 0)) : 0;
        std::memcpy(players_data + players_data_len, &caps, sizeof(caps));
        players_data_len += sizeof(caps);

        // name (null-terminated; clamped to max_name_len)
        std::memcpy(players_data + players_data_len, player.name.c_str(), name_len);
        players_data_len += name_len;
        players_data[players_data_len++] = std::byte{0};
    }
    entry_offsets[entry_count] = players_data_len; // sentinel

    // Compute segment boundaries (each segment fits within max_packet_size)
    constexpr int header_size = static_cast<int>(sizeof(af_player_info_packet));
    constexpr int max_payload = static_cast<int>(rf::max_packet_size) - header_size;

    int seg_boundaries[max_players + 2]; // preamble + entries + sentinel
    int total_segments = 0;
    {
        int seg_start = 0;
        while (seg_start < entry_count) {
            seg_boundaries[total_segments++] = seg_start;
            int seg_end = seg_start;
            while (seg_end < entry_count &&
                   entry_offsets[seg_end + 1] - entry_offsets[seg_start] <= max_payload) {
                ++seg_end;
            }
            if (seg_end == seg_start) {
                // Single chunk too large (should never happen)
                ++seg_end;
            }
            seg_start = seg_end;
        }
    }
    seg_boundaries[total_segments] = entry_count; // sentinel

    // Verify all segments fit before sending anything. Chunk-size invariants
    // (preamble+filename <= ~273 B, per-entry <= 41 B, max_payload ~= 505 B)
    // guarantee this in practice, but a silent partial send would leave the
    // receiver waiting on a missing segment forever.
    for (int seg = 0; seg < total_segments; ++seg) {
        int payload_len = entry_offsets[seg_boundaries[seg + 1]] - entry_offsets[seg_boundaries[seg]];
        if (payload_len > max_payload) {
            xlog::error("af_send_player_info_response: segment {} payload {} > max {}; aborting response",
                seg, payload_len, max_payload);
            return;
        }
    }

    // Build and send each segment
    std::byte packet_buf[rf::max_packet_size];
    for (int seg = 0; seg < total_segments; ++seg) {
        int first_entry = seg_boundaries[seg];
        int end_entry = seg_boundaries[seg + 1];

        int payload_offset = entry_offsets[first_entry];
        int payload_len = entry_offsets[end_entry] - payload_offset;

        af_player_info_packet hdr{};
        hdr.hdr.type = static_cast<uint8_t>(pf_packet_type::players);
        hdr.hdr.size = static_cast<uint16_t>(payload_len + header_size - sizeof(hdr.hdr));
        hdr.version = af_player_info_packet_version;
        hdr.response_id = response_id;
        hdr.sequence = static_cast<uint8_t>(seg);
        hdr.total_segments = static_cast<uint8_t>(total_segments);

        std::memcpy(packet_buf, &hdr, header_size);
        if (payload_len > 0) {
            std::memcpy(packet_buf + header_size, players_data + payload_offset, payload_len);
        }

        // cannot use af_send_packet because packet is sent to
        // online rfsb, not a connected client
        rf::net_send(addr, packet_buf, header_size + payload_len);
    }

    //xlog::trace("af_send_player_info_response: sent {} segments, {} players", total_segments, entry_count);
}
