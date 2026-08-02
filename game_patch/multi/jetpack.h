#pragma once

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
void jetpack_level_init();
void jetpack_do_frame();

// Start or stop the visual/audible thruster effects on any entity, local or
// remote. Safe to call redundantly.
void jetpack_apply_entity_thrust(rf::Entity* ep, bool on);

// Server side of a client's thrust state report. Rebroadcasts at most one state
// per player per throttle interval; identical repeats cost nothing, and anything
// held back is settled by the flush in jetpack_do_frame.
void jetpack_server_on_state_request(rf::Player* player, bool on);

// Draws the cosmetic pack on an entity's back.
void jetpack_render_attachment(rf::Entity* ep);

// Vertical fuel bar plus the discoverability overlays (the labelled spawn
// reveal and the first-use keybind hint). Drawn from the multiplayer HUD render
// path in multiplayer, and from the shared hud_render path in single player.
void jetpack_render_hud();

// Registers the single player `jetpack` console command.
void jetpack_apply_patch();
