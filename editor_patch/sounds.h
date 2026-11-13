#pragma once

#include <string>

// Registers user_maps\sounds and every subdirectory of it (any depth) as search paths for sound files.
void sounds_init_paths();
// Registers subdirectories created since startup and rescans every sound search path.
void reload_custom_sounds();

// Full path of a .wav or .ogg file of this name loose under user_maps\sounds or any subdirectory of
// it, or empty. Stock sounds only exist inside .vpp archives and are never found here.
std::string find_sound_on_disk(const char* filename);
