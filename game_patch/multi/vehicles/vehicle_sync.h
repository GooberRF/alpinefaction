#pragma once

#include "vehicle_internal.h"

void vehicle_update_interp_ownership(rf::Entity* vehicle, bool driver_boarding = false);
void vehicle_decorate_interp_orient(rf::Entity* vehicle);
void vehicle_server_body_do_frame(rf::Entity* ep);
void vehicle_ease_aim_do_frame();
rf::Entity* vehicle_local_firing_vehicle();
void vehicle_server_apply_fire(rf::Entity* vehicle, const VehicleFireState& state);
void vehicle_server_stop_fire(rf::Entity* vehicle);
void vehicle_broadcast_health(rf::Entity* vehicle, bool is_reliable, const rf::Vector3* hit_dir = nullptr);
void vehicle_server_sync_health(rf::Entity* vehicle);
void vehicle_rider_damage_feedback(int vehicle_handle, const rf::Vector3* hit_dir);
void vehicle_drop_combat_state(int vehicle_handle);

void vehicle_sync_apply_patch();
