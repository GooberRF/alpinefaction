#pragma once

namespace rf
{
    struct ImpactSoundSet;
    struct Vector3;
}
struct BreakableMaterialConfig;
struct BreakableMaterialState;

// Pre-built ISS globals (built after foley.tbl loads, persist for game lifetime).
// Config structs point to these via impact_iss_ptr.
extern rf::ImpactSoundSet* g_rock_debris_iss;
extern rf::ImpactSoundSet* g_ice_debris_iss;

// Resolve the debris impact sound set for a material config.
// Returns existing ISS from foley.tbl (impact_iss_name) or pre-built ISS (impact_iss_ptr).
rf::ImpactSoundSet* resolve_material_iss(const BreakableMaterialConfig& cfg);

// Play the break foley sound for a material at a world position.
void play_material_break_sound(const BreakableMaterialConfig& cfg, BreakableMaterialState& state,
                               const rf::Vector3& pos);

// Reset foley state on level load (clears cached foley IDs for re-lookup).
void sound_foley_level_cleanup(BreakableMaterialState* states, int count);

// Install foley-related patches.
void sound_foley_apply_patches();
