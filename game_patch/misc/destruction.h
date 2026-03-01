#pragma once

// Per-material debris subdivision configuration
struct DebrisConfig {
    float min_bsphere_radius; // stop subdividing when piece is smaller than this
    int max_subdivisions;     // max number of boolean cuts
    int min_faces_to_split;   // minimum face count to attempt boolean split
};

void destruction_do_patch();
void destruction_level_cleanup();
void apply_geoable_flags();
void apply_breakable_materials();
void g_solid_set_rf2_geo_limit(int limit);
