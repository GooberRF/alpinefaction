#include <cstddef>
#include <cstring>
#include <cassert>
#include <array>
#include <ranges>
#include <common/utils/list-utils.h>
#include <common/rfproto.h>
#include <xlog/xlog.h>
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/weapon.h"
#include "multi.h"
#include "network.h"
#include "server_internal.h"
#include "server.h"
#include "../hud/hud_world.h"
#include "alpine_packets.h"
#include "../misc/player.h"
#include "../hud/hud.h"
#include "../sound/sound.h"
#include "../misc/alpine_settings.h"

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
        rf::multi_io_send_buffered_reliable_packets(player);
        rf::multi_io_send_reliable(player, data, len, 0);
        rf::multi_io_send_buffered_reliable_packets(player);
    }
    else {
        //rf::net_send(player->net_data->addr, data, len);
        rf::multi_io_send(player, data, len);
    }
}

bool af_process_packet(const void* data, int len, const rf::NetAddr& addr, rf::Player* player)
{
    RF_GamePacketHeader header{};
    if (len < static_cast<int>(sizeof(header))) {
        return false;
    }

    std::memcpy(&header, data, sizeof(header));
    auto packet_type = static_cast<af_packet_type>(header.type);

    if (!packet_check_whitelist(static_cast<int>(packet_type))) {
        xlog::warn("Ignoring packet 0x{:x}", static_cast<int>(packet_type));
        return false;
    }
    else {
        xlog::trace("Processing packet 0x{:x}", static_cast<int>(packet_type));
    }

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
    damage_notify_packet.damage = static_cast<uint16_t>(std::max(1, rounded_damage)); // round damage with minimum 1

    damage_notify_packet.flags = 0; // init flags
    damage_notify_packet.flags = (damage_notify_packet.flags & ~0x01) | (died << 0);

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

    std::vector<af_obj_update> obj_updates;
    auto player_list = SinglyLinkedList{rf::player_list};

    // loop through players to gather info
    for (auto& other_player : player_list) {
        //xlog::info("starting payer list loop");
        if (!&other_player) {
            continue; // player not valid
        }

        if (&other_player == player) {
            continue; // player is myself
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
        obj_updates.push_back(obj_update);
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

    // Allocate memory dynamically for the packet
    auto packet_buf = std::make_unique<std::byte[]>(total_packet_size);
    if (!packet_buf) {
        return; // could not allocate memory
    }

    // Fill packet header
    RF_GamePacketHeader header{};
    header.type = static_cast<uint8_t>(af_packet_type::af_obj_update);
    header.size = static_cast<uint16_t>(object_data_size);

    // Copy data to packet buffer
    std::memcpy(packet_buf.get(), &header, sizeof(header));
    if (!obj_updates.empty()) {
        std::memcpy(packet_buf.get() + sizeof(header), obj_updates.data(), object_data_size);
    }

    if (!player) {
        xlog::error("af_obj_update: Attempted to send to an invalid player");
        return;
    }
    af_send_packet(player, packet_buf.get(), total_packet_size, false);
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

void serialize_payload(const HandicapPayload& payload, std::byte* buf, size_t& offset)
{
    buf[offset++] = static_cast<std::byte>(payload.amount);
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

void serialize_payload(
    const std::monostate& payload,
    const std::byte* const buf,
    const size_t& offset
) {
}

// send client request packet
void af_send_client_req_packet(const af_client_req_packet& packet)
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
    af_send_packet(rf::local_player, buf, total_len, false);
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

    const auto* header = static_cast<const RF_GamePacketHeader*>(data);
    if (header->type != static_cast<uint8_t>(af_packet_type::af_client_req)) {
        xlog::warn("af_process_client_req_packet: unexpected type {}", header->type);
        return;
    }

    const size_t expected_wire_size = sizeof(RF_GamePacketHeader) + header->size;
    if (expected_wire_size > len) {
        xlog::warn("af_process_client_req_packet: truncated packet ({} > {})", expected_wire_size, len);
        return;
    }

    if (header->size < sizeof(uint8_t)) {
        xlog::warn("af_process_client_req_packet: payload too small ({})", header->size);
        return;
    }

    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
    size_t offset = sizeof(RF_GamePacketHeader); // skip header

    rf::Player* player = rf::multi_find_player_by_addr(addr);

    if (!player || !player->net_data) {
        xlog::warn("af_process_client_req_packet: no valid player for addr");
        return;
    }

    af_client_req_type req_type = static_cast<af_client_req_type>(bytes[offset++]);
    af_client_req_packet packet;
    std::memcpy(&packet.header, data, sizeof(RF_GamePacketHeader));
    packet.req_type = req_type;

    switch (req_type) {
        case af_client_req_type::af_req_handicap: {
            if (offset + 1 > len) {
                xlog::warn("af_process_client_req_packet: Handicap payload too short");
                return;
            }
            uint8_t amount = bytes[offset];
            packet.payload = HandicapPayload{amount};

            handle_player_set_handicap(player, amount);
            break;
        }
        case af_client_req_type::af_req_server_cfg: {
            auto& pdata = get_player_additional_data(player);
            if (!pdata.remote_server_cfg_sent) {
                af_send_server_cfg(player);
                pdata.remote_server_cfg_sent = true;
            }
            break;
        }
        default: {
            xlog::warn("af_process_client_req_packet: Unknown req_type {}", static_cast<int>(req_type));
            return;
        }
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
    if (g_alpine_server_config_active_rules.location_pinging)
        af |= af_server_info_flags::SIF_LOCATION_PINGING;
    if (g_alpine_server_config_active_rules.spawn_delay.enabled)
        af |= af_server_info_flags::SIF_DELAYED_SPAWNS;
    if (g_alpine_server_config.signal_cfg_changed) {
        af |= af_server_info_flags::SIF_SERVER_CFG_CHANGED;
        for (const rf::Player& player : SinglyLinkedList{rf::player_list}) {
            auto& pdata = get_player_additional_data(&player);
            pdata.remote_server_cfg_sent = false;
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

    std::byte buf[sizeof(pkt)];
    std::memcpy(buf, &pkt, sizeof(pkt));
    af_send_packet(player, buf, static_cast<int>(sizeof(pkt)), true);
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
        if (!&p || !p.net_data)
            continue;
        af_send_packet(&p, buf, static_cast<int>(sizeof(pkt)), true);
    }
}

static void af_process_server_info_packet(const void* data, size_t len, const rf::NetAddr&)
{
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
    server_info.saving_enabled = (pkt.af_flags & af_server_info_flags::SIF_POSITION_SAVING) != 0;
    server_info.allow_fb_mesh = (pkt.af_flags & af_server_info_flags::SIF_ALLOW_FULLBRIGHT_MESHES) != 0;
    server_info.allow_lmap = (pkt.af_flags & af_server_info_flags::SIF_ALLOW_LIGHTMAPS_ONLY) != 0;
    server_info.allow_no_ss = (pkt.af_flags & af_server_info_flags::SIF_ALLOW_NO_SCREENSHAKE) != 0;
    server_info.no_player_collide = (pkt.af_flags & af_server_info_flags::SIF_NO_PLAYER_COLLIDE) != 0;
    server_info.allow_no_mf = (pkt.af_flags & af_server_info_flags::SIF_ALLOW_NO_MUZZLE_FLASH_LIGHT) != 0;
    server_info.click_limit = (pkt.af_flags & af_server_info_flags::SIF_CLICK_LIMITER) != 0;
    server_info.unlimited_fps = (pkt.af_flags & af_server_info_flags::SIF_ALLOW_UNLIMITED_FPS) != 0;
    server_info.gaussian_spread = (pkt.af_flags & af_server_info_flags::SIF_GAUSSIAN_SPREAD) != 0;
    server_info.location_pinging = (pkt.af_flags & af_server_info_flags::SIF_LOCATION_PINGING) != 0;
    server_info.delayed_spawns = (pkt.af_flags & af_server_info_flags::SIF_DELAYED_SPAWNS) != 0;

    if ((pkt.af_flags & af_server_info_flags::SIF_SERVER_CFG_CHANGED) != 0) {
        g_remote_server_cfg_popup.set_cfg_changed();
    }

    server_info.semi_auto_cooldown = static_cast<int>(pkt.semi_auto_cooldown);

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

    const rf::Player* const spectator = rf::multi_find_player_by_addr(addr);
    if (!spectator) {
        return;
    }

    af_spectate_start_packet spectate_start_packet{};
    if (len < sizeof(spectate_start_packet )) {
        return;
    }

    std::memcpy(&spectate_start_packet, data, sizeof(spectate_start_packet));

    rf::Player* const spectatee = rf::multi_find_player_by_id(
        spectate_start_packet.spectatee_id
    );
    if (!spectatee && spectate_start_packet.spectatee_id != FREE_SPEC_ID) {
        return;
    }

    update_player_active_status(spectator);

    auto& pdata = get_player_additional_data(spectator);
    const bool exited_spectate = spectatee == spectator;
    if (exited_spectate) {
        if (pdata.spectatee) {
            af_send_spectate_notify_packet(pdata.spectatee.value(), spectator, false);
            pdata.spectatee.reset();
        }
    } else {
        if (pdata.spectatee && pdata.spectatee.value() == spectatee) {
            return;
        }
        if (pdata.spectatee) {
            af_send_spectate_notify_packet(pdata.spectatee.value(), spectator, false);
        }
        af_send_spectate_notify_packet(spectatee, spectator, true);
        pdata.spectatee.emplace(spectatee);
    }
}

void af_send_spectate_notify_packet(
    rf::Player* const spectatee,
    const rf::Player* const spectator,
    const bool does_spectate
) {
    // Are we a server?
    if (!rf::is_server) {
        return;
    }

    if (!spectator || !spectator->net_data) {
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

    rf::multi_io_send_reliable(
        player,
        &server_msg_packet,
        server_msg_packet.header.size + sizeof(server_msg_packet.header),
        0
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

        if (is_player_minimum_af_client_version(&player, 1, 2)) {
            rf::multi_io_send_reliable(
                &player,
                &buf.packet,
                buf.packet.header.size + sizeof(buf.packet.header),
                0
            );
        } else {
            std::string legacy_msg = std::string("\xA6 ") + std::string(msg);
            send_chat_line_packet(legacy_msg, &player);
        }
    }
}

void af_send_automated_chat_msg(const std::string_view msg, rf::Player* player, bool tell_server) {
    if (!rf::is_server || !player) {
        return;
    }

    if (tell_server && rf::is_dedicated_server) {
        rf::console::print("Server (to {}): {}", player->name, msg);
    }

    if (is_player_minimum_af_client_version(player, 1, 2)) {
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
            rf::snd_play(4, 0, 0.f, 1.f);
        }
    }
}
