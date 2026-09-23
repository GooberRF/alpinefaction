#pragma once

// Client-side respawn signposting for Vehicle Factories: a world marker per factory plus a ghost
// hull that fills up as the respawn delay runs down.

namespace rf
{
    struct GRoom;
}

// Drops mesh pointers the level teardown already freed.
void vehicle_markers_level_init();
// After vehicle_tbl_overrides_level_init_post: the hull mesh names are only final once it has run.
void vehicle_markers_level_init_post();
// Draws the markers that live in this room, from the room pass, before its liquid surface.
void vehicle_markers_render_room(rf::GRoom* room);
// Late pass: whatever the room passes did not draw (a marker whose room never rendered).
void vehicle_markers_render();
