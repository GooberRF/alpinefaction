#pragma once

// Forward declarations
namespace rf
{
    struct Vector3;
}

struct CustomSoundEntry
{
    const char* filename;
    float min_range;
    float base_volume;
    float rolloff;
};

void disable_sound_before_cutscene_skip();
void enable_sound_after_cutscene_skip();
void set_sound_enabled(bool enabled);
int get_custom_sound_id(int custom_id);
void play_local_sound_2d(uint16_t sound_id, int group, float volume);
void play_local_sound_3d(uint16_t sound_id, rf::Vector3 pos, int group, float volume);
void play_chat_sound(std::string_view msg, bool is_taunt);
