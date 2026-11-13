#pragma once

#include "vehicle_internal.h"

// Clear a host_handle no longer naming a live synced vehicle that holds this rider; returns whether
// it did.
bool vehicle_clear_stale_host(rf::Entity* rider);
// Server: per-frame sweep freeing any rider whose vehicle is gone, dying or no longer holds him.
void vehicle_release_orphaned_riders();

// Server: the INVOLUNTARY exit - the voluntary one is vehicle_server_handle_exit, which additionally
// carries the exit velocity and the coast claim.
void vehicle_server_free_seat(rf::Entity* vehicle, int seat_index, int rider_handle);

// Client: keys 1..6 request seat 0..5 of the vehicle the local player is already in.
void vehicle_poll_seat_hotkeys();

// Client, once per frame: the under-reticle Use prompt for the vehicle the local player is looking
// at, or empty when none belongs on screen. Answered from the stock use probe and the server's own
// entry rule, so the prompt cannot disagree with what a Use press would do.
const std::string& vehicle_use_prompt_text();

// Client: drop PF_IN_ENCLOSED_VEHICLE from the local Player, which outlives the entity that was
// riding. Level teardown destroys a seated entity without running any exit path.
void vehicle_clear_local_enclosed_flag();

void vehicle_seats_apply_patch();
