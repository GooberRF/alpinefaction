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
void vehicle_tick_ammo_refill_mirrors();
void vehicle_rider_damage_feedback(int vehicle_handle, const rf::Vector3* hit_dir);
void vehicle_drop_combat_state(int vehicle_handle);

// Seeds a keyframe into ep's own ObjInterp ring, which the server's obj_update packs for a non-local
// entity. False when ep has no ring or the ring already ends at this tick.
bool obj_interp_push_keyframe(rf::Entity* ep, rf::Vector3 pos, rf::Vector3 phb, rf::Vector3 eye_phb,
                              rf::Vector3 vel, rf::Vector3 move, uint16_t tick);
// The same for an entity this machine simulates (a server bot), without the engine's per-keyframe physics step.
bool obj_interp_push_simulated_keyframe(rf::Entity* ep, const rf::Vector3& pos, const rf::Vector3& phb,
                                        const rf::Vector3& eye_phb, const rf::Vector3& vel, const rf::Vector3& move,
                                        uint16_t tick);

void vehicle_sync_apply_patch();
