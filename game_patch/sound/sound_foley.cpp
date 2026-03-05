#include <patch_common/CodeInjection.h>
#include "sound_foley.h"
#include "../rf/sound/sound.h"

// Impact sound set for rock debris chunks, matching "Geomod Debris Hit" from foley.tbl.
// Built once after foley.tbl loads using the same Geodebris wav files.
static rf::ImpactSoundSet* g_rock_debris_iss = nullptr;

// Rock break foley sound
static int g_rock_break_foley_id = -1;
static bool g_rock_break_foley_init = false;

rf::ImpactSoundSet* get_rock_debris_iss()
{
    return g_rock_debris_iss;
}

void play_rock_break_sound(const rf::Vector3& pos)
{
    if (!g_rock_break_foley_init) {
        g_rock_break_foley_init = true;
        g_rock_break_foley_id = rf::foley_lookup_by_name("Geomod Debris Hit");
        if (g_rock_break_foley_id < 0) {
            g_rock_break_foley_id = rf::foley_lookup_by_name("Glass Smash");
        }
    }
    if (g_rock_break_foley_id >= 0) {
        int snd_handle = rf::foley_get_sound_handle(g_rock_break_foley_id);
        if (snd_handle >= 0) {
            rf::snd_play_3d(snd_handle, pos, 1.0f, rf::Vector3{0.0f, 0.0f, 0.0f}, rf::SOUND_GROUP_EFFECTS);
        }
    }
}

void sound_foley_level_cleanup()
{
    g_rock_break_foley_init = false;
    g_rock_break_foley_id = -1;
}

// Injection at 0x00467c1c: right after FUN_00467d00 returns from parsing foley.tbl.
// Builds a custom ImpactSoundSet with the Geodebris wav files for rock debris impact sounds.
CodeInjection rock_debris_iss_init_injection{
    0x00467c1c,
    [](auto& regs) {
        static constexpr const char* k_geodebris_wavs[] = {
            "Geodebris_01.wav", "Geodebris_02.wav",
            "Geodebris_03.wav", "Geodebris_04.wav",
        };
        constexpr int num_sounds = std::size(k_geodebris_wavs);
        g_rock_debris_iss = new rf::ImpactSoundSet{};
        for (int i = 0; i < num_sounds; i++) {
            g_rock_debris_iss->sounds[i] = rf::snd_get_handle(k_geodebris_wavs[i], 5.0f, 0.9f, 1.0f);
        }
        g_rock_debris_iss->num_material_sounds[0] = num_sounds;
        g_rock_debris_iss->is_all_sounds = 1;
    },
};

void sound_foley_apply_patches()
{
    rock_debris_iss_init_injection.install();
}
