#pragma once

// Forward delcarations
namespace rf
{
    struct Player;
    struct NetAddr;
}

bool pf_process_packet(const void* data, int len, const rf::NetAddr& addr);
bool pf_process_raw_unreliable_packet(const void* data, int len, const rf::NetAddr& addr);
void send_pf_player_stats_packet(rf::Player* player);
