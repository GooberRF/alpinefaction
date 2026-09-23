#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <common/rope_curve.h>
#include <common/utils/list-utils.h>
#include <common/utils/string-utils.h>
#include <patch_common/CallHook.h>
#include <xlog/xlog.h>
#include "../misc/alpine_settings.h"
#include "../misc/level.h"
#include "../multi/demo/demo.h"
#include "../multi/multi.h"
#include "../os/os.h"
#include "../rf/bmpman.h"
#include "../rf/clutter.h"
#include "../rf/entity.h"
#include "../rf/file/file.h"
#include "../rf/gameseq.h"
#include "../rf/glare.h"
#include "../rf/gr/gr.h"
#include "../rf/gr/gr_light.h"
#include "../rf/math/matrix.h"
#include "../rf/math/vector.h"
#include "../rf/multi.h"
#include "../rf/object.h"
#include "../rf/os/frametime.h"
#include "../rf/player/player.h"
#include "../rf/v3d.h"
#include "../rf/vmesh.h"
#include "alpine_obj_common.h"
#include "alpine_rope.h"
#include "object.h"

namespace
{

// RF texture name buffers are 32 bytes, so anything longer could never resolve anyway. Matches
// the editor's rfl_name_over_long rule so both sides drop exactly the same names.
constexpr std::size_t rope_max_bitmap_name_len = 31;
constexpr std::size_t rope_max_bitmap_ext_len = 14;

constexpr uint32_t rope_flag_dynamic = 0x1;
constexpr uint32_t rope_flag_glow = 0x2;
constexpr uint32_t rope_flag_sway = 0x4;

constexpr float rope_sim_step = 1.0f / 60.0f;
constexpr int rope_max_substeps = 4;
constexpr int rope_constraint_iterations = 6;
constexpr float rope_max_frame_delta = 0.1f;
constexpr float rope_damping = 0.995f;
// Fixed, not rf::phys::gravity: the static catenary the dynamic rope is seeded from assumes a
// constant field, so a level that changes gravity must not make the two disagree.
constexpr float rope_gravity = 9.8f;
constexpr float rope_sleep_epsilon = 0.0005f;
constexpr float rope_sleep_seconds = 2.0f;

// Damage is an unbounded designer value; this converts it to a node velocity and caps what a
// single blast can do to a rope.
constexpr float rope_explosion_damage_to_speed = 0.05f;
constexpr float rope_explosion_max_speed = 25.0f;

// Wind ceiling, ~10 g. Every sane authored combination lands far below it, while the clamped
// worst case (amplitude 100 at speed 100) is held to 100 * step = 1.67 m/s of velocity per
// substep, so even four substeps stay under a single explosion's ceiling.
constexpr float rope_wind_max_accel = 100.0f;
// Radians of drive phase spread across the rope, so it ripples instead of translating rigidly.
constexpr float rope_wind_node_phase = 2.0f;

// Entities push nodes out of a sphere this much wider than their collision radius.
constexpr float rope_push_margin = 0.1f;
// A garbage radius must not make every rope in the level test positive.
constexpr float rope_push_max_radius = 20.0f;
constexpr float rope_push_max_speed = 15.0f;
constexpr float rope_push_vel_scale = 0.5f;
constexpr float rope_push_min_speed = 0.05f;
// A player's collision radius covers far more than the body a rope should drape around.
constexpr float rope_push_player_radius_scale = 0.5f;
constexpr float rope_push_vehicle_radius_scale = 0.75f;
// Penetration below this is not a push; without it a resting overlap re-arms the sleep timer.
constexpr float rope_push_epsilon = 1.0e-3f;

// A VBM header that declares no rate still has to animate at something; matches the rate stock
// effects are authored at.
constexpr float rope_default_bitmap_fps = 15.0f;
// A VBM header's rate is authored data: below the floor the frame index would never advance, above
// the ceiling the tick multiply runs away from anything a 255-frame clip could show.
constexpr float rope_min_bitmap_fps = 0.1f;
constexpr float rope_max_bitmap_fps = 240.0f;

constexpr float rope_aabb_pad = 0.05f;
constexpr float rope_two_pi = 6.2831855f;

constexpr uint8_t rope_deco_orient_follow = 0;
constexpr uint8_t rope_deco_orient_upright = 1;

const rf::Vector3 rope_world_up{0.0f, 1.0f, 0.0f};

constexpr rf::gr::Mode rope_mode_textured{
    rf::gr::TEXTURE_SOURCE_WRAP,
    rf::gr::COLOR_SOURCE_VERTEX_TIMES_TEXTURE,
    rf::gr::ALPHA_SOURCE_VERTEX_TIMES_TEXTURE,
    rf::gr::ALPHA_BLEND_ALPHA,
    rf::gr::ZBUFFER_TYPE_FULL_ALPHA_TEST,
    rf::gr::FOG_ALLOWED,
};

constexpr rf::gr::Mode rope_mode_textured_glow{
    rf::gr::TEXTURE_SOURCE_WRAP,
    rf::gr::COLOR_SOURCE_VERTEX_TIMES_TEXTURE,
    rf::gr::ALPHA_SOURCE_VERTEX_TIMES_TEXTURE,
    rf::gr::ALPHA_BLEND_ADDITIVE,
    rf::gr::ZBUFFER_TYPE_READ,
    rf::gr::FOG_NOT_ALLOWED,
};

constexpr rf::gr::Mode rope_mode_untextured{
    rf::gr::TEXTURE_SOURCE_NONE,
    rf::gr::COLOR_SOURCE_VERTEX,
    rf::gr::ALPHA_SOURCE_VERTEX,
    rf::gr::ALPHA_BLEND_ALPHA,
    rf::gr::ZBUFFER_TYPE_FULL_ALPHA_TEST,
    rf::gr::FOG_ALLOWED,
};

constexpr rf::gr::Mode rope_mode_untextured_glow{
    rf::gr::TEXTURE_SOURCE_NONE,
    rf::gr::COLOR_SOURCE_VERTEX,
    rf::gr::ALPHA_SOURCE_VERTEX,
    rf::gr::ALPHA_BLEND_ADDITIVE,
    rf::gr::ZBUFFER_TYPE_READ,
    rf::gr::FOG_NOT_ALLOWED,
};

// An instance that has moved this far from where obj_create last refused it gets another try.
constexpr float rope_glare_retry_dist_sq = 0.25f; // 0.5 m

// Per-slot extras: an instance-local transform offset plus the two optional effects. Travels with
// the mesh name as one wire unit, so dropping a slot drops its effects with it.
struct RopeSlotFx
{
    rf::Vector3 pos_offset{};
    rf::Vector3 rot_offset{}; // degrees, x = pitch, y = yaw, z = roll
    uint8_t flags = 0;

    std::string glare_bitmap;
    uint8_t glare_r = 255, glare_g = 255, glare_b = 255, glare_a = 255;
    float cone_angle = rope_curve::deco_fx_default_cone_angle;
    float intensity = rope_curve::deco_fx_default_intensity;
    float radius_distance = rope_curve::deco_fx_default_radius_distance;
    float radius_scale = rope_curve::deco_fx_default_radius_scale;
    float diminish_distance = rope_curve::deco_fx_default_diminish_distance;
    std::string volumetric_bitmap;
    float volumetric_height = 0.0f;
    float volumetric_length = 0.0f;

    uint8_t light_r = 255, light_g = 255, light_b = 255;
    float light_radius = rope_curve::deco_fx_default_light_radius;
    float light_intensity = rope_curve::deco_fx_default_light_intensity;

    bool has_glare() const { return (flags & rope_curve::deco_slot_flag_glare) != 0; }
    bool has_light() const { return (flags & rope_curve::deco_slot_flag_light) != 0; }
    bool has_offset() const { return pos_offset.len_sq() > 0.0f || rot_offset.len_sq() > 0.0f; }
};

// Live effect objects for one decoration instance. Glare objects belong to the object system once
// created; light handles are ours to release.
struct RopeFxInstance
{
    int glare_handle = -1;
    int light_handle = -1;
    // Where obj_create last refused a glare. Without it a sway or dynamic rope re-attempts the
    // failing creation, BSP room query included, on every frame it refreshes.
    rf::Vector3 glare_fail_pos{};
    bool glare_failed = false;
    // Same reason, no position term: the light pool is a fixed free list with no room query, so a
    // refusal is a level-wide budget problem that moving cannot fix.
    bool light_failed = false;
};

// Where one decoration instance sits this frame, offsets applied. The mesh draw, the glare and the
// light all read the same entry, so the three can never disagree about where an instance is.
struct RopeDecoXform
{
    rf::Vector3 pos{};
    rf::Matrix3 orient{};
};

// Meshes duplicated along a rope. `mesh_names` and the placement settings come straight off the
// wire; everything below them is derived and rebuilt when the rope's length changes.
struct RopeDeco
{
    bool enabled = false;
    std::vector<std::string> mesh_names;
    // Per slot, index-locked to mesh_names by the wire unit and by compaction.
    std::vector<RopeSlotFx> fx;
    uint8_t spacing_mode = 0;
    int32_t count = 10;
    float spacing = 1.0f;
    bool random_order = false;
    uint8_t orient_mode = rope_deco_orient_follow;

    std::vector<int> slots;       // per name: index into the module mesh cache, or -1
    std::vector<float> inst_t;    // per instance: normalized arc position
    std::vector<int> inst_name;   // per instance: index into mesh_names
    std::vector<int> draw_order;  // instance indices grouped by mesh, so draws batch
    float max_radius = 0.0f;
    // Largest authored position offset, so an offset instance cannot be culled away with a rope
    // whose own ribbon is off screen.
    float max_offset = 0.0f;
    int32_t built_len_q = INT32_MIN;

    // Effects, allocated only for a rope that actually authored some. One GlareInfo per glare slot
    // (heap, ours to delete); one RopeFxInstance per decoration instance.
    std::vector<rf::GlareInfo*> glare_infos; // per name, nullptr when the slot has no glare
    std::vector<RopeFxInstance> fx_inst;
    bool any_glare = false;
    bool any_light = false;

    // Per instance, rebuilt at most once per frame and shared by every pass that reads it.
    std::vector<RopeDecoXform> xforms;
    uint32_t xforms_frame = UINT32_MAX; // never built; the counter starts at 0

    bool has_fx() const { return any_glare || any_light; }
};

// Half-space latch for the decoration frames, held across instances and across frames so a
// near-vertical tangent cannot spin them. Seeded from the first direction that is actually well
// defined: a fixed seed would flip every decoration on a rope that happens to run the other way.
struct RopeDecoLatch
{
    rf::Vector3 dir{1.0f, 0.0f, 0.0f};
    bool valid = false;
};

struct AlpineRope
{
    int32_t uid = -1;
    rf::Vector3 pos{};
    int clutter_handle = -1;

    int32_t target_uid = -1;
    int target_handle = -1;
    float dangle_length = 3.0f;
    float slack = 0.5f;
    float weight = 1.0f;
    float thickness = 0.03f;
    int segments = 24;
    float uv_tiles_per_meter = 0.0f;
    rf::Color color{255, 255, 255, 255};
    std::string bitmap_name;
    int bitmap_handle = -1;
    // Per-frame handles of an animated bitmap, empty for a still one. Held so the render path can
    // bind a frame without touching the bitmap table.
    std::vector<int> bitmap_frames;
    float bitmap_fps = 0.0f;
    float sway_amplitude = 0.05f;
    float sway_speed = 1.0f;
    uint32_t flags = 0;
    bool initially_on = true;

    rf::Vector3 end_a{};
    rf::Vector3 end_b{};
    bool has_target = false;
    // Latched the first time the target actually resolves, with the length solved for it. A target
    // that later goes away leaves the rope hanging at that length instead of snapping back to the
    // authored dangle stub; an authored dangle never latches and keeps using dangle_length.
    bool had_target = false;
    float lost_length = 0.0f;
    float rope_length = 0.0f;
    float rest_length = 0.0f;

    std::vector<rf::Vector3> points;
    std::vector<rf::Vector3> prev;
    std::vector<float> arc;
    float total_arc = 0.0f;

    float accum = 0.0f;
    float sleep_timer = 0.0f;
    bool asleep = false;
    bool seeded = false;
    bool was_off = false;
    rf::Vector3 wind_axis{};

    RopeDeco deco;
    RopeDecoLatch deco_latch;
    // Effect placement is re-derived only when the shape it rides on can have moved; a static rope
    // that nothing drags pays for it once, at level init.
    bool fx_dirty = true;
    int fx_applied_on = -1; // -1 = never applied, so the first pass always runs

    rf::Vector3 aabb_min{};
    rf::Vector3 aabb_max{};

    int32_t qa[3] = {0, 0, 0};
    int32_t qb[3] = {0, 0, 0};
    bool endpoints_valid = false;

    bool is_dynamic() const { return (flags & rope_flag_dynamic) != 0; }
    bool is_glow() const { return (flags & rope_flag_glow) != 0; }
    bool has_sway() const { return (flags & rope_flag_sway) != 0; }
};

std::vector<AlpineRope> g_ropes;
std::vector<int> g_draw_order;
std::vector<rope_curve::Vec3> g_curve_points;
std::vector<float> g_curve_arc;
std::vector<rf::gr::Vertex> g_verts;
// The positions the ribbon is actually drawing this frame, so the width axis is derived from the
// swayed curve rather than the un-swayed nodes it was painted over.
std::vector<rf::Vector3> g_render_points;

// The wall clock every cosmetic phase in this module reads: real time, scaled by demo playback
// speed, and frozen while the game is paused so sway and bitmap animation stop with the sim.
// Accumulated from raw deltas rather than re-derived, because a pause has to hold the phase where
// it was instead of jumping it forward by however long the pause lasted. The accumulator is a
// double of milliseconds: it keeps the sub-millisecond remainder of the demo scaling, so there is
// no per-frame truncation drift, and since every delta added is >= 0 it can only move forward -
// a backwards step is the one thing that would flip the sine and reverse the animation.
double g_clock_acc_ms = 0.0;
int64_t g_clock_ms = 0;
int64_t g_clock_last_raw_ms = 0;
bool g_clock_started = false;

// Incremented once per sim frame. The decoration transforms are frame dependent (the sway paint
// moves them), and the sim pass, the render pass and every extra view all want the same ones.
// Unsigned so the increment has defined behaviour at the top of the range; wrapping cannot produce a
// false cache hit in practice, since a collision needs 2^32 sim frames inside one level.
uint32_t g_deco_xform_frame = 0;

// One entry per unique decoration mesh name in the level. The engine owns level meshes and frees
// them on unload, so these pointers are dropped at clear_state, never vmesh_free'd.
struct RopeDecoMesh
{
    rf::VMesh* mesh = nullptr;
    float radius = 0.0f;
};

std::vector<RopeDecoMesh> g_deco_meshes;
// Lowercased name -> index into g_deco_meshes, or -1 for a name already known to be unloadable.
// Keeping the failures means the warning is logged once per name, not once per rope.
std::unordered_map<std::string, int> g_deco_mesh_lookup;
std::vector<float> g_deco_arc;

// Mirrors the editor's rfl_name_over_long: over-long stem or over-long extension.
bool bitmap_name_over_long(const std::string& name)
{
    if (name.size() > rope_max_bitmap_name_len) {
        return true;
    }
    const std::size_t dot = name.rfind('.');
    return dot != std::string::npos && name.size() - dot > rope_max_bitmap_ext_len;
}

rope_curve::Vec3 to_curve(const rf::Vector3& v)
{
    return rope_curve::Vec3{v.x, v.y, v.z};
}

rf::Vector3 from_curve(const rope_curve::Vec3& v)
{
    return rf::Vector3{v.x, v.y, v.z};
}

bool update_endpoint_key(AlpineRope& rope)
{
    using rope_curve::quantize_1024;
    const int32_t a0 = quantize_1024(rope.end_a.x), a1 = quantize_1024(rope.end_a.y),
                  a2 = quantize_1024(rope.end_a.z);
    const int32_t b0 = quantize_1024(rope.end_b.x), b1 = quantize_1024(rope.end_b.y),
                  b2 = quantize_1024(rope.end_b.z);
    const bool changed = !rope.endpoints_valid || a0 != rope.qa[0] || a1 != rope.qa[1] ||
                         a2 != rope.qa[2] || b0 != rope.qb[0] || b1 != rope.qb[1] || b2 != rope.qb[2];
    rope.qa[0] = a0; rope.qa[1] = a1; rope.qa[2] = a2;
    rope.qb[0] = b0; rope.qb[1] = b1; rope.qb[2] = b2;
    rope.endpoints_valid = true;
    return changed;
}

bool sway_is_active(const AlpineRope& rope);

void update_aabb(AlpineRope& rope)
{
    if (rope.points.empty()) {
        rope.aabb_min = rope.pos;
        rope.aabb_max = rope.pos;
        return;
    }
    rf::Vector3 mn = rope.points.front();
    rf::Vector3 mx = mn;
    for (const rf::Vector3& p : rope.points) {
        mn.x = std::min(mn.x, p.x); mn.y = std::min(mn.y, p.y); mn.z = std::min(mn.z, p.z);
        mx.x = std::max(mx.x, p.x); mx.y = std::max(mx.y, p.y); mx.z = std::max(mx.z, p.z);
    }
    // Decorations hang off the curve, so the rope-level cull has to cover the largest of them or a
    // rope whose ribbon is off screen would take its meshes with it. The sway term only belongs
    // here when sway is actually driving the rope: an authored amplitude on a rope with the flag
    // off (or zero speed) would otherwise inflate the box by up to 100 m for nothing.
    const float sway_pad = sway_is_active(rope) ? rope.sway_amplitude : 0.0f;
    const float pad = rope.thickness * 0.5f + sway_pad + rope_aabb_pad + rope.deco.max_radius +
                      rope.deco.max_offset;
    mn -= pad;
    mx += pad;
    rope.aabb_min = mn;
    rope.aabb_max = mx;
}

void rebuild_arc_from_points(AlpineRope& rope)
{
    rope.arc.resize(rope.points.size());
    float acc = 0.0f;
    for (std::size_t i = 0; i < rope.points.size(); ++i) {
        if (i > 0) {
            acc += rope.points[i].distance_to(rope.points[i - 1]);
        }
        rope.arc[i] = acc;
    }
    rope.total_arc = acc;
}

// Resolves the rope's two world endpoints for this frame. The source is always the placed emitter
// position; the target end is re-fetched from the object every frame so movers and entities drag
// the rope with them.
void resolve_endpoints(AlpineRope& rope)
{
    rope.end_a = rope.pos;
    rope.has_target = false;

    if (rope.target_handle != -1) {
        rf::Object* obj = rf::obj_from_handle(rope.target_handle);
        if (obj) {
            // A target whose position has gone non-finite is treated as absent for this frame
            // rather than poisoning the curve; the handle stays, so it recovers on its own.
            if (std::isfinite(obj->pos.x) && std::isfinite(obj->pos.y) && std::isfinite(obj->pos.z)) {
                rope.end_b = obj->pos;
                rope.has_target = true;
            }
        }
        else {
            rope.target_handle = -1;
        }
    }

    // A rope that has lost its target hangs at the length it last solved. lost_length is frozen at
    // the moment of the loss and the handle is never re-resolved, so this can transition once.
    const float hang_length = rope.had_target ? rope.lost_length : rope.dangle_length;

    if (!rope.has_target) {
        rope.end_b = rf::Vector3{rope.pos.x, rope.pos.y - hang_length, rope.pos.z};
    }

    if (rope.has_target) {
        rope.rope_length = rope.end_a.distance_to(rope.end_b) + rope.slack;
    }
    else {
        rope.rope_length = hang_length;
    }
    if (!std::isfinite(rope.rope_length) || rope.rope_length <= 0.0f) {
        rope.rope_length = rope_curve::rope_min_dangle;
    }
    if (rope.has_target) {
        // Latched from the floored value, so the retained length is one the solver already accepts.
        rope.had_target = true;
        rope.lost_length = rope.rope_length;
    }
    rope.rest_length = rope.rope_length / static_cast<float>(rope.segments);
}

void build_static_shape(AlpineRope& rope)
{
    const rope_curve::Curve curve =
        rope.has_target ? rope_curve::solve(to_curve(rope.end_a), to_curve(rope.end_b), rope.rope_length)
                        : rope_curve::solve_dangle(to_curve(rope.end_a), rope.rope_length);

    rope_curve::build_polyline(curve, rope.segments + 1, g_curve_points, g_curve_arc);
    rope.points.resize(g_curve_points.size());
    for (std::size_t i = 0; i < g_curve_points.size(); ++i) {
        rope.points[i] = from_curve(g_curve_points[i]);
    }
    rope.arc = g_curve_arc;
    rope.total_arc = rope.arc.empty() ? 0.0f : rope.arc.back();
    update_aabb(rope);
}

void seed_dynamic(AlpineRope& rope)
{
    build_static_shape(rope);
    rope.prev = rope.points;
    rope.accum = 0.0f;
    rope.sleep_timer = 0.0f;
    rope.asleep = false;
    rope.seeded = true;
}

rf::Vector3 sway_offset_dir(const AlpineRope& rope)
{
    rf::Vector3 span = rope.end_b - rope.end_a;
    span.normalize_safe();
    rf::Vector3 perp = span.cross(rf::Vector3{0.0f, 1.0f, 0.0f});
    if (perp.len_sq() < 1.0e-6f) {
        perp = rf::Vector3{1.0f, 0.0f, 0.0f};
    }
    perp.normalize_safe();
    return perp;
}

// span x up flips sign as a mover swings the span through vertical, which would reverse the sway
// direction in a single frame; same half-space rule the ribbon uses for its side vectors. A rope is
// either dynamic (latched by the wind path) or static (latched by the render path), never both, so
// the two share rope.wind_axis without fighting over it.
rf::Vector3 sway_axis_latched(AlpineRope& rope)
{
    rf::Vector3 dir = sway_offset_dir(rope);
    if (dir.dot_prod(rope.wind_axis) < 0.0f) {
        dir = -dir;
    }
    rope.wind_axis = dir;
    return dir;
}

// Reduced against the sine's own period rather than a wall-clock modulus, so the phase is
// continuous across every hour boundary.
float sway_phase(const AlpineRope& rope, int64_t now_ms)
{
    const double seconds = static_cast<double>(now_ms) / 1000.0;
    const double period = rope.sway_speed > 0.0f ? 1.0 / static_cast<double>(rope.sway_speed) : 0.0;
    const double wrapped = period > 0.0 ? std::fmod(seconds, period) : 0.0;
    const float uid_phase = static_cast<float>(rope.uid & 0xFF) * (rope_two_pi / 256.0f);
    return static_cast<float>(wrapped * rope.sway_speed * rope_two_pi) + uid_phase;
}

bool sway_is_active(const AlpineRope& rope)
{
    return rope.has_sway() && rope.sway_amplitude > 0.0f && rope.sway_speed > 0.0f;
}

// Sway on a dynamic rope drives the simulation instead of painting a render-time offset: same
// lateral direction, same wall-clock phase, applied as an acceleration.
struct RopeWind
{
    bool active = false;
    rf::Vector3 dir{};
    float accel = 0.0f;
    float phase = 0.0f;
    float step_phase = 0.0f;
};

RopeWind make_wind(AlpineRope& rope, int64_t now_ms)
{
    RopeWind wind;
    if (!sway_is_active(rope)) {
        return wind;
    }
    wind.active = true;
    wind.dir = sway_axis_latched(rope);
    // Holding a free node at a lateral excursion of `amplitude` under a sinusoidal drive of
    // frequency `speed` needs an acceleration amplitude of amplitude * (2*pi*speed)^2; mass
    // divides it, exactly as it does for an explosion impulse.
    // The cap means a Dynamic rope's sway amplitude saturates: past
    // amplitude > rope_wind_max_accel * weight / (2*pi*speed)^2, roughly 2.5 * weight / speed^2,
    // raising the authored amplitude buys no further excursion. A painted (static) sway has no such
    // ceiling, so the same two numbers read differently on the two kinds of rope.
    const float omega = rope_two_pi * rope.sway_speed;
    wind.accel = std::min(rope.sway_amplitude * omega * omega / rope.weight, rope_wind_max_accel);
    wind.phase = sway_phase(rope, now_ms);
    wind.step_phase = omega * rope_sim_step;
    return wind;
}

void verlet_step(AlpineRope& rope, const RopeWind& wind, float wind_phase)
{
    const std::size_t n = rope.points.size();
    if (n < 2 || rope.prev.size() != n) {
        return;
    }

    const std::size_t last = n - 1;
    const float dt2 = rope_sim_step * rope_sim_step;
    const float gravity_step = -rope_gravity * dt2;
    const bool windy = wind.active && rope.arc.size() == n;
    const float inv_total = rope.total_arc > 1.0e-6f ? 1.0f / rope.total_arc : 0.0f;

    for (std::size_t i = 0; i < n; ++i) {
        if (i == 0 || (i == last && rope.has_target)) {
            continue;
        }
        const rf::Vector3 cur = rope.points[i];
        rf::Vector3 vel = (cur - rope.prev[i]) * rope_damping;
        vel.y += gravity_step;
        if (windy) {
            const float node_phase = wind_phase + rope.arc[i] * inv_total * rope_wind_node_phase;
            vel += wind.dir * (wind.accel * std::sin(node_phase) * dt2);
        }
        rope.points[i] = cur + vel;
        rope.prev[i] = cur;
    }

    rope.points[0] = rope.end_a;
    rope.prev[0] = rope.end_a;
    if (rope.has_target) {
        rope.points[last] = rope.end_b;
        rope.prev[last] = rope.end_b;
    }

    const float rest = rope.rest_length;
    for (int iter = 0; iter < rope_constraint_iterations; ++iter) {
        for (std::size_t i = 0; i + 1 < n; ++i) {
            rf::Vector3 delta = rope.points[i + 1] - rope.points[i];
            const float len = delta.len();
            if (!(len > 1.0e-6f)) {
                continue;
            }
            const float correction = (len - rest) / len;
            const bool pin_a = (i == 0);
            const bool pin_b = ((i + 1) == last && rope.has_target);
            if (pin_a && pin_b) {
                continue;
            }
            if (pin_a) {
                rope.points[i + 1] -= delta * correction;
            }
            else if (pin_b) {
                rope.points[i] += delta * correction;
            }
            else {
                const rf::Vector3 half = delta * (correction * 0.5f);
                rope.points[i] += half;
                rope.points[i + 1] -= half;
            }
        }
    }

    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(rope.points[i].x) || !std::isfinite(rope.points[i].y) ||
            !std::isfinite(rope.points[i].z)) {
            // One bad node poisons every later step, so reseed from the analytic shape instead.
            seed_dynamic(rope);
            return;
        }
    }

    if (windy) {
        // Wind never stops driving the rope, so it must never latch asleep: a swaying dynamic rope
        // pays for its substeps for as long as the level is loaded. Deliberate - Sway on a Dynamic
        // rope is an explicit authoring choice.
        rope.sleep_timer = 0.0f;
        return;
    }

    // Sleep is measured as net movement across the whole step: the integrator always adds the
    // gravity term, but at rest the constraint solve puts every node back where it started. For
    // every free node prev still holds that pre-integration position.
    float max_delta = 0.0f;
    for (std::size_t i = 0; i < n; ++i) {
        if (i == 0 || (i == last && rope.has_target)) {
            continue;
        }
        max_delta = std::max(max_delta, (rope.points[i] - rope.prev[i]).len());
    }

    if (max_delta < rope_sleep_epsilon) {
        rope.sleep_timer += rope_sim_step;
        if (rope.sleep_timer >= rope_sleep_seconds) {
            rope.asleep = true;
        }
    }
    else {
        rope.sleep_timer = 0.0f;
    }
}

// One frame's snapshot of everything that can brush a rope. Players and vehicles are all entities,
// so the entity list is the whole population; it is walked once per frame, not once per rope.
struct RopePusher
{
    rf::Vector3 pos{};
    rf::Vector3 vel{};
    float radius = 0.0f;
    int handle = -1;
    bool moving = false;
};

std::vector<RopePusher> g_pushers;
bool g_has_dynamic = false;

void collect_pushers()
{
    g_pushers.clear();
    if (!g_has_dynamic) {
        return;
    }
    for (rf::Entity& entity : DoublyLinkedList{rf::entity_list}) {
        if (entity.life <= 0.0f || rf::entity_is_dying(&entity)) {
            continue;
        }
        // An entity riding a vehicle is hidden inside it; the vehicle itself is the pusher.
        if ((static_cast<int>(entity.obj_flags) & static_cast<int>(rf::OF_HIDDEN)) != 0) {
            continue;
        }
        if (!std::isfinite(entity.pos.x) || !std::isfinite(entity.pos.y) ||
            !std::isfinite(entity.pos.z)) {
            continue;
        }
        if (!std::isfinite(entity.radius) || entity.radius <= 0.0f) {
            continue;
        }
        rf::Vector3 vel = entity.p_data.vel;
        if (!std::isfinite(vel.x) || !std::isfinite(vel.y) || !std::isfinite(vel.z)) {
            vel = rf::Vector3{0.0f, 0.0f, 0.0f};
        }
        // A driven vehicle stays a vehicle: the driver is a separate handle, so it never matches
        // the player test below.
        float radius = entity.radius;
        if (rf::entity_is_vehicle(&entity)) {
            radius *= rope_push_vehicle_radius_scale;
        }
        // The local player is the one entity that is always a player, and in single player it is the
        // only one, so testing it first answers without walking player_list at all. The walk behind
        // it is what answers for remote players.
        else if ((rf::local_player && entity.handle == rf::local_player->entity_handle) ||
                 rf::player_from_entity_handle(entity.handle) != nullptr) {
            radius *= rope_push_player_radius_scale;
        }
        RopePusher& pusher = g_pushers.emplace_back();
        pusher.pos = entity.pos;
        pusher.vel = vel;
        pusher.radius = std::min(radius, rope_push_max_radius);
        pusher.handle = entity.handle;
        pusher.moving = vel.len_sq() > rope_push_min_speed * rope_push_min_speed;
    }
}

// Positional shove out of each entity's sphere, applied once per frame before the substeps so the
// verlet integrator turns the displacement into motion on its own.
bool push_entities(AlpineRope& rope, float dt)
{
    const std::size_t n = rope.points.size();
    if (g_pushers.empty() || n < 2 || rope.prev.size() != n) {
        return false;
    }

    const std::size_t last = n - 1;
    const float max_delta = rope_push_max_speed * rope_sim_step;
    bool touched = false;

    for (const RopePusher& pusher : g_pushers) {
        // The rope's own target carries the pinned end node at its center: the shove would fight
        // the constraint solve every frame and the rope could never settle.
        if (pusher.handle != -1 && pusher.handle == rope.target_handle) {
            continue;
        }
        // Someone standing still inside a sleeping rope must not churn it back awake every frame.
        if (!pusher.moving && rope.asleep) {
            continue;
        }
        const float r = pusher.radius + rope.thickness + rope_push_margin;
        // Same reject shape as the explosion path: sphere against the cached AABB.
        if (pusher.pos.x < rope.aabb_min.x - r || pusher.pos.x > rope.aabb_max.x + r ||
            pusher.pos.y < rope.aabb_min.y - r || pusher.pos.y > rope.aabb_max.y + r ||
            pusher.pos.z < rope.aabb_min.z - r || pusher.pos.z > rope.aabb_max.z + r) {
            continue;
        }
        // Anything parked on a pinned node is that same unwinnable fight, whichever end it is.
        if ((rope.points[0] - pusher.pos).len_sq() < r * r) {
            continue;
        }
        if (rope.has_target && (rope.points[last] - pusher.pos).len_sq() < r * r) {
            continue;
        }
        for (std::size_t i = 0; i < n; ++i) {
            if (i == 0 || (i == last && rope.has_target)) {
                continue;
            }
            rf::Vector3 dir = rope.points[i] - pusher.pos;
            const float dist = dir.len();
            const float pen = r - dist;
            if (!(pen > rope_push_epsilon)) {
                continue;
            }
            if (dist > 1.0e-4f) {
                dir *= 1.0f / dist;
            }
            else {
                // Node sitting exactly on the entity origin: any horizontal direction will do, and
                // normalize_safe already falls back to one for a zero vector.
                dir = rf::Vector3{pusher.vel.x, 0.0f, pusher.vel.z};
                dir.normalize_safe();
            }
            const rf::Vector3 new_pos = rope.points[i] + dir * pen;
            // Velocity the node already carries through the shove, which a same-frame explosion may
            // have set: only the push's own contribution is capped, because capping the composed
            // vector would let a brush past a rope weaken the blast that just hit it.
            const rf::Vector3 base_vel = new_pos - rope.prev[i];
            // Bias along the entity's own motion. The push runs once per frame but one frame can
            // feed several substeps, so the bias window is capped at a single sim step - below 60
            // fps an uncapped dt would inject it repeatedly. Then capped with the explosion path's
            // cap*step pattern so an absurd entity speed cannot inject more than the ceiling no
            // matter how deep the overlap was.
            rf::Vector3 push_vel = pusher.vel * (rope_push_vel_scale * std::min(dt, rope_sim_step));
            const float len = push_vel.len();
            if (len > max_delta) {
                push_vel *= max_delta / len;
            }
            rope.points[i] = new_pos;
            rope.prev[i] = new_pos - (base_vel + push_vel);
            touched = true;
        }
    }

    if (touched) {
        rope.asleep = false;
        rope.sleep_timer = 0.0f;
    }
    return touched;
}

bool rope_is_on(const AlpineRope& rope);
void update_deco_instances(AlpineRope& rope);
void rope_fx_update(AlpineRope& rope, bool on, int64_t now_ms);

void update_rope(AlpineRope& rope, float dt, int64_t now_ms)
{
    resolve_endpoints(rope);
    const bool endpoints_moved = update_endpoint_key(rope);
    update_deco_instances(rope);

    // A static rope nothing drags never moves its effects, so it only ever pays for the one pass
    // level init already armed. A painted sway moves them every frame; a dynamic rope re-arms below
    // only when it actually integrated, so one that has gone to sleep stops paying too.
    if (endpoints_moved || (!rope.is_dynamic() && sway_is_active(rope))) {
        rope.fx_dirty = true;
    }

    if (!rope.is_dynamic()) {
        if (endpoints_moved || rope.points.size() != static_cast<std::size_t>(rope.segments) + 1) {
            build_static_shape(rope);
        }
        rope_fx_update(rope, rope_is_on(rope), now_ms);
        return;
    }

    if (!rope.seeded || rope.points.size() != static_cast<std::size_t>(rope.segments) + 1) {
        seed_dynamic(rope);
    }

    // A switched-off rope draws nothing, so it must not pay for substeps either - wind alone would
    // keep it out of the sleep latch for the rest of the level.
    if (!rope_is_on(rope)) {
        rope.was_off = true;
        // Frozen, but still anchored: a target that moves while the rope is off would otherwise
        // leave the shape stranded until it is switched back on.
        if (endpoints_moved) {
            seed_dynamic(rope);
        }
        rope_fx_update(rope, false, now_ms);
        return;
    }
    if (rope.was_off) {
        rope.was_off = false;
        seed_dynamic(rope); // resume from the rest catenary, not from however it was frozen
    }

    if (endpoints_moved) {
        rope.asleep = false;
        rope.sleep_timer = 0.0f;
    }

    const bool pushed = push_entities(rope, dt);

    if (rope.asleep) {
        rope_fx_update(rope, true, now_ms);
        return;
    }

    const RopeWind wind = make_wind(rope, now_ms);

    if (!(rope.accum >= 0.0f)) {
        rope.accum = 0.0f; // never let a poisoned accumulator survive into the substep loop
    }
    rope.accum += dt;
    int steps = 0;
    while (rope.accum >= rope_sim_step && steps < rope_max_substeps) {
        // One sim step of phase per sim step, so the drive frequency is the authored one at any
        // frame rate while the base phase stays locked to the wall clock.
        verlet_step(rope, wind, wind.phase + static_cast<float>(steps) * wind.step_phase);
        rope.accum -= rope_sim_step;
        ++steps;
    }
    if (rope.accum > rope_sim_step) {
        rope.accum = 0.0f; // starved frame: drop the backlog rather than spiral
    }
    if (steps > 0 || pushed) {
        rebuild_arc_from_points(rope);
        update_aabb(rope);
        rope.fx_dirty = true;
    }
    rope_fx_update(rope, true, now_ms);
}

// Toggled at runtime by linking the rope's anchor to the Alpine Rope_State event (id 162), which
// clears or sets OF_HIDDEN on the rope anchors among its links. Force_Unhide (id 119) still does
// the same thing to anything it is linked to. Stock Hide/Unhide events cannot: their body
// (0x004BC340) only reaches a clutter whose vmesh (+0x80) is non-null, and an anchor has none.
bool rope_is_on(const AlpineRope& rope)
{
    rf::Object* obj = rf::obj_from_handle(rope.clutter_handle);
    if (!obj) {
        return false;
    }
    return (static_cast<int>(obj->obj_flags) & static_cast<int>(rf::OF_HIDDEN)) == 0;
}

// Render-time sway for static ropes: a wall-clock displacement painted over the solved curve,
// recomputed from scratch each frame so it can neither drift nor accumulate. A dynamic rope is
// driven by real wind in the sim instead, so it never paints.
struct RopeSwayPaint
{
    bool active = false;
    rf::Vector3 dir{};
    float amount = 0.0f;
};

RopeSwayPaint make_sway_paint(AlpineRope& rope, int64_t now_ms)
{
    RopeSwayPaint paint;
    if (!sway_is_active(rope) || rope.is_dynamic()) {
        return paint;
    }
    paint.active = true;
    paint.dir = sway_axis_latched(rope);
    paint.amount = rope.sway_amplitude * std::sin(sway_phase(rope, now_ms));
    return paint;
}

// The position the ribbon actually draws point i at. Decorations ride the same function, so they
// cannot drift off the rope they are hanging on.
rf::Vector3 rope_render_point(const AlpineRope& rope, std::size_t i, const RopeSwayPaint& paint,
                              float inv_total)
{
    rf::Vector3 p = rope.points[i];
    if (paint.active) {
        const float t = rope.total_arc > 1.0e-6f ? rope.arc[i] * inv_total : 0.0f;
        // Windowed to zero at every pinned end; a free far end is allowed to swing.
        const float window = rope.has_target ? std::sin(3.14159265f * t) : std::sin(1.5707963f * t);
        p += paint.dir * (paint.amount * window);
    }
    return p;
}

// The bitmap handle to bind this frame. Chosen from the wall clock alone, so two draws in the same
// millisecond always agree and a dropped frame cannot make the animation stutter or run backwards.
int rope_bitmap_handle_now(const AlpineRope& rope, int64_t now_ms)
{
    const std::size_t frames = rope.bitmap_frames.size();
    if (frames < 2) {
        return rope.bitmap_handle;
    }
    const int64_t tick = static_cast<int64_t>(static_cast<double>(now_ms) * rope.bitmap_fps / 1000.0);
    return rope.bitmap_frames[static_cast<std::size_t>(tick % static_cast<int64_t>(frames))];
}

// True when the rope actually submitted geometry, which is what decides whether the shared gr
// state needs restoring afterwards.
bool render_rope(AlpineRope& rope, const rf::Vector3& eye, int64_t now_ms)
{
    const std::size_t n = rope.points.size();
    if (n < 2 || rope.arc.size() != n) {
        return false;
    }
    if (rf::gr::cull_bounding_box(rope.aabb_min, rope.aabb_max)) {
        return false;
    }

    // Sway never touches the simulation.
    const RopeSwayPaint paint = make_sway_paint(rope, now_ms);

    const float half = rope.thickness * 0.5f;
    const float inv_total = rope.total_arc > 1.0e-6f ? 1.0f / rope.total_arc : 0.0f;

    g_verts.resize(n * 2);

    // Tangents come from the swayed positions, not from rope.points: the width axis has to be
    // perpendicular to the curve that is actually drawn, and the paint bends that curve.
    g_render_points.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        g_render_points[i] = rope_render_point(rope, i, paint, inv_total);
    }

    rf::Vector3 prev_tangent{0.0f, 1.0f, 0.0f};
    rf::Vector3 prev_side{};
    for (std::size_t i = 0; i < n; ++i) {
        const rf::Vector3 p = g_render_points[i];

        rf::Vector3 tangent{};
        if (i == 0) {
            tangent = g_render_points[1] - g_render_points[0];
        }
        else if (i == n - 1) {
            tangent = g_render_points[n - 1] - g_render_points[n - 2];
        }
        else {
            rf::Vector3 d0 = g_render_points[i] - g_render_points[i - 1];
            rf::Vector3 d1 = g_render_points[i + 1] - g_render_points[i];
            d0.normalize_safe();
            d1.normalize_safe();
            tangent = d0 + d1;
        }
        if (tangent.len_sq() < 1.0e-12f) {
            tangent = prev_tangent;
        }
        tangent.normalize_safe();
        prev_tangent = tangent;

        rf::Vector3 side = tangent.cross(p - eye);
        if (side.len_sq() < 1.0e-12f) {
            side = tangent.cross(rf::Vector3{0.0f, 1.0f, 0.0f});
        }
        side.normalize_safe();
        // The tangent reverses at a vertical fold, which flips the side vector and bowties the
        // ribbon; keeping consecutive sides on the same half-space stops that.
        if (i > 0 && side.dot_prod(prev_side) < 0.0f) {
            side = -side;
        }
        prev_side = side;
        side *= half;

        // V runs along the rope's length, U across its width.
        const float v = (rope.uv_tiles_per_meter > 0.0f) ? rope.arc[i] * rope.uv_tiles_per_meter
                                                         : rope.arc[i] * inv_total;

        rf::Vector3 pa = p + side;
        rf::Vector3 pb = p - side;
        rf::gr::Vertex& va = g_verts[i * 2];
        rf::gr::Vertex& vb = g_verts[i * 2 + 1];
        rf::gr::rotate_vertex(&va, &pa);
        rf::gr::rotate_vertex(&vb, &pb);
        // Only u1/v1 is filled, and no vertex color: the draw below passes TMAP_FLAG_TEXTURED
        // alone, so the second UV set is never sampled and the tint comes from gr::set_color via
        // current_color. Adding TMAP_FLAG_RGB would make the renderer read per-vertex colors
        // rotate_vertex never writes - the colors would have to be filled here first.
        va.u1 = 0.0f; va.v1 = v;
        vb.u1 = 1.0f; vb.v1 = v;
    }

    const bool textured = rope.bitmap_handle >= 0;
    const rf::gr::Mode mode = textured ? (rope.is_glow() ? rope_mode_textured_glow : rope_mode_textured)
                                       : (rope.is_glow() ? rope_mode_untextured_glow : rope_mode_untextured);

    rf::gr::set_texture(textured ? rope_bitmap_handle_now(rope, now_ms) : -1, -1);
    rf::gr::set_color(rope.color.red, rope.color.green, rope.color.blue, rope.color.alpha);

    for (std::size_t i = 0; i + 1 < n; ++i) {
        // Shared edge vertices: adjacent quads reuse these exact positions, so the ribbon cannot
        // crack. Winding matches gr_3d_bitmap_oriented_wh's front face (normal toward the eye).
        rf::gr::Vertex* quad[4] = {&g_verts[i * 2], &g_verts[i * 2 + 1], &g_verts[i * 2 + 3],
                                   &g_verts[i * 2 + 2]};
        rf::gr::poly(4, quad, rf::gr::TMAP_FLAG_TEXTURED, mode, 0, 0.0f);
    }
    return true;
}

// ─── Decorations ────────────────────────────────────────────────────────────

// Absurd geometry must not blow the rope's cull box out to cover the level.
constexpr float rope_deco_max_radius = 100.0f;

// Loads a decoration mesh once per level and caches it by lowercased name. Level init only: a
// mesh load in the render hook drags the bitmap system in with it and corrupts it.
int resolve_deco_mesh(const std::string& name)
{
    if (name.empty()) {
        return -1;
    }
    const std::string key = string_to_lower(name);
    auto it = g_deco_mesh_lookup.find(key);
    if (it != g_deco_mesh_lookup.end()) {
        return it->second; // -1 for a name already known to be unloadable, so it warns once
    }

    // vmesh_load hard-sets STATIC, which a .vfx cannot be: it would load as a mesh with no frames
    // and draw nothing. Rejected here rather than in the engine so the mapper gets told.
    if (string_ends_with(key, ".vfx")) {
        xlog::warn("[AlpineRope] Decoration mesh '{}' is an animated .vfx; decorations are static "
                   "geometry only", name);
        g_deco_mesh_lookup.emplace(key, -1);
        return -1;
    }

    rf::VMesh* mesh = rf::vmesh_load(name.c_str(), rf::MESH_TYPE_STATIC, -1);
    if (!mesh) {
        xlog::warn("[AlpineRope] Failed to load decoration mesh '{}'", name);
        g_deco_mesh_lookup.emplace(key, -1);
        return -1;
    }

    rf::Vector3 bbox_min{}, bbox_max{};
    rf::vmesh_get_bbox(mesh, &bbox_min, &bbox_max);
    // Radius about the mesh origin, because that is the point sitting on the curve, not the bbox
    // centre the engine would use.
    const rf::Vector3 extent{std::max(std::fabs(bbox_min.x), std::fabs(bbox_max.x)),
                             std::max(std::fabs(bbox_min.y), std::fabs(bbox_max.y)),
                             std::max(std::fabs(bbox_min.z), std::fabs(bbox_max.z))};
    float radius = extent.len();
    if (!std::isfinite(radius) || radius < 0.0f) {
        radius = 0.0f;
    }
    radius = std::min(radius, rope_deco_max_radius);

    const int index = static_cast<int>(g_deco_meshes.size());
    g_deco_meshes.push_back(RopeDecoMesh{mesh, radius});
    g_deco_mesh_lookup.emplace(key, index);
    return index;
}

uint32_t deco_rand(uint32_t& state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

void build_deco_instances(AlpineRope& rope)
{
    RopeDeco& deco = rope.deco;
    deco.inst_t.clear();
    deco.inst_name.clear();
    deco.draw_order.clear();

    const int names = static_cast<int>(deco.mesh_names.size());
    const float len = rope.rope_length;
    if (!deco.enabled || names <= 0 || !(len > 0.0f)) {
        return;
    }

    rope_curve::build_decoration_arcs(len, deco.spacing_mode, deco.count, deco.spacing, g_deco_arc);
    const std::size_t n = g_deco_arc.size();
    if (n == 0) {
        return;
    }

    // Stored normalized: a dynamic rope's measured arc length breathes around the nominal one, and
    // decorations have to stay put on the rope rather than creep along it.
    const float inv_len = 1.0f / len;
    deco.inst_t.resize(n);
    deco.inst_name.resize(n);

    // Identity depends only on (uid, index), so a rope whose length crosses a spacing boundary
    // keeps every surviving instance's mesh instead of reshuffling the whole run. Each complete
    // block of `names` instances is one permutation, so every mesh still appears exactly once per
    // block. Never rand(): that is shared libc state any other system can advance out from under us.
    const uint32_t base = static_cast<uint32_t>(rope.uid) * 2654435761u + 0x9E3779B9u;
    int perm[rope_curve::deco_max_meshes] = {};
    std::size_t built_block = static_cast<std::size_t>(-1);

    for (std::size_t i = 0; i < n; ++i) {
        deco.inst_t[i] = std::clamp(g_deco_arc[i] * inv_len, 0.0f, 1.0f);
        const std::size_t slot = i % static_cast<std::size_t>(names);
        if (!deco.random_order) {
            deco.inst_name[i] = static_cast<int>(slot);
            continue;
        }
        const std::size_t block = i / static_cast<std::size_t>(names);
        if (block != built_block) {
            built_block = block;
            uint32_t state = base ^ (static_cast<uint32_t>(block) * 0x85EBCA6Bu);
            if (state == 0) {
                state = 0x1234567u;
            }
            for (int k = 0; k < names; ++k) {
                perm[k] = k;
            }
            for (int k = names - 1; k > 0; --k) {
                std::swap(perm[k], perm[deco_rand(state) % static_cast<uint32_t>(k + 1)]);
            }
        }
        deco.inst_name[i] = perm[slot];
    }

    // Grouped by mesh, so a rope carrying six kinds of lantern costs six draw runs rather than one
    // pipeline change per instance. Counting pass then placement pass: every inst_name is in
    // [0, names) by construction above, so one walk per pass covers it and the cost stays linear in
    // instances instead of names * instances.
    deco.draw_order.resize(n);
    int slot_start[rope_curve::deco_max_meshes + 1] = {};
    for (std::size_t i = 0; i < n; ++i) {
        ++slot_start[deco.inst_name[i] + 1];
    }
    for (int name = 0; name < names; ++name) {
        slot_start[name + 1] += slot_start[name];
    }
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t at = static_cast<std::size_t>(slot_start[deco.inst_name[i]]++);
        deco.draw_order[at] = static_cast<int>(i);
    }
}

// The instance list only depends on the rope's length and its authored placement settings, and the
// settings never change at runtime, so this is the whole rebuild trigger.
void update_deco_instances(AlpineRope& rope)
{
    if (!rope.deco.enabled) {
        return;
    }
    const int32_t len_q = rope_curve::quantize_1024(rope.rope_length);
    if (len_q == rope.deco.built_len_q) {
        return;
    }
    rope.deco.built_len_q = len_q;
    build_deco_instances(rope);
    rope.fx_dirty = true;
}

void deco_orient_from_tangent(uint8_t mode, const rf::Vector3& tangent, RopeDecoLatch& latch,
                              rf::Matrix3& out)
{
    if (mode == rope_deco_orient_follow) {
        rf::Vector3 fwd = tangent;
        fwd.normalize_safe();
        rf::Vector3 right = rope_world_up.cross(fwd);
        if (right.len_sq() < 1.0e-8f) {
            // Vertical tangent: every horizontal direction is a valid right vector, so take the
            // latched one instead of whichever way the cross product happens to collapse.
            right = latch.dir;
        }
        else {
            right.normalize_safe();
            // Same half-space rule the ribbon uses for its side vectors, here keeping the roll
            // continuous instead of the winding.
            if (latch.valid && right.dot_prod(latch.dir) < 0.0f) {
                right = -right;
            }
            latch.dir = right;
            latch.valid = true;
        }
        out.fvec = fwd;
        out.rvec = right;
        out.uvec = fwd.cross(right);
        out.uvec.normalize_safe();
        return;
    }

    rf::Vector3 fwd{tangent.x, 0.0f, tangent.z};
    if (fwd.len_sq() < 1.0e-8f) {
        fwd = latch.dir; // hanging straight down: hold the last heading that was well defined
    }
    else {
        fwd.normalize_safe();
        if (latch.valid && fwd.dot_prod(latch.dir) < 0.0f) {
            fwd = -fwd;
        }
        latch.dir = fwd;
        latch.valid = true;
    }
    out.fvec = fwd;
    out.uvec = rope_world_up;
    out.rvec = rope_world_up.cross(fwd);
    out.rvec.normalize_safe();
}

constexpr float rope_deg_to_rad = 3.14159265f / 180.0f;

// Instance-local offsets: the position offset runs along the base frame's own axes, then the
// rotation offset turns the mesh inside that frame. The single derivation for the mesh draw, the
// glare and the light, so the three can never disagree about where an instance is.
void apply_slot_offsets(const RopeSlotFx& fx, RopeDecoXform& xform)
{
    if (!fx.has_offset()) {
        return;
    }
    xform.pos += xform.orient.rvec * fx.pos_offset.x + xform.orient.uvec * fx.pos_offset.y +
                 xform.orient.fvec * fx.pos_offset.z;
    if (fx.rot_offset.len_sq() > 0.0f) {
        rf::Matrix3 rot{};
        rot.set_from_angles(fx.rot_offset.x * rope_deg_to_rad, fx.rot_offset.z * rope_deg_to_rad,
                            fx.rot_offset.y * rope_deg_to_rad);
        xform.orient.mul(rot);
    }
}

// Instance transforms in arc order, taken from the points the ribbon is drawing this frame. The
// instances are sorted by arc position, so one merged walk covers the whole rope.
// Built at most once per rope per frame: the effect pass and the mesh pass want the same transforms,
// and so does every extra view, while nothing can move the rope between them - the sim writes its
// nodes before the effect pass and the render passes only read. The latch walk is not idempotent, so
// sharing the build is also what keeps the decoration roll from advancing twice a frame.
void build_deco_xforms(AlpineRope& rope, const RopeSwayPaint& paint, float inv_total)
{
    RopeDeco& deco = rope.deco;
    const std::size_t n = deco.inst_t.size();
    const std::size_t points = rope.points.size();
    if (deco.xforms_frame == g_deco_xform_frame && deco.xforms.size() == n) {
        return;
    }
    deco.xforms_frame = g_deco_xform_frame;
    deco.xforms.resize(n);

    std::size_t seg = 0;
    rf::Vector3 seg_a = rope_render_point(rope, 0, paint, inv_total);
    rf::Vector3 seg_b = rope_render_point(rope, 1, paint, inv_total);

    for (std::size_t i = 0; i < n; ++i) {
        const float s = deco.inst_t[i] * rope.total_arc;
        while (seg + 2 < points && rope.arc[seg + 1] < s) {
            ++seg;
            seg_a = seg_b;
            seg_b = rope_render_point(rope, seg + 1, paint, inv_total);
        }
        const float span = rope.arc[seg + 1] - rope.arc[seg];
        const float f = span > 1.0e-6f ? std::clamp((s - rope.arc[seg]) / span, 0.0f, 1.0f) : 0.0f;
        deco.xforms[i].pos = seg_a + (seg_b - seg_a) * f;
        deco_orient_from_tangent(deco.orient_mode, seg_b - seg_a, rope.deco_latch,
                                 deco.xforms[i].orient);
        const int name = deco.inst_name[i];
        if (name >= 0 && static_cast<std::size_t>(name) < deco.fx.size()) {
            apply_slot_offsets(deco.fx[static_cast<std::size_t>(name)], deco.xforms[i]);
        }
    }
}

bool render_rope_decorations(AlpineRope& rope, const RopeSwayPaint& paint)
{
    RopeDeco& deco = rope.deco;
    const std::size_t points = rope.points.size();
    if (deco.inst_t.empty() || points < 2 || rope.arc.size() != points) {
        return false;
    }

    const float inv_total = rope.total_arc > 1.0e-6f ? 1.0f / rope.total_arc : 0.0f;
    build_deco_xforms(rope, paint, inv_total);

    rf::MeshRenderParams params{};
    params.init_defaults();

    bool drew_any = false;
    int current_name = -1;
    rf::VMesh* mesh = nullptr;
    float radius = 0.0f;
    for (int index : deco.draw_order) {
        const int name = deco.inst_name[static_cast<std::size_t>(index)];
        if (name != current_name) {
            current_name = name;
            const int slot = deco.slots[static_cast<std::size_t>(name)];
            mesh = slot >= 0 ? g_deco_meshes[static_cast<std::size_t>(slot)].mesh : nullptr;
            radius = slot >= 0 ? g_deco_meshes[static_cast<std::size_t>(slot)].radius : 0.0f;
        }
        if (!mesh) {
            continue;
        }
        RopeDecoXform& xform = deco.xforms[static_cast<std::size_t>(index)];
        if (rf::gr::cull_sphere(xform.pos, radius)) {
            continue;
        }
        params.orient = xform.orient;
        rf::vmesh_render(mesh, &xform.pos, &xform.orient, &params);
        drew_any = true;
    }
    return drew_any;
}

// ─── Decoration slot FX ─────────────────────────────────────────────────────

// Every GlareInfo this module owns. The glare objects themselves belong to the object system, the
// same split alpine_corona.cpp works to.
std::vector<rf::GlareInfo*> g_rope_glare_infos;

// Legacy backends would have to patch CPU lightmaps to light anything from a moving source, which
// is prohibitive; the D3D11 renderer lights meshes and geometry from the live light list.
bool rope_fx_lights_enabled()
{
    return is_d3d11();
}

// A handle whose pool entry the engine already reclaimed would unlink through a null prev pointer,
// the known double-delete crash class, so every delete path goes through here.
void rope_light_release(RopeFxInstance& inst)
{
    // Every path here means the instance wants no light right now, so whenever one is wanted again
    // it deserves a fresh attempt rather than the old failure latch - the pool may have freed space
    // in the meantime. Same lifecycle as glare_failed in rope_glare_disable.
    inst.light_failed = false;
    if (inst.light_handle < 0) {
        return;
    }
    rf::gr::Light* light = rf::gr::light_get_from_handle(inst.light_handle);
    if (light && light->type != rf::gr::LT_NONE) {
        rf::gr::light_delete(inst.light_handle, 0);
    }
    inst.light_handle = -1;
}

// Moving a point light needs no recreation: light_create_point (0x004D8ED0) derives nothing from the
// position beyond the vec itself - it leaves room 0 and stores radius squared, both independent of
// where the light sits - and the D3D11 gather reads Light::vec live. What a create or a delete does
// change is the light cache: both bump the key gr_light_find_all_by_gsolid (0x004D9870) tests, and
// both end by dropping the current filter results. An in-place move needs the same two steps, done
// once by the caller after all of a rope's lights have moved (see rope_light_invalidate_cache).
// False when the pool entry is no longer ours, which is the caller's cue to create the light again.
bool rope_light_move(int handle, const rf::Vector3& pos, bool& moved)
{
    rf::gr::Light* light = rf::gr::light_get_from_handle(handle);
    if (!light || light->type == rf::gr::LT_NONE) {
        return false;
    }
    if (light->vec != pos) {
        light->vec = pos;
        moved = true;
    }
    return true;
}

void rope_light_invalidate_cache()
{
    ++rf::gr::light_cache_key;
    rf::gr::light_filter_reset();
}

// Level-init only: bm::load pulls in the bitmap system, which must never happen from a render or
// sim path. Same field semantics as the alpine corona parse, including the halved cone angle.
rf::GlareInfo* rope_glare_info_create(int32_t uid, const RopeSlotFx& fx)
{
    auto* gi = new rf::GlareInfo{};
    gi->light_color = rf::gr::Color{fx.glare_r, fx.glare_g, fx.glare_b, fx.glare_a};

    if (!fx.glare_bitmap.empty()) {
        gi->corona_bitmap = rf::bm::load(fx.glare_bitmap.c_str(), -1, true);
        if (gi->corona_bitmap < 0) {
            xlog::warn("[AlpineRope] uid={} failed to load glare bitmap '{}'", uid, fx.glare_bitmap);
            gi->corona_bitmap = 0;
        }
    }

    gi->cone_angle = fx.cone_angle * alpine_glare_cone_angle_factor;
    gi->intensity = fx.intensity;
    gi->radius_scale = fx.radius_scale;
    gi->radius_distance = fx.radius_distance;
    gi->diminish_distance = fx.diminish_distance;

    if (!fx.volumetric_bitmap.empty()) {
        gi->volumetric_bitmap = rf::bm::load(fx.volumetric_bitmap.c_str(), -1, true);
        if (gi->volumetric_bitmap < 0) {
            xlog::warn("[AlpineRope] uid={} failed to load volumetric bitmap '{}'", uid,
                       fx.volumetric_bitmap);
            gi->volumetric_bitmap = 0;
        }
        gi->volumetric_height = fx.volumetric_height;
        gi->volumetric_length = fx.volumetric_length;
    }

    gi->reflection_bitmap = 0;
    g_rope_glare_infos.push_back(gi);
    return gi;
}

// alpine_corona_create_all's recipe minus the anchor clutter: no parent, no uid, the glare carries
// nothing but our GlareInfo. Inherits the engine's occlusion fade, room culling, distance sizing
// and splitscreen handling for free.
rf::Glare* rope_glare_create(const rf::Vector3& pos, const rf::Matrix3& orient, rf::GlareInfo* info)
{
    rf::ObjectCreateInfo oci{};
    oci.pos = pos;
    oci.orient = orient;

    rf::Object* obj = rf::obj_create(rf::OT_GLARE, -1, -1, &oci, 0x30000, nullptr);
    if (!obj) {
        return nullptr;
    }

    auto* glare = reinterpret_cast<rf::Glare*>(obj);
    glare->enabled = true;
    glare->info = info;
    glare->info_index = -1;
    glare->flags = 0;
    glare->last_covering_objh = -1;
    glare->last_covering_mover_brush = nullptr;
    glare->last_covering_face = nullptr;
    glare->last_rendered_intensity[0] = 0.0f;
    glare->last_rendered_intensity[1] = 0.0f;
    glare->last_rendered_radius[0] = 0.0f;
    glare->last_rendered_radius[1] = 0.0f;
    glare->is_rod = false;
    glare->parent_player = nullptr;

    rf::Glare* tail = rf::glare_list_tail;
    glare->prev = tail;
    glare->next = &rf::glare_list;
    tail->next = glare;
    rf::glare_list_tail = glare;
    return glare;
}

void rope_glare_disable(RopeFxInstance& inst)
{
    // Every path here means the instance wants no glare right now, so whenever one is wanted again
    // it deserves a fresh attempt rather than the old failure latch.
    inst.glare_failed = false;
    if (inst.glare_handle == -1) {
        return;
    }
    rf::Object* obj = rf::obj_from_handle(inst.glare_handle);
    if (obj) {
        reinterpret_cast<rf::Glare*>(obj)->enabled = false;
    }
    else {
        inst.glare_handle = -1;
    }
}

// Cosmetics-gated level init: allocates one GlareInfo per glare slot and loads its bitmaps.
void rope_fx_level_init(AlpineRope& rope)
{
    RopeDeco& deco = rope.deco;
    deco.any_glare = false;
    deco.any_light = false;
    deco.glare_infos.assign(deco.fx.size(), nullptr);

    for (std::size_t i = 0; i < deco.fx.size(); ++i) {
        const RopeSlotFx& fx = deco.fx[i];
        if (fx.has_glare()) {
            deco.glare_infos[i] = rope_glare_info_create(rope.uid, fx);
            deco.any_glare = true;
        }
        if (fx.has_light()) {
            deco.any_light = true;
        }
    }

    rope.fx_dirty = true;
    rope.fx_applied_on = -1;
}

// Glare positions and light handles for one rope. Runs from the sim path, so it keeps working while
// the rope is off screen - the engine renders the glares and lights on its own schedule.
void rope_fx_update(AlpineRope& rope, bool on, int64_t now_ms)
{
    RopeDeco& deco = rope.deco;
    if (!deco.has_fx()) {
        return;
    }

    const std::size_t n = deco.inst_t.size();
    if (deco.fx_inst.size() < n) {
        // Grows with the instance list (a mover can lengthen a rope in meters mode) and never
        // shrinks, so the surplus objects are reused instead of churned.
        deco.fx_inst.resize(n);
    }

    const int on_state = on ? 1 : 0;
    const bool on_changed = rope.fx_applied_on != on_state;

    if (!on) {
        if (on_changed) {
            for (RopeFxInstance& inst : deco.fx_inst) {
                rope_glare_disable(inst);
                rope_light_release(inst);
            }
            rope.fx_applied_on = on_state;
        }
        return;
    }

    if (!rope.fx_dirty && !on_changed) {
        return;
    }

    const std::size_t points = rope.points.size();
    if (points < 2 || rope.arc.size() != points || deco.inst_name.size() != n) {
        return;
    }

    const float inv_total = rope.total_arc > 1.0e-6f ? 1.0f / rope.total_arc : 0.0f;
    build_deco_xforms(rope, make_sway_paint(rope, now_ms), inv_total);

    const bool lights_on = rope_fx_lights_enabled();
    bool lights_moved = false;

    for (std::size_t i = 0; i < deco.fx_inst.size(); ++i) {
        RopeFxInstance& inst = deco.fx_inst[i];
        if (i >= n) {
            // The rope shortened past this instance: dark, but kept for when it lengthens again.
            rope_glare_disable(inst);
            rope_light_release(inst);
            continue;
        }

        const int name = deco.inst_name[i];
        const RopeSlotFx* fx = (name >= 0 && static_cast<std::size_t>(name) < deco.fx.size())
                                   ? &deco.fx[static_cast<std::size_t>(name)]
                                   : nullptr;
        const RopeDecoXform& xform = deco.xforms[i];

        rf::GlareInfo* info = nullptr;
        if (fx && fx->has_glare() && static_cast<std::size_t>(name) < deco.glare_infos.size()) {
            info = deco.glare_infos[static_cast<std::size_t>(name)];
        }
        if (info) {
            rf::Glare* glare = nullptr;
            if (inst.glare_handle != -1) {
                glare = reinterpret_cast<rf::Glare*>(rf::obj_from_handle(inst.glare_handle));
                if (!glare) {
                    inst.glare_handle = -1;
                }
            }
            if (!glare && (!inst.glare_failed ||
                           (xform.pos - inst.glare_fail_pos).len_sq() > rope_glare_retry_dist_sq)) {
                glare = rope_glare_create(xform.pos, xform.orient, info);
                if (glare) {
                    inst.glare_handle = glare->handle;
                    inst.glare_failed = false;
                }
                else {
                    inst.glare_fail_pos = xform.pos;
                    inst.glare_failed = true;
                }
            }
            if (glare) {
                // Re-assigned because a pooled instance can be reused for a different slot after the
                // instance list regrows.
                glare->info = info;
                glare->pos = xform.pos;
                glare->orient = xform.orient;
                glare->enabled = true;
                // obj_create resolves Object::room once and the engine never revisits it, but the
                // per-room glare enqueue (0x00488230) gates on it, so a glare that moves has to
                // re-derive its own room or it blinks out across portals.
                glare->update_room();
            }
        }
        else {
            rope_glare_disable(inst);
        }

        if (fx && fx->has_light() && lights_on) {
            // Created once and then moved in place: nothing light_create_point derives from the
            // position outlives the vec write, so a rebuild per frame would buy nothing.
            if (inst.light_handle < 0 || !rope_light_move(inst.light_handle, xform.pos, lights_moved)) {
                // A refused create is latched: the pool is a fixed free list, so a rope whose
                // effects refresh every frame would otherwise re-ask for the whole level.
                if (!inst.light_failed) {
                    rf::Vector3 pos = xform.pos;
                    inst.light_handle = rf::gr::light_create_point(
                        &pos, fx->light_radius, fx->light_intensity, fx->light_r / 255.0f,
                        fx->light_g / 255.0f, fx->light_b / 255.0f, true,
                        rf::gr::LightShadowcastCondition::SHADOWCAST_EDITOR, 0);
                    inst.light_failed = inst.light_handle < 0;
                }
            }
        }
        else {
            rope_light_release(inst);
        }
    }

    // Nothing queries lights inside the loop, so one invalidation covers every move above.
    if (lights_moved) {
        rope_light_invalidate_cache();
    }

    rope.fx_dirty = false;
    rope.fx_applied_on = on_state;
}

// Handles only: the glare objects are the level's to destroy, exactly as the corona pairs are. A
// still-live glare is switched off first, so it stops rendering before its GlareInfo goes away.
void rope_fx_release(AlpineRope& rope)
{
    for (RopeFxInstance& inst : rope.deco.fx_inst) {
        rope_glare_disable(inst);
        rope_light_release(inst);
        inst.glare_handle = -1;
    }
    rope.deco.fx_inst.clear();
    rope.deco.glare_infos.clear();
}

// An animated bitmap is one head entry followed by one entry per frame (bm_load 0x0050F6E0), which
// is what bm_get_cache_slot (0x0050F440) walks when the engine resolves a frame off its own clock.
// Collecting the frame handles here lets the rope bind the frame it wants: one handle per frame
// also guarantees the D3D11 handle cache and the dynamic geometry batcher see the change.
void rope_collect_bitmap_frames(AlpineRope& rope)
{
    rope.bitmap_frames.clear();
    rope.bitmap_fps = 0.0f;
    if (rope.bitmap_handle < 0 || !rf::bm::bitmaps) {
        return;
    }
    // handle_to_index reduces the handle modulo the slot count, so it can never come back negative;
    // what does have to be checked is that this head's frame slots all land inside the table, since
    // num_frames is file data and the head can sit anywhere in it.
    const int slots = rf::bm::num_cache_slots;
    const int head = rf::bm::handle_to_index(rope.bitmap_handle);
    if (slots <= 0 || head >= slots) {
        return;
    }
    const rf::bm::BitmapEntry& entry = rf::bm::bitmaps[head];
    if (entry.animated_entry_type != 1 || entry.num_frames < 2) {
        return;
    }
    const int frames = entry.num_frames;
    if (head + 1 + frames > slots) {
        xlog::warn("[AlpineRope] uid={} bitmap '{}' declares {} frames past the end of the bitmap "
                   "table, animating it as a still", rope.uid, rope.bitmap_name, frames);
        return;
    }

    // frames_per_ms is what the loader stored from the file's own rate; a header that declares none
    // leaves it at zero, and an authored rate outside the sane band is held to it - below the floor
    // the frame index would never advance, above the ceiling the tick multiply runs away.
    float fps = entry.frames_per_ms * 1000.0f;
    if (!std::isfinite(fps) || fps <= 0.0f) {
        fps = rope_default_bitmap_fps;
    }
    rope.bitmap_fps = std::clamp(fps, rope_min_bitmap_fps, rope_max_bitmap_fps);
    rope.bitmap_frames.reserve(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        rope.bitmap_frames.push_back(rf::bm::bitmaps[head + 1 + i].handle);
    }
}

void log_rope_chunk_truncated(uint32_t created)
{
    xlog::warn("[AlpineRope] Chunk is truncated, keeping {} rope(s)", created);
}

CallHook<void(rf::Vector3*, float, float, int, int)> rope_explosion_apply_radius_damage_hook{
    // Fourth and only unwrapped caller of explosion_apply_radius_damage (0x00488DC0). The
    // function itself opens on FLD m32fp + FCOMP m32fp, and SubHook cannot relocate FCOMP, so the
    // call sites are the only hookable points; the other three already carry AF CallHooks and
    // call alpine_rope_apply_explosion directly.
    0x0043660D,
    [](rf::Vector3* pos, float damage, float radius, int killer_handle, int damage_type) {
        rope_explosion_apply_radius_damage_hook.call_target(pos, damage, radius, killer_handle,
                                                            damage_type);
        if (pos) {
            alpine_rope_apply_explosion(*pos, radius, damage);
        }
    },
};

} // namespace

void alpine_rope_load_chunk(rf::File& file, std::size_t chunk_len)
{
    std::size_t remaining = chunk_len;

    rf::File::ChunkGuard chunk_guard{file, remaining};

    AlpineChunkReader reader{file, remaining};

    // Payload growth appends per-record fields gated on the RFL content version (mesh flag-block
    // precedent); no chunk version - a reader never sees a file above its supported RFL version.
    uint32_t count = 0;
    if (!reader.read_bytes(&count, sizeof(count))) {
        xlog::warn("[AlpineRope] Failed to read count from chunk (len={})", chunk_len);
        return;
    }
    if (count > rope_curve::rope_max_count) {
        xlog::warn("[AlpineRope] Chunk declares {} ropes, reading the first {}", count, rope_curve::rope_max_count);
        count = rope_curve::rope_max_count;
    }

    xlog::info("[AlpineRope] Loading {} rope(s) from chunk (len={})", count, chunk_len);

    uint32_t created = 0;
    for (uint32_t i = 0; i < count; ++i) {
        AlpineRope rope;
        rf::Matrix3 orient{};
        std::string script_name;

        if (!reader.read_bytes(&rope.uid, sizeof(rope.uid))) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&rope.pos.x, sizeof(float))) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&rope.pos.y, sizeof(float))) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&rope.pos.z, sizeof(float))) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&orient.rvec.x, sizeof(float))) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&orient.rvec.y, sizeof(float))) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&orient.rvec.z, sizeof(float))) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&orient.uvec.x, sizeof(float))) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&orient.uvec.y, sizeof(float))) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&orient.uvec.z, sizeof(float))) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&orient.fvec.x, sizeof(float))) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&orient.fvec.y, sizeof(float))) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&orient.fvec.z, sizeof(float))) return log_rope_chunk_truncated(created);
        if (!reader.read_string(script_name)) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&rope.target_uid, sizeof(rope.target_uid))) return log_rope_chunk_truncated(created);

        float dangle = 0.0f, slack = 0.0f, weight = 0.0f, thickness = 0.0f, uv_tiles = 0.0f;
        float sway_amplitude = 0.0f, sway_speed = 0.0f;
        int32_t segments = 0;
        uint32_t packed_color = 0;
        uint8_t initially_on = 1;

        if (!reader.read_bytes(&dangle, sizeof(dangle))) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&slack, sizeof(slack))) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&weight, sizeof(weight))) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&thickness, sizeof(thickness))) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&segments, sizeof(segments))) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&uv_tiles, sizeof(uv_tiles))) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&packed_color, sizeof(packed_color))) return log_rope_chunk_truncated(created);
        if (!reader.read_string(rope.bitmap_name)) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&sway_amplitude, sizeof(sway_amplitude))) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&sway_speed, sizeof(sway_speed))) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&rope.flags, sizeof(rope.flags))) return log_rope_chunk_truncated(created);
        if (!reader.read_bytes(&initially_on, sizeof(initially_on))) return log_rope_chunk_truncated(created);

        // Decoration sub-block, gated like the mesh destructible block: a rope with no decoration
        // meshes at all costs exactly one zero byte. The Decorations checkbox is a field INSIDE the
        // block, not the gate on it, so unchecking it keeps the authored mesh list on disk.
        uint8_t has_deco_block = 0;
        uint8_t decorations_enabled = 0;
        if (!reader.read_bytes(&has_deco_block, sizeof(has_deco_block))) return log_rope_chunk_truncated(created);
        if (has_deco_block != 0) {
            if (!reader.read_bytes(&decorations_enabled, sizeof(decorations_enabled))) return log_rope_chunk_truncated(created);
            uint8_t mesh_count = 0;
            if (!reader.read_bytes(&mesh_count, sizeof(mesh_count))) return log_rope_chunk_truncated(created);
            if (mesh_count < 1 || mesh_count > rope_curve::deco_max_meshes) {
                // The count was read, so this is not truncation - but every field behind it is
                // measured from the mesh names, so there is no way to work out where this record
                // ends and the whole rest of the chunk goes with it.
                xlog::warn("[AlpineRope] uid={} declares {} decoration meshes, which is outside "
                           "1..{}; the record length cannot be derived, so the rest of the chunk is "
                           "dropped and {} rope(s) are kept",
                           rope.uid, mesh_count, rope_curve::deco_max_meshes, created);
                return;
            }
            // Each slot's extras travel with its name as one unit, so a dropped name takes its own
            // effects with it and can never leave the rest of the slots reading someone else's.
            std::vector<std::string> names(mesh_count);
            std::vector<RopeSlotFx> slot_fx(mesh_count);
            for (uint8_t m = 0; m < mesh_count; ++m) {
                if (!reader.read_string(names[m])) return log_rope_chunk_truncated(created);
                if (names[m].size() >= max_mesh_name) {
                    xlog::warn("[AlpineRope] uid={} decoration mesh name is {} chars, ignoring it",
                               rope.uid, names[m].size());
                    names[m].clear();
                }

                RopeSlotFx& fx = slot_fx[m];
                float offsets[6] = {};
                for (float& v : offsets) {
                    if (!reader.read_bytes(&v, sizeof(float))) return log_rope_chunk_truncated(created);
                }
                const float max_pos = rope_curve::deco_fx_max_pos_offset;
                const float max_rot = rope_curve::deco_fx_max_rot_offset;
                fx.pos_offset = rf::Vector3{rope_curve::clamp_finite(offsets[0], -max_pos, max_pos, 0.0f),
                                            rope_curve::clamp_finite(offsets[1], -max_pos, max_pos, 0.0f),
                                            rope_curve::clamp_finite(offsets[2], -max_pos, max_pos, 0.0f)};
                fx.rot_offset = rf::Vector3{rope_curve::clamp_finite(offsets[3], -max_rot, max_rot, 0.0f),
                                            rope_curve::clamp_finite(offsets[4], -max_rot, max_rot, 0.0f),
                                            rope_curve::clamp_finite(offsets[5], -max_rot, max_rot, 0.0f)};

                uint8_t slot_flags = 0;
                if (!reader.read_bytes(&slot_flags, sizeof(slot_flags))) return log_rope_chunk_truncated(created);
                fx.flags = slot_flags & rope_curve::deco_slot_flag_mask;

                if (fx.has_glare()) {
                    if (!reader.read_string(fx.glare_bitmap)) return log_rope_chunk_truncated(created);
                    if (bitmap_name_over_long(fx.glare_bitmap)) {
                        xlog::warn("[AlpineRope] uid={} glare bitmap '{}' is too long, ignoring it",
                                   rope.uid, fx.glare_bitmap);
                        fx.glare_bitmap.clear();
                    }
                    if (!reader.read_bytes(&fx.glare_r, sizeof(uint8_t))) return log_rope_chunk_truncated(created);
                    if (!reader.read_bytes(&fx.glare_g, sizeof(uint8_t))) return log_rope_chunk_truncated(created);
                    if (!reader.read_bytes(&fx.glare_b, sizeof(uint8_t))) return log_rope_chunk_truncated(created);
                    if (!reader.read_bytes(&fx.glare_a, sizeof(uint8_t))) return log_rope_chunk_truncated(created);

                    float glare_floats[5] = {};
                    for (float& v : glare_floats) {
                        if (!reader.read_bytes(&v, sizeof(float))) return log_rope_chunk_truncated(created);
                    }
                    fx.cone_angle =
                        rope_curve::clamp_finite(glare_floats[0], 0.0f, rope_curve::deco_fx_max_cone_angle,
                                                 rope_curve::deco_fx_default_cone_angle);
                    fx.intensity = rope_curve::clamp_finite(glare_floats[1], 0.0f, rope_curve::deco_fx_max_intensity,
                                                rope_curve::deco_fx_default_intensity);
                    fx.radius_distance =
                        rope_curve::clamp_finite(glare_floats[2], 0.0f, rope_curve::deco_fx_max_radius_distance,
                                     rope_curve::deco_fx_default_radius_distance);
                    fx.radius_scale =
                        rope_curve::clamp_finite(glare_floats[3], 0.0f, rope_curve::deco_fx_max_radius_scale,
                                     rope_curve::deco_fx_default_radius_scale);
                    fx.diminish_distance = rope_curve::clamp_finite(
                        glare_floats[4], -rope_curve::deco_fx_max_diminish_distance,
                        rope_curve::deco_fx_max_diminish_distance,
                        rope_curve::deco_fx_default_diminish_distance);

                    if (!reader.read_string(fx.volumetric_bitmap)) return log_rope_chunk_truncated(created);
                    // Gated exactly as the corona chunk gates it: the two floats are only present
                    // when the volumetric bitmap name is non-empty.
                    if (!fx.volumetric_bitmap.empty()) {
                        float vol[2] = {};
                        for (float& v : vol) {
                            if (!reader.read_bytes(&v, sizeof(float))) return log_rope_chunk_truncated(created);
                        }
                        fx.volumetric_height =
                            rope_curve::clamp_finite(vol[0], 0.0f, rope_curve::deco_fx_max_volumetric, 0.0f);
                        fx.volumetric_length =
                            rope_curve::clamp_finite(vol[1], 0.0f, rope_curve::deco_fx_max_volumetric, 0.0f);
                    }
                    if (bitmap_name_over_long(fx.volumetric_bitmap)) {
                        xlog::warn("[AlpineRope] uid={} volumetric bitmap '{}' is too long, ignoring it",
                                   rope.uid, fx.volumetric_bitmap);
                        fx.volumetric_bitmap.clear();
                    }
                }

                if (fx.has_light()) {
                    if (!reader.read_bytes(&fx.light_r, sizeof(uint8_t))) return log_rope_chunk_truncated(created);
                    if (!reader.read_bytes(&fx.light_g, sizeof(uint8_t))) return log_rope_chunk_truncated(created);
                    if (!reader.read_bytes(&fx.light_b, sizeof(uint8_t))) return log_rope_chunk_truncated(created);
                    float light_radius = 0.0f, light_intensity = 0.0f;
                    if (!reader.read_bytes(&light_radius, sizeof(float))) return log_rope_chunk_truncated(created);
                    if (!reader.read_bytes(&light_intensity, sizeof(float))) return log_rope_chunk_truncated(created);
                    fx.light_radius = rope_curve::clamp_finite(light_radius, rope_curve::deco_fx_min_light_radius,
                                                   rope_curve::deco_fx_max_light_radius,
                                                   rope_curve::deco_fx_default_light_radius);
                    fx.light_intensity =
                        rope_curve::clamp_finite(light_intensity, 0.0f, rope_curve::deco_fx_max_light_intensity,
                                     rope_curve::deco_fx_default_light_intensity);
                }
            }

            // Compacted like the editor keeps its slots, so a cleared name cannot leave the two
            // sides disagreeing about the mesh cycle. Names and extras move together.
            for (uint8_t m = 0; m < mesh_count; ++m) {
                if (names[m].empty()) {
                    continue;
                }
                rope.deco.mesh_names.push_back(std::move(names[m]));
                rope.deco.fx.push_back(std::move(slot_fx[m]));
            }

            uint8_t spacing_mode = 0, random_order = 0, orient_mode = 0;
            int32_t deco_count = 0;
            float deco_spacing = 0.0f;
            if (!reader.read_bytes(&spacing_mode, sizeof(spacing_mode))) return log_rope_chunk_truncated(created);
            if (!reader.read_bytes(&deco_count, sizeof(deco_count))) return log_rope_chunk_truncated(created);
            if (!reader.read_bytes(&deco_spacing, sizeof(deco_spacing))) return log_rope_chunk_truncated(created);
            if (!reader.read_bytes(&random_order, sizeof(random_order))) return log_rope_chunk_truncated(created);
            if (!reader.read_bytes(&orient_mode, sizeof(orient_mode))) return log_rope_chunk_truncated(created);

            rope.deco.enabled = decorations_enabled != 0 && !rope.deco.mesh_names.empty();
            rope.deco.spacing_mode =
                spacing_mode > rope_curve::deco_spacing_mode_max ? 0 : spacing_mode;
            rope.deco.count =
                std::clamp<int32_t>(deco_count, rope_curve::deco_min_count, rope_curve::deco_max_count);
            rope.deco.spacing = rope_curve::clamp_finite(deco_spacing, rope_curve::deco_min_spacing,
                                             rope_curve::deco_max_spacing, 1.0f);
            rope.deco.random_order = random_order != 0;
            rope.deco.orient_mode =
                orient_mode > rope_deco_orient_upright ? rope_deco_orient_follow : orient_mode;
            rope.deco.slots.assign(rope.deco.mesh_names.size(), -1);
        }

        rope.dangle_length = rope_curve::clamp_finite(dangle, rope_curve::rope_min_dangle,
                                                     rope_curve::rope_max_dangle, 3.0f);
        rope.slack = rope_curve::clamp_finite(slack, 0.0f, rope_curve::rope_max_slack, 0.5f);
        rope.weight = rope_curve::clamp_finite(weight, rope_curve::rope_min_weight,
                                              rope_curve::rope_max_weight, 1.0f);
        rope.thickness = rope_curve::clamp_finite(thickness, rope_curve::rope_min_thickness,
                                                 rope_curve::rope_max_thickness, 0.03f);
        rope.segments = std::clamp(segments, rope_curve::rope_min_segments, rope_curve::rope_max_segments);
        rope.uv_tiles_per_meter =
            rope_curve::clamp_finite(uv_tiles, 0.0f, rope_curve::rope_max_uv_tiles, 0.0f);
        rope.color = rf::Color::from_hex(packed_color, true);
        rope.sway_amplitude =
            rope_curve::clamp_finite(sway_amplitude, 0.0f, rope_curve::rope_max_sway_amplitude, 0.05f);
        rope.sway_speed =
            rope_curve::clamp_finite(sway_speed, 0.0f, rope_curve::rope_max_sway_speed, 1.0f);
        rope.initially_on = initially_on != 0;

        if (!std::isfinite(rope.pos.x) || !std::isfinite(rope.pos.y) || !std::isfinite(rope.pos.z)) {
            xlog::warn("[AlpineRope] uid={} has a non-finite position, skipping", rope.uid);
            continue;
        }
        // The rope itself only uses pos; orient just seeds the anchor clutter, so a malformed one
        // costs the record nothing but an identity basis.
        if (!alpine_orient_is_sane(orient)) {
            xlog::warn("[AlpineRope] uid={} has a malformed orientation, using identity", rope.uid);
            orient = rf::Matrix3{{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
        }
        if (bitmap_name_over_long(rope.bitmap_name)) {
            xlog::warn("[AlpineRope] uid={} bitmap name '{}' is too long, ignoring it", rope.uid,
                       rope.bitmap_name);
            rope.bitmap_name.clear();
        }
        if (script_name.size() > rope_curve::rope_max_script_name_len) {
            xlog::warn("[AlpineRope] uid={} script name is {} chars, ignoring it", rope.uid,
                       script_name.size());
            script_name.clear();
        }

        rf::Clutter* clutter = alpine_create_anchor_clutter(rope.pos, orient, rope.uid,
                                                           script_name.c_str());
        if (!clutter) {
            xlog::warn("[AlpineRope] Failed to create clutter for uid={}", rope.uid);
            continue;
        }
        rope.clutter_handle = clutter->handle;
        if (!rope.initially_on) {
            rf::obj_hide(clutter);
        }

        g_ropes.push_back(std::move(rope));
        ++created;
    }

    xlog::info("[AlpineRope] Created {} rope(s)", created);
}

void alpine_rope_level_init()
{
    if (g_ropes.empty()) {
        return;
    }

    const bool cosmetics = !rf::is_dedicated_server && !is_headless_mode();
    g_has_dynamic = false;

    for (AlpineRope& rope : g_ropes) {
        g_has_dynamic = g_has_dynamic || rope.is_dynamic();
        rope.target_handle = -1;
        if (rope.target_uid != -1) {
            rf::Object* target = rf::obj_lookup_from_uid(rope.target_uid);
            if (target) {
                rope.target_handle = target->handle;
            }
            else {
                xlog::warn("[AlpineRope] uid={} target uid {} not found, hanging free", rope.uid,
                           rope.target_uid);
            }
        }

        if (cosmetics && !rope.bitmap_name.empty()) {
            rope.bitmap_handle = rf::bm::load(rope.bitmap_name.c_str(), -1, true);
            if (rope.bitmap_handle < 0) {
                xlog::warn("[AlpineRope] uid={} failed to load bitmap '{}'", rope.uid,
                           rope.bitmap_name);
                rope.bitmap_handle = -1;
            }
            else {
                rope_collect_bitmap_frames(rope);
            }
        }

        if (cosmetics && rope.deco.enabled) {
            rope.deco.max_radius = 0.0f;
            rope.deco.max_offset = 0.0f;
            for (std::size_t m = 0; m < rope.deco.mesh_names.size(); ++m) {
                const int slot = resolve_deco_mesh(rope.deco.mesh_names[m]);
                rope.deco.slots[m] = slot;
                if (slot >= 0) {
                    rope.deco.max_radius = std::max(rope.deco.max_radius,
                                                    g_deco_meshes[static_cast<std::size_t>(slot)].radius);
                }
                if (m < rope.deco.fx.size()) {
                    rope.deco.max_offset =
                        std::max(rope.deco.max_offset, rope.deco.fx[m].pos_offset.len());
                }
            }
            // Bitmaps for the glare slots load here and nowhere else.
            rope_fx_level_init(rope);
        }

        rope.seeded = false;
        rope.endpoints_valid = false;
        if (!cosmetics) {
            // Nothing past the anchor clutter is ever read without a renderer, so a dedicated
            // server skips the curve solve and the node arrays entirely.
            continue;
        }
        resolve_endpoints(rope);
        update_endpoint_key(rope);
        update_deco_instances(rope);
        if (rope.is_dynamic()) {
            seed_dynamic(rope);
        }
        else {
            build_static_shape(rope);
        }
        // Glares and lights exist from the first frame, so a rope nothing ever moves is fully set
        // up here and the sim path leaves it alone. The module clock is still at its reset value, so
        // the sway phase this bakes in is the same one the first sim frame will continue from.
        rope_fx_update(rope, rope.initially_on, g_clock_ms);
    }

    if (!cosmetics) {
        g_draw_order.clear();
        return;
    }

    // One texture and one mode per rope, so ordering by bitmap (and by blend mode within a
    // texture) lets the dynamic geometry batcher collapse runs of ropes into a single draw call.
    g_draw_order.resize(g_ropes.size());
    for (std::size_t i = 0; i < g_ropes.size(); ++i) {
        g_draw_order[i] = static_cast<int>(i);
    }
    std::sort(g_draw_order.begin(), g_draw_order.end(), [](int a, int b) {
        if (g_ropes[a].bitmap_handle != g_ropes[b].bitmap_handle) {
            return g_ropes[a].bitmap_handle < g_ropes[b].bitmap_handle;
        }
        return g_ropes[a].is_glow() < g_ropes[b].is_glow();
    });
}

bool alpine_rope_is_rope(int handle)
{
    if (handle == -1) {
        return false;
    }
    for (const AlpineRope& rope : g_ropes) {
        if (rope.clutter_handle == handle) {
            return true;
        }
    }
    return false;
}

void alpine_rope_clear_state()
{
    // Bitmap handles are name-deduped by bmpman with no refcount, so releasing one here could
    // destroy an entry the rest of the level still holds. Same bounded leak weather and the
    // corona loader accept. The clutter objects belong to the object system.
    // Decoration meshes belong to the level, and the engine frees those on unload; the cache only
    // drops its pointers, and must never vmesh_free them.
    // Lights are released here while the pool entries are still ours (this runs from level_shutdown,
    // before the engine reclaims the level's lights, and again at load init where it is a no-op).
    // Glare objects belong to the object system like the corona pairs; only their GlareInfos are
    // ours, and a still-live glare is switched off before its info goes away.
    for (AlpineRope& rope : g_ropes) {
        rope_fx_release(rope);
    }
    for (rf::GlareInfo* gi : g_rope_glare_infos) {
        delete gi;
    }
    g_rope_glare_infos.clear();

    g_ropes.clear();
    g_draw_order.clear();
    g_pushers.clear();
    g_deco_meshes.clear();
    g_deco_mesh_lookup.clear();
    g_render_points.clear();
    g_deco_arc.clear();
    g_has_dynamic = false;

    // Module state has to reset here, not just at level init: a level with no ropes would otherwise
    // leave the clock's raw baseline stale for the next one that has them.
    g_clock_acc_ms = 0.0;
    g_clock_ms = 0;
    g_clock_last_raw_ms = 0;
    g_clock_started = false;
    g_deco_xform_frame = 0;
}

void alpine_rope_do_frame()
{
    if (g_ropes.empty() || rf::is_dedicated_server || is_headless_mode()) {
        return;
    }

    // The raw baseline advances even through a pause, so unpausing resumes the phase where it was
    // left instead of jumping it forward by the length of the pause. A raw read that went backwards
    // contributes nothing rather than rewinding the clock.
    const int64_t raw_ms = timer::get_i64(1000);
    if (!g_clock_started) {
        g_clock_started = true;
        g_clock_last_raw_ms = raw_ms;
    }
    const int64_t raw_delta_ms = raw_ms > g_clock_last_raw_ms ? raw_ms - g_clock_last_raw_ms : 0;
    g_clock_last_raw_ms = raw_ms;

    if (rf::game_paused) {
        return;
    }
    const float time_scale = demo_playback_sim_time_scale();
    const float dt = std::clamp(rf::frametime, 0.0f, rope_max_frame_delta) * time_scale;
    if (!(dt >= 0.0f)) {
        return; // NaN fails the comparison, which is the point
    }

    if (time_scale >= 0.0f) {
        g_clock_acc_ms += static_cast<double>(raw_delta_ms) * static_cast<double>(time_scale);
        g_clock_ms = static_cast<int64_t>(g_clock_acc_ms);
    }
    ++g_deco_xform_frame;

    collect_pushers();
    for (AlpineRope& rope : g_ropes) {
        update_rope(rope, dt, g_clock_ms);
    }
}

void alpine_rope_render()
{
    if (g_ropes.empty() || rf::is_dedicated_server || is_headless_mode()) {
        return;
    }
    if (g_draw_order.size() != g_ropes.size()) {
        return;
    }

    const rf::Vector3 eye = rf::gr::eye_pos;
    // The module clock, not the wall clock: frozen while paused and scaled with demo playback, and
    // constant across every view this frame draws.
    const int64_t now_ms = g_clock_ms;

    // was_off still set means a dynamic rope has been switched back on but the sim has not re-seeded
    // it yet, so the only shape it holds is however it was frozen when it went off. One blank frame
    // is a better answer than one wrong one.
    bool drew_any = false;
    for (int index : g_draw_order) {
        AlpineRope& rope = g_ropes[static_cast<std::size_t>(index)];
        if (!rope_is_on(rope) || rope.was_off) {
            continue;
        }
        drew_any |= render_rope(rope, eye, now_ms);
    }

    if (drew_any) {
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::set_texture(-1, -1);
    }

    // Decorations go after every ribbon, so the gr state the ribbons share is already restored and
    // the mesh renderer starts from a clean pipeline.
    bool drew_deco = false;
    for (int index : g_draw_order) {
        AlpineRope& rope = g_ropes[static_cast<std::size_t>(index)];
        if (!rope.deco.enabled || !rope_is_on(rope) || rope.was_off) {
            continue;
        }
        if (rf::gr::cull_bounding_box(rope.aabb_min, rope.aabb_max)) {
            continue;
        }
        drew_deco |= render_rope_decorations(rope, make_sway_paint(rope, now_ms));
    }

    if (drew_deco) {
        rf::gr::set_color(255, 255, 255, 255);
        rf::gr::set_texture(-1, -1);
    }
}

void alpine_rope_apply_explosion(const rf::Vector3& pos, float radius, float strength)
{
    if (g_ropes.empty() || rf::is_dedicated_server || is_headless_mode()) {
        return;
    }
    if (!std::isfinite(radius) || radius < 1.0e-6f || !std::isfinite(strength) || strength <= 0.0f) {
        return;
    }
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) {
        return;
    }

    const float peak = std::min(strength * rope_explosion_damage_to_speed, rope_explosion_max_speed);
    const float inv_radius = 1.0f / radius;

    for (AlpineRope& rope : g_ropes) {
        if (!rope.is_dynamic() || !rope.seeded) {
            continue;
        }
        const std::size_t n = rope.points.size();
        if (n < 2 || rope.prev.size() != n) {
            continue;
        }
        // Sphere vs the cached AABB expanded by the radius: most blasts reach no rope at all, and
        // this rejects those without walking a node list.
        if (pos.x < rope.aabb_min.x - radius || pos.x > rope.aabb_max.x + radius ||
            pos.y < rope.aabb_min.y - radius || pos.y > rope.aabb_max.y + radius ||
            pos.z < rope.aabb_min.z - radius || pos.z > rope.aabb_max.z + radius) {
            continue;
        }
        const std::size_t last = n - 1;
        bool touched = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (i == 0 || (i == last && rope.has_target)) {
                continue;
            }
            rf::Vector3 dir = rope.points[i] - pos;
            const float dist = dir.len();
            if (dist >= radius) {
                continue;
            }
            const float falloff = 1.0f - dist * inv_radius;
            if (dist > 1.0e-4f) {
                dir *= 1.0f / dist;
            }
            else {
                dir = rf::Vector3{0.0f, 1.0f, 0.0f};
            }
            const float speed = std::min(peak * falloff / rope.weight, rope_explosion_max_speed);
            rope.prev[i] -= dir * (speed * rope_sim_step);
            touched = true;
        }
        if (touched) {
            // Every blast subtracts from prev, so blasts landing in the same frame stack. Cap the
            // implied per-node velocity so the total can never exceed one blast's ceiling.
            const float max_delta = rope_explosion_max_speed * rope_sim_step;
            for (std::size_t i = 0; i < n; ++i) {
                if (i == 0 || (i == last && rope.has_target)) {
                    continue;
                }
                const rf::Vector3 v = rope.points[i] - rope.prev[i];
                const float len = v.len();
                if (len > max_delta) {
                    rope.prev[i] = rope.points[i] - v * (max_delta / len);
                }
            }
            rope.asleep = false;
            rope.sleep_timer = 0.0f;
        }
    }
}

void alpine_rope_apply_patch()
{
    rope_explosion_apply_radius_damage_hook.install();
}
