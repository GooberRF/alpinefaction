#pragma once

// Measured obj_update flow on the client, from cl_netmeter sampling in netmeter.cpp.
// Rates are packets per second; jitter is the mean deviation of inter-packet time in ms.
struct NetMeterStats
{
    float in_rate;
    float in_jitter;
    float in_max_gap;  // largest inter-arrival time in the window, ms
    float in_loss;     // estimated % of expected packets missing
    float in_stale;    // peak ms since the last obj_update, over the display refresh interval
    float out_rate;
    float out_jitter;
    float interp_delay; // avg ms the remote entities lag their newest keyframe
    float in_kbps;      // all inbound traffic over the last complete second, KB/s (engine psnet stats)
    float out_kbps;     // all outbound traffic, same source
};

bool netmeter_enabled();
NetMeterStats netmeter_get_stats();
// Called by the obj_update send hook in network.cpp with the packet's scheduled wire time
void netmeter_record_out(int wire_ms);
void netmeter_apply_patches();
