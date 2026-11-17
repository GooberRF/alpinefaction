#pragma once

#include <cstdint>

namespace rf
{
    struct Entity;
    struct Player;
}

// Jetpacks mutator. Every player wears a jetpack; holding jump while falling
// burns fuel to thrust upward. The movement is entirely client-authoritative —
// the server only advertises the mutator and relays thrust on/off state so the
// steam and engine sound show up for everybody.

// True when the jetpack should be running for the local session: the server
// mutator in multiplayer, the `jetpack` console command in single player.
bool jetpacks_are_active();
// The mutator or command above, or `ep` holding the Jetpack powerup.
bool jetpack_is_active_for(const rf::Entity* ep);
// True while any entity may be flying, which is when the jetpack has per-frame work to do.
bool jetpacks_in_play();
// True when the fuel gauge can show for the local or spectated player.
bool jetpack_gauge_may_show();
// The Jetpack powerup was picked up by, or has left, the entity.
void jetpack_on_powerup_changed(int entity_handle, bool granted);
void jetpack_level_init();
void jetpack_do_frame();
// Drops every viewer-side fuel estimate, so the spectate gauge waits for a fresh anchor.
void jetpack_reset_remote_fuel();

// Start or stop the visual/audible thruster effects on any entity, local or
// remote. Safe to call redundantly.
void jetpack_apply_entity_thrust(rf::Entity* ep, bool on);

// Remote thrust transition with the owner's reported fuel: drives the effects and
// anchors the viewer-side fuel estimate behind the spectate/demo gauge.
void jetpack_on_remote_state(rf::Entity* ep, bool on, uint8_t fuel_pct);

// Server side of a client's thrust state report. Rebroadcasts at most one state
// per player per throttle interval; identical repeats cost nothing, and anything
// held back is settled by the flush in jetpack_do_frame.
void jetpack_server_on_state_request(rf::Player* player, bool on, uint8_t fuel_pct);

// Draws the cosmetic pack on an entity's back.
void jetpack_render_attachment(rf::Entity* ep);

// Vertical fuel bar plus the discoverability overlays (the labelled spawn
// reveal and the first-use keybind hint). Drawn from the multiplayer HUD render
// path in multiplayer, and from the shared hud_render path in single player.
void jetpack_render_hud();
// Screen y of the fuel gauge's label top, where the gauge draws when shown.
int jetpack_fuel_gauge_label_top_y();

// Registers the single player `jetpack` console command.
void jetpack_apply_patch();
