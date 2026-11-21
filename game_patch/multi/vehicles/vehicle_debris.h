#pragma once

namespace rf
{
    struct Entity;
}

// Client-side wreckage for multiplayer vehicles: a dying hull breaks into the pieces of its debris mesh and its
// attached parts, keeping its team colours, some of them burning. Every machine throws its own, so nothing is sent on
// the wire and nothing exists on a dedicated server.

// From the hull's death on every machine, before entity_die frees the seats, so the pieces keep the team art its
// driver gave the hull.
void vehicle_debris_spawn(rf::Entity* hull);
void vehicle_debris_do_frame();
void vehicle_debris_clear();
// The team a debris piece was thrown with, or -1 for any other object.
int vehicle_debris_team(int debris_handle);
// Also gives a hull that dies out of the liquid it was made for a dry explosion, on every machine.
void vehicle_debris_install();
