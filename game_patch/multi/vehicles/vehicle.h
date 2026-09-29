#pragma once

#include <cstdint>
#include <numbers>
#include <utility>
#include <vector>
#include "../../rf/math/vector.h"
#include "../../rf/math/matrix.h"

namespace rf
{
    struct Entity;
    struct Player;
    struct VMesh;
}

struct AlpineVehicleFactoryInfo;

// WIRE-FROZEN row ids - af_kill_info's damage_type HIGH NIBBLE when AF_KILL_FLAG_VEHICLE is set.
enum VehicleDamageClass
{
    VDC_JEEP = 0,
    VDC_APC,
    VDC_DRILLER,
    VDC_FIGHTER,
    VDC_SUB,
    VDC_TURRET, // every use_function-4 manned turret
    VDC_COUNT,
};

// This entity's row above, or -1 for anything that is not a synced vehicle or turret.
int vehicle_damage_class(const rf::Entity* ep);

// The same row from an entity TYPE index, for a factory record that has no live hull.
int vehicle_damage_class_from_type(int entity_type);

// Display name of a VehicleDamageClass row; cased for use after a possessive ("Bob's APC").
const char* vehicle_class_display_name(int vehicle_class);

// entities.tbl $Use: "vehicle" (1) or "turret" (4).
bool vehicle_is_synced_entity_type(int entity_type_index);
bool vehicle_is_synced_entity_type(const rf::Entity* ep);

// The synced vehicle hull this rider is aboard, or null - a null rider and host_handle -1 both
// resolve to null. Says nothing about the rider's own liveness.
rf::Entity* vehicle_ridden_hull(const rf::Entity* rider);

// As above, and null once the HULL has begun dying.
rf::Entity* vehicle_ridden_live_hull(const rf::Entity* rider);

// Contact box: world centre, the hull's own axes, and the hull-LOCAL half extents.
struct VehicleHullObb
{
    rf::Vector3 center{};
    rf::Matrix3 orient{};
    rf::Vector3 half{};
};

// 15-axis SAT on the UNTRIMMED hull boxes; `margin` inflates both on every face.
bool vehicle_hull_obb_overlap(const VehicleHullObb& a, const VehicleHullObb& b, float margin);

// The one spelling of 2*pi in the vehicle tree.
inline constexpr float vehicle_two_pi = 2.0f * std::numbers::pi_v<float>;

// Slack allowed on every face of BOTH boxes.
inline constexpr float vehicle_ram_contact_margin = 0.10f;  // server ram/crush sweep
inline constexpr float vphys_pair_contact_margin = 0.15f;   // per-body pair contact cushion

// The vehicle the local player is currently driving (not riding as a gunner), or null.
rf::Entity* vehicle_local_driven_vehicle();

// A seat that neither drives nor owns the hull's weapon, or null.
rf::Entity* vehicle_passenger_vehicle(rf::Entity* rider);

// Passenger seat this machine draws the view from - own or spectated; out_rider gets its occupant.
rf::Entity* vehicle_fp_view_passenger_vehicle(rf::Entity** out_rider);

// How far the hull's cspheres reach beyond the origin along dir (a UNIT vector).
float vehicle_collision_reach_along(rf::Entity* ep, const rf::Vector3& dir);

// A synced use_function-1 vehicle that is neither PF_AUTOMOBILE nor water-bound.
bool vehicle_is_flyer(rf::Entity* ep);

// Rebuild the fire frame from the AUTHORITATIVE hull matrix, never the interpolated angles.
void vehicle_rebuild_eye_orient(rf::Entity* ep, const rf::Matrix3& hull);
// Re-assert the driver's synced aim on a freshly rebuilt eye_orient; on the DRIVING machine only a
// third-person convergence aim is re-asserted.
void vehicle_refresh_aim_orient(rf::Entity* vehicle);

// The phb physics_make_orient (0x004A0D70) turns back into this matrix; RF has two conventions.
rf::Vector3 vehicle_make_orient_phb(const rf::Matrix3& orient);

// Server: create a vehicle/turret entity and announce it to every in-game client.
rf::Entity* vehicle_spawn(const char* class_name, const rf::Vector3& pos, const rf::Matrix3& orient);

// Seat identity on the wire is the interface point INDEX; the tag handle behind it is a
// per-machine vmesh value.
int vehicle_seat_index_from_tag(const rf::Entity* vehicle, int tag_handle);
bool vehicle_seat_tag_from_index(const rf::Entity* vehicle, int seat_index, int* out_tag_handle);

// WIRE SENTINEL in the af_client_req 0xC use request's u8 seat_index: "the lowest UNOCCUPIED seat".
// Never a real index; resolved server-side on the enter path only, so the swap path rejects it.
inline constexpr uint8_t vehicle_seat_auto = 0xFF;

// Server: board seat seat_index of vehicle_handle (or vehicle_seat_auto for the lowest free seat),
// change to seat_index when already aboard that same vehicle, or get out (vehicle_handle -1).
void vehicle_server_handle_use_request(rf::Player* pp, int vehicle_handle, uint8_t seat_index);

// Why a player may not board a hull. Computed from replicated state alone, so a client can ask it.
enum VehicleEntryReason
{
    VEHICLE_ENTRY_OK = 0,
    VEHICLE_ENTRY_OCCUPIED_OTHER_TEAM,
    VEHICLE_ENTRY_LOCKED_OTHER_TEAM,
    VEHICLE_ENTRY_NO_SEAT,
    VEHICLE_ENTRY_SUB_OUT_OF_WATER,
    VEHICLE_ENTRY_SEAT_TAKEN,
};

// THE entry rule, both sides of the wire: hull team + lock flag + occupancy, nothing server-only.
// reason_out may be null; it gets a VehicleEntryReason. seat is the seat the press would ask for:
// vehicle_seat_auto tests "any free seat", an explicit index tests that one seat.
bool vehicle_hull_entry_allowed(const rf::Entity* hull, const rf::Player* pp, int* reason_out,
                                int seat = vehicle_seat_auto);

// The hull's affiliation, -1 none. Replicated, so this answers on a client too.
int vehicle_hull_team(int vehicle_handle);

// Any seat holds a player who is pp's enemy. Seats are replicated, so this answers on a client too.
bool vehicle_occupied_by_enemy(const rf::Player* pp, const rf::Entity* vehicle);

// Server: a hull left standing empty this long after its first rider goes back to its factory.
inline constexpr int vehicle_unoccupied_destroy_s = 180;

// Client: local clock the hull's unoccupied auto-return is due at, or 0 while it is not running.
int64_t vehicle_hull_unoccupied_deadline(int vehicle_handle);

// Client: appends (local hull handle, deadline ms) for every hull with a running auto-return.
void vehicle_unoccupied_hulls(std::vector<std::pair<int, int64_t>>& out);

// Server: set a factory's affiliation. An un-entered hull follows it; a pending slot spawns now
// unless spawn_waiting is false (level-init ownership, which must not pre-empt a delayed first spawn).
void vehicle_factory_set_team(int factory_index, int team, bool spawn_waiting = true);

// The g_vehicle_factories index of the factory carrying this RFL uid, -1 if there is none.
int vehicle_factory_index_by_uid(int uid);

// Client: the per-hull attributes af_vehicle_state carries beside the seat array.
void vehicle_apply_hull_attrs_from_packet(int vehicle_handle, uint8_t team, uint8_t flags,
                                          uint16_t unoccupied_s, const int32_t* seat_rider,
                                          int seat_count);

// Client: converge this hull's seats on the occupancy an af_vehicle_state states. Idempotent.
// changed_seat is af_vehicle_state_changed_none for a bulk assert with no event to cue.
// hull_vel is the packet's quantised hull velocity, read only when this receiver is the NEW
// seat-0 rider: nothing else can carry a coasting hull's momentum across the handoff.
void vehicle_apply_seat_occupancy_from_packet(int vehicle_handle, const int32_t* seat_rider,
                                              int seat_count, uint8_t changed_seat,
                                              const int16_t* hull_vel);

// Client: re-apply every stated occupancy still naming a rider this machine could not resolve,
// until he turns up, the hull dies or the statement ages out.
void vehicle_retry_pending_seat_occupancy();

// Server: THE af_vehicle_state send. Every seat change states the hull's whole occupancy, so
// changed_seat only drives receiver cues. `vehicle` null (hull already gone) states "nobody aboard".
void vehicle_broadcast_seat_occupancy(int vehicle_handle, const rf::Entity* vehicle,
                                      int changed_seat);
void vehicle_send_seat_occupancy_to(rf::Player* pp, const rf::Entity* vehicle, int changed_seat);

// Server: replay every hull's seat occupancy to a joining player.
void vehicle_send_seat_states_to(rf::Player* pp);

// Client mirror of one Vehicle Factory's server-side respawn state, indexed like g_vehicle_factories.
struct VehicleFactoryUi
{
    uint8_t state = 0;       // af_vehicle_factory_state_value
    int64_t deadline_ms = 0; // client clock (timer::get_i64(1000)) the pending spawn is due at
    int64_t spawned_ms = 0;  // client clock of the last pending -> alive transition
};

// Client: apply an af_vehicle_factory_state announcement. Idempotent.
// team is the wire encoding: 0 red, 1 blue, 0xFF none.
void vehicle_apply_factory_state_from_packet(uint16_t factory_index, uint8_t state,
                                            uint16_t s_remaining, uint8_t team);

// Server: replay every factory's respawn state to a joining player.
void vehicle_send_factory_states_to(rf::Player* pp);

// Client: the factory respawn mirror, for world HUD markers. Empty outside a factory level.
int vehicle_factory_ui_count();
const VehicleFactoryUi* vehicle_factory_ui(int i);
const AlpineVehicleFactoryInfo* vehicle_factory(int i);
// Elapsed fraction of the authored respawn delay: 1 while alive, 0 once the factory gave up.
float vehicle_factory_ui_progress(int i, int64_t now);

// Server: free the seat a leaving player sits in. MUST be called from player_destroy, while his
// entity still resolves, or the exit announcement names a rider clients cannot identify.
void vehicle_on_player_disconnect(rf::Player* pp);

// Server: is this an obj_update row a driving client legitimately sends for its vehicle?
bool vehicle_is_driver_obj_update_row(const rf::Player* pp, const rf::Entity* ep, int flags);

// The occupant that owns the vehicle's weapon: the jeep gunner, the driver otherwise.
rf::Entity* vehicle_firing_seat_occupant(rf::Entity* vehicle);
bool vehicle_local_owns_firing_seat(rf::Entity* ep);

// Render: a Bagman bag carrier aboard, else the driver, else the lowest occupied seat; null for an
// empty hull, which never outlines.
rf::Entity* vehicle_outline_occupant(rf::Entity* vehicle);

// Render: a jeep's four tires - instances of one shared static mesh, not hull geometry.
void vehicle_render_jeep_tires(rf::Entity* ep);

// The shared tire mesh for this level, or null before the first jeep is seen / if it failed to load.
// The D3D11 outline pass must register its sub-meshes under each jeep's entity handle, or four
// separate static draws inherit the last drawn character's outline.
rf::VMesh* vehicle_jeep_tire_mesh();

// Render: the entity whose entity_render call is on the stack, or null outside one.
rf::Entity* vehicle_rendering_entity();

// Render: the UV offset for the belt of the tracked hull currently being rendered, on that class's
// tiling axis, plus its table row. False with all outputs cleared for every other draw.
bool vehicle_tread_scroll_for_draw(int& config_out, float& u_out, float& v_out);

// Is this bitmap the belt texture of that table row? Matched on the BASENAME: an ATX has no ext.
bool vehicle_is_tread_bitmap(int bm_handle, int config_index);

bool vehicle_suppress_local_fire(const rf::Player* pp);

// Server: a client reported its vehicle trigger state (action 0/1); action 2 is server->client only.
void vehicle_server_handle_fire_request(rf::Player* pp, int vehicle_handle, uint8_t action,
                                        uint8_t alt_fire);

// Client: reproduce a discrete shot the server announced.
void vehicle_apply_fire_from_packet(int vehicle_handle, uint8_t action, uint8_t alt_fire);

// Hull pitch/bank the stock obj_update row has no slot for, quantized as angle * 32767 / pi and
// belonging to the sample stamped tick; steer_q is angle * 127 / 0.75 and is render-only.
void vehicle_server_handle_orient_report(rf::Player* pp, int vehicle_handle, uint16_t tick,
                                         int16_t pitch_q, int16_t bank_q, int16_t aim_pitch_q,
                                         int16_t aim_head_q, int8_t steer_q);
void vehicle_apply_orient_from_packet(int vehicle_handle, uint16_t tick, int16_t pitch_q,
                                      int16_t bank_q, int16_t aim_pitch_q, int16_t aim_head_q,
                                      int8_t steer_q);

// Client: store an af_vehicle_health update, and the values the HUD shows for a vehicle.
void vehicle_store_health_from_packet(int vehicle_handle, float life, float max_life, int primary_ammo,
                                      int secondary_ammo);
// Total ammo (clip + reserve) for one weapon type, or -1 for a weapon with no ammo pool at all.
// Only an exact 0 means EMPTY, so a caller gating on emptiness must test == 0, never <= 0.
int vehicle_weapon_ammo(const rf::Entity* vehicle, int weapon_type);
float vehicle_hud_life(const rf::Entity* vehicle);
float vehicle_hud_max_life(const rf::Entity* vehicle);

// Server, from obj_damage: the per-class / per-damage-type damage scales. MUST be passed the RAW
// killer, before vehicle_filter_obj_damage rewrites it; returns damage unchanged off the server.
float vehicle_scale_damage(int victim_handle, int killer_handle, int damage_type, float damage);

// Server, from obj_damage, BEFORE the stock body: *killer_handle is rewritten in place with the
// occupant responsible for a vehicle killer. False means the damage must not be applied at all.
bool vehicle_filter_obj_damage(int victim_handle, int* killer_handle, int damage_type);

// Server: a vehicle's own weapon hitting one of its occupants; yes drops the damage.
bool vehicle_damage_is_own_vehicle(const rf::Entity* victim, int killer_handle);

// Server: swap a vehicle killer for the occupant responsible, and remember who last hurt a vehicle
// so its explosion can credit them.
int vehicle_resolve_damage_killer(rf::Entity* victim, int killer_handle, int damage_type);
void vehicle_note_damage(rf::Entity* victim, int killer_handle);

// Server, from entity_damage: the 10000 DT_EXPLOSIVE blast entity_die deals to a destroyed vehicle's
// occupants. A guaranteed kill, so it must be exempt from every PvP damage reducer.
bool vehicle_is_occupant_death_blast(const rf::Entity* victim, int damage_type);

// Server: a client reported that the vehicle it drives ran a player over.
void vehicle_server_handle_crush_report(rf::Player* pp, int vehicle_handle, int victim_handle);

// Server, from entity_damage: the VehicleDamageClass of the hull that ran this victim over, or -1.
// Must be asked BEFORE the damage lands, while the victim still resolves.
int vehicle_roadkill_damage_class(int victim_handle, int killer_handle);

// The VehicleDamageClass of the vehicle this entity rides in, or -1.
int vehicle_occupied_damage_class(const rf::Entity* rider);

// entity_die frees seats with entity_detach_leech, which the exit-broadcast hook never sees.
void vehicle_before_entity_die(rf::Entity* ep);
void vehicle_after_entity_die(rf::Entity* ep);

void vehicle_client_do_frame();

// Any occupant of a synced ENTITY_USE_VEHICLE hull. Turret occupants are deliberately out - their
// pose IS their aim.
bool vehicle_rider_pose_is_seat_locked(rf::Entity* ep);

// Writes a seat-locked rider's body frame (orient, p_data.orient, next_orient); false for anyone else.
bool vehicle_pin_rider_body(rf::Entity* ep);

// The answer player_process_controls gets at 0x004A6101: true keeps the rider on his OWN
// ControlInfo, false re-points it at the hull's. Also the OF_KEEP_ORIENT_ON_HOST predicate.
bool __cdecl vehicle_rider_keeps_own_orient(rf::Entity* ep);

// While first-person spectating a synced vehicle's DRIVER, the cockpit pose: the rider's eye_pos
// with the HULL frame the view was composed from. False elsewhere; before the widescreen stretch.
bool vehicle_cockpit_view_pose(rf::Vector3* out_pos, rf::Matrix3* out_orient);

void vehicle_level_init();
void vehicle_level_init_post();
void vehicle_do_frame();
void vehicle_on_multi_shutdown();
void vehicle_tbl_overrides_level_init_post();
void vehicle_tbl_overrides_revert();
void vehicle_apply_patches();
