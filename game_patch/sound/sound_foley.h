#pragma once

namespace rf
{
    struct ImpactSoundSet;
    struct Vector3;
}

// Get the rock debris impact sound set (Geodebris wav files).
// Built once after foley.tbl loads; returns nullptr before that.
rf::ImpactSoundSet* get_rock_debris_iss();

// Play the rock break foley sound at a world position.
void play_rock_break_sound(const rf::Vector3& pos);

// Reset foley state on level load.
void sound_foley_level_cleanup();

// Install foley-related patches.
void sound_foley_apply_patches();
