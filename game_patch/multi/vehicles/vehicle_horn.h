#pragma once

namespace rf
{
    struct Player;
}

// The jeep driver's horn: Primary Attack held in seat 0 of a jeep sounds it.

// Server: a driver's horn edge, from af_vehicle_fire's AF_VEHICLE_HORN_START / AF_VEHICLE_HORN_STOP.
void vehicle_server_handle_horn_request(rf::Player* pp, int vehicle_handle, bool held);
// Server: this hull's horn is sounding, as af_vehicle_state states it.
bool vehicle_server_horn_sounding(int vehicle_handle);
void vehicle_server_horn_do_frame();
// Client: report the local driver's edges and keep every audible horn's loop on its hull.
void vehicle_horn_client_do_frame();
void vehicle_horn_level_init();
void vehicle_horn_install();
