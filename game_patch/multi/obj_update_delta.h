#pragma once

// Acked-baseline delta compression of the server -> client obj_update stream (af_obj_update_delta
// 0x65 / af_obj_update_ack 0x66). Wire format and rules: research/functions-deep/obj_update_packet.md.
// Kept free of game headers so rf/player/player.h can embed Sender by value.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace rf
{
    struct Entity;
    struct NetAddr;
    struct Player;
}

namespace obj_update_delta
{
    constexpr int max_age = 32;   // baselines older than the ack bitfield are unprovable
    constexpr int ring_size = 64; // > max_age so a stale slot never aliases a live seq

    // Client-visible state of one entity as of one packet. Groups a record did not carry are
    // inherited from its baseline, so an entry is always complete.
    struct Entry
    {
        int handle = 0;
        uint8_t groups = 0; // OUF bits ever populated (0x01 pos, 0x80 powerup, 0x04 weapon, 0x20 health)
        uint16_t tick = 0;
        float pos[3] = {};
        int16_t pitch = 0;
        int16_t yaw = 0;
        uint8_t state = 0;
        uint8_t vel[3] = {};
        uint8_t powerup = 0;
        uint8_t weapon = 0;
        uint8_t health = 0;
        uint8_t armor = 0;
        uint8_t hitdir = 0;
        bool has_ammo = false;
        uint8_t ammo_type = 0;
        uint16_t clip = 0;
        uint16_t reserve = 0;
    };

    struct Snapshot
    {
        uint16_t seq = 0;
        bool valid = false;
        bool acked = false;
        std::vector<Entry> entries; // in packet order; delta records index into this
    };

    struct Ring
    {
        std::array<Snapshot, ring_size> slots{};
        Snapshot* find(uint16_t seq);
        void store(uint16_t seq, std::vector<Entry>&& entries);
        void clear();
    };

    // Server-side, per recipient
    struct Sender
    {
        Ring ring;
        uint16_t next_seq = 0;
        std::vector<Entry> pending; // records already copied into the packet being built
        std::optional<Entry> last;  // record packed but not yet copied (the loop may flush first)
    };

    // Server: called from pack_obj_update_data_hook with the stock record; rewrites it in place
    // into a delta record and returns the new length.
    int server_pack(rf::Player* pp, rf::Entity* ep, uint8_t* rec, int len);
    // Server: replaces multi_io_send of a finished stock packet. `final` distinguishes the
    // post-loop send (the last packed record is in the buffer) from a mid-loop flush (it is not).
    void server_send(rf::Player* pp, const uint8_t* packet, int len, bool final);
    void server_on_ack(rf::Player* pp, uint16_t newest_seq, uint32_t bits);

    // Client
    void client_receive(const uint8_t* payload, size_t len, const rf::NetAddr& addr);
    bool client_build_ack(uint16_t& newest_seq, uint32_t& bits);
    void client_reset();

    void on_level_init(); // both sides: handles are reused across levels
    void selfcheck();     // codec round trips; asserts
}
