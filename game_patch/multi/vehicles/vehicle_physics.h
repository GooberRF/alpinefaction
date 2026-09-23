#pragma once

// No bt* type, allocation or header crosses out of multi/vehicles/physics.
// Ownership is a PARTITION: a driver's client simulates his own hull, a server every DRIVERLESS
// synced car, flyer and sub, and a turret - which has no motion to author - nobody.

namespace rf
{
    struct Camera;
    struct Entity;
    struct GeomodParams;
    struct GRoom;
    struct Vector3;
}

// True means the camera entity is already posed and camera_do_frame must not run (passenger view).
bool vehicle_physics_camera_do_frame(rf::Camera* camera);

// True while a Bullet body owns this entity's motion, so stock integration must not run for it.
bool vehicle_physics_drives(const rf::Entity* ep);

// Live velocity of the body the LOCAL player DRIVES; false and `out` untouched if none is held here.
bool vehicle_physics_driven_velocity(int vehicle_handle, rf::Vector3* out);

// Front-wheel steer angle of the locally DRIVEN body, in RADIANS, positive toward the hull's right.
bool vehicle_physics_driven_steer_angle(int vehicle_handle, float* out);

// Creates or destroys the local body to match who is driving; call on every seat change.
void vehicle_physics_on_seat_change();

// Frame top: lifecycle only. The step runs from a hook on the input read in gameplay_do_frame.
void vehicle_physics_do_frame();

// The vphys_dbg world-space collision overlay; a no-op unless vphys_dbg is on.
void vehicle_physics_render_debug();

// Every crater, whatever the source: rebuilds the affected rooms' Bullet collision meshes.
void vehicle_physics_notify_geomod(const rf::Vector3& pos, float radius);

// geomod_queue_add with the vphys hook stepped over; ONLY for the RF2 smoke replay's landed crater.
void vehicle_physics_geomod_queue_add_raw(rf::GeomodParams* params);

// A room's face set has ALREADY changed engine-side: marks exactly that room for rebuild.
void vehicle_physics_notify_room_geometry_changed(rf::GRoom* room);

// True when this CLASS has its DRIVER's eye frame described by the af_vehicle_orient supplement.
bool vehicle_physics_class_syncs_driver_aim(const rf::Entity* vehicle);

// Fastest a VehicleDamageClass may legitimately travel: the cap the post-step enforces on the
// LIVE tuning, so a wire-side check and the simulation agree. 0 for a class that cannot move.
float vehicle_physics_class_max_speed(int vdc_class);

// The drown test: the hull ORIGIN below a liquid room's plane, and never the sub.
bool vphys_hull_submerged(const rf::Entity* ep);

// Ensure a server body for this driverless hull; true means one exists after the call. A null
// seed_vel seeds from the entity - only the driver's Use-exit must pass a velocity captured first.
bool vehicle_physics_server_ensure(rf::Entity* ep, const rf::Vector3* seed_vel);

// Destroy the server body for this vehicle; never touches the LOCAL player's driven body.
void vehicle_physics_server_release(int vehicle_handle);

// `out_asleep` is strictly stronger than "below the speed threshold". False when there is no body.
bool vehicle_physics_server_state(int vehicle_handle, rf::Vector3* out_pos, float* out_linear_speed,
                                  float* out_angular_speed, bool* out_asleep);

// Does THIS machine simulate a server body for this hull? The ownership question, asked directly.
bool vehicle_physics_server_owns(int vehicle_handle);

// The hull box every sweep measures, from the entity alone. Either output may be null.
void vehicle_physics_entity_hull_box(const rf::Entity* ep, rf::Vector3* out_half,
                                     rf::Vector3* out_center);

// The SERVER body's live velocity; p_data.vel is deliberately zero for one, so it is no source.
bool vehicle_physics_server_velocity(int vehicle_handle, rf::Vector3* out);

// Not optional before a shove: updateActivationState ZEROES a sleeping body's velocity every step.
void vehicle_physics_server_wake(int vehicle_handle);

// Change this SERVER body's velocity by `delta_v` world u/s along `dir` (need not be normalized).
bool vehicle_physics_server_shove(int vehicle_handle, const rf::Vector3& dir, float delta_v);

void vehicle_physics_level_init();
// MUST run after the RFL chunk parse - vehicle_physics_level_init fires before it.
void vehicle_physics_level_init_post();
void vehicle_physics_on_multi_shutdown();
void vehicle_physics_apply_patches();
