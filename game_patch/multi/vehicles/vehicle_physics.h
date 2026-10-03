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

// True means the camera entity is already posed and camera_do_frame must not run (orbit view).
bool vehicle_physics_camera_do_frame(rf::Camera* camera);

// Change View while seated: flips the driver's or the jeep gunner's third-person setting. False for
// every other seat, which has no choice (fighter/sub drivers first person, passengers third).
bool vehicle_physics_camera_toggle_view();

// Whether the local player's current seat is viewed through the orbit camera.
bool vehicle_physics_camera_local_seat_is_orbit();

// A third-person driver's mouse and look keys turn the orbit, not the hull.
bool vehicle_physics_camera_owns_driver_look();

// 0x0049DE50's look deltas for the local gunner or passenger in the orbit view: they turn the orbit
// instead, and are replaced by the deltas that put his eye on the camera's aim. False: untouched.
bool vehicle_physics_camera_take_rider_look(rf::Entity* ep, float& pitch_delta, float& yaw_delta);

// The local third-person APC driver's convergence aim (world, unit); false keeps the rebuilt frame.
// out_capped: whether the hull limits moved it off the aim point, or the look past them is fading it off.
bool vehicle_physics_camera_driver_aim(const rf::Entity* vehicle, const rf::Entity* driver,
                                       rf::Vector3* out_dir, bool* out_capped = nullptr);

// Screen offset of the reticle from the view centre; false hides it.
bool vehicle_physics_camera_reticle_offset(float* out_dx, float* out_dy);

// True while a Bullet body owns this entity's motion, so stock integration must not run for it.
bool vehicle_physics_drives(const rf::Entity* ep);

// Live velocity of the body the LOCAL player DRIVES; false and `out` untouched if none is held here.
bool vehicle_physics_driven_velocity(int vehicle_handle, rf::Vector3* out);

// Front-wheel steer angle of the locally DRIVEN body, in RADIANS, positive toward the hull's right.
bool vehicle_physics_driven_steer_angle(int vehicle_handle, float* out);

// For each hull-local point, its nearest wheel's height above where that wheel sits loaded at rest:
// from the body this machine simulates, else (allow_probe) one straight-down level cast per point.
bool vehicle_physics_wheel_offsets(const rf::Entity* ep, const rf::Vector3* points, int num_points,
                                   bool allow_probe, float* out);

// Creates or destroys the local body to match who is driving; call on every seat change.
void vehicle_physics_on_seat_change();

// Frame top: lifecycle only. The step runs from a hook on the input read in gameplay_do_frame.
void vehicle_physics_do_frame();

// The dbg_vphys world-space collision overlay; a no-op unless dbg_vphys is on.
void vehicle_physics_render_debug();

// geomod_queue_add with the vphys hook stepped over; ONLY for the RF2 smoke replay's landed crater.
void vehicle_physics_geomod_queue_add_raw(rf::GeomodParams* params);

// A room's face set has ALREADY changed engine-side: marks exactly that room for rebuild.
void vehicle_physics_notify_room_geometry_changed(rf::GRoom* room);

// True when this CLASS has its DRIVER's eye frame described by the af_vehicle_orient supplement.
bool vehicle_physics_class_syncs_driver_aim(const rf::Entity* vehicle);

// Fastest a VehicleDamageClass may legitimately travel: the cap the post-step enforces on the
// LIVE tuning, so a wire-side check and the simulation agree. 0 for a class that cannot move.
float vehicle_physics_class_max_speed(int vdc_class);

// The speed the model drives a VehicleDamageClass at, below that cap. 0 for a class that cannot move.
float vehicle_physics_class_top_speed(int vdc_class);

// 1 + the class restitution: the largest velocity change a contact gives per unit of approach speed.
float vehicle_physics_class_bounce_gain(int vdc_class);

// The largest velocity change a contact can give this class: its cap, plus the bounce off it.
float vehicle_physics_class_max_impact_speed(int vdc_class);

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

void vehicle_physics_level_reset();
// MUST run after the RFL chunk parse - vehicle_physics_level_reset fires before it.
void vehicle_physics_level_init_post();
void vehicle_physics_on_multi_shutdown();
void vehicle_physics_apply_patches();
