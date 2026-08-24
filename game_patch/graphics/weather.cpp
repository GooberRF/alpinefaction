#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>
#include <xlog/xlog.h>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <patch_common/Traits.h>
#include "gr.h"
#include "weather.h"
#include "../main/main.h"
#include "../misc/alpine_settings.h"
#include "../multi/demo/demo.h"
#include "../os/console.h"
#include "../os/os.h"
#include "../rf/bmpman.h"
#include "../rf/collide.h"
#include "../rf/gameseq.h"
#include "../rf/geometry.h"
#include "../rf/gr/gr.h"
#include "../rf/math/matrix.h"
#include "../rf/os/frametime.h"
#include "../rf/particle_emitter.h"
#include "../rf/player/camera.h"
#include "../rf/player/player.h"
#include "../object/object.h"

namespace
{

constexpr float two_pi = 6.2831855f;
constexpr float max_frame_delta = 0.1f;
constexpr float liquid_surface_margin = 0.1f;

constexpr int plankton_reseed_gap_ms = 250;
constexpr float plankton_reseed_move_dist = 15.0f;

// How far past its own sim box a region stays awake. The stock radius was 35 against a rain box of
// 26, so an auto activation distance keeps that same 9 unit margin whatever the box ends up being.
constexpr float weather_activity_margin = 9.0f;
constexpr int weather_visible_frame_slack = 1;
constexpr float weather_min_box_extent = 0.01f;

// Generous ceilings for RFL-authored floats - wide enough that no sane level notices them, narrow
// enough that a hostile one cannot hand the sim a value it turns into garbage geometry.
constexpr float weather_max_dim = 10000.0f;
constexpr float weather_max_speed = 10000.0f;
constexpr float weather_max_streak_seconds = 10.0f;
constexpr float weather_max_active_distance = 1000.0f;
constexpr float weather_min_visible_distance = 4.0f;
constexpr float weather_max_visible_distance = 200.0f;

// The blocked-by-geometry ceiling cache. One downward trace per (x, z) column, filled lazily under a
// per-frame budget shared by every region.
constexpr float weather_column_width_min = 0.25f;
constexpr float weather_column_width_max = 8.0f;
constexpr int weather_column_max_cells_axis = 512;
constexpr int weather_column_trace_budget = 128;
constexpr int weather_column_geomod_delay_frames = 30;

constexpr int rain_count = 4096;
constexpr float rain_box_half_x = 26.0f;
constexpr float rain_box_half_y = 18.0f;
constexpr float rain_box_half_z = 26.0f;
constexpr float rain_streak_half_width = 0.0125f;

constexpr int snow_count = 3072;
constexpr float snow_box_half_x = 22.0f;
constexpr float snow_box_half_y = 14.0f;
constexpr float snow_box_half_z = 22.0f;
constexpr float snow_drift_speed = 0.25f;
constexpr float snow_spin_min = 0.5f;
constexpr float snow_spin_max = 1.5f;

// Every active region draws out of one shared arena instead of owning a fixed pool, so the number of
// regions a level can run at once is bounded by a particle budget rather than by a slot count.
constexpr int weather_arena_count = 16384;
constexpr float weather_repartition_drift = 0.25f;

// Nominal density per type: the stock pool spread over the full camera box for that type. A segment
// carries this density (times the region's density_scale) over its own sim volume, so a region looks
// exactly as dense as it did when every region owned a full camera-box pool.
constexpr float rain_density = rain_count / (rain_box_half_x * rain_box_half_y * rain_box_half_z * 8.0f);
constexpr float snow_density = snow_count / (snow_box_half_x * snow_box_half_y * snow_box_half_z * 8.0f);

// Untextured world-space quad: TEXTURE_SOURCE_NONE selects the diffuse colour source directly, so
// the tmapper takes colour and alpha from gr::set_color. Depth is read but not written so
// overlapping streaks blend instead of occluding each other.
constexpr rf::gr::Mode rain_streak_mode{
    rf::gr::TEXTURE_SOURCE_NONE,
    rf::gr::COLOR_SOURCE_VERTEX,
    rf::gr::ALPHA_SOURCE_VERTEX,
    rf::gr::ALPHA_BLEND_ALPHA,
    rf::gr::ZBUFFER_TYPE_READ,
    rf::gr::FOG_NOT_ALLOWED,
};

struct WeatherParticle
{
    rf::Vector3 pos;
    float phase;
    float drift_x;
    float drift_z;
    float angle;
    float spin;
};

struct WeatherBox
{
    rf::Vector3 center;
    float half_x;
    float half_y;
    float half_z;
};

// Values are explicit and stable: this becomes an editor dropdown and an RFL field. New types append.
enum class WeatherRegionType : int
{
    rain = 0,
    snow = 1,
};

// Same deal as the type. Future shapes (cylinder) append.
enum class WeatherRegionShape : int
{
    box = 0,
    sphere = 1,
};

// A mapper-placed weather volume. Defaults are the tuned constants the room-flag version used.
// The type has no default - whoever creates the region picks one.
struct WeatherRegion
{
    int uid = -1;
    bool enabled = true;
    rf::Vector3 center{};
    rf::Matrix3 orient{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    rf::Vector3 half_extents{1.0f, 1.0f, 1.0f};
    float radius = 1.0f;
    WeatherRegionShape shape = WeatherRegionShape::box;
    WeatherRegionType type;
    float density_scale = 1.0f;
    float active_distance = 0.0f;   // 0 = auto: the effective visible distance plus weather_activity_margin
    float visible_distance = 0.0f;  // 0 = auto: the type's own sim box half extent
    bool block_by_geometry = false;
    float column_width = 0.5f;
    rf::Color rain_color{170, 190, 215, 110};
    float rain_fall_speed = 14.0f;
    float rain_wind_x = 1.1f;
    float rain_wind_z = 0.4f;
    float rain_streak_seconds = 0.035f;
    rf::Color snow_color{235, 240, 255, 190};
    float snow_fall_speed = 1.2f;
    float snow_sway_amplitude = 0.55f;
    float snow_sway_speed = 1.7f;
    float snow_sprite_radius = 0.035f;
    std::string snow_bitmap = "af_gbrsnowfl01.tga";
    int snow_bm_handle = -1;
};

// The grid is indexed by world column.
struct WeatherColumnCell
{
    int32_t cx = INT_MIN;
    int32_t cz = INT_MIN;
    int32_t retrace_frame = 0;
    float floor_y = 0.0f;
};

// Runtime state for one region, kept parallel to g_weather_regions. The arena segment is described by
// offset/length; wanted records whether the region was in the active set at the last repartition, which
// is what makes the layout self-consistent when the arena is too small to honour every share.
struct WeatherRegionState
{
    WeatherBox box{};
    float desired = 0.0f;
    int offset = -1;
    int length = 0;
    // Where this region's surviving particles are coming from, valid only inside weather_repartition.
    int move_src = -1;
    int move_count = 0;
    bool active = false;
    bool wanted = false;
    bool seeded = false;
    std::vector<WeatherColumnCell> columns;
    int cols_x = 0;
    int cols_z = 0;
    float cell = 0.0f;
    float region_top = 0.0f;
    float region_bottom = 0.0f;
};

std::vector<WeatherRegion> g_weather_regions;
std::vector<WeatherRegionState> g_weather_states;
WeatherParticle g_weather_arena[weather_arena_count];
float g_weather_share_scale = 1.0f;

rf::Camera* weather_get_camera()
{
    return rf::local_player ? rf::local_player->cam : nullptr;
}

rf::GRoom* weather_find_room(const rf::Vector3& pos)
{
    return rf::g_level_solid ? rf::find_room(rf::g_level_solid, &pos) : nullptr;
}

// Mirrors the engine point_in_liquid test at 0x004CE080
bool room_liquid_surface_y(rf::GRoom* room, float& out_y)
{
    if (!room || !room->contains_liquid) {
        return false;
    }
    out_y = room->bbox_min.y + room->liquid_depth;
    return true;
}

void plankton_reseed(const rf::Vector3& camera_pos, rf::GRoom* room)
{
    std::uniform_real_distribution<float> offset_dist{-rf::g_plankton_box_extent, rf::g_plankton_box_extent};
    float surface_y = 0.0f;
    const bool has_surface = room_liquid_surface_y(room, surface_y);

    for (auto& particle : rf::g_plankton_particles) {
        particle.pos.x = camera_pos.x + offset_dist(g_rng);
        particle.pos.y = camera_pos.y + offset_dist(g_rng);
        particle.pos.z = camera_pos.z + offset_dist(g_rng);
        if (has_surface) {
            particle.pos.y = std::min(particle.pos.y, surface_y - liquid_surface_margin);
        }
        particle.vel.zero();
    }
}

// The engine only respawns a plankton particle when it leaves the camera box, so a flock that never
// leaves it keeps drifting as one compact ball. Redistribute it whenever the camera enters water.
void plankton_update_state()
{
    static int64_t last_call_ms = 0;
    static rf::Vector3 last_camera_pos{0.0f, 0.0f, 0.0f};
    static bool paused_on_last_call = false;

    rf::Camera* camera = weather_get_camera();
    if (!camera) {
        return;
    }

    const rf::Vector3 camera_pos = rf::camera_get_pos(camera);
    const int64_t now_ms = timer::get_i64(1000);
    const bool was_paused = paused_on_last_call;
    const bool entered_water = now_ms - last_call_ms > plankton_reseed_gap_ms;
    const bool teleported = camera_pos.distance_to(last_camera_pos) > plankton_reseed_move_dist;

    last_call_ms = now_ms;
    last_camera_pos = camera_pos;
    paused_on_last_call = rf::game_paused;

    if (was_paused || !(entered_water || teleported)) {
        return;
    }
    plankton_reseed(camera_pos, weather_find_room(camera_pos));
}

FunHook<void()> plankton_update_render_hook{
    0x004980F0,
    []() {
        plankton_update_state();
        plankton_update_render_hook.call_target();
    },
};

// Every broad-phase test (activity distance, room visibility, camera box overlap) runs on the
// shape's enclosing AABB. Only the per-particle draw filter cares about the shape itself.
void weather_region_bounds(const WeatherRegion& region, rf::Vector3& out_min, rf::Vector3& out_max)
{
    rf::Vector3 extents{};
    switch (region.shape) {
        case WeatherRegionShape::box: {
            // Envelope of the oriented box: each basis vector projects its own half dim onto every
            // world axis. An identity box fills its envelope exactly; a rotated one does not.
            const rf::Matrix3& o = region.orient;
            const rf::Vector3& h = region.half_extents;
            extents = rf::Vector3{
                std::fabs(o.rvec.x) * h.x + std::fabs(o.uvec.x) * h.y + std::fabs(o.fvec.x) * h.z,
                std::fabs(o.rvec.y) * h.x + std::fabs(o.uvec.y) * h.y + std::fabs(o.fvec.y) * h.z,
                std::fabs(o.rvec.z) * h.x + std::fabs(o.uvec.z) * h.y + std::fabs(o.fvec.z) * h.z,
            };
            break;
        }
        case WeatherRegionShape::sphere:
            extents = rf::Vector3{region.radius, region.radius, region.radius};
            break;
    }
    out_min = region.center - extents;
    out_max = region.center + extents;
}

float weather_aabb_dist_sq(const rf::Vector3& pos, const rf::Vector3& bb_min, const rf::Vector3& bb_max)
{
    const float dx = std::max({bb_min.x - pos.x, 0.0f, pos.x - bb_max.x});
    const float dy = std::max({bb_min.y - pos.y, 0.0f, pos.y - bb_max.y});
    const float dz = std::max({bb_min.z - pos.z, 0.0f, pos.z - bb_max.z});
    return dx * dx + dy * dy + dz * dz;
}

bool weather_aabb_overlap(const rf::Vector3& a_min, const rf::Vector3& a_max, const rf::Vector3& b_min,
    const rf::Vector3& b_max)
{
    return a_min.x <= b_max.x && a_max.x >= b_min.x &&
           a_min.y <= b_max.y && a_max.y >= b_min.y &&
           a_min.z <= b_max.z && a_max.z >= b_min.z;
}

// Visibility comes straight from the portal renderer: the room render list holds every normal room
// drawn this frame, and detail rooms get last_frame_rendered_* stamped with frame_count at 0x004D46C8.
bool weather_region_visible(const rf::Vector3& region_min, const rf::Vector3& region_max, rf::GRoom** rooms,
    int num_rooms)
{
    for (int i = 0; i < num_rooms; ++i) {
        rf::GRoom* room = rooms[i];
        if (room && weather_aabb_overlap(region_min, region_max, room->bbox_min, room->bbox_max)) {
            return true;
        }
    }
    for (rf::GRoom* room : rf::g_level_solid->cached_detail_room_list) {
        if (room && room->last_frame_rendered_normal >= rf::frame_count - weather_visible_frame_slack &&
            weather_aabb_overlap(region_min, region_max, room->bbox_min, room->bbox_max)) {
            return true;
        }
    }
    return false;
}

// Only ever called when the region list itself changes - level load, level exit.
void weather_reset_states()
{
    g_weather_states.assign(g_weather_regions.size(), WeatherRegionState{});
    g_weather_share_scale = 1.0f;
}

// The sim/wrap box is the camera box clipped to the region's enclosing AABB. Clipping on its own used
// to fmod-compress a whole camera-box pool into whatever slab was left; the segment is now sized from
// the clipped volume, so the density inside it is exactly what an unclipped pool used to deliver and no
// particle is spent outside the region. Returns false when the two boxes do not meaningfully overlap.
bool weather_make_box(const rf::Vector3& camera_pos, float half_x, float half_y, float half_z,
    const rf::Vector3& region_min, const rf::Vector3& region_max, WeatherBox& out)
{
    const rf::Vector3 box_min{
        std::max(camera_pos.x - half_x, region_min.x),
        std::max(camera_pos.y - half_y, region_min.y),
        std::max(camera_pos.z - half_z, region_min.z),
    };
    const rf::Vector3 box_max{
        std::min(camera_pos.x + half_x, region_max.x),
        std::min(camera_pos.y + half_y, region_max.y),
        std::min(camera_pos.z + half_z, region_max.z),
    };
    if (box_max.x - box_min.x < weather_min_box_extent ||
        box_max.y - box_min.y < weather_min_box_extent ||
        box_max.z - box_min.z < weather_min_box_extent) {
        return false;
    }
    out.center = (box_min + box_max) * 0.5f;
    out.half_x = (box_max.x - box_min.x) * 0.5f;
    out.half_y = (box_max.y - box_min.y) * 0.5f;
    out.half_z = (box_max.z - box_min.z) * 0.5f;
    return true;
}

// The draw filter. Box regions are oriented, so the test runs in region space: the orientation's
// transpose is three dot products against its basis vectors, then a plain interval test.
bool weather_point_in_region(const WeatherRegion& region, const rf::Vector3& pos)
{
    const rf::Vector3 delta = pos - region.center;
    switch (region.shape) {
        case WeatherRegionShape::box:
            return std::fabs(delta.dot_prod(region.orient.rvec)) <= region.half_extents.x &&
                   std::fabs(delta.dot_prod(region.orient.uvec)) <= region.half_extents.y &&
                   std::fabs(delta.dot_prod(region.orient.fvec)) <= region.half_extents.z;
        case WeatherRegionShape::sphere:
            return delta.len_sq() <= region.radius * region.radius;
    }
    return false;
}

// The nominal density and the sim box a region runs with. An explicit visible_distance replaces the
// type's horizontal half extent and the vertical one follows the type's own ratio, so a resized box
// keeps its shape. Density always stays the type's anchor: it is a particles-per-cubic-unit figure, so
// a wider box claims proportionally more particles rather than spreading the same pool thinner.
void weather_region_sim(const WeatherRegion& region, float& density, float& half_x, float& half_y,
    float& half_z)
{
    float type_half_x = rain_box_half_x;
    float type_half_y = rain_box_half_y;
    density = rain_density;
    switch (region.type) {
        case WeatherRegionType::rain:
            break;
        case WeatherRegionType::snow:
            type_half_x = snow_box_half_x;
            type_half_y = snow_box_half_y;
            density = snow_density;
            break;
    }
    half_x = region.visible_distance > 0.0f ? region.visible_distance : type_half_x;
    half_y = half_x * (type_half_y / type_half_x);
    half_z = half_x;
}

// Runs the activity gates for every placed region and works out how many particles each one wants this
// frame. There is no nearest-N selection: every region that passes is active, and the arena decides how
// much of what they asked for they actually get.
void weather_update_states(const rf::Vector3& camera_pos)
{
    rf::GRoom** rooms = nullptr;
    int num_rooms = 0;
    rf::g_get_room_render_list(&rooms, &num_rooms);

    for (std::size_t i = 0; i < g_weather_regions.size(); ++i) {
        const WeatherRegion& region = g_weather_regions[i];
        WeatherRegionState& state = g_weather_states[i];
        state.active = false;
        state.desired = 0.0f;

        if (!region.enabled) {
            continue;
        }

        rf::Vector3 region_min;
        rf::Vector3 region_max;
        weather_region_bounds(region, region_min, region_max);
        state.region_top = region_max.y;
        state.region_bottom = region_min.y;

        float density = 0.0f;
        float half_x = 0.0f;
        float half_y = 0.0f;
        float half_z = 0.0f;
        weather_region_sim(region, density, half_x, half_y, half_z);

        const float activity = region.active_distance > 0.0f ? region.active_distance
                                                             : half_x + weather_activity_margin;
        if (weather_aabb_dist_sq(camera_pos, region_min, region_max) > activity * activity) {
            continue;
        }
        if (!weather_region_visible(region_min, region_max, rooms, num_rooms)) {
            continue;
        }

        // Nothing in this region could pass the draw filter, so it sits out the frame entirely. Re-entry
        // cannot pop because every particle the skipped frames would have moved was invisible anyway.
        if (!weather_make_box(camera_pos, half_x, half_y, half_z, region_min, region_max, state.box)) {
            continue;
        }

        // A sphere claims the particle share of its enclosing box on purpose: budgeting it by the
        // sphere's own volume would leave the same density looking thinner inside a sphere than a box.
        const float volume = state.box.half_x * state.box.half_y * state.box.half_z * 8.0f;
        state.desired = density * std::clamp(region.density_scale, 0.0f, 1.0f) * volume;
        state.active = true;
    }
}

// Rain ignores drift and spin, but seeds them anyway so a segment handed to another type by a
// repartition never starts from stale values.
void weather_seed(WeatherParticle* particles, int count, const WeatherBox& box, WeatherRegionType type)
{
    float drift_speed = snow_drift_speed;
    float spin_min = snow_spin_min;
    float spin_max = snow_spin_max;
    switch (type) {
        case WeatherRegionType::rain:
        case WeatherRegionType::snow:
            break;
    }

    std::uniform_real_distribution<float> unit_dist{-1.0f, 1.0f};
    std::uniform_real_distribution<float> phase_dist{0.0f, two_pi};
    std::uniform_real_distribution<float> spin_dist{spin_min, spin_max};

    for (int i = 0; i < count; ++i) {
        particles[i].pos.x = box.center.x + unit_dist(g_rng) * box.half_x;
        particles[i].pos.y = box.center.y + unit_dist(g_rng) * box.half_y;
        particles[i].pos.z = box.center.z + unit_dist(g_rng) * box.half_z;
        particles[i].phase = phase_dist(g_rng);
        particles[i].drift_x = unit_dist(g_rng) * drift_speed;
        particles[i].drift_z = unit_dist(g_rng) * drift_speed;
        particles[i].angle = phase_dist(g_rng);
        particles[i].spin = spin_dist(g_rng) * (unit_dist(g_rng) < 0.0f ? -1.0f : 1.0f);
    }
}

// Relaying out the arena every frame would mean reseeding half of it every frame, so the layout only
// gets rebuilt when the active set changes or somebody's share has drifted far enough to notice.
bool weather_layout_stale()
{
    for (const auto& state : g_weather_states) {
        const bool wants = state.active && state.desired >= 1.0f;
        if (wants != state.wanted) {
            return true;
        }
        if (!wants || state.length <= 0) {
            continue;
        }
        // The floor matches weather_repartition: a share that rounds below one particle still gets
        // one, so comparing the raw share would read stale forever once the arena is oversubscribed.
        const float share = std::max(state.desired * g_weather_share_scale, 1.0f);
        const float allocated = static_cast<float>(state.length);
        if (share > allocated * (1.0f + weather_repartition_drift) ||
            share < allocated * (1.0f - weather_repartition_drift)) {
            return true;
        }
    }
    return false;
}

// Segments are packed in region order, so adding or dropping a region slides every segment after it.
// A region keeps its own particles across that slide - they are interchangeable and permanently
// wrapping anyway - so only the grown tails need seeding and a boundary region never teleports.
void weather_repartition()
{
    float total = 0.0f;
    for (const auto& state : g_weather_states) {
        if (state.active && state.desired >= 1.0f) {
            total += state.desired;
        }
    }

    g_weather_share_scale = 1.0f;
    if (total > static_cast<float>(weather_arena_count)) {
        g_weather_share_scale = static_cast<float>(weather_arena_count) / total;
    }

    // Lay every segment out before touching the arena, so the moves below know all the destinations.
    int offset = 0;
    for (auto& state : g_weather_states) {
        const int old_offset = state.offset;
        const int old_length = state.length;
        const bool was_seeded = state.seeded;

        state.wanted = state.active && state.desired >= 1.0f;
        state.offset = -1;
        state.length = 0;
        state.seeded = false;
        state.move_src = -1;
        state.move_count = 0;
        if (!state.wanted) {
            continue;
        }

        const int remaining = weather_arena_count - offset;
        if (remaining <= 0) {
            continue;
        }
        const int share = static_cast<int>(state.desired * g_weather_share_scale + 0.5f);
        const int length = std::clamp(share, 1, remaining);

        if (was_seeded && old_offset >= 0) {
            state.move_src = old_offset;
            state.move_count = std::min(old_length, length);
        }
        state.offset = offset;
        state.length = length;
        state.seeded = true;
        offset += length;
    }

    // Both layouts are packed in region order, so the surviving ranges keep their relative order and
    // no two of them share a source. Running the segments that slide left front to back and the ones
    // that slide right back to front therefore means no move ever writes over a range that has not
    // been read yet; memmove covers the self-overlap when a segment slides by less than its length.
    for (auto& state : g_weather_states) {
        if (state.move_count > 0 && state.offset < state.move_src) {
            std::memmove(g_weather_arena + state.offset, g_weather_arena + state.move_src,
                static_cast<std::size_t>(state.move_count) * sizeof(WeatherParticle));
        }
    }
    for (auto it = g_weather_states.rbegin(); it != g_weather_states.rend(); ++it) {
        if (it->move_count > 0 && it->offset > it->move_src) {
            std::memmove(g_weather_arena + it->offset, g_weather_arena + it->move_src,
                static_cast<std::size_t>(it->move_count) * sizeof(WeatherParticle));
        }
    }

    for (std::size_t i = 0; i < g_weather_states.size(); ++i) {
        WeatherRegionState& state = g_weather_states[i];
        if (state.length > state.move_count) {
            weather_seed(g_weather_arena + state.offset + state.move_count,
                state.length - state.move_count, state.box, g_weather_regions[i].type);
        }
    }
}

bool weather_wrap_axis(float& value, float center, float half)
{
    float delta = value - center;
    if (std::fabs(delta) <= half) {
        return false;
    }
    const float size = half * 2.0f;
    delta = std::fmod(delta + half, size);
    if (delta < 0.0f) {
        delta += size;
    }
    value = center + delta - half;
    return true;
}

// Leaving through the top or bottom rerolls the horizontal position, so a particle never comes back
// down the same column it left.
void weather_wrap_particle(WeatherParticle& particle, const WeatherBox& box,
    std::uniform_real_distribution<float>& unit_dist)
{
    weather_wrap_axis(particle.pos.x, box.center.x, box.half_x);
    weather_wrap_axis(particle.pos.z, box.center.z, box.half_z);
    if (weather_wrap_axis(particle.pos.y, box.center.y, box.half_y)) {
        particle.pos.x = box.center.x + unit_dist(g_rng) * box.half_x;
        particle.pos.z = box.center.z + unit_dist(g_rng) * box.half_z;
    }
}

// Draw filter only - the particle keeps simulating either way.
bool weather_particle_drawn(const WeatherRegion& region, const rf::Vector3& pos, bool has_surface,
    float surface_y)
{
    if (has_surface && pos.y < surface_y) {
        return false;
    }
    return weather_point_in_region(region, pos);
}

// Sized off the sim box rather than the region, so a huge region with a small visible distance still
// only carries the columns the sim box can ever reach. The two spare rows absorb the box sliding
// with the camera without ever aliasing a live column onto the one directly opposite it.
void weather_column_grid_prepare(WeatherRegionState& state, const WeatherRegion& region)
{
    float density = 0.0f;
    float half_x = 0.0f;
    float half_y = 0.0f;
    float half_z = 0.0f;
    weather_region_sim(region, density, half_x, half_y, half_z);

    const float span = half_x * 2.0f;
    const float cell = std::max(region.column_width, span / (weather_column_max_cells_axis - 2));
    const int cols = static_cast<int>(std::ceil(span / cell)) + 2;
    if (state.columns.empty() || cols != state.cols_x || cell != state.cell) {
        state.cols_x = cols;
        state.cols_z = cols;
        state.cell = cell;
        state.columns.assign(static_cast<std::size_t>(cols) * cols, WeatherColumnCell{});
    }
}

// The ceiling above one world column: one trace straight down the region's own height, cached.
bool weather_column_floor(WeatherRegionState& state, float x, float z, int& budget, float& floor_y)
{
    const int32_t cx = static_cast<int32_t>(std::floor(x / state.cell));
    const int32_t cz = static_cast<int32_t>(std::floor(z / state.cell));
    const int mx = ((cx % state.cols_x) + state.cols_x) % state.cols_x;
    const int mz = ((cz % state.cols_z) + state.cols_z) % state.cols_z;
    WeatherColumnCell& slot = state.columns[mx + mz * state.cols_x];

    const bool hit = slot.cx == cx && slot.cz == cz;
    const bool needs_trace = !hit || (slot.retrace_frame != 0 && rf::frame_count >= slot.retrace_frame);
    if (!needs_trace) {
        floor_y = slot.floor_y;
        return true;
    }
    if (budget <= 0) {
        if (!hit) {
            return false;
        }
        floor_y = slot.floor_y;
        return true;
    }

    --budget;
    rf::Vector3 p0{(cx + 0.5f) * state.cell, state.region_top, (cz + 0.5f) * state.cell};
    rf::Vector3 p1{p0.x, state.region_bottom, p0.z};
    rf::GCollisionOutput out{};
    slot.cx = cx;
    slot.cz = cz;
    slot.retrace_frame = 0;
    slot.floor_y = rf::collide_linesegment_level_solid(p0, p1, 0, &out)
        ? out.hit_point.y
        : -std::numeric_limits<float>::max();
    floor_y = slot.floor_y;
    return true;
}

int weather_region_bitmap(const std::string& name, int& handle)
{
    if (handle == -1) {
        handle = rf::bm::load(name.c_str(), -1, true);
    }
    return handle;
}

void rain_do_frame(WeatherParticle* particles, int count, const WeatherBox& box, const WeatherRegion& region,
    WeatherRegionState& state, int& trace_budget, const rf::Vector3& camera_pos, bool has_surface,
    float surface_y, float dt)
{
    std::uniform_real_distribution<float> unit_dist{-1.0f, 1.0f};

    rf::Vector3 fall_dir{region.rain_wind_x, -region.rain_fall_speed, region.rain_wind_z};
    const float speed = fall_dir.len();
    if (speed < 0.001f) {
        return;
    }
    fall_dir /= speed;
    const float streak_half_len = std::max(speed * region.rain_streak_seconds * 0.5f, 0.001f);

    rf::gr::set_color(region.rain_color);

    rf::Matrix3 orient{};
    orient.uvec = fall_dir;

    for (int i = 0; i < count; ++i) {
        WeatherParticle& particle = particles[i];
        particle.pos.x += region.rain_wind_x * dt;
        particle.pos.y -= region.rain_fall_speed * dt;
        particle.pos.z += region.rain_wind_z * dt;

        weather_wrap_particle(particle, box, unit_dist);

        if (!weather_particle_drawn(region, particle.pos, has_surface, surface_y)) {
            continue;
        }
        if (region.block_by_geometry) {
            float floor_y = 0.0f;
            if (!weather_column_floor(state, particle.pos.x, particle.pos.z, trace_budget, floor_y) ||
                particle.pos.y < floor_y) {
                continue;
            }
        }

        // Turn the streak into a quad that faces the camera while staying aligned with the fall vector.
        rf::Vector3 right = fall_dir.cross(camera_pos - particle.pos);
        const float right_len = right.len();
        if (right_len < 0.0001f) {
            continue;
        }
        right /= right_len;
        orient.rvec = right;

        const rf::Vector3 center = particle.pos - fall_dir * streak_half_len;
        gr_3d_bitmap_oriented_wh(&center, &orient, rain_streak_half_width, streak_half_len, rain_streak_mode);
    }
}

void snow_do_frame(WeatherParticle* particles, int count, const WeatherBox& box, WeatherRegion& region,
    WeatherRegionState& state, int& trace_budget, const rf::Matrix3& camera_orient, bool has_surface,
    float surface_y, float dt)
{
    std::uniform_real_distribution<float> unit_dist{-1.0f, 1.0f};

    rf::gr::set_texture(weather_region_bitmap(region.snow_bitmap, region.snow_bm_handle), -1);
    rf::gr::set_color(region.snow_color);

    // The billboard is the camera plane spun by the flake's own angle, which is what the stock
    // bitmap_3d_angle did in screen space - minus its per-particle x87 sincos and bitmap size query.
    rf::Matrix3 orient{};

    for (int i = 0; i < count; ++i) {
        WeatherParticle& particle = particles[i];
        particle.phase += region.snow_sway_speed * dt;
        particle.angle += particle.spin * dt;
        particle.pos.x += (std::sin(particle.phase) * region.snow_sway_amplitude + particle.drift_x) * dt;
        particle.pos.z += (std::cos(particle.phase * 0.7f) * region.snow_sway_amplitude + particle.drift_z) * dt;
        particle.pos.y -= region.snow_fall_speed * dt;

        weather_wrap_particle(particle, box, unit_dist);

        if (!weather_particle_drawn(region, particle.pos, has_surface, surface_y)) {
            continue;
        }
        if (region.block_by_geometry) {
            float floor_y = 0.0f;
            if (!weather_column_floor(state, particle.pos.x, particle.pos.z, trace_budget, floor_y) ||
                particle.pos.y < floor_y) {
                continue;
            }
        }

        const float sin_a = std::sin(particle.angle);
        const float cos_a = std::cos(particle.angle);
        orient.rvec = camera_orient.rvec * cos_a + camera_orient.uvec * sin_a;
        orient.uvec = camera_orient.uvec * cos_a - camera_orient.rvec * sin_a;

        gr_3d_bitmap_oriented_wh(&particle.pos, &orient, region.snow_sprite_radius,
            region.snow_sprite_radius, rf::gr::glow_3d_bitmap_mode);
    }
}

ConsoleCommand2 r_weather_cmd{
    "r_weather",
    []() {
        g_alpine_game_config.weather = !g_alpine_game_config.weather;
        rf::console::print("Weather region effects are {}",
            g_alpine_game_config.weather ? "enabled" : "disabled");
    },
    "Toggle rendering of level weather regions",
};

}

// Weather regions authored in the editor. The registry is cleared at level init before any chunk is
// read.
void weather_load_chunk(rf::File& file, std::size_t chunk_len)
{
    std::size_t remaining = chunk_len;

    rf::File::ChunkGuard chunk_guard{file, remaining};

    bool read_error = false;

    auto read_bytes = [&](void* dst, std::size_t n) -> bool {
        if (remaining < n) { read_error = true; return false; }
        int got = file.read(dst, n);
        if (got != static_cast<int>(n)) {
            if (got > 0) remaining -= got;
            read_error = true;
            return false;
        }
        remaining -= n;
        return true;
    };

    auto read_string = [&]() -> std::string {
        uint16_t len = 0;
        if (!read_bytes(&len, sizeof(len))) return "";
        if (len == 0) return "";
        std::string result(len, '\0');
        if (!read_bytes(result.data(), len)) return "";
        return result;
    };

    auto all_finite = [](const float* values, std::size_t n) {
        for (std::size_t i = 0; i < n; ++i) {
            if (!std::isfinite(values[i])) return false;
        }
        return true;
    };

    uint32_t count = 0;
    if (!read_bytes(&count, sizeof(count))) {
        xlog::warn("[Weather] Failed to read region count from chunk (len={})", chunk_len);
        return;
    }
    if (count > 10000) count = 10000;

    xlog::info("[Weather] Loading {} weather region(s) from chunk (len={})", count, chunk_len);

    for (uint32_t i = 0; i < count; ++i) {
        WeatherRegion region;
        int32_t uid = 0;
        uint8_t always_show_range = 0; // editor-only display flag
        uint8_t initially_enabled = 1;
        uint8_t block_by_geometry = 0;
        int32_t shape = 0;
        int32_t type = 0;
        float width = 0.0f;
        float height = 0.0f;
        float depth = 0.0f;

        if (!read_bytes(&uid, sizeof(uid))) return;
        if (!read_bytes(&region.center.x, sizeof(float))) return;
        if (!read_bytes(&region.center.y, sizeof(float))) return;
        if (!read_bytes(&region.center.z, sizeof(float))) return;
        if (!read_bytes(&region.orient.rvec.x, sizeof(float))) return;
        if (!read_bytes(&region.orient.rvec.y, sizeof(float))) return;
        if (!read_bytes(&region.orient.rvec.z, sizeof(float))) return;
        if (!read_bytes(&region.orient.uvec.x, sizeof(float))) return;
        if (!read_bytes(&region.orient.uvec.y, sizeof(float))) return;
        if (!read_bytes(&region.orient.uvec.z, sizeof(float))) return;
        if (!read_bytes(&region.orient.fvec.x, sizeof(float))) return;
        if (!read_bytes(&region.orient.fvec.y, sizeof(float))) return;
        if (!read_bytes(&region.orient.fvec.z, sizeof(float))) return;
        read_string(); // script_name (discard)
        if (read_error) return;
        if (!read_bytes(&shape, sizeof(shape))) return;
        if (!read_bytes(&type, sizeof(type))) return;
        if (!read_bytes(&width, sizeof(float))) return;
        if (!read_bytes(&height, sizeof(float))) return;
        if (!read_bytes(&depth, sizeof(float))) return;
        if (!read_bytes(&region.radius, sizeof(float))) return;
        if (!read_bytes(&region.density_scale, sizeof(float))) return;
        if (!read_bytes(&region.rain_color.red, sizeof(rf::ubyte))) return;
        if (!read_bytes(&region.rain_color.green, sizeof(rf::ubyte))) return;
        if (!read_bytes(&region.rain_color.blue, sizeof(rf::ubyte))) return;
        if (!read_bytes(&region.rain_color.alpha, sizeof(rf::ubyte))) return;
        if (!read_bytes(&region.rain_fall_speed, sizeof(float))) return;
        if (!read_bytes(&region.rain_wind_x, sizeof(float))) return;
        if (!read_bytes(&region.rain_wind_z, sizeof(float))) return;
        if (!read_bytes(&region.rain_streak_seconds, sizeof(float))) return;
        if (!read_bytes(&region.snow_color.red, sizeof(rf::ubyte))) return;
        if (!read_bytes(&region.snow_color.green, sizeof(rf::ubyte))) return;
        if (!read_bytes(&region.snow_color.blue, sizeof(rf::ubyte))) return;
        if (!read_bytes(&region.snow_color.alpha, sizeof(rf::ubyte))) return;
        if (!read_bytes(&region.snow_fall_speed, sizeof(float))) return;
        if (!read_bytes(&region.snow_sway_amplitude, sizeof(float))) return;
        if (!read_bytes(&region.snow_sway_speed, sizeof(float))) return;
        if (!read_bytes(&region.snow_sprite_radius, sizeof(float))) return;
        std::string snow_bitmap = read_string();
        if (read_error) return;
        if (snow_bitmap.size() >= max_bitmap_name) {
            xlog::warn("[Weather] Ignoring over-long snow bitmap name on region uid {}", uid);
            snow_bitmap.clear();
        }
        if (!snow_bitmap.empty()) {
            region.snow_bitmap = std::move(snow_bitmap);
        }
        if (!read_bytes(&always_show_range, sizeof(always_show_range))) return;
        if (!read_bytes(&initially_enabled, sizeof(initially_enabled))) return;
        if (!read_bytes(&region.active_distance, sizeof(float))) return;
        if (!read_bytes(&region.visible_distance, sizeof(float))) return;
        if (!read_bytes(&block_by_geometry, sizeof(block_by_geometry))) return;
        if (!read_bytes(&region.column_width, sizeof(float))) return;

        region.uid = uid;
        region.enabled = initially_enabled != 0;

        // Every float here is remote input, and the ones used as sizes never reach
        // the position based draw guards, so a record carrying a NaN or an infinity is dropped
        // outright and everything that survives is clamped into a range the sim can carry.
        const float record_floats[] = {
            region.center.x, region.center.y, region.center.z,
            region.orient.rvec.x, region.orient.rvec.y, region.orient.rvec.z,
            region.orient.uvec.x, region.orient.uvec.y, region.orient.uvec.z,
            region.orient.fvec.x, region.orient.fvec.y, region.orient.fvec.z,
            width, height, depth, region.radius, region.density_scale,
            region.active_distance, region.visible_distance, region.column_width,
            region.rain_fall_speed, region.rain_wind_x, region.rain_wind_z,
            region.rain_streak_seconds, region.snow_fall_speed, region.snow_sway_amplitude,
            region.snow_sway_speed, region.snow_sprite_radius,
        };
        if (!all_finite(record_floats, std::size(record_floats))) {
            xlog::warn("[Weather] Skipping region uid {} with a non-finite field", uid);
            continue;
        }

        width = std::clamp(width, 0.0f, weather_max_dim);
        height = std::clamp(height, 0.0f, weather_max_dim);
        depth = std::clamp(depth, 0.0f, weather_max_dim);
        region.radius = std::clamp(region.radius, 0.0f, weather_max_dim);
        region.density_scale = std::clamp(region.density_scale, 0.0f, 1.0f);
        region.active_distance = std::clamp(region.active_distance, 0.0f, weather_max_active_distance);
        // Zero stays zero because it means auto; anything the mapper actually asked for gets a floor,
        // since a sub-metre sim box would wrap particles faster than they could be seen falling.
        region.visible_distance = region.visible_distance > 0.0f
            ? std::clamp(region.visible_distance, weather_min_visible_distance, weather_max_visible_distance)
            : 0.0f;
        region.rain_fall_speed = std::clamp(region.rain_fall_speed, 0.0f, weather_max_speed);
        region.rain_wind_x = std::clamp(region.rain_wind_x, -weather_max_speed, weather_max_speed);
        region.rain_wind_z = std::clamp(region.rain_wind_z, -weather_max_speed, weather_max_speed);
        region.rain_streak_seconds = std::clamp(region.rain_streak_seconds, 0.0f, weather_max_streak_seconds);
        region.snow_fall_speed = std::clamp(region.snow_fall_speed, 0.0f, weather_max_speed);
        region.snow_sway_amplitude = std::clamp(region.snow_sway_amplitude, 0.0f, weather_max_dim);
        region.snow_sway_speed = std::clamp(region.snow_sway_speed, 0.0f, weather_max_speed);
        region.snow_sprite_radius = std::clamp(region.snow_sprite_radius, 0.0f, weather_max_dim);
        region.column_width = std::clamp(region.column_width, weather_column_width_min, weather_column_width_max);
        region.block_by_geometry = block_by_geometry != 0;

        // Dimensions are stored full-size in the file, matching how gas regions store theirs.
        region.half_extents = rf::Vector3{width * 0.5f, height * 0.5f, depth * 0.5f};

        // A region written by a newer editor can carry a shape or type this build does not know.
        // Dropping it beats simulating it as the wrong volume.
        if (shape != static_cast<int32_t>(WeatherRegionShape::box) &&
            shape != static_cast<int32_t>(WeatherRegionShape::sphere)) {
            xlog::warn("[Weather] Skipping region uid {} with unknown shape {}", uid, shape);
            continue;
        }
        if (type < static_cast<int32_t>(WeatherRegionType::rain) ||
            type > static_cast<int32_t>(WeatherRegionType::snow)) {
            xlog::warn("[Weather] Skipping region uid {} with unknown type {}", uid, type);
            continue;
        }
        region.shape = static_cast<WeatherRegionShape>(shape);
        region.type = static_cast<WeatherRegionType>(type);

        g_weather_regions.push_back(std::move(region));
    }

    if (read_error) {
        xlog::warn("[Weather] Read error while parsing weather region chunk; loaded {} region(s)",
            g_weather_regions.size());
    }

    weather_reset_states();
}

// Called by the Weather_Region_State event for each of its link UIDs. Regions are not engine
// objects, so an event link to one is never resolved to a handle and arrives here as a raw UID.
bool weather_set_region_enabled(int uid, bool enabled)
{
    bool matched = false;
    for (auto& region : g_weather_regions) {
        if (region.uid == uid) {
            region.enabled = enabled;
            matched = true;
        }
    }
    return matched;
}

static void weather_drop_columns(std::size_t i)
{
    if (i >= g_weather_states.size()) {
        return;
    }
    WeatherRegionState& state = g_weather_states[i];
    state.columns.clear();
    state.cols_x = 0;
    state.cols_z = 0;
    state.cell = 0.0f;
}

// Called by the Anchor_Marker events for each of their link UIDs, same as the other region types.
// Bounds are derived per frame; only the ceiling cache, traced against the old bounds, is dropped.
bool weather_move_region(int uid, const rf::Vector3& pos)
{
    bool matched = false;
    for (std::size_t i = 0; i < g_weather_regions.size(); ++i) {
        if (g_weather_regions[i].uid == uid) {
            g_weather_regions[i].center = pos;
            weather_drop_columns(i);
            matched = true;
        }
    }
    return matched;
}

bool weather_move_region(int uid, const rf::Vector3& pos, const rf::Matrix3& orient)
{
    bool matched = false;
    for (std::size_t i = 0; i < g_weather_regions.size(); ++i) {
        if (g_weather_regions[i].uid == uid) {
            g_weather_regions[i].center = pos;
            g_weather_regions[i].orient = orient;
            weather_drop_columns(i);
            matched = true;
        }
    }
    return matched;
}

// A geomod can open or close a ceiling.
void weather_notify_geomod(const rf::Vector3& pos, float radius)
{
    for (std::size_t i = 0; i < g_weather_regions.size() && i < g_weather_states.size(); ++i) {
        if (!g_weather_regions[i].block_by_geometry) {
            continue;
        }
        WeatherRegionState& state = g_weather_states[i];
        const float reach = radius + state.cell;
        const float reach_sq = reach * reach;
        for (auto& slot : state.columns) {
            if (slot.cx == INT_MIN || slot.retrace_frame != 0) {
                continue;
            }
            const float dx = (slot.cx + 0.5f) * state.cell - pos.x;
            const float dz = (slot.cz + 0.5f) * state.cell - pos.z;
            if (dx * dx + dz * dz <= reach_sq) {
                slot.retrace_frame = rf::frame_count + weather_column_geomod_delay_frames;
            }
        }
    }
}

// Bitmap handles are name-deduped by bmpman and bmpman has no refcount, so releasing one here would
// destroy the entry another region (or the rest of the level) is still holding. Same deliberate
// bounded leak the corona and alpine mesh loaders accept.
void weather_clear_regions()
{
    g_weather_regions.clear();
    weather_reset_states();
}

void weather_render()
{
    if (!g_alpine_game_config.weather) {
        return;
    }
    if (g_weather_states.size() != g_weather_regions.size()) {
        weather_reset_states();
    }

    rf::Camera* camera = weather_get_camera();
    if (!camera || !rf::g_level_solid || g_weather_regions.empty()) {
        return;
    }

    // Everything up to here is pointer tests, and the gates below are AABB arithmetic against the
    // room render list, so a level whose regions are all far away or switched off costs no room
    // query at all.
    const rf::Vector3 camera_pos = rf::camera_get_pos(camera);
    weather_update_states(camera_pos);
    if (weather_layout_stale()) {
        weather_repartition();
    }

    bool any_active = false;
    for (const auto& state : g_weather_states) {
        if (state.active && state.offset >= 0 && state.length > 0) {
            any_active = true;
            break;
        }
    }
    if (!any_active) {
        return;
    }

    // Only something that wants to draw pays for the camera's room, and the segments stay laid out
    // and seeded while submerged so surfacing costs nothing.
    rf::GRoom* camera_room = camera->camera_entity ? rf::camera_get_room(camera) : nullptr;
    float surface_y = 0.0f;
    const bool has_surface = room_liquid_surface_y(camera_room, surface_y);
    if (has_surface && camera_pos.y <= surface_y) {
        return;
    }

    const rf::Matrix3 camera_orient = rf::camera_get_orient(camera);
    const float dt = std::clamp(rf::frametime, 0.0f, max_frame_delta) * demo_playback_sim_time_scale();
    int trace_budget = weather_column_trace_budget;

    for (std::size_t i = 0; i < g_weather_regions.size(); ++i) {
        WeatherRegionState& state = g_weather_states[i];
        if (!state.active || state.offset < 0 || state.length <= 0) {
            continue;
        }
        WeatherRegion& region = g_weather_regions[i];
        WeatherParticle* particles = g_weather_arena + state.offset;

        if (region.block_by_geometry) {
            weather_column_grid_prepare(state, region);
        }

        switch (region.type) {
            case WeatherRegionType::rain:
                rain_do_frame(particles, state.length, state.box, region, state, trace_budget,
                    camera_pos, has_surface, surface_y, dt);
                break;
            case WeatherRegionType::snow:
                snow_do_frame(particles, state.length, state.box, region, state, trace_budget,
                    camera_orient, has_surface, surface_y, dt);
                break;
        }
    }

    // The tint and the snow texture are global gr state, and everything drawn after this hook would
    // inherit them.
    rf::gr::set_color(255, 255, 255, 255);
    rf::gr::set_texture(-1, -1);
}

void weather_apply_patch()
{
    weather_reset_states();
    plankton_update_render_hook.install();
    r_weather_cmd.register_cmd();
}
