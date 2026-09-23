#pragma once

#include <cstddef>
#include "../rf/file/file.h"

namespace rf
{
    struct Vector3;
}

void alpine_rope_load_chunk(rf::File& file, std::size_t chunk_len);

// Resolves target uids to handles and builds the first geometry. Must run after the level's
// objects exist, i.e. from level_init_post.
void alpine_rope_level_init();
void alpine_rope_clear_state();

void alpine_rope_do_frame();
void alpine_rope_render();

// True when the handle is a rope's anchor clutter. Lets the Rope_State event touch only ropes.
bool alpine_rope_is_rope(int handle);

// Client side cosmetic shove for dynamic ropes. `strength` is the explosion's damage value.
void alpine_rope_apply_explosion(const rf::Vector3& pos, float radius, float strength);

void alpine_rope_apply_patch();
