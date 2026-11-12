#pragma once

// Client-side cosmetic wingtip trails for multiplayer flyers: a vapour ribbon from each wingtip of a hull flying fast,
// brighter in hard turns. Derived from the hull's own motion, so nothing is sent on the wire.

// Drops every trail, e.g. at level init or when a demo seek jumps the hulls.
void vehicle_trails_clear();
void vehicle_trails_do_frame();
void vehicle_trails_render();
