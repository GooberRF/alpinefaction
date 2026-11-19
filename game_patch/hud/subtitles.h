#pragma once

#include "../rf/math/vector.h"

// Subtitles for ambient NPC voice lines and for the in-engine cutscenes, neither
// of which the original game captions at all. Driven by optional TOML tables in a
// packfile; with no table present the feature is off entirely.

// Called from the snd_play / snd_play_3d hooks. Alpine already hooks both, so the
// feature calls into them rather than installing a second FunHook on the same
// address -- two hooks on one function means whichever installs last silently wins.
void subtitles_on_sound_play(int handle, const rf::Vector3* pos);

// Called from the snd_music_play hook: a cutscene streams one pre-mixed track per
// scene, so its subtitles are timed against the start of playback.
void subtitles_cutscene_begin(const char* track);

// Called from both hud_msg_render and cutscene_do_frame -- the HUD is not drawn
// during cutscenes. Repeat calls within one frame are ignored.
void subtitles_render();
