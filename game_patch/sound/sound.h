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

// Named custom sound IDs, keep in sync with the order of entries in gamesound_parse_custom_sounds.
// Dedicated servers don't load custom sound IDs, so we need to hardcode the refs.
namespace custom_sound_id
{
    constexpr int af_achievement       = 0;
    constexpr int af_ping_location     = 1;
    constexpr int af_hit_sound         = 2;
    constexpr int af_kill_sound        = 3;
    constexpr int console_large_03     = 4;  // Console_Large_03.wav
    constexpr int ann_time_expired     = 5;  // MP_ANN_04.wav
    constexpr int ann_match_over       = 6;  // MP_ANN_06.wav
    constexpr int ann_five_kills_left  = 7;  // MP_ANN_10.wav
    constexpr int ann_one_kill_left    = 8;  // MP_ANN_11.wav
}

// Hardcoded stock sounds.tbl indices
namespace stock_sound_id
{
    constexpr int end_voice            = 4;    // end_voice.wav
    constexpr int beep_01              = 29;   // Beep01.wav
    constexpr int jolt_01              = 35;   // Jolt_01.wav
    constexpr int menu_select          = 41;   // menu_select.wav
    constexpr int panel_highlight      = 42;   // panel_highlight.wav
    constexpr int panel_button_click   = 43;   // panel_button_click.wav
    constexpr int checkbox_on          = 44;   // checkbox_on.wav
    constexpr int checkbox_off         = 45;   // checkbox_off.wav
    constexpr int flag_respawn         = 65;   // Flag_Respawn.wav
    constexpr int ann_game_over        = 78;   // MP_ANN-GO.wav
    constexpr int ann_winner           = 80;   // MP_ANN_ALLYOURBASE.wav
}
