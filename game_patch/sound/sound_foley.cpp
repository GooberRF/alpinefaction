#include <patch_common/CodeInjection.h>
#include "sound_foley.h"
#include "../misc/destruction.h"
#include "../rf/sound/sound.h"
#include "../rf/geometry.h"

// Pre-built impact sound sets for materials that don't have stock Custom Sound Sets.
// Built once after foley.tbl loads; persist for the game's lifetime.
rf::ImpactSoundSet* g_rock_debris_iss = nullptr;
rf::ImpactSoundSet* g_ice_debris_iss = nullptr;

static rf::ImpactSoundSet* build_custom_iss(const char* const* wavs, int count,
                                             float min_range, float base_volume)
{
    auto* iss = new rf::ImpactSoundSet{};
    for (int i = 0; i < count; i++) {
        iss->sounds[i] = rf::snd_get_handle(wavs[i], min_range, base_volume, 1.0f);
    }
    iss->num_material_sounds[0] = count;
    iss->is_all_sounds = 1;
    return iss;
}

rf::ImpactSoundSet* resolve_material_iss(const BreakableMaterialConfig& cfg)
{
    if (cfg.impact_iss_ptr)
        return *cfg.impact_iss_ptr;
    if (cfg.impact_iss_name)
        return rf::material_find_impact_sound_set(cfg.impact_iss_name);
    return nullptr;
}

void play_material_break_sound(const BreakableMaterialConfig& cfg, BreakableMaterialState& state,
                               const rf::Vector3& pos)
{
    if (!state.break_foley_init) {
        state.break_foley_init = true;
        if (cfg.break_foley_name) {
            state.break_foley_id = rf::foley_lookup_by_name(cfg.break_foley_name);
        }
    }
    if (state.break_foley_id >= 0) {
        int snd_handle = rf::foley_get_sound_handle(state.break_foley_id);
        if (snd_handle >= 0) {
            rf::snd_play_3d(snd_handle, pos, 1.0f, rf::Vector3{0.0f, 0.0f, 0.0f}, rf::SOUND_GROUP_EFFECTS);
        }
    }
}

void sound_foley_level_cleanup(BreakableMaterialState* states, int count)
{
    for (int i = 0; i < count; i++) {
        states[i].break_foley_init = false;
        states[i].break_foley_id = -1;
    }
}

// Injection at 0x00467c1c: right after FUN_00467d00 returns from parsing foley.tbl.
// Builds custom ImpactSoundSets for materials that don't have stock Custom Sound Sets.
CodeInjection foley_iss_init_injection{
    0x00467c1c,
    [](auto& regs) {
        static constexpr const char* k_rock_wavs[] = {
            "Geodebris_01.wav", "Geodebris_02.wav",
            "Geodebris_03.wav", "Geodebris_04.wav",
        };
        g_rock_debris_iss = build_custom_iss(k_rock_wavs, std::size(k_rock_wavs), 5.0f, 0.9f);

        static constexpr const char* k_ice_wavs[] = {
            "Ice_Break_02.wav", "Ice_Break_03.wav",
            "Ice_Hit_Ice1.wav", "Ice_Hit_Ice2.wav",
        };
        g_ice_debris_iss = build_custom_iss(k_ice_wavs, std::size(k_ice_wavs), 5.0f, 0.9f);
    },
};

void sound_foley_apply_patches()
{
    foley_iss_init_injection.install();
}
