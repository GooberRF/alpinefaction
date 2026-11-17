#pragma once

// Client-side cosmetic particles, sounds and lights for multiplayer vehicles: wheel and tread dust,
// impacts, scrape sparks, fighter downwash, water spray and wakes, and damage smoke and fire. Every
// machine derives them from what it already sees of the hull, so nothing is sent on the wire.

void vehicle_fx_level_init(bool load_assets);
// Drops every hull's effect state, e.g. when a demo seek jumps the hulls.
void vehicle_fx_clear();
void vehicle_fx_do_frame();
void vehicle_fx_install();
