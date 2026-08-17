#pragma once

// Internal wiring between the demo module translation units.

namespace rf
{
    struct NetAddr;
}

void demo_record_do_patch();
void demo_playback_do_patch();
void demo_powerup_timers_do_patch();

// Clears the powerup respawn tracker. Called from playback's reset_ctx - the single
// funnel for teardown, engine multi_stop and backward-seek session restarts.
void demo_powerup_timers_reset();

// Fabricates an already-CONNECTED entry in rf::net_rel_sockets bound to addr and
// returns its slot id (-1 when no slot is free). Used by the recorder's virtual player
// (stock senders like send_boolean_packet pre-check reliable_socket != -1 before the
// tapped funnels) and by playback (in place of the blocking connect handshake). The
// owner must keep last_packet_received fresh so the slot never reads as timed out.
int demo_alloc_fake_reliable_socket(const rf::NetAddr& addr);
