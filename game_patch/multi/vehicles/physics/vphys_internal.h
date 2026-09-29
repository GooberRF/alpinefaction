#pragma once

// Bullet is confined to multi/vehicles/physics/: no bt* type leaves this header.
#include "btBulletDynamicsCommon.h"

#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include "../vehicle.h"
#include "../vehicle_physics.h"
#include "../../../rf/entity.h"
#include "../../../rf/geometry.h"
#include "../../../rf/math/matrix.h"
#include "../../../rf/math/vector.h"
#include "../../../rf/object.h"
#include "../../../rf/player/camera.h"

// One block per class. Linear params are world u/s^2; angular ones a rate (rad/s) or gain (1/s).
struct VehiclePhysicsParams
{
    float mass_scale = 1.0f;        // multiplies the entity's own p_data.mass
    float gravity_scale = 1.0f;     // fraction of RF's level gravity applied in the bt world
    float hover_gain = 1.0f;        // 1.0 = base lift exactly cancels gravity (VTOL hover)
    // Fraction of the base hover lift a SERVER-OWNED hull still makes; read on the flight path only.
    float deadstick_lift = 1.0f;

    float thrust_accel = 40.0f;     // hull-forward thrust from ci.move.z
    float strafe_accel = 30.0f;     // hull-lateral thruster from ci.move.x
    float lift_accel = 35.0f;       // world-vertical thruster from ci.move.y

    float max_speed = 25.0f;        // the speed the drag ramp holds the hull at
    float speed_soft_frac = 0.65f;  // fraction of max_speed where the extra drag starts
    float speed_drag_accel = 110.0f; // peak extra drag (reached at max_speed), units/s^2
    float linear_damping = 0.25f;   // bt linear damping (general "thick air" feel)
    float angular_damping = 0.15f;  // bt angular damping

    float pitch_rate = 5.6f;        // rad/s of nose-up rate commanded at full (normalized) ci.rot.x
    float yaw_rate = 5.6f;          // rad/s of nose-right rate commanded at full ci.rot.y
    float rot_servo = 25.0f;        // 1/s: torque per unit of rate error, on all three axes
    float bank_gain = 0.45f;        // radians of banked-turn roll target at full yaw input
    float bank_max = 0.40f;         // radians: hard clamp on the commanded bank, either way
    float bank_servo = 4.0f;        // 1/s: roll RATE commanded per radian of bank error
    // Nose pitch in degrees below which bank has FULL authority; fades out over the next 35.
    float bank_fade_deg = 45.0f;

    // Sphere-class hull reach; + ground_clearance = the bt shape radius. <= 0 derives from cspheres.
    float hull_radius = 2.0f;
    // Added to hull_radius for the bt sphere, so the ORIGIN's standoff from a wall is unchanged.
    float ground_clearance = 0.5f;
    float restitution = 0.15f;      // fraction of the into-surface speed returned as a bounce
    float hull_friction = 0.05f;
    float obstacle_range = 4.0f;    // kinematic-obstacle radius, in hull radii
    float obstacle_min_size = 0.8f; // world units: clutter smaller than this is not an obstacle

    float ceiling_band = 20.0f;     // world units below the level ceiling where climb is shaped
    float ceiling_sink = 6.0f;      // the drift enforced at or above the ceiling

    float sub_freeboard = 0.6f;        // sub: how far the hull top floats above the liquid surface
    float sub_surface_drag = 2.0f;     // sub: vertical drag per second while the hull spans the surface

    // Land vehicles on btRaycastVehicle. engine_force/brake_force are ACCELERATIONS (world u/s^2).
    float engine_force = 18.0f;         // forward drive acceleration at full throttle (u/s^2)
    float brake_force = 26.0f;          // deceleration when braking into motion (u/s^2)
    float idle_brake_force = 5.0f;      // light rolling brake with no throttle, so it rolls to rest
    // Exponential rate constants (1/s) on the APPLIED force; the commanded value is unchanged.
    float throttle_ramp = 20.0f;        // 1/s toward the commanded engine force
    float brake_ramp = 20.0f;           // 1/s toward the commanded brake force
    // Used when the commanded force is smaller in magnitude than the applied one (lift-off, reversal).
    float throttle_release_ramp = 20.0f;
    float throttle_stall_speed = 2.0f;  // below this the stall rate is used
    float throttle_stall_ramp = 8.0f;   // 1/s, the build rate while effectively stopped
    float car_max_speed = 26.0f;        // world u/s the engine stops pushing past
    float steer_angle_max = 0.55f;      // radians of front-wheel steer at full lock, low speed
    // ci.rot.y is a mouse RATE, so it is integrated into a held position, not fed to an angle target.
    float steer_input_gain = 7.0f;      // 1/s: how fast held input winds the wheel toward full lock
    float steer_return_rate = 5.0f;     // 1/s: how fast the held position unwinds with no input
    float steer_rate = 12.0f;           // rad/s: SMOOTHING on the applied angle, not the limiter
    float steer_speed_falloff = 0.05f;  // higher = less steer lock at speed: 1/(1 + f*|speed|)
    float suspension_stiffness = 35.0f;
    float suspension_damping = 2.3f;       // rebound (relaxation) damping
    float suspension_compression = 4.4f;   // compression damping
    float suspension_rest_length = 0.25f;  // the wheel hangs this far below its connection point
    float suspension_max_travel_cm = 40.0f;
    float suspension_max_force = 100000.0f; // high, so a heavy hull's weight is never clamped short
    float wheel_radius = 0.0f;          // <= 0 (the default) uses each spring csphere's own radius
    float wheel_friction = 5.0f;        // tyre grip (btRaycastVehicle frictionSlip)
    // Rear-axle grip at LOW speed, absolute; <= 0 means "same as wheel_friction".
    float wheel_friction_rear = 0.0f;
    // What the rear grip blends linearly to at car_max_speed; <= 0 keeps the single constant.
    float wheel_friction_rear_high = 0.0f;
    // 1/s of yaw-rate damping about the hull's up axis, scaled by how far the car is sliding.
    float yaw_damp = 0.0f;
    float roll_influence = 0.08f;       // 0 = no body roll under lateral load, 1 = full
    float upright_gain = 3.5f;          // 1/s: roll/pitch RATE commanded per radian of tilt
    float upright_servo = 9.0f;         // 1/s: angular acceleration per unit of rate error
    float upright_air_scale = 0.30f;    // fraction of that authority kept with every wheel airborne
    float tilt_damp = 3.0f;             // 1/s: extra damping on the roll+pitch rates while grounded
    float upright_ground_blend = 1.0f;  // 0 = world up, 1 = the terrain normal
    float upright_ref_max_deg = 50.0f;  // the reference may never lean further than this
    // Mean in-contact normal length below which they disagree and the reference falls back to world up.
    float upright_normal_agree = 0.85f;
    // The airborne servo engages only when all four of these hold at once (genuinely stranded).
    float upright_recover_up_max = 0.30f; // hull up.y below this counts as tipped over
    float upright_recover_speed = 2.5f;   // and moving slower than this
    float upright_recover_omega = 1.2f;   // and turning slower than this (rad/s)
    float upright_recover_delay = 1.0f;   // continuously, for this long, before the servo engages
    // After this longer delay the servo engages regardless of attitude.
    float upright_recover_any_delay = 1.5f;
    // Servo authority while only PARTLY supported (fewer contacts than a plane needs).
    float upright_partial_scale = 0.0f;
    float air_angular_damp = 0.0f; // 1/s of angular-velocity damping while airborne

    float downforce_accel = 0.0f;    // u/s^2 of extra down at car_max_speed, quadratic below it
    // TOTAL gravity while fully airborne, not an extra on gravity_scale; blended by airborne fraction.
    float air_gravity = 1.0f;
    float wheel_inset = 0.80f;          // corner-fallback wheels: fraction of the half-extent used
    // Non-zero: this many evenly spaced wheels per side instead of the spring spheres; totals kept.
    float tread_wheels_per_side = 0.0f;
    // The wheel is a DISC: arc_samples points around the lower-leading arc are probed.
    float wheel_cylinder_cast = 1.0f;
    float wheel_arc_samples = 6.0f;
    // Smallest y an edge contact normal may have; the suspension force is applied along it.
    float wheel_edge_min_normal_y = 0.6f;

    // Fraction of the braking applied to the CHASSIS; btRaycastVehicle's m_brake is an impulse BOUND.
    float brake_chassis_scale = 1.0f;

    float high_centre_assist = 0.0f; // u/s^2 forward while bottomed, grounded and stalled

    float drive_wheels = 2.0f;          // 0 = rear, 1 = front, 2 = all
    float car_linear_damping = 0.05f;
    float car_angular_damping = 0.35f;
    float chassis_clearance = 0.4f;     // DEBUG OVERLAY ONLY: the yellow size reference vphys_dbg draws

    // Per-face trims, hull-local; a NEGATIVE trim EXPANDS. No bottom trim - it is the wheel line.
    float chassis_trim_side = 0.0f;  // taken off BOTH +X and -X
    float chassis_trim_front = 0.0f; // taken off +Z
    float chassis_trim_rear = 0.0f;  // taken off -Z
    float chassis_trim_top = 0.0f;   // taken off +Y
    // How far the contact box's BOTTOM sits ABOVE the wheel contact line; <= 0 derives it as the
    // wheel radius.
    float chassis_bottom_raise = 0.0f;

    // Max height of a climbable obstruction; read only by the obstacle registry's height filter.
    float step_climb_height = 0.0f;

    // Scales the driller's top speed AND engine force while the drills are on.
    float drill_speed_scale = 1.0f;

    // The engine's drill-bit reach ahead of the hull ORIGIN; not used by the simulation.
    float drill_probe_reach = 0.0f;

    // 0 = the strafe keys steer (ci.move.x); non-zero uses the mouse integrator on ci.rot.y.
    float steer_from_mouse = 0.0f;

    // The orbit chase camera: always for passengers; for land-vehicle drivers and the jeep gunner by choice.
    float cam_enable = 0.0f;
    float cam_dist = 0.0f;      // <= 0: derive from the hull box (2.0 * half.z + 2.0)
    float cam_height = 0.0f;    // <= 0: derive from the hull box (1.3 * half.y + 0.9)
    float cam_collide_margin = 0.45f; // gap kept between the camera and whatever the probe hit
    float cam_pull_rate = 60.0f;      // u/s the camera is allowed to rush IN on a fresh hit
    float cam_extend_rate = 8.0f;     // u/s it eases back OUT once the obstruction clears

    // Per-axis control signs; the car model reuses yaw_sign for steering and thrust_sign for drive.
    float pitch_sign = 1.0f;
    float yaw_sign = 1.0f;
    float bank_sign = 1.0f;
    float thrust_sign = 1.0f;
    float strafe_sign = 1.0f;
    float lift_sign = 1.0f;
};

enum VehiclePhysicsClass
{
    VPHYS_CLASS_FIGHTER = 0, // any synced flyer: Fighter01, shuttle, masako_fighter
    VPHYS_CLASS_SUB = 1,
    VPHYS_CLASS_JEEP = 2,    // btRaycastVehicle land vehicles below
    VPHYS_CLASS_APC = 3,
    VPHYS_CLASS_DRILLER = 4,
    VPHYS_CLASS_COUNT = 5,
};

// The marshalling is the IDENTITY: an RF triple goes to Bullet verbatim, left-handed basis and all.
inline btVector3 to_bt(const rf::Vector3& v)
{
    return btVector3(v.x, v.y, v.z);
}

inline rf::Vector3 from_bt(const btVector3& v)
{
    return rf::Vector3{v.x(), v.y(), v.z()};
}

// Matrix3's columns are rvec/uvec/fvec and btMatrix3x3's ctor is row major, hence the transpose.
inline btMatrix3x3 to_bt(const rf::Matrix3& m)
{
    return btMatrix3x3(m.rvec.x, m.uvec.x, m.fvec.x,
                       m.rvec.y, m.uvec.y, m.fvec.y,
                       m.rvec.z, m.uvec.z, m.fvec.z);
}

inline rf::Matrix3 from_bt(const btMatrix3x3& m)
{
    return rf::Matrix3{
        rf::Vector3{m[0][0], m[1][0], m[2][0]},
        rf::Vector3{m[0][1], m[1][1], m[2][1]},
        rf::Vector3{m[0][2], m[1][2], m[2][2]},
    };
}

inline bool vphys_finite(const rf::Vector3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

inline bool vphys_finite(const rf::Matrix3& m)
{
    return vphys_finite(m.rvec) && vphys_finite(m.uvec) && vphys_finite(m.fvec);
}

// Half extents plus the CENTRE they are measured about - not the hull origin for a land vehicle.
struct HullBox
{
    btVector3 half{0.25f, 0.25f, 0.25f};
    btVector3 center{0.0f, 0.0f, 0.0f};
};

struct RfVehicleRaycaster;

// The solver's fixed substep; btRaycastVehicle consumes setBrake as an IMPULSE per substep.
constexpr float vphys_fixed_timestep = 1.0f / 120.0f;

// maxSubSteps of the one stepSimulation call; vphys_frame_step_time clamps with the same number.
constexpr int vphys_max_substeps = 8;

// m_localTime is protected with no getter, and it is the ONLY substep accumulator there is.
class VphysDynamicsWorld : public btDiscreteDynamicsWorld
{
public:
    using btDiscreteDynamicsWorld::btDiscreteDynamicsWorld;
    btScalar local_time() const { return m_localTime; }
};

// Heap-owned so its address is stable: RfVehicleRaycaster holds a back-pointer to it.
struct VehicleSimBody
{
    int vehicle_handle = -1;
    int vehicle_class = 0;        // VehiclePhysicsClass
    // Server-owned (driverless): inputs read released, lift scaled by deadstick_lift, may SLEEP.
    bool server_owned = false;
    // Registered with the world? Detached while a server body sleeps - updateActions ignores sleep.
    bool car_action_in_world = false;

    btSphereShape* shape = nullptr;          // flyer/sub hull (null for an automobile)
    btDefaultMotionState* motion_state = nullptr;
    // The dynamic body, sphere hull OR car chassis; its user pointer points back at this struct.
    btRigidBody* body = nullptr;
    btBoxShape* car_shape = nullptr;         // car only: the chassis box
    btCompoundShape* car_compound = nullptr; // holds car_shape at the hull box's local centre
    RfVehicleRaycaster* raycaster = nullptr;
    btRaycastVehicle* raycast_vehicle = nullptr;

    HullBox car_box{};       // the TRIMMED contact box the probes seat against, rebuilt each step
    HullBox car_box_base{};  // the raw csphere box the trims are taken off, fixed at boarding
    HullBox car_shape_box{}; // what car_shape/car_compound were last built with
    float body_mass = 0.0f;   // what setMassProps was last given, so mass_scale can be live
    float body_radius = 0.0f; // the sphere the shape was last built with (hull_standoff)
    int num_wheels = 0;
    // The spring-sphere count the class tuning is authored against; 0 = the box-corner fallback.
    int wheel_ref_count = 0;
    // How far the WHEELS reach below the hull origin; every height test measures against this line.
    float wheel_reach = 0.0f;
    float auto_bottom_raise = 0.0f;
    // Each wheel's own spring-sphere centre Y, so sag compensation re-derives from live stiffness.
    std::vector<float> wheel_center_y;
    // What the last gather asked for; a SLEEPING body must re-present it or obstacles_sync drops it.
    std::vector<int> obstacle_handles;

    // Read by RfVehicleRaycaster::castRay through its owner back-pointer; per BODY, never global.
    rf::Vector3 wheel_fwd{0.0f, 0.0f, 1.0f}; // hull forward axis for the arc plane
    bool wheel_cylinder = true;              // wheel_cylinder_cast: the disc model (off = one ray)
    int wheel_arc_samples = 6;               // wheel_arc_samples
    float wheel_rest_len = 0.25f;            // this frame's suspension rest length, to recover radius
    float wheel_edge_min_normal_y = 0.6f;    // cap on an edge normal's backward lean

    float steer_current = 0.0f; // the applied steer angle, smoothed across frames
    float steer_input = 0.0f;   // the HELD steering position in [-1,1]
    // The ramped drive/brake ACCELERATIONS the wheels are given.
    float engine_accel = 0.0f;
    float brake_accel = 0.0f;
    float engine_cmd = 0.0f;
    // Chassis box on a ground-facing plane; ground_material (the exit gate) is wheels OR this.
    bool chassis_ground_contact = false;
    // What the frame's single manifold pass found; only a recomputing frame copies it across.
    bool chassis_ground_contact_pass = false;
    // The upright servo's up reference, kept for the vphys_dbg overlay later in the same frame.
    rf::Vector3 upright_ref{0.0f, 1.0f, 0.0f};
    // How long the stranded test has continuously held, and whether the auto-right is engaged.
    float upright_recover_timer = 0.0f;
    // How long the hull has been at rest with no wheel on the ground, at ANY attitude.
    float upright_prop_timer = 0.0f;
    bool upright_recovering = false;

    // Frame time banked since the last stepping frame, so the gated model integrates the same total.
    float pre_step_dt = 0.0f;

    // timer::get_i64(1000) instant the IDLE BRAKE resumes after a ram shove; 0 = never shoved.
    int64_t shove_brake_until_ms = 0;

    rf::Vector3 written_pos{}; // the position last written into the entity, for correction detection
    // The liquid surface the sub was last actually under; a breaching hull's room stops answering.
    float liquid_surface = 0.0f;
    bool liquid_surface_valid = false;
};

struct VehiclePhysicsObstacle
{
    btBoxShape* shape = nullptr;
    btDefaultMotionState* motion_state = nullptr;
    btRigidBody* body = nullptr;
    // The rebuild key, all four parts: handles recycle, and a corpse swap changes only the vmesh.
    int info_index = -1;
    int obj_type = -1;
    const void* vmesh = nullptr;
    btVector3 built_half{0.0f, 0.0f, 0.0f};
    bool is_static = false; // level clutter: posed once, never re-written
};

// How long a server body's IDLE BRAKE is suppressed after a ram shove (ms).
constexpr int64_t vehicle_shove_brake_grace_ms = 700;

struct VehiclePhysicsWorld
{
    btDefaultCollisionConfiguration* config = nullptr;
    btCollisionDispatcher* dispatcher = nullptr;
    btDbvtBroadphase* broadphase = nullptr;
    btSequentialImpulseConstraintSolver* solver = nullptr;
    VphysDynamicsWorld* world = nullptr;

    // Heap elements so addresses survive growth; only sim_body_add/erase maintain both containers.
    std::vector<std::unique_ptr<VehicleSimBody>> bodies;
    std::unordered_map<int, VehicleSimBody*> by_handle;
    VehicleSimBody* driven = nullptr;

    std::unordered_map<int, VehiclePhysicsObstacle> obstacles;

    bool manifolds_dirty = false;
};

// Bullet requires the filter to match BOTH ways: (maskA & groupB) && (maskB & groupA).
constexpr short vphys_group_hull = 0x80;
constexpr short vphys_group_level = 0x40;
// A hull pairs with everything EXCEPT another hull: vehicle-vs-vehicle is vphys_apply_pair_response.
constexpr short vphys_mask_no_hull = static_cast<short>(~vphys_group_hull);

// A vehicle ANOTHER machine simulates: mask of nothing, so no vehicle pair ever reaches the solver.
constexpr short vphys_group_vehicle_box = 0x20;
constexpr short vphys_mask_none = 0;

enum class VehicleOrbitSeat
{
    none,
    passenger,
    driver, // jeep, APC, driller
    gunner, // the jeep gunner
};

struct VehicleChaseCamera
{
    bool active = false;
    int vehicle_handle = -1;
    int rider_handle = -1; // whose seat the orbit is on, so a target switch re-seeds the view
    float dist = 0.0f;       // the distance in use, after the wall probe and its rate limit
    rf::CameraMode saved_mode = rf::CAMERA_FIRST_PERSON;

    // False: a spectated passenger, viewed along his own eye frame with no orbit of our own.
    bool orbit = false;
    VehicleOrbitSeat seat = VehicleOrbitSeat::none;
    int cls = -1;
    // Hull reference: springs (with their rates) toward a heading target that a land vehicle holds
    // while slow or reversing, and toward a share of the hull pitch.
    float ref_yaw = 0.0f;
    float ref_pitch = 0.0f;
    float ref_yaw_vel = 0.0f;
    float ref_pitch_vel = 0.0f;
    float target_yaw = 0.0f;
    // The hull as the camera measures it: last flat heading, smoothed yaw rate and velocity.
    float hull_yaw = 0.0f;
    float hull_yaw_rate = 0.0f;
    rf::Vector3 hull_pos{};
    rf::Vector3 hull_vel{};
    // Smoothed focus, snapped when the seat (its interface tag) changes.
    rf::Vector3 focus{};
    rf::Vector3 focus_vel{};
    int seat_tag = -1;
    // The player's orbit, relative to the reference; drift eases it back after idle_s.
    float rel_yaw = 0.0f;
    float rel_pitch = 0.0f;
    float idle_s = 0.0f;
    // Last posed view, and the centre-ray point the aiming seats converge on.
    rf::Vector3 pos{};
    rf::Vector3 look{};
    rf::Vector3 aim_point{};
    bool aim_valid = false;
};

// Shared globals, defined once in the TU named beside each.
extern VehiclePhysicsWorld g_vphys;         // vphys_world.cpp
extern bool g_level_has_bullet_vehicles;    // vphys_world.cpp
extern VehicleChaseCamera g_vcam;           // vphys_camera.cpp
extern bool g_vphys_dbg;                    // vphys_debug.cpp

// ---- vphys_world.cpp ----
bool vphys_class_is_automobile(int cls);
VehicleSimBody* sim_body_from_handle(int handle);
void sim_body_destroy(VehicleSimBody* b);
void driven_body_destroy();
const VehiclePhysicsParams& params_for_class(int cls);
HullBox hull_local_box(const rf::Object* op);
VehicleSimBody* body_create(rf::Entity* ep, int cls);
void world_create();
void world_destroy();
void level_mesh_build();
void level_mesh_rebuild_pending();
void level_mesh_poll_remesh();
bool level_mesh_geomod_settled();
bool level_mesh_active();
// One chunk as the overlay sees it: its real triangle bounds, and whether the last remesh built it.
struct LevelMeshDebugChunk
{
    rf::Vector3 aabb_min{};
    rf::Vector3 aabb_max{};
    bool recently_rebuilt = false;
};
void level_mesh_debug_collect(const rf::Vector3& center, float radius,
                              std::vector<LevelMeshDebugChunk>& out);
float level_mesh_debug_cell_size();
rf::Vector3 level_mesh_debug_cell_origin(const rf::Vector3& p);
void obstacles_update_all();
// step_time is THIS frame's step (0 if none); engine_dt is the PREVIOUS frame's rf::frametime.
void movers_update_all(bool step_will_run, float step_time, float engine_dt);
void vphys_world_install_patches();
// Whether the Bullet chassis collides with this mover brush at all.
bool mover_brush_is_solid(const rf::Object& mb);

// ---- vphys_car.cpp ----
// collide_linesegment_world plus the mover-brush solidity rule above.
bool vphys_collide_solid_segment(const rf::Vector3& a, const rf::Vector3& b, rf::PCollisionOut& out);
void car_teardown(VehicleSimBody& b);
HullBox hull_contact_box(const HullBox& base, const VehiclePhysicsParams& p, float bottom_raise);
// The chassis bottom raise: the class's authored value, else the hull's wheel radius. `b` supplies
// the radius measured at body creation; null re-measures it, for an entity that holds no body.
float hull_bottom_raise(const VehiclePhysicsParams& p, const rf::Object* op, const VehicleSimBody* b);
float hull_radius_for(const VehiclePhysicsParams& p, const rf::Object* op);
float hull_standoff(const VehiclePhysicsParams& p, const rf::Object* op);
float hull_wheel_radius(const rf::Object* op, const VehiclePhysicsParams& p);
float wheel_sag(const VehicleSimBody& b, const VehiclePhysicsParams& p, int num_wheels);
void body_seed_from_entity(VehicleSimBody& b, rf::Entity* ep);
void body_apply_mass(VehicleSimBody& b, const VehiclePhysicsParams& p, const rf::Entity* ep);
void body_apply_shape(VehicleSimBody& b, const VehiclePhysicsParams& p, rf::Entity* ep);
void car_body_create(VehicleSimBody& b, rf::Entity* ep, int cls);
void car_apply_box_shape(VehicleSimBody& b, const VehiclePhysicsParams& p, rf::Entity* ep,
                         const HullBox& box);
bool apply_car_controls(VehicleSimBody& b, rf::Entity* ep, const VehiclePhysicsParams& p, float dt);
// dt is frame time SUMMED since the last stepping frame; step_time is the window a force acts over.
void apply_car_model(VehicleSimBody& b, const VehiclePhysicsParams& p, float dt, float step_time);

// ---- vphys_flight.cpp ----
bool liquid_surface_for(VehicleSimBody& b, rf::Entity* ep, float& out);
void apply_flight_model(VehicleSimBody& b, rf::Entity* ep, const VehiclePhysicsParams& p, int cls);

// ---- vphys_step.cpp ----
// The VPHYS class of a hull, a pure mapping from its wire-frozen VehicleDamageClass; -1 for a
// turret, which has no motion to simulate, and for anything that is not a synced vehicle.
int vphys_class_for(const rf::Entity* ep);
void vphys_reconcile();
float vphys_frame_step_time(float dt);
void vphys_prev_frame_dt_reset();

// ---- vphys_camera.cpp ----
float vphys_cam_distance(const VehiclePhysicsParams& p, const HullBox& box);
float vphys_cam_height(const VehiclePhysicsParams& p, const HullBox& box);
bool vphys_chase_camera_do_frame(rf::Camera* camera);
void vphys_camera_install_patches();

// ---- vphys_debug.cpp ----
void vphys_render_debug();
