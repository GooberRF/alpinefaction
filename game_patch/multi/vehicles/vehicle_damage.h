#pragma once

#include "vehicle_internal.h"

// Server: remember this driver as the killer a driverless hull still credits for 2.5 s.
void vehicle_coast_memory_set(rf::Entity* vehicle, rf::Player* pp, rf::Entity* rider);
void vehicle_server_coast_crush_sweep(rf::Entity* vehicle, const rf::Vector3& hull_pos);
// Server: the same roadkill sweep for a DRIVEN hull, credited to its seat-0 driver.
void vehicle_server_driven_crush_sweep(rf::Entity* vehicle);
void vehicle_server_ram_sweep();
// Server: per-frame drill contact damage, driven while the driller's drill is on.
void vehicle_server_drill_damage_sweep(rf::Entity* vehicle);
// Server: one received row's speed for a client-driven hull, for the crash report bound.
void vehicle_note_observed_speed(int vehicle_handle, float speed);
// Server: one hull's life and ammo regeneration for this frame.
void vehicle_regen_do_frame(rf::Entity* ep);

void vehicle_damage_apply_patch();
