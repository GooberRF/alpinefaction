#pragma once

#include <cstdint>
#include "../rf/math/vector.h"
#include "../rf/math/matrix.h"
#include "../rf/os/timestamp.h"

namespace rf
{
    struct Player;
    struct Entity;
    struct Item;
    struct VMesh;
}

enum class SalFlagState : uint8_t
{
    AtSpawn = 0,
    Carried = 1,
    Dropped = 2,
    Delayed = 3, // no flag item exists yet; spawn_delay_timer is running
};

struct SalvageInfo
{
    bool active = false;
    bool spawn_known = false;
    rf::Vector3 spawn_pos{};
    rf::Matrix3 spawn_orient{};
    bool bases_known = false;
    rf::Vector3 base_red_pos{};
    rf::Vector3 base_blue_pos{};
    int flag_item_type = -1;   // rf::item_lookup_type("flag_red")
    int flag_item_handle = -1; // server-side only
    rf::Player* carrier = nullptr;
    SalFlagState state = SalFlagState::Delayed;
    // Client-side: cleared at level init, set by the first state packet applied.
    // A late joiner starts at the default state with zero caps, so every field of
    // that first packet reads as a delta; the one-shot transition cues are only
    // inferred from the second packet onward.
    bool first_state_packet_applied = false;
    rf::Timestamp return_timer;        // Dropped -> AtSpawn
    rf::Timestamp spawn_delay_timer;   // Delayed -> AtSpawn
    rf::Timestamp pickup_unlock_timer; // brief IF_NO_PICKUP window after a spawn
    rf::Timestamp refresh_timer;       // periodic re-broadcast while a timer is ticking
    rf::Timestamp respawn_retry_timer; // throttles recovery attempts when item_create fails
    rf::Vector3 last_carrier_pos{};
    int red_caps = 0;
    int blue_caps = 0;
};

extern SalvageInfo g_salvage_info;

void salvage_level_init();
void salvage_level_init_post();
void salvage_do_frame();
// Client half of the per-frame flag attachment. Ticked from the game frame, next
// to the other gametype frame handlers; the server ticks it from salvage_do_frame.
void salvage_client_do_frame();
void salvage_on_entity_will_die(rf::Entity* ep);
void salvage_on_player_disconnect(rf::Player* player);
void salvage_handle_drop_flag_request(rf::Player* player);

// Reverts, then (re)applies the class-wide overrides Salvage puts on the flag_red
// item class: IIF_SPINS_IN_MULTI and the Salvage flag mesh. Must run after the
// engine's multi_ctf_level_init (which clears the spin bit) and before any flag
// item is created.
void salvage_apply_flag_class_overrides();

// Drops the flag_red class overrides and every piece of local Salvage state when
// the local client leaves multiplayer. Called from multi_stop: the engine only runs
// its CTF teardown for an outgoing CTF game, so nothing else would put the flag
// class back before single player, and salvage_level_init - the only other reset -
// runs from multi_level_init, which is too late to stop the previous session's
// dynamic-light handle from being deleted a second time.
void salvage_on_multi_shutdown();

// Team capture counts. Setters are client-side only (the server owns the truth).
int salvage_get_red_team_score();
int salvage_get_blue_team_score();
void salvage_set_red_team_score(int v);
void salvage_set_blue_team_score(int v);

SalFlagState salvage_get_state();
rf::Player* salvage_get_carrier();
bool salvage_player_is_carrier(const rf::Player* player);
const rf::Vector3& salvage_get_spawn_pos();
bool salvage_spawn_is_known(); // false until the server has resolved/replicated the home
// Nothing about the bases is replicated: both sides resolve them from the
// level-placed base items. Servers do it eagerly at level init and lazily on
// first use before that; clients only lazily.
bool salvage_get_base_positions(rf::Vector3* out_red, rf::Vector3* out_blue);
// Base the local player's team delivers to. False when there is no local player
// or the level is missing a base.
bool salvage_get_local_team_base_pos(rf::Vector3* out_pos);

// Milliseconds left on whichever timer the current state is running, or 0.
int salvage_get_time_left_ms();

// Client helpers.
// Position of the flag *on the ground*: false unless the state is AtSpawn or
// Dropped, so a flag riding on a carrier's back is never mistaken for a pickup.
bool salvage_get_client_flag_pos(rf::Vector3* out_pos);
// Position of the flag item whatever its state. Server-authoritative when called
// on a server; used to replicate the flag position with the state packet.
bool salvage_get_flag_pos(rf::Vector3* out_pos);
bool salvage_is_flag_item(const rf::Item* item);
bool salvage_viewer_is_carrier_first_person();
// The flag's mesh and world transform for the renderer's x-ray outline. The flag is
// a .vfx (MESH_TYPE_ANIM_FX) mesh, so this hands over the VMesh rather than a
// VifLodMesh like the bagman queries do. False when there is nothing to outline:
// no flag item yet, a state with no flag in the world, or the viewer is the carrier
// looking out of their own eyes.
bool salvage_query_flag_outline(rf::VMesh** out_vmesh, rf::Vector3* out_pos, rf::Matrix3* out_orient);
// Glues the carried flag item to the carrier's $prop_flag, the same alignment
// stock CTF performs in multi_ctf_move_flags. Runs on both sides: the attachment
// is local and derives entirely from the replicated carrier entity.
void salvage_move_carried_flag();
void salvage_play_return_sound();
// Green pulsing point light on the carrier (Carried) or on the flag item
// (AtSpawn/Dropped). Client-side and per-frame: delete + recreate, exactly like
// bagman_update_dynamic_light. Ticked from hud_world_do_frame.
void salvage_update_dynamic_light();

// Server -> client state replication.
void salvage_broadcast_state();
void salvage_force_state_sync_to(rf::Player* player);

// Applied by af_process_salvage_state_packet on clients. flag_pos is the server's
// authoritative flag item position: the item is no longer recreated on each
// transition, so clients need it to place a dropped flag.
void salvage_apply_state_from_packet(uint8_t state, uint8_t carrier_player_id, uint16_t time_left_ms,
                                     uint16_t red_caps, uint16_t blue_caps, const rf::Vector3& spawn_pos,
                                     const rf::Vector3& flag_pos);

// Server-side transition entry points invoked from the engine hooks.
void salvage_on_flag_touch(rf::Player* player, rf::Item* item);
void salvage_on_base_touch(rf::Player* player, rf::Item* item);

void salvage_do_patch();
