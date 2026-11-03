#pragma once

struct GSolid;

// Level > Repair holes when building
bool build_repair_enabled();
void build_repair_toggle();
// Level > Rebuild with out-of-level areas
bool build_out_of_level_enabled();
void build_out_of_level_toggle();
// Level > Find out-of-level areas
void build_repair_find_out_of_level();

// Called around the room builder (0x00485990)
void build_repair_before_rooms(GSolid* solid);
void build_repair_after_rooms(GSolid* solid);

void ApplyBuildRepairPatches();
