#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <common/rope_curve.h>
#include "../rf/file/file.h"
#include "../rf/math/vector.h"

void alpine_rope_load_chunk(rf::File& file, std::size_t chunk_len);

// Resolves target uids to handles and builds the first geometry. Must run after the level's
// objects exist, i.e. from level_init_post.
void alpine_rope_level_init();
void alpine_rope_clear_state();

void alpine_rope_do_frame();
void alpine_rope_render();

// True when the handle is a rope's anchor clutter. Lets the Rope_State event touch only ropes.
bool alpine_rope_is_rope(int handle);

struct AlpineRopeState
{
    int32_t uid = -1;
    bool on = true;
};

// Server: the current on/off state of every rope switched since the level loaded (or only such zip
// lines). A joining client only replays one-shot triggers, so whatever else switched a rope has to
// reach it this way; ropes nothing has switched cost nothing.
void alpine_rope_collect_states(std::vector<AlpineRopeState>& out, bool ziplines_only);
// From obj_hide/obj_unhide: notes a rope whose anchor was switched. The load-time hide of an initially
// off rope runs before the rope is registered, so it does not count.
void alpine_rope_on_anchor_toggled(int handle);
// Switches the rope whose anchor clutter is `handle` on or off.
void alpine_rope_set_anchor_on(int handle, bool on);
// Client: switches the rope with this uid on or off, as Rope_State would.
void alpine_rope_apply_state(int32_t uid, bool on);

// Client side cosmetic shove for dynamic ropes. `strength` is the explosion's damage value.
void alpine_rope_apply_explosion(const rf::Vector3& pos, float radius, float strength);

// A rope flagged as a zip line, solved against its current endpoints. Arc length runs from the
// emitter end (0) to the target end (curve.length).
struct AlpineZipline
{
    int32_t uid = -1;
    float down_speed = rope_curve::zip_default_down_speed;
    float up_speed = rope_curve::zip_default_up_speed;
    rope_curve::ZipDirection direction = rope_curve::ZipDirection::two_way;
    rope_curve::Curve curve;
    // Uniform arc-length samples of `curve`, sample_step apart, for nearest-point queries.
    std::vector<rf::Vector3> points;
    float sample_step = 0.0f;
    rf::Vector3 aabb_min{};
    rf::Vector3 aabb_max{};
};

// Re-solves every zip line against its current endpoints. Runs on every machine, renderer or not.
void alpine_rope_update_ziplines();
// Zip lines that can be ridden right now: switched on, target resolved. Valid until the next update.
const std::vector<const AlpineZipline*>& alpine_rope_active_ziplines();
const AlpineZipline* alpine_rope_find_zipline(int32_t uid);
// Every zip line with a resolved target, switched on or not: what waypoint generation lays rides along.
std::vector<const AlpineZipline*> alpine_rope_all_ziplines();

void alpine_rope_apply_patch();
