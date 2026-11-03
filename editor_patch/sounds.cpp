#include <string>
#include "sounds.h"
#include "search_paths.h"

namespace
{

// The game also plays .ogg. RED's own loader (0x00512C50) reads RIFF WAVE only and fails cleanly on
// anything else, so an .ogg found here just doesn't preview.
SearchPathTree g_sound_paths{".wav .ogg", "sound"};

} // namespace

void sounds_init_paths()
{
    g_sound_paths.init({"user_maps\\sounds"}, {"user_maps\\sounds"});
}

void reload_custom_sounds()
{
    g_sound_paths.reload();
}

std::string find_sound_on_disk(const char* filename)
{
    return g_sound_paths.find_on_disk(filename);
}
