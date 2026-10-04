#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <common/vehicle_orient.h>
#include "vehicle.h"
#include "../alpine_packets.h"
#include "../server_internal.h"
#include "../../misc/level.h"
#include "../../rf/entity.h"
#include "../../rf/math/matrix.h"
#include "../../rf/math/vector.h"
#include "../../rf/os/timestamp.h"
#include "../../rf/player/player.h"

// Server: the trigger state the firing seat owner last reported; the two triggers are independent.
struct VehicleFireState
{
    bool primary_held = false;
    bool alt_held = false;
    uint8_t requester_id = 0xFF;

    bool any_held() const { return primary_held || alt_held; }
};

// Server: what every client was last told this vehicle's life and ammo were, and when.
struct VehicleHealthSync
{
    float last_sent_life = -1.0f;
    int last_sent_primary_ammo = -2;   // -1 is a legal "no such weapon", so the unsent marker is -2
    int last_sent_secondary_ammo = -2;
    int64_t last_sent_refill_due_ms[2] = {-1, -1};
    rf::Timestamp next_send;
    int64_t last_change_ms = 0;
    bool settled_sent = true; // nothing has changed since the last reliable send
    rf::Vector3 hit_dir{};    // travel direction of the latest attributed hit, for the riders' indicators
    int64_t hit_ms = -1;
};

// Client: the last af_vehicle_health for a vehicle, by local object handle.
struct VehicleHealth
{
    float life = 0.0f;
    float max_life = 0.0f;
};

// Time after a weapon's last real shot until it refills to its spawn ammo in one step.
inline constexpr int64_t vehicle_ammo_regen_delay_ms = 5000;

// How long after its own predicted shots the firing client ignores a higher server total: a stale packet
// arrives within about RTT + the 100 ms send floor, while the reliable settle (sent 1 s after the last
// change) must still get through.
inline constexpr int64_t vehicle_ammo_stale_raise_ms = 800;

// Client side, per weapon slot (primary, secondary): the total last applied from the server and when this
// machine last saw it drop, so a packet sent before its own latest shots cannot top the mirror back up.
struct VehicleAmmoMirror
{
    int total[2] = {-1, -1};
    int64_t last_drop_ms[2] = {};
    // Counted down in demo time between packets; 0 = no refill pending.
    float refill_ms_left[2] = {};
    // The last packet's refill_ms was nonzero: a raise in the next one with refill_ms 0 is that refill.
    bool refill_pending[2] = {};
};

// Server: one vehicle weapon's ammo regeneration state, captured at spawn.
struct VehicleAmmoRegen
{
    int weapon_type = -1;
    int spawn_ammo = 0;   // the refill target; 0 = this weapon does not regenerate
    int last_seen_ammo = 0;
    int64_t last_fire_ms = 0; // driven by an OBSERVED drop in ammo or a trigger held on a loaded gun
};

// Server: hull regeneration and melee repair state. spawn_max_life is captured ONCE at creation and
// is the only ceiling either may raise life to - the tbl overrides rewrite info->max_life under a
// live level. last_damage_ms is driven by an OBSERVED drop in life, not by a damage callback.
struct VehicleRegen
{
    float spawn_max_life = 0.0f;
    float last_seen_life = 0.0f;
    int64_t last_damage_ms = 0;
    // [0] primary, [1] secondary; independent of the life regen.
    VehicleAmmoRegen ammo[2];
};

// The hull pitch and bank a non-driving machine was last told about, and the obj_update tick
// they belong to: every keyframe at or after that tick carries them.
struct VehicleOrientSupplement
{
    float pitch = 0.0f;
    float bank = 0.0f;
    // The driver's aim, as ABSOLUTE world angles.
    float aim_pitch = 0.0f;
    float aim_head = 0.0f;
    // Render-only, positive toward the hull's right; the DRIVING machine never reads it back.
    float steer = 0.0f;
    uint16_t tick = 0;
    bool valid = false;
    int64_t stored_ms = 0;
    // The VIEW copy of the aim; the fire paths keep reading the raw pair.
    float eased_aim_pitch = 0.0f;
    float eased_aim_head = 0.0f;
    bool eased_valid = false;
    // The hull frame the eye above was composed from, by the same statement - never a fresh read.
    rf::Matrix3 eye_hull_orient{};
    bool eye_hull_valid = false;
};

// Driver side: the last supplement actually put on the wire for a vehicle.
struct VehicleOrientSend
{
    int16_t pitch = 0;
    int16_t bank = 0;
    int16_t aim_pitch = 0;
    int16_t aim_head = 0;
    int8_t steer = 0;
    uint16_t tick = 0;
    bool valid = false;
    rf::Timestamp keepalive;
};

// Server: the replication state of a fully-unmanned vehicle, filled by one producer,
// vehicle_server_body_do_frame. Every sample authored from a server body carries velocity zero.
struct VehicleKinematics
{
    bool active = false;      // moved as of the last tick (drives the settle-frame sample)
    bool broadcast = false;   // the send injection should force-serialize it this frame
    bool asleep = false;      // Bullet's verdict last frame; the sleep edge authors the exact rest row
    bool row_valid = false;   // row_pos/row_orient hold the pose of the last row watchers were given
    rf::Vector3 row_pos{};
    rf::Matrix3 row_orient{};
    // Coast keyframes must CONTINUE the driver's interp-tick timeline rather than restart on the
    // server's clock, or a watcher's hull freezes (tick behind its anchor) or warps (ahead of it).
    uint16_t base_tick = 0;   // driver's last relayed keyframe tick, captured at seed
    int64_t base_time_ms = 0; // server timer::get_i64(1000) at seed - the origin the offset counts from
    bool tick_seeded = false; // base_tick/base_time_ms are valid (the ring had a driver keyframe)
};

// The replicated per-hull attributes: authored on the server, mirrored on clients from
// af_vehicle_state. team/lock_to_team/entered_once exist on both sides so either can answer
// vehicle_hull_entry_allowed; the two timestamps are one-sided, as marked.
struct VehicleState
{
    int team = -1;            // -1 none, 0 red, 1 blue
    bool lock_to_team = false;
    bool entered_once = false;
    int64_t unoccupied_since_ms = 0;    // server: when the hull was last left empty; 0 = not running
    int64_t unoccupied_deadline_ms = 0; // client: local clock the auto-return is due at; 0 = none
    bool horn = false;                  // client: the server states the driver's horn is sounding
};

// Server: a jeep horn whose driver holds the button; `on` stays false while his START waits out the
// rate window.
struct VehicleHorn
{
    uint8_t requester_id = 0xFF;
    bool on = false;
};

// Client: a stated occupancy this machine could not fully resolve - a rider named by an
// af_vehicle_state whose entity_create has not landed yet. Re-applied each frame until it does.
struct VehiclePendingSeats
{
    int wire_vehicle_handle = -1;   // the server's handle, as received: the re-apply re-resolves it
    // The hull the statement was made about. A recycled wire handle must not re-apply to another hull.
    int local_vehicle_handle = -1;
    int32_t seat_rider[af_vehicle_state_max_seats]{};
    int seat_count = 0;
    int16_t hull_vel[3]{};
    int64_t expiry_ms = 0;          // stamped once, when the hull first goes pending
};

// Server: a Vehicle Factory and the respawn timer for whatever stands on it, vehicle or turret.
struct VehicleSpawnSlot
{
    int entity_type = -1;
    int factory_index = -1;   // index into g_vehicle_factories, which clients hold too
    rf::Vector3 pos{};
    rf::Matrix3 orient{};
    bool is_turret = false;   // the factory's class is a manned turret (use_function 4)
    bool entered_once = false;
    int factory_delay_ms = 0; // the mapper's delay, unless the server overrides it
    int handle = -1;
    rf::Timestamp respawn_timer;
    int failures = 0;
    int64_t last_retry_announce_ms = 0; // rate limit for the retry broadcast; 0 = none sent yet
    bool given_up = false;
};

// Server: who a COASTING hull still answers for, and until when - the attribution and crush windows
// are one. Keyed by NET ID: the ex-driver can disconnect, die and respawn inside the window.
struct VehicleCoastMemory
{
    uint8_t player_id = 0xFF;
    int exit_entity_handle = -1;
    int64_t expiry_ms = 0;
};

// Server: one hull-vs-hull contact EPISODE, keyed by the ordered handle pair. Damage lands on
// IMPACT and never again while the hulls stay in contact; the approach is judged on a windowed PEAK
// because the physics pre-step kills the closing component on the first frame.
struct VehicleRamPair
{
    int64_t overlap_since_ms = 0; // when the boxes started overlapping; 0 = not overlapping
    float peak_closing = 0.0f;    // fastest approach seen inside the peak window
    float peak_a_toward = 0.0f;   // the two toward components AT that sample, so the rammer
    float peak_b_toward = 0.0f;   // identity and the mass split come from one consistent frame
    int64_t peak_ms = 0;          // when the peak was sampled, for the staleness window
    bool latched = false;         // a hit was minted; no further hit until they separate
    int64_t latched_ms = 0;       // when, for the re-arm floor
};

using VehicleRamKey = std::pair<int, int>; // ordered (lower handle, higher handle)

// Server: the killer of the blow that took a hull from alive to dead, by net id too so credit follows
// the PLAYER if that entity is gone by the hull's entity_die. entity_handle -1 = nobody.
struct VehicleLethalKiller
{
    int entity_handle = -1;
    uint8_t player_id = 0xFF;
    // What that blow was, for the riders' kill lines: an on-foot weapon, or the killer's hull class.
    int weapon_type = -1;
    bool splash = false;
    int vehicle_class = -1;
};

// In fixed time buckets: server, the fastest |vel| a client-driven hull's received rows carried;
// every machine, the FIRST pose (received row, or this machine's own body) noted in each bucket, and when.
struct VehicleObservedSpeed
{
    static constexpr int bucket_count = 4;
    float bucket_peak[bucket_count]{}; // [0] is the current bucket
    rf::Vector3 bucket_pos[bucket_count]{};
    float bucket_heading[bucket_count]{};
    int64_t bucket_pose_ms[bucket_count]{};
    bool bucket_posed[bucket_count]{};
    int64_t bucket_start_ms = 0;
    rf::Vector3 newest_pos{};
    float newest_heading = 0.0f;
    int64_t newest_ms = 0;
    // The hull box's longest origin-to-face half extent: what turns a heading change into a distance.
    float reach = 0.0f;
    // The pose at the last step past the change floor, and when; a parked or silent hull never steps.
    rf::Vector3 anchor_pos{};
    float anchor_heading = 0.0f;
    int64_t changed_ms = 0;
    bool anchored = false;
    bool changed = false;
};

// Client render: jeep wheel roll, integrated from a POSITION delta (a coasting hull wires zero vel).
struct VehicleWheelSpin
{
    rf::Vector3 last_pos{};
    bool has_last = false;
    float angle = 0.0f; // radians, wrapped to [0, 2pi)
    // Displayed front-pair steer angle, positive toward the hull's right; eased, not assigned.
    float steer = 0.0f;
    // Displayed per-tire lift along the hull's up axis, in spinner prop order; eased, not assigned.
    float suspension[4] = {};
    bool has_suspension = false;
};

// Client render: tread UV scroll, integrated from a POSITION delta like VehicleWheelSpin.
struct VehicleTreadScroll
{
    rf::Vector3 last_pos{};
    bool has_last = false;
    float offset = 0.0f; // UV units on the row's tiling axis, wrapped to (-1, 1)
    int config = -1;     // which vehicle_tread_configs row this hull is
};

// Every map here is keyed by the vehicle's local object handle and dropped wholesale on level init.
struct VehicleModuleState
{
    std::vector<int> synced_handles;
    std::unordered_map<int, VehicleFireState> fire;
    // Server: when each hull's continuous gun may next turn on.
    std::unordered_map<int, rf::Timestamp> fire_rearm;
    std::unordered_map<int, VehicleHealthSync> health_sync;
    std::unordered_map<int, int> last_damager; // vehicle handle -> attacker entity handle
    std::unordered_map<int, VehicleLethalKiller> lethal_killer;
    // What bounds a crash report and gates a run-over; fed by received rows and local bodies.
    std::unordered_map<int, VehicleObservedSpeed> observed_speed;
    std::unordered_map<int, VehicleHealth> health; // client side
    std::unordered_map<int, VehicleAmmoMirror> ammo_mirror; // client side
    // What arrived for a vehicle this machine watches, and what it last sent for the one it drives.
    std::unordered_map<int, VehicleOrientSupplement> orient;
    std::unordered_map<int, VehicleOrientSend> orient_sent;
    // Server: coast/sink state of fully-unmanned vehicles. Empty for a vehicle at rest.
    std::unordered_map<int, VehicleKinematics> kinematics;
    // Server: an entry is created when the hull is spawned and lives as long as the vehicle does.
    std::unordered_map<int, VehicleRegen> regen;
    // Server: how long each vehicle has had nothing beneath it, for the void-fall death.
    std::unordered_map<int, float> void_timer;
    // Server: an entry exists only while the hull is continuously in its lethal medium - under water
    // for a non-sub, out of it for a sub.
    std::unordered_map<int, int64_t> hazard_since;
    // Factory-spawned hull attributes; team is NOT obj->team, which feeds the engine's friendliness
    // rules. Maintained on the server and mirrored on clients, both keyed by local object handle.
    std::unordered_map<int, VehicleState> hull_state;
    // Client: hulls whose last stated occupancy named a rider this machine cannot resolve yet.
    std::unordered_map<int, VehiclePendingSeats> pending_seats;
    std::unordered_map<int, rf::Timestamp> orient_relay_cooldown; // by player id
    // Server: bounds one sender's roadkill traffic whatever it names, unlike crush_cooldown below.
    std::unordered_map<int, rf::Timestamp> crush_report_cooldown; // by player id
    // One roadkill per victim per cooldown: one contact can span several physics substeps.
    std::unordered_map<int, rf::Timestamp> crush_cooldown; // by victim entity handle
    // One crash per hull per cooldown; the client keeps a slightly longer window to spare the report.
    std::unordered_map<int, rf::Timestamp> crash_cooldown; // by vehicle handle
    // Server: bounds one sender's crash reports, apart from his roadkill reports.
    std::unordered_map<int, rf::Timestamp> crash_report_cooldown; // by player id
    // Server: an entry exists only inside the memory window.
    std::unordered_map<int, VehicleCoastMemory> coast_memory;
    std::map<VehicleRamKey, VehicleRamPair> ram_pairs;
    std::vector<VehicleSpawnSlot> factories;
    // Client: one entry per g_vehicle_factories record, sized by vehicle_level_init_post.
    std::vector<VehicleFactoryUi> factory_ui;
    std::unordered_map<int, VehicleWheelSpin> wheel_spin;
    std::unordered_map<int, VehicleTreadScroll> tread_scroll;
    // Client: the trigger reports currently standing with the server, one per trigger.
    int reported_fire_vehicle = -1;
    bool reported_primary_held = false;
    bool reported_alt_held = false;
    // Client: the turret the local operator has zoomed in, -1 for none, and last frame's alt trigger.
    int turret_zoom_handle = -1;
    bool turret_zoom_alt_down = false;
    // Server: an entry exists only while the jeep's driver holds the horn.
    std::unordered_map<int, VehicleHorn> horn;
    std::unordered_map<int, rf::Timestamp> horn_cooldown; // by player id
    // Client: the horn edge currently standing with the server.
    int reported_horn_vehicle = -1;
    bool reported_horn_held = false;
    // Use-request cadence, both sides of the wire. Server: one window per player id covering EVERY
    // af_client_req use subtype. Client: the local window on the seat hotkeys.
    std::unordered_map<int, rf::Timestamp> use_cooldown;
    rf::Timestamp seat_swap_local_cooldown;
    // Client: which seat hotkeys (1..6 -> bit 0..5) were down last frame, for edge detection.
    uint8_t seat_key_down_mask = 0;
    // Client: the 10 Hz $Use probe cache and the prompt string derived from it.
    int64_t use_probe_next_ms = 0;
    rf::Vector3 use_probe_pos{};
    int use_probe_handle = -1;
    int use_probe_tag = -1;
    std::string use_prompt_text;
    int use_prompt_class = -1;
    int use_prompt_reason = -1;
    // Client: when unmanned hulls next re-look-up their room.
    int64_t next_room_check_ms = 0;
};

// Server roadkill floor, world u/s: the hull's own speed and its closing speed on the victim.
inline constexpr float vehicle_crush_min_speed = 1.5f;

extern VehicleModuleState g_vehicle_state;

// Parsed from the RFL before the module's level init, so the level loader clears it instead.
extern std::vector<AlpineVehicleFactoryInfo> g_vehicle_factories;

// Resolve a handle to a vehicle hull: null unless it names a live entity of a synced vehicle type.
inline rf::Entity* vehicle_synced_entity(int handle)
{
    rf::Entity* ep = rf::entity_from_handle(handle);
    return vehicle_is_synced_entity_type(ep) ? ep : nullptr;
}

// As above, and null once the hull has begun dying.
inline rf::Entity* vehicle_live_synced_entity(int handle)
{
    rf::Entity* ep = vehicle_synced_entity(handle);
    return ep && !rf::entity_is_dying(ep) ? ep : nullptr;
}

rf::Vector3 vehicle_matrix_phb(const rf::Matrix3& orient);
bool vehicle_class_open_seats(const rf::Entity* vehicle);
void vehicle_init_synced_entity(rf::Entity* ep);
// Server: capture this hull's per-weapon ammo ceiling at creation, beside the life ceiling.
void vehicle_capture_spawn_ammo(rf::Entity* ep);
void vehicle_ammo_regen_runtime_reset();
// Server: the local clock time this weapon slot refills at, 0 when no refill is pending.
int64_t vehicle_ammo_refill_due_ms(const rf::Entity* ep, int slot_index);
void track_synced_entity(rf::Entity* ep);
// Seats this hull offers, capped at what the occupancy packet can state; vehicle_seat refuses the rest.
int vehicle_seat_count(const rf::Entity* vehicle);
const rf::EntityInterfacePoint* vehicle_seat(const rf::Entity* vehicle, int index);
int vehicle_seat_leech(const rf::Entity* vehicle, int index);
rf::Entity* vehicle_driver_entity(const rf::Entity* vehicle);

// The local player's held vehicle controls count: active gameplay with no menu, console or chat on
// top. game_paused alone is never set in multiplayer (0x00436270 returns on is_multi).
bool vehicle_local_input_live();

// This hull's replicated attributes, or null when it has none (a dbg_vehicle_spawn hull).
inline VehicleState* vehicle_hull_state(int vehicle_handle)
{
    auto it = g_vehicle_state.hull_state.find(vehicle_handle);
    return it == g_vehicle_state.hull_state.end() ? nullptr : &it->second;
}

// This hull's orient supplement, or null unless one exists AND holds a valid angle sample.
inline VehicleOrientSupplement* vehicle_orient_supplement(int vehicle_handle)
{
    auto it = g_vehicle_state.orient.find(vehicle_handle);
    return (it == g_vehicle_state.orient.end() || !it->second.valid) ? nullptr : &it->second;
}

// The player riding seat `seat` of this hull, or null for an empty seat or an unresolvable rider.
rf::Player* vehicle_seat_occupant_player(const rf::Entity* vehicle, int seat);

// Server: the factory slot whose live hull this is, or null.
VehicleSpawnSlot* vehicle_slot_for_hull(int vehicle_handle);
// Any seat the occupancy packet can state holds a rider.
bool vehicle_hull_occupied(const rf::Entity* vehicle);
// Server: re-state a turret factory whose hull just changed occupancy; other slots are unaffected.
void vehicle_slot_announce_for_hull(int vehicle_handle);
// Server: every boarding - the hull takes the boarder's team, clears the auto-return, and the first
// one announces the factory as TAKEN.
void vehicle_on_hull_boarded(rf::Entity* vehicle, const rf::Entity* rider);
// THE af_vehicle_state attribute derivation, shared by both senders; reads the hull's own state.
af_vehicle_state_attrs vehicle_build_hull_attrs(int vehicle_handle);
