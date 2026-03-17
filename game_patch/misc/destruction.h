#pragma once

#include <cstdint>

// Forward declarations
namespace rf {
    struct ImpactSoundSet;
    enum class DetailMaterial : uint8_t;
}

// Per-material debris subdivision configuration
struct DebrisConfig {
    float min_bsphere_radius; // stop subdividing when piece is smaller than this
    int max_subdivisions;     // max number of boolean cuts
    int min_faces_to_split;   // minimum face count to attempt boolean split
};

// Per-material configuration for breakable detail brushes (all non-glass materials).
// One static instance per material type, indexed by (DetailMaterial - 1).
struct BreakableMaterialConfig {
    // Damage resistance
    float damage_factors[12];        // DT_COUNT entries, per damage-type multiplier (0.0-1.0)
    float direct_hit_factor;         // multiplier for projectile direct hits

    // Debris geometry
    DebrisConfig debris;             // subdivision params
    const char* cap_texture;         // texture for cap faces on debris chunks
    float cap_texels_per_meter;      // UV density for cap faces

    // Debris physics
    float upward_velocity;           // m/s upward on break
    float horizontal_scatter;        // m/s random horizontal spread
    float explosion_push_speed;      // m/s outward from explosion center

    // Sound — break event (foley.tbl entity sound)
    const char* break_foley_name;    // foley name to play on break

    // Sound — debris impact ISS (one of two modes)
    const char* impact_iss_name;     // if non-null: look up from foley Custom Sound Sets at init
    rf::ImpactSoundSet** impact_iss_ptr; // if non-null: use this pre-built ISS (for Rock/Ice)

    // Particle/explosion effect
    const char* explosion_name;      // explosion.tbl particle name
};

// Per-material runtime state (lazy-initialized, reset on level load).
struct BreakableMaterialState {
    int cap_bm = -1;
    bool cap_bm_loaded = false;
    int break_foley_id = -1;
    bool break_foley_init = false;
};

// Lookup config/state by material type. Returns nullptr for Glass.
const BreakableMaterialConfig* get_material_config(rf::DetailMaterial mat);
BreakableMaterialState* get_material_state(rf::DetailMaterial mat);

void destruction_do_patch();
void destruction_level_cleanup();
void apply_geoable_flags();
void apply_breakable_materials();
void g_solid_set_rf2_geo_limit(int limit);
