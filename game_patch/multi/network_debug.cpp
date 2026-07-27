#include <cstdint>
#include <patch_common/CodeInjection.h>
#include <xlog/xlog.h>
#include <common/rfproto.h>
#include <common/utils/list-utils.h>
#include "network.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/os/console.h"
#include "../os/os.h"

// -----------------------------------------------------------------------------
// Reliable-socket drop instrumentation, experimental debug only.
// This logging is intended to be used to better diagnose and eventually fix
// long-standing netcode issues, such as occasionally items not being picked up.
//
// RF's reliable-UDP layer (nw_rel_send @ 0x0052A310) has two failure paths that
// drop a "reliable" packet with no visible log:
//   1. All 75 in-flight send slots are occupied (window saturated) -> the socket
//      is forced to TIMED_OUT (status 2) and the packet is dropped. Stock code
//      logs nothing here at all. The socket is later reaped by the connection
//      health check, disconnecting that client.
//   2. A send is attempted while the socket is not CONNECTED (e.g. CONNECTING /
//      limbo). The packet is dropped, never queued, never retransmitted, and no
//      disconnect follows - a genuinely silent reliable-packet loss (the stock
//      "Can't send packet because of state" string is formatted into a throwaway
//      buffer and discarded).
// Both manifest as unexplained desyncs (e.g. a missed item pickup) rather than an
// obvious error, so surface them here. Warnings are throttled to at most one per
// socket per second per site to avoid flooding the log during a burst.
//
// These hooks are installed only when the process is launched with `-debug`
// -----------------------------------------------------------------------------

static rf::Player* find_player_by_reliable_socket(unsigned socket_id)
{
    for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (player.net_data && player.net_data->reliable_socket == socket_id) {
            return &player;
        }
    }
    return nullptr;
}

static const char* reliable_socket_status_name(int status)
{
    switch (status) {
    case 0: return "EMPTY";
    case 1: return "CONNECTED";
    case 2: return "TIMED_OUT";
    case 3: return "UNK_3";
    case 4: return "CONNECTED_REMOTE";
    case 5: return "CONNECTING";
    default: return "?";
    }
}

// Throttle: at most one log per socket per second (each site passes its own array).
// Uses AF's 64-bit monotonic timer (timer::get_i64) rather than the engine's 32-bit
// timer::get: the 32-bit value deliberately truncates get_i64 and rolls over ~every
// 24.8 days of uptime, whereas the 64-bit ms count effectively never wraps, so the
// elapsed subtraction below is always correct without any wrap-handling.
static bool reliable_drop_should_log(int socket_id, int64_t* last_warn_ms)
{
    if (socket_id < 0 || socket_id >= rf::NET_MAX_REL_SOCKETS) {
        return true;
    }
    int64_t now = timer::get_i64(1000);
    if (last_warn_ms[socket_id] == 0 || now - last_warn_ms[socket_id] >= 1000) {
        last_warn_ms[socket_id] = now;
        return true;
    }
    return false;
}

// In nw_rel_send the outgoing packet pointer is param_2, which the stock prologue
// reads from [esp+0x374] (param_1 from [esp+0x370]); esp is unchanged at all three
// injection sites below. The first payload byte is the RF packet type. Returns -1 if
// the pointer is null (should not happen for a real send, but guard regardless).
static int reliable_send_packet_type(uintptr_t esp)
{
    const uint8_t* const* data_slot = reinterpret_cast<const uint8_t* const*>(esp + 0x374);
    const uint8_t* data = *data_slot;
    return data ? static_cast<int>(data[0]) : -1;
}

// Names for the reliable server->client packets most likely to show up in a drop.
// Unlisted types fall back to the raw hex code in the log message.
static const char* reliable_packet_type_name(int type)
{
    switch (type) {
    case RF_GPT_TRIGGER_ACTIVATE:      return "trigger_activate";
    case RF_GPT_STATE_INFO_DONE:       return "state_info_done";
    case RF_GPT_PREGAME_BOOLEAN:       return "pregame_boolean";
    case RF_GPT_PREGAME_GLASS:         return "pregame_glass";
    case RF_GPT_PREGAME_REMOTE_CHARGE: return "pregame_remote_charge";
    case 0x27:                         return "obj_kill"; // no rfproto constant
    case RF_GPT_ITEM_APPLY:            return "item_apply";
    case RF_GPT_BOOLEAN:               return "boolean";
    case RF_GPT_RESPAWN:               return "respawn";
    case RF_GPT_ENTITY_CREATE:         return "entity_create";
    case RF_GPT_ITEM_CREATE:           return "item_create";
    case RF_GPT_RELOAD:                return "reload";
    case RF_GPT_SOUND:                 return "sound";
    case RF_GPT_GLASS_KILL:            return "glass_kill";
    default:                           return "?";
    }
}

// nw_rel_send: no free send slot -> window saturated, status about to be set to
// TIMED_OUT. EBX = &net_rel_sockets[id]; fires just before `mov [ebx+0x67a], 2`.
static CodeInjection net_rel_send_window_saturated_injection{
    0x0052A3A7,
    [](auto& regs) {
        static int64_t last_warn_ms[rf::NET_MAX_REL_SOCKETS] = {};
        rf::NetReliableSocket* sock = regs.ebx;
        int id = static_cast<int>(sock - rf::net_rel_sockets);
        if (!reliable_drop_should_log(id, last_warn_ms)) {
            return;
        }
        rf::Player* pp = find_player_by_reliable_socket(static_cast<unsigned>(id));
        const char* who = pp ? pp->name.c_str() : "no player";
        int pkt = reliable_send_packet_type(static_cast<uintptr_t>(regs.esp));
        xlog::warn("Reliable socket {} ({}) send window SATURATED (75 unACK'd) - DROPPED {} packet "
                   "(0x{:02X}); socket will be reaped and the client disconnected (AF reserves 32 "
                   "slots, so this means stock reliable traffic overwhelmed the window)",
                   id, who, reliable_packet_type_name(pkt), pkt);
    },
};

// nw_rel_send: send attempted while socket status != CONNECTED. Packet is dropped
// with no retransmit and no disconnect. EAX = socket id, ECX = current status.
// CONNECTING/EMPTY are expected during joins and level transitions (kept at debug);
// TIMED_OUT/UNK_3 at send time is a genuine silent loss on a live socket (warn).
// Separate throttles so an expected debug event never suppresses a real warn.
static CodeInjection net_rel_send_not_connected_injection{
    0x0052A373,
    [](auto& regs) {
        static int64_t last_debug_ms[rf::NET_MAX_REL_SOCKETS] = {};
        static int64_t last_warn_ms[rf::NET_MAX_REL_SOCKETS] = {};
        int id = static_cast<int>(regs.eax);
        int status = static_cast<int>(regs.ecx);
        bool expected = (status == 5 /*CONNECTING*/ || status == 0 /*EMPTY*/);
        if (!reliable_drop_should_log(id, expected ? last_debug_ms : last_warn_ms)) {
            return;
        }
        rf::Player* pp = find_player_by_reliable_socket(static_cast<unsigned>(id));
        const char* who = pp ? pp->name.c_str() : "no player";
        int pkt = reliable_send_packet_type(static_cast<uintptr_t>(regs.esp));
        if (expected) {
            xlog::debug("Reliable send skipped: socket {} ({}) {} packet (0x{:02X}) status {} ({}) - not yet connected",
                        id, who, reliable_packet_type_name(pkt), pkt, status, reliable_socket_status_name(status));
        }
        else {
            xlog::warn("Reliable send DROPPED (no retransmit): socket {} ({}) {} packet (0x{:02X}) status {} ({}) - not CONNECTED",
                       id, who, reliable_packet_type_name(pkt), pkt, status, reliable_socket_status_name(status));
        }
    },
};

// nw_rel_send: a free send slot was found and a packet is about to be queued.
// Count how many of the 75 slots are already occupied (unACK'd) and warn when the
// window is filling, so a socket trending toward a saturation-disconnect is visible
// BEFORE it actually dies. EBX = &net_rel_sockets[id]; the slot for this send is
// still null here, so in-flight after this send is (occupied + 1).
static CodeInjection net_rel_send_window_high_water_injection{
    0x0052A3BC,
    [](auto& regs) {
        static int64_t last_warn_ms[rf::NET_MAX_REL_SOCKETS] = {};
        constexpr int high_water = 60; // of 75 in-flight
        rf::NetReliableSocket* sock = regs.ebx;
        int in_flight = 1; // counting the packet about to be queued
        for (void* slot : sock->sbuffers) {
            if (slot) {
                ++in_flight;
            }
        }
        if (in_flight < high_water) {
            return;
        }
        int id = static_cast<int>(sock - rf::net_rel_sockets);
        if (!reliable_drop_should_log(id, last_warn_ms)) {
            return;
        }
        rf::Player* pp = find_player_by_reliable_socket(static_cast<unsigned>(id));
        const char* who = pp ? pp->name.c_str() : "no player";
        int pkt = reliable_send_packet_type(static_cast<uintptr_t>(regs.esp));
        xlog::warn("Reliable socket {} ({}) send window filling: {}/75 in-flight (unACK'd) - queuing {} "
                   "packet (0x{:02X}), approaching saturation",
                   id, who, in_flight, reliable_packet_type_name(pkt), pkt);
    },
};

// in game_boot_do_frame / rf_init
CodeInjection net_debug_startup_notice_injection{
    0x004B253D,
    [](auto&) {
        rf::console::print("-- Network debug logging ENABLED (-debug) --");
        xlog::info("Network debug logging ENABLED via -debug switch");
    },
};

void network_debug_apply_patches()
{
    // Only install debug hooks when launched with -debug.
    if (!raw_command_line_has_switch(L"-debug")) {
        return;
    }
    net_rel_send_window_saturated_injection.install();
    net_rel_send_not_connected_injection.install();
    net_rel_send_window_high_water_injection.install();
    net_debug_startup_notice_injection.install();
}
