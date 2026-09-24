#include <windows.h>
#include <commctrl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <common/rope_curve.h>
#include <xlog/xlog.h>
#include "alpine_color_picker.h"
#include "alpine_spinner.h"
#include "mesh_browser.h"
#include "rope_emitter.h"
#include "level.h"
#include "resources.h"
#include "vtypes.h"
#include "alpine_obj.h"
#include "textures.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

// ─── Globals ─────────────────────────────────────────────────────────────────

static int g_rope_emitter_icon_handle = -1;
static std::vector<DedRopeEmitter*> g_rope_emitter_clipboard;

static void rope_emitter_load_icon()
{
    if (g_rope_emitter_icon_handle < 0) {
        g_rope_emitter_icon_handle = bm_load("Icon_AFRope.tga", -1, 1);
    }
}

// Same predicate the game reader applies: finite and roughly unit-length on each axis.
static bool rope_orient_is_sane(const Matrix3& orient)
{
    for (const Vector3* v : {&orient.rvec, &orient.uvec, &orient.fvec}) {
        const float len_sq = v->x * v->x + v->y * v->y + v->z * v->z;
        if (!std::isfinite(len_sq) || len_sq < 0.9f || len_sq > 1.1f) {
            return false;
        }
    }
    return true;
}

static_assert(sizeof(DedRopeEmitter::deco_meshes) / sizeof(std::string) ==
                  rope_curve::deco_max_meshes,
              "decoration slot count must match the shared placement limit");

// The name slots are kept compacted, so the first empty one ends the list.
static uint8_t rope_deco_mesh_count(const DedRopeEmitter* rope)
{
    uint8_t n = 0;
    while (n < rope_curve::deco_max_meshes && !rope->deco_meshes[n].empty()) n++;
    return n;
}

// A dropped name must not leave a hole the count walk would stop at. The slot's effects move with
// it, so a compacted slot never inherits another slot's glare or light.
static void rope_compact_deco_meshes(DedRopeEmitter* rope)
{
    std::string kept[rope_curve::deco_max_meshes];
    DedRopeSlotFx kept_fx[rope_curve::deco_max_meshes];
    int n = 0;
    for (int i = 0; i < rope_curve::deco_max_meshes; i++) {
        if (!rope->deco_meshes[i].empty()) {
            kept[n] = rope->deco_meshes[i];
            kept_fx[n] = rope->deco_fx[i];
            n++;
        }
    }
    for (int i = 0; i < rope_curve::deco_max_meshes; i++) {
        rope->deco_meshes[i] = (i < n) ? kept[i] : std::string{};
        rope->deco_fx[i] = (i < n) ? kept_fx[i] : DedRopeSlotFx{};
    }
}

// Same ranges game_patch/object/alpine_rope.cpp applies at parse, from the shared constants.
static void rope_clamp_slot_fx(DedRopeSlotFx& fx)
{
    const float max_pos = rope_curve::deco_fx_max_pos_offset;
    const float max_rot = rope_curve::deco_fx_max_rot_offset;
    fx.pos_x = rope_curve::clamp_finite(fx.pos_x, -max_pos, max_pos, 0.0f);
    fx.pos_y = rope_curve::clamp_finite(fx.pos_y, -max_pos, max_pos, 0.0f);
    fx.pos_z = rope_curve::clamp_finite(fx.pos_z, -max_pos, max_pos, 0.0f);
    fx.rot_pitch = rope_curve::clamp_finite(fx.rot_pitch, -max_rot, max_rot, 0.0f);
    fx.rot_yaw = rope_curve::clamp_finite(fx.rot_yaw, -max_rot, max_rot, 0.0f);
    fx.rot_roll = rope_curve::clamp_finite(fx.rot_roll, -max_rot, max_rot, 0.0f);
    fx.flags &= rope_curve::deco_slot_flag_mask;

    fx.cone_angle = rope_curve::clamp_finite(fx.cone_angle, 0.0f, rope_curve::deco_fx_max_cone_angle,
                                             rope_curve::deco_fx_default_cone_angle);
    fx.intensity = rope_curve::clamp_finite(fx.intensity, 0.0f, rope_curve::deco_fx_max_intensity,
                                            rope_curve::deco_fx_default_intensity);
    fx.radius_distance =
        rope_curve::clamp_finite(fx.radius_distance, 0.0f, rope_curve::deco_fx_max_radius_distance,
                                 rope_curve::deco_fx_default_radius_distance);
    fx.radius_scale =
        rope_curve::clamp_finite(fx.radius_scale, 0.0f, rope_curve::deco_fx_max_radius_scale,
                                 rope_curve::deco_fx_default_radius_scale);
    fx.diminish_distance = rope_curve::clamp_finite(
        fx.diminish_distance, -rope_curve::deco_fx_max_diminish_distance,
        rope_curve::deco_fx_max_diminish_distance, rope_curve::deco_fx_default_diminish_distance);
    fx.volumetric_height = rope_curve::clamp_finite(fx.volumetric_height, 0.0f,
                                                    rope_curve::deco_fx_max_volumetric, 0.0f);
    fx.volumetric_length = rope_curve::clamp_finite(fx.volumetric_length, 0.0f,
                                                    rope_curve::deco_fx_max_volumetric, 0.0f);

    fx.light_radius = rope_curve::clamp_finite(fx.light_radius, rope_curve::deco_fx_min_light_radius,
                                               rope_curve::deco_fx_max_light_radius,
                                               rope_curve::deco_fx_default_light_radius);
    fx.light_intensity =
        rope_curve::clamp_finite(fx.light_intensity, 0.0f, rope_curve::deco_fx_max_light_intensity,
                                 rope_curve::deco_fx_default_light_intensity);

    // Same caps the loaders apply, so a name either side would drop never survives a save.
    if (rfl_name_over_long(fx.glare_bitmap)) fx.glare_bitmap.clear();
    if (rfl_name_over_long(fx.volumetric_bitmap)) fx.volumetric_bitmap.clear();
}

static void rope_clamp_properties(DedRopeEmitter* rope)
{
    for (auto& fx : rope->deco_fx) {
        rope_clamp_slot_fx(fx);
    }
    rope->deco_spacing_mode =
        rope->deco_spacing_mode > rope_curve::deco_spacing_mode_max ? 0 : rope->deco_spacing_mode;
    rope->deco_count = std::clamp(rope->deco_count, rope_curve::deco_min_count,
                                  rope_curve::deco_max_count);
    rope->deco_spacing = rope_curve::clamp_finite(rope->deco_spacing, rope_curve::deco_min_spacing,
                                                  rope_curve::deco_max_spacing, 1.0f);
    rope->dangle_length = rope_curve::clamp_finite(rope->dangle_length, rope_curve::rope_min_dangle,
                                                  rope_curve::rope_max_dangle, 3.0f);
    rope->slack = rope_curve::clamp_finite(rope->slack, 0.0f, rope_curve::rope_max_slack, 0.5f);
    rope->weight = rope_curve::clamp_finite(rope->weight, rope_curve::rope_min_weight,
                                            rope_curve::rope_max_weight, 1.0f);
    rope->thickness = rope_curve::clamp_finite(rope->thickness, rope_curve::rope_min_thickness,
                                               rope_curve::rope_max_thickness, 0.03f);
    rope->segments =
        std::clamp(rope->segments, rope_curve::rope_min_segments, rope_curve::rope_max_segments);
    rope->uv_tiles_per_meter = rope_curve::clamp_finite(rope->uv_tiles_per_meter, 0.0f,
                                                       rope_curve::rope_max_uv_tiles, 0.0f);
    rope->sway_amplitude = rope_curve::clamp_finite(rope->sway_amplitude, 0.0f,
                                                    rope_curve::rope_max_sway_amplitude, 0.05f);
    rope->sway_speed =
        rope_curve::clamp_finite(rope->sway_speed, 0.0f, rope_curve::rope_max_sway_speed, 1.0f);
}

// ─── Cleanup ─────────────────────────────────────────────────────────────────

void DestroyDedRopeEmitter(DedRopeEmitter* rope)
{
    if (!rope) return;
    rope->field_4.free();
    rope->script_name.free();
    rope->class_name.free();
    delete rope;
}

// ─── Serialization ──────────────────────────────────────────────────────────

void rope_emitter_serialize_chunk(CDedLevel& level, rf::File& file)
{
    auto& ropes = level.GetAlpineLevelProperties().rope_emitter_objects;
    if (ropes.empty()) return;

    auto start_pos = level.BeginRflSection(file, alpine_rope_emitter_chunk_id);

    // Payload growth appends per-record fields gated on the RFL content version (mesh flag-block
    // precedent); no chunk version - a reader never sees a file above its supported RFL version.
    file.write<uint32_t>(static_cast<uint32_t>(ropes.size()));

    for (auto* rope : ropes) {
        file.write<int32_t>(rope->uid);
        file.write<float>(rope->pos.x);
        file.write<float>(rope->pos.y);
        file.write<float>(rope->pos.z);
        file.write<float>(rope->orient.rvec.x);
        file.write<float>(rope->orient.rvec.y);
        file.write<float>(rope->orient.rvec.z);
        file.write<float>(rope->orient.uvec.x);
        file.write<float>(rope->orient.uvec.y);
        file.write<float>(rope->orient.uvec.z);
        file.write<float>(rope->orient.fvec.x);
        file.write<float>(rope->orient.fvec.y);
        file.write<float>(rope->orient.fvec.z);
        write_rfl_string(file, rope->script_name);
        file.write<int32_t>(rope->target_uid);
        file.write<float>(rope->dangle_length);
        file.write<float>(rope->slack);
        file.write<float>(rope->weight);
        file.write<float>(rope->thickness);
        file.write<int32_t>(rope->segments);
        file.write<float>(rope->uv_tiles_per_meter);
        // Packed so the game's Color::from_hex(v, true) reads it back: red in the high byte.
        file.write<uint32_t>((static_cast<uint32_t>(rope->color_r) << 24) |
                             (static_cast<uint32_t>(rope->color_g) << 16) |
                             (static_cast<uint32_t>(rope->color_b) << 8) |
                             static_cast<uint32_t>(rope->color_a));
        write_rfl_string(file, rope->bitmap);
        file.write<float>(rope->sway_amplitude);
        file.write<float>(rope->sway_speed);
        file.write<uint32_t>(rope->flags);
        file.write<uint8_t>(rope->initially_on ? 1 : 0);

        // Decoration sub-block, gated like the mesh destructible block: a rope with no decoration
        // meshes at all costs exactly one zero byte, so untouched levels round-trip byte for byte.
        // The gate is the mesh list, NOT the Decorations checkbox - that travels as a field inside
        // the block, so unchecking it and saving keeps the authored list instead of erasing it.
        const uint8_t mesh_count = rope_deco_mesh_count(rope);
        const bool has_deco_block = mesh_count > 0;
        file.write<uint8_t>(has_deco_block ? 1 : 0);
        if (has_deco_block) {
            file.write<uint8_t>(rope->decorations_enabled ? 1 : 0);
            file.write<uint8_t>(mesh_count);
            // Per-slot unit: the name carries its own offsets and gated effect blocks, so the
            // reader's compaction can drop a whole slot without misaligning the rest.
            for (uint8_t m = 0; m < mesh_count; m++) {
                const DedRopeSlotFx& fx = rope->deco_fx[m];
                write_rfl_string(file, rope->deco_meshes[m]);
                file.write<float>(fx.pos_x);
                file.write<float>(fx.pos_y);
                file.write<float>(fx.pos_z);
                file.write<float>(fx.rot_pitch);
                file.write<float>(fx.rot_yaw);
                file.write<float>(fx.rot_roll);
                file.write<uint8_t>(fx.flags);
                if (fx.has_glare()) {
                    write_rfl_string(file, fx.glare_bitmap);
                    file.write<uint8_t>(fx.glare_r);
                    file.write<uint8_t>(fx.glare_g);
                    file.write<uint8_t>(fx.glare_b);
                    file.write<uint8_t>(fx.glare_a);
                    file.write<float>(fx.cone_angle);
                    file.write<float>(fx.intensity);
                    file.write<float>(fx.radius_distance);
                    file.write<float>(fx.radius_scale);
                    file.write<float>(fx.diminish_distance);
                    write_rfl_string(file, fx.volumetric_bitmap);
                    // Gated exactly as the corona chunk gates it.
                    if (!fx.volumetric_bitmap.empty()) {
                        file.write<float>(fx.volumetric_height);
                        file.write<float>(fx.volumetric_length);
                    }
                }
                if (fx.has_light()) {
                    file.write<uint8_t>(fx.light_r);
                    file.write<uint8_t>(fx.light_g);
                    file.write<uint8_t>(fx.light_b);
                    file.write<float>(fx.light_radius);
                    file.write<float>(fx.light_intensity);
                }
            }
            file.write<uint8_t>(rope->deco_spacing_mode);
            file.write<int32_t>(rope->deco_count);
            file.write<float>(rope->deco_spacing);
            file.write<uint8_t>(rope->deco_random_order ? 1 : 0);
            file.write<uint8_t>(rope->deco_orient_mode);
        }
    }

    level.EndRflSection(file, start_pos);
}

static void rope_emitter_log_truncated(uint32_t loaded)
{
    xlog::warn("[RopeEmitter] Chunk is truncated, keeping {} rope emitter object(s)", loaded);
}

void rope_emitter_deserialize_chunk(CDedLevel& level, rf::File& file, std::size_t chunk_len)
{
    auto& ropes = level.GetAlpineLevelProperties().rope_emitter_objects;
    std::size_t remaining = chunk_len;

    rf::File::ChunkGuard chunk_guard{file, remaining};

    auto read_bytes = [&](void* dst, std::size_t n) -> bool {
        if (remaining < n) return false;
        int got = file.read(dst, n);
        if (got != static_cast<int>(n) || file.error()) {
            if (got > 0) remaining -= got;
            return false;
        }
        remaining -= n;
        return true;
    };

    // Length prefixed string routed through read_bytes so `remaining` stays in step with the
    // ChunkGuard on every failure path.
    auto read_string = [&](std::string& out) -> bool {
        out.clear();
        uint16_t len = 0;
        if (!read_bytes(&len, sizeof(len))) return false;
        if (len == 0) return true;
        // Bounds first, so a hostile length prefix cannot buy a 64 KB allocation.
        if (remaining < len) return false;
        out.assign(len, '\0');
        if (!read_bytes(out.data(), len)) {
            out.clear();
            return false;
        }
        return true;
    };

    // Payload growth appends per-record fields gated on the RFL content version (mesh flag-block
    // precedent); no chunk version - a reader never sees a file above its supported RFL version.
    uint32_t count = 0;
    if (!read_bytes(&count, sizeof(count))) return;
    if (count > rope_curve::rope_max_count) count = rope_curve::rope_max_count;

    uint32_t loaded = 0;
    for (uint32_t i = 0; i < count; i++) {
        auto* rope = new DedRopeEmitter();
        memset(static_cast<DedObject*>(rope), 0, sizeof(DedObject));
        rope->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
        rope->type = DedObjectType::DED_ROPE_EMITTER;

        if (!read_bytes(&rope->uid, sizeof(rope->uid))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (!read_bytes(&rope->pos.x, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (!read_bytes(&rope->pos.y, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (!read_bytes(&rope->pos.z, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (!read_bytes(&rope->orient.rvec.x, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (!read_bytes(&rope->orient.rvec.y, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (!read_bytes(&rope->orient.rvec.z, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (!read_bytes(&rope->orient.uvec.x, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (!read_bytes(&rope->orient.uvec.y, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (!read_bytes(&rope->orient.uvec.z, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (!read_bytes(&rope->orient.fvec.x, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (!read_bytes(&rope->orient.fvec.y, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (!read_bytes(&rope->orient.fvec.z, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }

        std::string sname;
        if (!read_string(sname)) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (sname.size() > rope_curve::rope_max_script_name_len) {
            xlog::warn("[RopeEmitter] uid={} script name is {} chars, ignoring it", rope->uid,
                       sname.size());
            sname.clear();
        }
        rope->script_name.assign_0(sname.c_str());

        if (!read_bytes(&rope->target_uid, sizeof(rope->target_uid))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (!read_bytes(&rope->dangle_length, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (!read_bytes(&rope->slack, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (!read_bytes(&rope->weight, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (!read_bytes(&rope->thickness, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (!read_bytes(&rope->segments, sizeof(int32_t))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (!read_bytes(&rope->uv_tiles_per_meter, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }

        uint32_t packed_color = 0;
        if (!read_bytes(&packed_color, sizeof(packed_color))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        rope->color_r = static_cast<uint8_t>((packed_color >> 24) & 0xFF);
        rope->color_g = static_cast<uint8_t>((packed_color >> 16) & 0xFF);
        rope->color_b = static_cast<uint8_t>((packed_color >> 8) & 0xFF);
        rope->color_a = static_cast<uint8_t>(packed_color & 0xFF);

        if (!read_string(rope->bitmap)) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (rfl_name_over_long(rope->bitmap)) {
            xlog::warn("[RopeEmitter] uid={} bitmap name '{}' is too long, ignoring it", rope->uid,
                       rope->bitmap);
            rope->bitmap.clear();
        }

        if (!read_bytes(&rope->sway_amplitude, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (!read_bytes(&rope->sway_speed, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (!read_bytes(&rope->flags, sizeof(rope->flags))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }

        uint8_t initially_on = 1;
        if (!read_bytes(&initially_on, sizeof(initially_on))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        rope->initially_on = initially_on != 0;

        // Read exactly as written: the block is present whenever the rope has any decoration mesh,
        // and the Decorations checkbox is the first field inside it.
        uint8_t has_deco_block = 0;
        uint8_t decorations_enabled = 0;
        if (!read_bytes(&has_deco_block, sizeof(has_deco_block))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
        if (has_deco_block != 0) {
            if (!read_bytes(&decorations_enabled, sizeof(decorations_enabled))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
            uint8_t mesh_count = 0;
            if (!read_bytes(&mesh_count, sizeof(mesh_count))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
            if (mesh_count < 1 || mesh_count > rope_curve::deco_max_meshes) {
                // The count was read, so this is not truncation - but every field behind it is
                // measured from the mesh names, so there is no way to work out where this record
                // ends and the whole rest of the chunk goes with it.
                xlog::warn("[RopeEmitter] uid={} declares {} decoration meshes, which is outside "
                           "1..{}; the record length cannot be derived, so the rest of the chunk is "
                           "dropped and {} rope emitter object(s) are kept",
                           rope->uid, mesh_count, rope_curve::deco_max_meshes, loaded);
                DestroyDedRopeEmitter(rope);
                return;
            }
            // Per-slot unit, read exactly as written: name, offsets, slot flags, then whichever
            // effect blocks the flags declare.
            for (uint8_t m = 0; m < mesh_count; m++) {
                if (!read_string(rope->deco_meshes[m])) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                if (rope->deco_meshes[m].size() > rfl_mesh_name_max_len) {
                    xlog::warn("[RopeEmitter] uid={} decoration mesh name is {} chars, ignoring it",
                               rope->uid, rope->deco_meshes[m].size());
                    rope->deco_meshes[m].clear();
                }

                DedRopeSlotFx& fx = rope->deco_fx[m];
                if (!read_bytes(&fx.pos_x, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                if (!read_bytes(&fx.pos_y, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                if (!read_bytes(&fx.pos_z, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                if (!read_bytes(&fx.rot_pitch, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                if (!read_bytes(&fx.rot_yaw, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                if (!read_bytes(&fx.rot_roll, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                if (!read_bytes(&fx.flags, sizeof(uint8_t))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                fx.flags &= rope_curve::deco_slot_flag_mask;

                if (fx.has_glare()) {
                    if (!read_string(fx.glare_bitmap)) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                    if (rfl_name_over_long(fx.glare_bitmap)) fx.glare_bitmap.clear();
                    if (!read_bytes(&fx.glare_r, sizeof(uint8_t))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                    if (!read_bytes(&fx.glare_g, sizeof(uint8_t))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                    if (!read_bytes(&fx.glare_b, sizeof(uint8_t))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                    if (!read_bytes(&fx.glare_a, sizeof(uint8_t))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                    if (!read_bytes(&fx.cone_angle, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                    if (!read_bytes(&fx.intensity, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                    if (!read_bytes(&fx.radius_distance, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                    if (!read_bytes(&fx.radius_scale, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                    if (!read_bytes(&fx.diminish_distance, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                    if (!read_string(fx.volumetric_bitmap)) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                    if (!fx.volumetric_bitmap.empty()) {
                        if (!read_bytes(&fx.volumetric_height, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                        if (!read_bytes(&fx.volumetric_length, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                    }
                    if (rfl_name_over_long(fx.volumetric_bitmap)) fx.volumetric_bitmap.clear();
                }

                if (fx.has_light()) {
                    if (!read_bytes(&fx.light_r, sizeof(uint8_t))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                    if (!read_bytes(&fx.light_g, sizeof(uint8_t))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                    if (!read_bytes(&fx.light_b, sizeof(uint8_t))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                    if (!read_bytes(&fx.light_radius, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                    if (!read_bytes(&fx.light_intensity, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
                }
            }

            uint8_t spacing_mode = 0, random_order = 0, orient_mode = 0;
            if (!read_bytes(&spacing_mode, sizeof(spacing_mode))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
            if (!read_bytes(&rope->deco_count, sizeof(int32_t))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
            if (!read_bytes(&rope->deco_spacing, sizeof(float))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
            if (!read_bytes(&random_order, sizeof(random_order))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }
            if (!read_bytes(&orient_mode, sizeof(orient_mode))) { DestroyDedRopeEmitter(rope); return rope_emitter_log_truncated(loaded); }

            rope->decorations_enabled = decorations_enabled != 0;
            rope->deco_spacing_mode =
                spacing_mode > rope_curve::deco_spacing_mode_max ? 0 : spacing_mode;
            rope->deco_random_order = random_order != 0;
            rope->deco_orient_mode = orient_mode > 1 ? 0 : orient_mode;
            rope_compact_deco_meshes(rope);
        }

        rope_clamp_properties(rope);

        if (!std::isfinite(rope->pos.x) || !std::isfinite(rope->pos.y) || !std::isfinite(rope->pos.z)) {
            xlog::warn("[RopeEmitter] uid={} has a non-finite position, skipping", rope->uid);
            DestroyDedRopeEmitter(rope);
            continue;
        }
        // Only the anchor marker uses the orientation, so a malformed one costs an identity basis
        // rather than the record; matches the game reader.
        if (!rope_orient_is_sane(rope->orient)) {
            xlog::warn("[RopeEmitter] uid={} has a malformed orientation, using identity", rope->uid);
            rope->orient.rvec = {1.0f, 0.0f, 0.0f};
            rope->orient.uvec = {0.0f, 1.0f, 0.0f};
            rope->orient.fvec = {0.0f, 0.0f, 1.0f};
        }

        ropes.push_back(rope);
        level.master_objects.add(static_cast<DedObject*>(rope));
        loaded++;
    }

    xlog::info("[RopeEmitter] Loaded {} rope emitter object(s)", loaded);
}

// ─── Curve preview ──────────────────────────────────────────────────────────

// Matches the game's resolver: RF's obj_lookup_from_uid walks only the global object list, so a
// uid that lives on a mover keyframe is a dangle in-game and previews as one here. First match
// wins, as a linear scan of master_objects would give.
static void rope_emitter_build_uid_index(CDedLevel* level,
                                         std::unordered_map<int, DedObject*>& index)
{
    auto& master = level->master_objects;
    index.clear();
    index.reserve(static_cast<std::size_t>(master.size));
    for (int i = 0; i < master.size; i++) {
        DedObject* obj = master.data_ptr[i];
        if (obj) index.emplace(obj->uid, obj);
    }
}

// Everything the solved polyline depends on.
static void rope_emitter_preview_key(const DedRopeEmitter* rope, bool has_target,
                                     const Vector3& target_pos,
                                     int32_t (&key)[ded_rope_preview_key_len])
{
    using rope_curve::quantize_1024;
    key[0] = quantize_1024(rope->pos.x);
    key[1] = quantize_1024(rope->pos.y);
    key[2] = quantize_1024(rope->pos.z);
    key[3] = has_target ? 1 : 0;
    key[4] = has_target ? quantize_1024(target_pos.x) : 0;
    key[5] = has_target ? quantize_1024(target_pos.y) : 0;
    key[6] = has_target ? quantize_1024(target_pos.z) : 0;
    key[7] = quantize_1024(rope->slack);
    key[8] = rope->segments;
    key[9] = quantize_1024(rope->dangle_length);
    key[10] = rope->target_uid;
}

// Solves the same curve the game does, minus the verlet simulation: a dynamic rope previews as
// the analytic shape it is seeded from. Takes the shape inputs directly so the live dialog
// preview can solve from staged values the rope object does not carry yet.
static bool rope_emitter_solve_preview(const Vector3& pos, float slack, float dangle_length,
                                       int segments, bool has_target, const Vector3& target_pos,
                                       std::vector<rope_curve::Vec3>& points, float& out_length)
{
    const rope_curve::Vec3 a{pos.x, pos.y, pos.z};

    const rope_curve::Vec3 b{target_pos.x, target_pos.y, target_pos.z};
    float rope_length;
    if (has_target) {
        const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
        rope_length = std::sqrt(dx * dx + dy * dy + dz * dz) + slack;
    }
    else {
        rope_length = dangle_length;
    }
    // Same floor the game applies, so a zero length previews as a short rope rather than nothing.
    if (!std::isfinite(rope_length) || rope_length <= 0.0f) rope_length = rope_curve::rope_min_dangle;

    const rope_curve::Curve curve = has_target ? rope_curve::solve(a, b, rope_length)
                                               : rope_curve::solve_dangle(a, rope_length);

    static std::vector<float> arc;
    rope_curve::build_polyline(curve, segments + 1, points, arc);
    out_length = curve.length;
    return points.size() >= 2;
}

// Tick markers where each decoration instance sits. Same placement derivation the game runs, and
// the same arc-space mapping onto the polyline, so a marker and its mesh land on the same spot.
static void rope_emitter_draw_deco_markers(const std::vector<Vector3>& points, float length,
                                           uint8_t spacing_mode, int count, float spacing,
                                           float thickness)
{
    const std::size_t n = points.size();
    if (n < 2 || !(length > 0.0f)) return;

    static std::vector<float> arcs;
    rope_curve::build_decoration_arcs(length, spacing_mode, count, spacing, arcs);
    if (arcs.empty()) return;

    // build_polyline samples at uniform arc steps, so arc position maps onto the polyline directly.
    const float step = length / static_cast<float>(n - 1);
    if (!(step > 0.0f)) return;

    const float tick = std::max(thickness * 2.0f, 0.06f);

    for (float arc_pos : arcs) {
        const float s = std::clamp(arc_pos, 0.0f, length);
        const std::size_t seg = std::min(static_cast<std::size_t>(s / step), n - 2);
        const float f = std::clamp((s - step * static_cast<float>(seg)) / step, 0.0f, 1.0f);
        const float x = points[seg].x + (points[seg + 1].x - points[seg].x) * f;
        const float y = points[seg].y + (points[seg + 1].y - points[seg].y) * f;
        const float z = points[seg].z + (points[seg + 1].z - points[seg].z) * f;
        draw_3d_line(x - tick, y, z, x + tick, y, z, 255, 255, 0);
        draw_3d_line(x, y - tick, z, x, y + tick, z, 255, 255, 0);
        draw_3d_line(x, y, z - tick, x, y, z + tick, 255, 255, 0);
    }
}

// ─── Properties Dialog ──────────────────────────────────────────────────────

static std::vector<DedRopeEmitter*> g_selected_rope_emitters;

// ─── Live viewport preview ──────────────────────────────────────────────────
// Everything rope_emitter_render draws with, staged while the main dialog is open so the viewport
// tracks the fields. Nothing here reaches a rope object: only IDOK writes. Values are clamped to
// the same ranges rope_clamp_properties applies, so a half typed field cannot reach the solver.

struct RopePreview
{
    bool active = false;
    // Target UID is per-rope identity, so a multi-select cannot edit it and must not preview one
    // rope's target on the rest.
    bool stage_target = false;
    int32_t target_uid = -1;
    float slack = 0.5f;
    float dangle_length = 3.0f;
    float thickness = 0.03f;
    int32_t segments = 24;
    uint8_t color_r = 255, color_g = 255, color_b = 255;
    bool decorations_enabled = false;
    uint8_t deco_spacing_mode = 0;
    int32_t deco_count = 10;
    float deco_spacing = 1.0f;
};
static RopePreview g_rope_preview;

static const int rope_deco_mesh_idcs[rope_curve::deco_max_meshes] = {
    IDC_ROPE_DECO_MESH1, IDC_ROPE_DECO_MESH2, IDC_ROPE_DECO_MESH3,
    IDC_ROPE_DECO_MESH4, IDC_ROPE_DECO_MESH5, IDC_ROPE_DECO_MESH6,
};

static const int rope_deco_browse_idcs[rope_curve::deco_max_meshes] = {
    IDC_ROPE_DECO_BROWSE1, IDC_ROPE_DECO_BROWSE2, IDC_ROPE_DECO_BROWSE3,
    IDC_ROPE_DECO_BROWSE4, IDC_ROPE_DECO_BROWSE5, IDC_ROPE_DECO_BROWSE6,
};

static const int rope_deco_fx_idcs[rope_curve::deco_max_meshes] = {
    IDC_ROPE_DECO_FX1, IDC_ROPE_DECO_FX2, IDC_ROPE_DECO_FX3,
    IDC_ROPE_DECO_FX4, IDC_ROPE_DECO_FX5, IDC_ROPE_DECO_FX6,
};

// Per-slot effects staged for the whole dialog session: the FX popup writes here, and only the main
// dialog's OK writes these onto the ropes.
static DedRopeSlotFx g_rope_dlg_fx[rope_curve::deco_max_meshes];

// The mesh list and placement rules, staged the same way: the Decorations popup edits this copy and
// the main dialog's OK is still the only thing that reaches a rope.
struct RopeDecoStaging
{
    std::string meshes[rope_curve::deco_max_meshes];
    uint8_t spacing_mode = 0;
    int32_t count = 10;
    float spacing = 1.0f;
    bool random_order = false;
    uint8_t orient_mode = 0;
};
static RopeDecoStaging g_rope_dlg_deco;

// A slot's FX button carries a star as soon as the slot has anything on it, so a populated slot
// reads off the list instead of needing six popups opened to find it.
static bool rope_slot_fx_is_set(const DedRopeSlotFx& fx)
{
    return fx.has_glare() || fx.has_light() || fx.pos_x != 0.0f || fx.pos_y != 0.0f ||
           fx.pos_z != 0.0f || fx.rot_pitch != 0.0f || fx.rot_yaw != 0.0f || fx.rot_roll != 0.0f;
}

static void rope_update_fx_labels(HWND hdlg)
{
    for (int i = 0; i < rope_curve::deco_max_meshes; i++) {
        SetDlgItemTextA(hdlg, rope_deco_fx_idcs[i],
                        rope_slot_fx_is_set(g_rope_dlg_fx[i]) ? "FX*..." : "FX...");
    }
}

static bool rope_emitter_update_target_info(HWND hdlg);

// Greying on the main panel, the destructible-checkbox pattern throughout: a field the rope cannot
// use is disabled with its label and its spinner. A dangling rope has no slack to give, a targeted
// one never reads its dangle length, and mass and the sway pair only mean anything under the flag
// that drives them.
static void rope_emitter_update_state(HWND hdlg)
{
    // Keyed on whether the uid RESOLVES, not merely on whether one was typed: the game hangs a rope
    // whose target it cannot find free and reads dangle_length, so greying dangle on a typed but
    // missing uid would grey the one field the rope is actually going to use. This also refreshes
    // the readout, so the two can never disagree about what the rope is hanging from.
    const bool has_target = rope_emitter_update_target_info(hdlg);
    const bool single = g_selected_rope_emitters.size() == 1;
    const bool dynamic = IsDlgButtonChecked(hdlg, IDC_ROPE_DYNAMIC) == BST_CHECKED;
    const bool sway = IsDlgButtonChecked(hdlg, IDC_ROPE_SWAY) == BST_CHECKED;
    const bool decorations = IsDlgButtonChecked(hdlg, IDC_ROPE_DECORATIONS) == BST_CHECKED;

    static const int slack_controls[] = {IDC_ROPE_SLACK, IDC_ROPE_SLACK_SPIN, IDC_ROPE_SLACK_LABEL};
    for (int id : slack_controls) {
        EnableWindow(GetDlgItem(hdlg, id), has_target);
    }

    static const int dangle_controls[] = {IDC_ROPE_DANGLE_LENGTH, IDC_ROPE_DANGLE_LENGTH_SPIN,
                                          IDC_ROPE_DANGLE_LENGTH_LABEL};
    for (int id : dangle_controls) {
        EnableWindow(GetDlgItem(hdlg, id), !has_target);
    }

    static const int mass_controls[] = {IDC_ROPE_WEIGHT, IDC_ROPE_WEIGHT_SPIN,
                                        IDC_ROPE_WEIGHT_LABEL};
    for (int id : mass_controls) {
        EnableWindow(GetDlgItem(hdlg, id), dynamic);
    }

    static const int sway_controls[] = {
        IDC_ROPE_SWAY_AMPLITUDE, IDC_ROPE_SWAY_AMPLITUDE_SPIN, IDC_ROPE_SWAY_AMPLITUDE_LABEL,
        IDC_ROPE_SWAY_SPEED, IDC_ROPE_SWAY_SPEED_SPIN, IDC_ROPE_SWAY_SPEED_LABEL,
    };
    for (int id : sway_controls) {
        EnableWindow(GetDlgItem(hdlg, id), sway);
    }

    // The decoration list is one shared staging set, so writing it over a whole selection would
    // collapse them onto the first rope's decorations: single-select only, like Script Name and
    // Target UID. The checkbox goes with the list rather than staying live as a no-op.
    EnableWindow(GetDlgItem(hdlg, IDC_ROPE_DECORATIONS), single);
    EnableWindow(GetDlgItem(hdlg, IDC_ROPE_DECO_EDIT), single && decorations);
}

// What the typed uid actually resolves to, beside the field, and whether it resolved at all - which
// is what decides the Slack / Dangle Len greying. Single-select only: a multi-select cannot edit the
// uid at all, so the field it reads is disabled there.
static bool rope_emitter_update_target_info(HWND hdlg)
{
    BOOL translated = FALSE;
    const int target_uid =
        static_cast<int>(GetDlgItemInt(hdlg, IDC_ROPE_TARGET_UID, &translated, TRUE));
    if (!translated || target_uid == -1) {
        SetDlgItemTextA(hdlg, IDC_ROPE_TARGET_INFO, "(hangs free)");
        return false;
    }

    // Same resolver the viewport preview uses, so the readout and the drawn curve can never
    // disagree about what the rope is hanging from.
    DedObject* found = nullptr;
    if (auto* level = CDedLevel::Get()) {
        auto& master = level->master_objects;
        for (int i = 0; i < master.size; i++) {
            DedObject* obj = master.data_ptr[i];
            if (obj && obj->uid == target_uid) {
                found = obj;
                break;
            }
        }
    }
    if (!found) {
        SetDlgItemTextA(hdlg, IDC_ROPE_TARGET_INFO, "(not found)");
        return false;
    }

    // A rope pointed at its own uid resolves to its own anchor, so the span is zero and the whole
    // rope collapses to the length floor. It resolves, so the greying is unchanged; the readout is
    // the only place that can say this is not what the mapper meant.
    if (!g_selected_rope_emitters.empty() && target_uid == g_selected_rope_emitters[0]->uid) {
        SetDlgItemTextA(hdlg, IDC_ROPE_TARGET_INFO, "(targets itself)");
        return true;
    }

    const char* name = found->script_name.c_str();
    char buf[128];
    if (name && name[0] != '\0') {
        snprintf(buf, sizeof(buf), "-> %s '%s'", get_type_display_name(found->type), name);
    }
    else {
        snprintf(buf, sizeof(buf), "-> %s", get_type_display_name(found->type));
    }
    SetDlgItemTextA(hdlg, IDC_ROPE_TARGET_INFO, buf);
    return true;
}

// Color display state, mirroring the Level Properties sun color controls: a swatch tinted to the
// current color, a "<r, g, b>" text field, and a button that opens the shared picker.
static uint8_t g_rope_color_r = 255;
static uint8_t g_rope_color_g = 255;
static uint8_t g_rope_color_b = 255;

static void rope_emitter_update_color_controls(HWND hdlg)
{
    alpine_dlg_set_color_controls(hdlg, IDC_ROPE_COLOR_SWATCH, IDC_ROPE_COLOR_VALUE, g_rope_color_r,
                                  g_rope_color_g, g_rope_color_b);
}

static bool rope_emitter_read_color_text(HWND hdlg, uint8_t& r, uint8_t& g, uint8_t& b)
{
    return alpine_dlg_parse_color_text(hdlg, IDC_ROPE_COLOR_VALUE, r, g, b);
}

// Placement fields the marker preview draws with, taken from the staging. Clamped exactly as the
// popup clamps them, so whichever of the two last wrote them the solver sees the same ranges.
static void rope_deco_preview_from_staging()
{
    g_rope_preview.deco_spacing_mode = g_rope_dlg_deco.spacing_mode > rope_curve::deco_spacing_mode_max
                                           ? 0
                                           : g_rope_dlg_deco.spacing_mode;
    g_rope_preview.deco_count = std::clamp(g_rope_dlg_deco.count, rope_curve::deco_min_count,
                                           rope_curve::deco_max_count);
    g_rope_preview.deco_spacing = rope_curve::clamp_finite(g_rope_dlg_deco.spacing,
                                                    rope_curve::deco_min_spacing,
                                                    rope_curve::deco_max_spacing, 1.0f);
}

static void rope_emitter_capture_preview(HWND hdlg)
{
    BOOL translated = FALSE;
    const int target_uid = static_cast<int>(GetDlgItemInt(hdlg, IDC_ROPE_TARGET_UID, &translated, TRUE));
    g_rope_preview.stage_target = g_selected_rope_emitters.size() == 1;
    g_rope_preview.target_uid = translated ? target_uid : -1;

    g_rope_preview.slack = rope_curve::clamp_finite(alpine_dlg_get_float_field(hdlg, IDC_ROPE_SLACK),
                                                    0.0f, rope_curve::rope_max_slack, 0.0f);
    g_rope_preview.dangle_length =
        rope_curve::clamp_finite(alpine_dlg_get_float_field(hdlg, IDC_ROPE_DANGLE_LENGTH),
                                 rope_curve::rope_min_dangle, rope_curve::rope_max_dangle,
                                 rope_curve::rope_min_dangle);
    g_rope_preview.thickness =
        rope_curve::clamp_finite(alpine_dlg_get_float_field(hdlg, IDC_ROPE_THICKNESS),
                                 rope_curve::rope_min_thickness, rope_curve::rope_max_thickness,
                                 rope_curve::rope_min_thickness);
    g_rope_preview.segments =
        std::clamp(alpine_dlg_get_int_field(hdlg, IDC_ROPE_SEGMENTS), rope_curve::rope_min_segments,
                   rope_curve::rope_max_segments);

    // The text field is authoritative for RGB and may not have lost focus yet; a value that
    // doesn't parse leaves the swatch's color standing.
    uint8_t r = g_rope_color_r, g = g_rope_color_g, b = g_rope_color_b;
    alpine_dlg_parse_color_text(hdlg, IDC_ROPE_COLOR_VALUE, r, g, b);
    g_rope_preview.color_r = r;
    g_rope_preview.color_g = g;
    g_rope_preview.color_b = b;

    g_rope_preview.decorations_enabled =
        IsDlgButtonChecked(hdlg, IDC_ROPE_DECORATIONS) == BST_CHECKED;
    // The placement fields live on the Decorations popup now, so this panel takes them from the
    // staging that popup writes; while it is open it drives the same three fields directly.
    rope_deco_preview_from_staging();
}

static void rope_emitter_refresh_preview(HWND hdlg)
{
    if (!g_rope_preview.active) return;
    rope_emitter_capture_preview(hdlg);
    redraw_all_viewports();
}

static void rope_emitter_pick_color(HWND hdlg)
{
    alpine_dlg_pick_color(hdlg, IDC_ROPE_COLOR_SWATCH, IDC_ROPE_COLOR_VALUE, g_rope_color_r,
                          g_rope_color_g, g_rope_color_b);
}

// Bitmap currently shown in the preview, tracked so an edit-box keystroke only touches the bitmap
// manager when the name actually changed.
static std::string g_rope_bitmap_preview_name;
static int g_rope_bitmap_preview_handle = -1;

static void rope_emitter_update_bitmap_preview(HWND hdlg, bool force)
{
    char buf[256] = {};
    GetDlgItemTextA(hdlg, IDC_ROPE_BITMAP, buf, sizeof(buf));
    if (!force && g_rope_bitmap_preview_name == buf) return;

    g_rope_bitmap_preview_name = buf;
    g_rope_bitmap_preview_handle = alpine_dlg_resolve_bitmap(buf);
    InvalidateRect(GetDlgItem(hdlg, IDC_ROPE_BITMAP_PREVIEW), nullptr, TRUE);
}

// ─── Per-slot FX popup ──────────────────────────────────────────────────────

// The slot the popup is editing. A copy: its OK writes back into the main dialog's staging, and
// Cancel leaves both the staging and the ropes untouched.
static DedRopeSlotFx g_rope_fx_popup;

static void rope_fx_popup_update_state(HWND hdlg)
{
    const bool glare = IsDlgButtonChecked(hdlg, IDC_ROPEFX_GLARE_ENABLE) == BST_CHECKED;
    static const int glare_controls[] = {
        IDC_ROPEFX_GLARE_GROUP, IDC_ROPEFX_GLARE_BITMAP, IDC_ROPEFX_GLARE_BROWSE,
        IDC_ROPEFX_GLARE_COLOR_SWATCH, IDC_ROPEFX_GLARE_COLOR_CHANGE, IDC_ROPEFX_GLARE_COLOR_VALUE,
        IDC_ROPEFX_GLARE_COLOR_A, IDC_ROPEFX_CONE_ANGLE, IDC_ROPEFX_INTENSITY,
        IDC_ROPEFX_RADIUS_DISTANCE, IDC_ROPEFX_RADIUS_SCALE, IDC_ROPEFX_DIMINISH_DISTANCE,
        IDC_ROPEFX_VOL_BITMAP, IDC_ROPEFX_VOL_BROWSE, IDC_ROPEFX_VOL_HEIGHT, IDC_ROPEFX_VOL_LENGTH,
        IDC_ROPEFX_GLARE_BITMAP_LABEL, IDC_ROPEFX_GLARE_COLOR_LABEL, IDC_ROPEFX_GLARE_ALPHA_LABEL,
        IDC_ROPEFX_CONE_ANGLE_LABEL, IDC_ROPEFX_INTENSITY_LABEL, IDC_ROPEFX_RADIUS_SCALE_LABEL,
        IDC_ROPEFX_RADIUS_DISTANCE_LABEL, IDC_ROPEFX_DIMINISH_DISTANCE_LABEL,
        IDC_ROPEFX_VOL_BITMAP_LABEL, IDC_ROPEFX_VOL_HEIGHT_LABEL, IDC_ROPEFX_VOL_LENGTH_LABEL,
        IDC_ROPEFX_CONE_ANGLE_SPIN, IDC_ROPEFX_INTENSITY_SPIN, IDC_ROPEFX_RADIUS_SCALE_SPIN,
        IDC_ROPEFX_RADIUS_DISTANCE_SPIN, IDC_ROPEFX_DIMINISH_DISTANCE_SPIN,
        IDC_ROPEFX_VOL_HEIGHT_SPIN, IDC_ROPEFX_VOL_LENGTH_SPIN,
    };
    for (int id : glare_controls) {
        EnableWindow(GetDlgItem(hdlg, id), glare);
    }

    const bool light = IsDlgButtonChecked(hdlg, IDC_ROPEFX_LIGHT_ENABLE) == BST_CHECKED;
    static const int light_controls[] = {
        IDC_ROPEFX_LIGHT_GROUP, IDC_ROPEFX_LIGHT_COLOR_SWATCH, IDC_ROPEFX_LIGHT_COLOR_CHANGE,
        IDC_ROPEFX_LIGHT_COLOR_VALUE, IDC_ROPEFX_LIGHT_RADIUS, IDC_ROPEFX_LIGHT_INTENSITY,
        IDC_ROPEFX_LIGHT_COLOR_LABEL, IDC_ROPEFX_LIGHT_RADIUS_LABEL,
        IDC_ROPEFX_LIGHT_INTENSITY_LABEL, IDC_ROPEFX_LIGHT_RADIUS_SPIN,
        IDC_ROPEFX_LIGHT_INTENSITY_SPIN,
    };
    for (int id : light_controls) {
        EnableWindow(GetDlgItem(hdlg, id), light);
    }
}

// The texture browser runs its own modal loop off the main frame, which would leave this dialog
// clickable; same disable/re-activate dance the rope bitmap field does.
static void rope_fx_browse_bitmap(HWND hdlg, int field_idc)
{
    char current[MAX_PATH] = {};
    GetDlgItemTextA(hdlg, field_idc, current, sizeof(current));
    const int current_handle = alpine_dlg_resolve_bitmap(current);

    EnableWindow(hdlg, FALSE);
    int picked = texture_browser_pick("Effects", current_handle);
    EnableWindow(hdlg, TRUE);
    SetActiveWindow(hdlg);
    if (picked >= 0) {
        const char* name = bm_get_filename(picked);
        SetDlgItemTextA(hdlg, field_idc, name ? name : "");
    }
}

// The popup's controls as one clamped struct. Shared by OK and by Apply to all slots, so the two
// can never disagree about what "what is typed here" means.
static DedRopeSlotFx rope_fx_popup_read(HWND hdlg)
{
    DedRopeSlotFx fx{};
    fx.pos_x = alpine_dlg_get_float_field(hdlg, IDC_ROPEFX_POS_X);
    fx.pos_y = alpine_dlg_get_float_field(hdlg, IDC_ROPEFX_POS_Y);
    fx.pos_z = alpine_dlg_get_float_field(hdlg, IDC_ROPEFX_POS_Z);
    fx.rot_pitch = alpine_dlg_get_float_field(hdlg, IDC_ROPEFX_ROT_PITCH);
    fx.rot_yaw = alpine_dlg_get_float_field(hdlg, IDC_ROPEFX_ROT_YAW);
    fx.rot_roll = alpine_dlg_get_float_field(hdlg, IDC_ROPEFX_ROT_ROLL);

    fx.flags = 0;
    if (IsDlgButtonChecked(hdlg, IDC_ROPEFX_GLARE_ENABLE) == BST_CHECKED) {
        fx.flags |= rope_curve::deco_slot_flag_glare;
    }
    if (IsDlgButtonChecked(hdlg, IDC_ROPEFX_LIGHT_ENABLE) == BST_CHECKED) {
        fx.flags |= rope_curve::deco_slot_flag_light;
    }

    char buf[MAX_PATH] = {};
    GetDlgItemTextA(hdlg, IDC_ROPEFX_GLARE_BITMAP, buf, sizeof(buf));
    fx.glare_bitmap = buf;
    GetDlgItemTextA(hdlg, IDC_ROPEFX_VOL_BITMAP, buf, sizeof(buf));
    fx.volumetric_bitmap = buf;

    // The text fields are authoritative for RGB: one may have been typed into and not yet lost
    // focus. A value that doesn't parse leaves the staged color standing.
    fx.glare_r = g_rope_fx_popup.glare_r;
    fx.glare_g = g_rope_fx_popup.glare_g;
    fx.glare_b = g_rope_fx_popup.glare_b;
    alpine_dlg_parse_color_text(hdlg, IDC_ROPEFX_GLARE_COLOR_VALUE, fx.glare_r, fx.glare_g, fx.glare_b);
    fx.glare_a = static_cast<uint8_t>(
        std::min(GetDlgItemInt(hdlg, IDC_ROPEFX_GLARE_COLOR_A, nullptr, FALSE), 255u));
    fx.light_r = g_rope_fx_popup.light_r;
    fx.light_g = g_rope_fx_popup.light_g;
    fx.light_b = g_rope_fx_popup.light_b;
    alpine_dlg_parse_color_text(hdlg, IDC_ROPEFX_LIGHT_COLOR_VALUE, fx.light_r, fx.light_g, fx.light_b);

    fx.cone_angle = alpine_dlg_get_float_field(hdlg, IDC_ROPEFX_CONE_ANGLE);
    fx.intensity = alpine_dlg_get_float_field(hdlg, IDC_ROPEFX_INTENSITY);
    fx.radius_distance = alpine_dlg_get_float_field(hdlg, IDC_ROPEFX_RADIUS_DISTANCE);
    fx.radius_scale = alpine_dlg_get_float_field(hdlg, IDC_ROPEFX_RADIUS_SCALE);
    fx.diminish_distance = alpine_dlg_get_float_field(hdlg, IDC_ROPEFX_DIMINISH_DISTANCE);
    fx.volumetric_height = alpine_dlg_get_float_field(hdlg, IDC_ROPEFX_VOL_HEIGHT);
    fx.volumetric_length = alpine_dlg_get_float_field(hdlg, IDC_ROPEFX_VOL_LENGTH);
    fx.light_radius = alpine_dlg_get_float_field(hdlg, IDC_ROPEFX_LIGHT_RADIUS);
    fx.light_intensity = alpine_dlg_get_float_field(hdlg, IDC_ROPEFX_LIGHT_INTENSITY);

    rope_clamp_slot_fx(fx);
    return fx;
}

static INT_PTR CALLBACK RopeSlotFxDialogProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        const DedRopeSlotFx& fx = g_rope_fx_popup;

        alpine_dlg_set_float_field(hdlg, IDC_ROPEFX_POS_X, fx.pos_x);
        alpine_dlg_set_float_field(hdlg, IDC_ROPEFX_POS_Y, fx.pos_y);
        alpine_dlg_set_float_field(hdlg, IDC_ROPEFX_POS_Z, fx.pos_z);
        alpine_dlg_set_float_field(hdlg, IDC_ROPEFX_ROT_PITCH, fx.rot_pitch);
        alpine_dlg_set_float_field(hdlg, IDC_ROPEFX_ROT_YAW, fx.rot_yaw);
        alpine_dlg_set_float_field(hdlg, IDC_ROPEFX_ROT_ROLL, fx.rot_roll);

        SetDlgItemTextA(hdlg, IDC_ROPEFX_GLARE_BITMAP, fx.glare_bitmap.c_str());
        alpine_dlg_set_color_controls(hdlg, IDC_ROPEFX_GLARE_COLOR_SWATCH,
                                      IDC_ROPEFX_GLARE_COLOR_VALUE, fx.glare_r, fx.glare_g,
                                      fx.glare_b);
        SetDlgItemInt(hdlg, IDC_ROPEFX_GLARE_COLOR_A, fx.glare_a, FALSE);
        alpine_dlg_set_float_field(hdlg, IDC_ROPEFX_CONE_ANGLE, fx.cone_angle);
        alpine_dlg_set_float_field(hdlg, IDC_ROPEFX_INTENSITY, fx.intensity);
        alpine_dlg_set_float_field(hdlg, IDC_ROPEFX_RADIUS_DISTANCE, fx.radius_distance);
        alpine_dlg_set_float_field(hdlg, IDC_ROPEFX_RADIUS_SCALE, fx.radius_scale);
        alpine_dlg_set_float_field(hdlg, IDC_ROPEFX_DIMINISH_DISTANCE, fx.diminish_distance);
        SetDlgItemTextA(hdlg, IDC_ROPEFX_VOL_BITMAP, fx.volumetric_bitmap.c_str());
        alpine_dlg_set_float_field(hdlg, IDC_ROPEFX_VOL_HEIGHT, fx.volumetric_height);
        alpine_dlg_set_float_field(hdlg, IDC_ROPEFX_VOL_LENGTH, fx.volumetric_length);

        alpine_dlg_set_color_controls(hdlg, IDC_ROPEFX_LIGHT_COLOR_SWATCH,
                                      IDC_ROPEFX_LIGHT_COLOR_VALUE, fx.light_r, fx.light_g,
                                      fx.light_b);
        alpine_dlg_set_float_field(hdlg, IDC_ROPEFX_LIGHT_RADIUS, fx.light_radius);
        alpine_dlg_set_float_field(hdlg, IDC_ROPEFX_LIGHT_INTENSITY, fx.light_intensity);

        CheckDlgButton(hdlg, IDC_ROPEFX_GLARE_ENABLE, fx.has_glare() ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_ROPEFX_LIGHT_ENABLE, fx.has_light() ? BST_CHECKED : BST_UNCHECKED);

        // Ranges match rope_clamp_slot_fx, so a spinner cannot reach a value the OK path would
        // clamp away.
        const float max_pos = rope_curve::deco_fx_max_pos_offset;
        const float max_rot = rope_curve::deco_fx_max_rot_offset;
        alpine_spinner_init(hdlg, IDC_ROPEFX_POS_X, IDC_ROPEFX_POS_X_SPIN, 0.01f, -max_pos, max_pos, 2);
        alpine_spinner_init(hdlg, IDC_ROPEFX_POS_Y, IDC_ROPEFX_POS_Y_SPIN, 0.01f, -max_pos, max_pos, 2);
        alpine_spinner_init(hdlg, IDC_ROPEFX_POS_Z, IDC_ROPEFX_POS_Z_SPIN, 0.01f, -max_pos, max_pos, 2);
        alpine_spinner_init(hdlg, IDC_ROPEFX_ROT_PITCH, IDC_ROPEFX_ROT_PITCH_SPIN, 1.0f, -max_rot, max_rot, 1);
        alpine_spinner_init(hdlg, IDC_ROPEFX_ROT_YAW, IDC_ROPEFX_ROT_YAW_SPIN, 1.0f, -max_rot, max_rot, 1);
        alpine_spinner_init(hdlg, IDC_ROPEFX_ROT_ROLL, IDC_ROPEFX_ROT_ROLL_SPIN, 1.0f, -max_rot, max_rot, 1);
        alpine_spinner_init(hdlg, IDC_ROPEFX_CONE_ANGLE, IDC_ROPEFX_CONE_ANGLE_SPIN, 1.0f, 0.0f,
                            rope_curve::deco_fx_max_cone_angle, 1);
        alpine_spinner_init(hdlg, IDC_ROPEFX_INTENSITY, IDC_ROPEFX_INTENSITY_SPIN, 0.05f, 0.0f,
                            rope_curve::deco_fx_max_intensity, 2);
        alpine_spinner_init(hdlg, IDC_ROPEFX_RADIUS_SCALE, IDC_ROPEFX_RADIUS_SCALE_SPIN, 0.1f, 0.0f,
                            rope_curve::deco_fx_max_radius_scale, 2);
        alpine_spinner_init(hdlg, IDC_ROPEFX_RADIUS_DISTANCE, IDC_ROPEFX_RADIUS_DISTANCE_SPIN, 0.1f,
                            0.0f, rope_curve::deco_fx_max_radius_distance, 2);
        // Signed, and stock authoring values sit within a tenth of zero, so it steps finer.
        alpine_spinner_init(hdlg, IDC_ROPEFX_DIMINISH_DISTANCE, IDC_ROPEFX_DIMINISH_DISTANCE_SPIN,
                            0.05f, -rope_curve::deco_fx_max_diminish_distance,
                            rope_curve::deco_fx_max_diminish_distance, 2);
        alpine_spinner_init(hdlg, IDC_ROPEFX_VOL_HEIGHT, IDC_ROPEFX_VOL_HEIGHT_SPIN, 0.1f, 0.0f,
                            rope_curve::deco_fx_max_volumetric, 2);
        alpine_spinner_init(hdlg, IDC_ROPEFX_VOL_LENGTH, IDC_ROPEFX_VOL_LENGTH_SPIN, 0.1f, 0.0f,
                            rope_curve::deco_fx_max_volumetric, 2);
        alpine_spinner_init(hdlg, IDC_ROPEFX_LIGHT_RADIUS, IDC_ROPEFX_LIGHT_RADIUS_SPIN, 0.1f,
                            rope_curve::deco_fx_min_light_radius,
                            rope_curve::deco_fx_max_light_radius, 2);
        alpine_spinner_init(hdlg, IDC_ROPEFX_LIGHT_INTENSITY, IDC_ROPEFX_LIGHT_INTENSITY_SPIN, 0.05f,
                            0.0f, rope_curve::deco_fx_max_light_intensity, 2);

        rope_fx_popup_update_state(hdlg);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_ROPEFX_GLARE_ENABLE:
        case IDC_ROPEFX_LIGHT_ENABLE:
            rope_fx_popup_update_state(hdlg);
            return TRUE;
        case IDC_ROPEFX_GLARE_BROWSE:
            rope_fx_browse_bitmap(hdlg, IDC_ROPEFX_GLARE_BITMAP);
            return TRUE;
        case IDC_ROPEFX_VOL_BROWSE:
            rope_fx_browse_bitmap(hdlg, IDC_ROPEFX_VOL_BITMAP);
            return TRUE;
        case IDC_ROPEFX_GLARE_COLOR_VALUE:
            if (HIWORD(wp) == EN_KILLFOCUS) {
                alpine_dlg_parse_color_text(hdlg, IDC_ROPEFX_GLARE_COLOR_VALUE,
                                            g_rope_fx_popup.glare_r, g_rope_fx_popup.glare_g,
                                            g_rope_fx_popup.glare_b);
                alpine_dlg_set_color_controls(hdlg, IDC_ROPEFX_GLARE_COLOR_SWATCH,
                                              IDC_ROPEFX_GLARE_COLOR_VALUE, g_rope_fx_popup.glare_r,
                                              g_rope_fx_popup.glare_g, g_rope_fx_popup.glare_b);
            }
            break;
        case IDC_ROPEFX_LIGHT_COLOR_VALUE:
            if (HIWORD(wp) == EN_KILLFOCUS) {
                alpine_dlg_parse_color_text(hdlg, IDC_ROPEFX_LIGHT_COLOR_VALUE,
                                            g_rope_fx_popup.light_r, g_rope_fx_popup.light_g,
                                            g_rope_fx_popup.light_b);
                alpine_dlg_set_color_controls(hdlg, IDC_ROPEFX_LIGHT_COLOR_SWATCH,
                                              IDC_ROPEFX_LIGHT_COLOR_VALUE, g_rope_fx_popup.light_r,
                                              g_rope_fx_popup.light_g, g_rope_fx_popup.light_b);
            }
            break;
        case IDC_ROPEFX_GLARE_COLOR_CHANGE:
            alpine_dlg_pick_color(hdlg, IDC_ROPEFX_GLARE_COLOR_SWATCH, IDC_ROPEFX_GLARE_COLOR_VALUE,
                                  g_rope_fx_popup.glare_r, g_rope_fx_popup.glare_g,
                                  g_rope_fx_popup.glare_b);
            return TRUE;
        case IDC_ROPEFX_LIGHT_COLOR_CHANGE:
            alpine_dlg_pick_color(hdlg, IDC_ROPEFX_LIGHT_COLOR_SWATCH, IDC_ROPEFX_LIGHT_COLOR_VALUE,
                                  g_rope_fx_popup.light_r, g_rope_fx_popup.light_g,
                                  g_rope_fx_popup.light_b);
            return TRUE;
        case IDC_ROPEFX_APPLY_ALL: {
            // Writes all six slots' staging immediately, its own included, and they stay written
            // however this popup is then closed - a Cancel here no longer restores the slot's
            // pre-apply value. An OK just rewrites its own slot with whatever the controls hold.
            const DedRopeSlotFx fx = rope_fx_popup_read(hdlg);
            for (auto& slot : g_rope_dlg_fx) {
                slot = fx;
            }
            if (HWND parent = GetParent(hdlg)) {
                rope_update_fx_labels(parent);
            }
            return TRUE;
        }
        case IDOK:
            g_rope_fx_popup = rope_fx_popup_read(hdlg);
            EndDialog(hdlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_NOTIFY:
        if (alpine_spinner_handle_notify(hdlg, lp)) return TRUE;
        break;
    }
    return FALSE;
}

// Opens the popup on one slot's staged effects; an OK there commits that slot to the staging.
// Apply to all slots inside it has already written all six by the time this returns, so a Cancel
// after one leaves the applied values in place.
static void rope_emitter_edit_slot_fx(HWND hdlg, int slot)
{
    g_rope_fx_popup = g_rope_dlg_fx[slot];
    INT_PTR result = DialogBoxParam(
        reinterpret_cast<HINSTANCE>(&__ImageBase),
        MAKEINTRESOURCE(IDD_ALPINE_ROPE_SLOT_FX),
        hdlg,
        RopeSlotFxDialogProc,
        0
    );
    if (result == IDOK) {
        g_rope_dlg_fx[slot] = g_rope_fx_popup;
    }
    rope_update_fx_labels(hdlg);
}

// ─── Decorations popup ──────────────────────────────────────────────────────

// The spacing method decides which of the two numeric inputs stays live - the three ends modes
// place their instances on the anchors, so neither applies. The module checkbox that gates the
// whole popup is on the main panel, which is the only way in here.
static void rope_deco_update_state(HWND hdlg)
{
    // CB_ERR on an empty combo falls through to the count mode, which is the default anyway.
    const LRESULT mode = SendDlgItemMessageA(hdlg, IDC_ROPE_DECO_SPACING_MODE, CB_GETCURSEL, 0, 0);
    const bool by_count = mode <= static_cast<LRESULT>(rope_curve::DecoSpacing::fixed_count);
    const bool by_spacing = mode == static_cast<LRESULT>(rope_curve::DecoSpacing::every_n_meters);
    EnableWindow(GetDlgItem(hdlg, IDC_ROPE_DECO_COUNT), by_count);
    EnableWindow(GetDlgItem(hdlg, IDC_ROPE_DECO_COUNT_SPIN), by_count);
    EnableWindow(GetDlgItem(hdlg, IDC_ROPE_DECO_COUNT_LABEL), by_count);
    EnableWindow(GetDlgItem(hdlg, IDC_ROPE_DECO_SPACING), by_spacing);
    EnableWindow(GetDlgItem(hdlg, IDC_ROPE_DECO_SPACING_SPIN), by_spacing);
    EnableWindow(GetDlgItem(hdlg, IDC_ROPE_DECO_SPACING_LABEL), by_spacing);
}

// The marker preview follows this popup's own fields while it is up: the main panel's staging has
// not been written yet, and leaving the viewport frozen behind a modal is exactly what the live
// preview exists to avoid.
static void rope_deco_refresh_preview(HWND hdlg)
{
    if (!g_rope_preview.active) return;
    const LRESULT sel = SendDlgItemMessageA(hdlg, IDC_ROPE_DECO_SPACING_MODE, CB_GETCURSEL, 0, 0);
    g_rope_preview.deco_spacing_mode =
        (sel > 0 && sel <= rope_curve::deco_spacing_mode_max) ? static_cast<uint8_t>(sel) : 0;
    g_rope_preview.deco_count = std::clamp(alpine_dlg_get_int_field(hdlg, IDC_ROPE_DECO_COUNT),
                                           rope_curve::deco_min_count, rope_curve::deco_max_count);
    g_rope_preview.deco_spacing =
        rope_curve::clamp_finite(alpine_dlg_get_float_field(hdlg, IDC_ROPE_DECO_SPACING),
                          rope_curve::deco_min_spacing, rope_curve::deco_max_spacing, 1.0f);
    redraw_all_viewports();
}

static INT_PTR CALLBACK RopeDecorationsDialogProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        HWND spacing_combo = GetDlgItem(hdlg, IDC_ROPE_DECO_SPACING_MODE);
        SendMessageA(spacing_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Fixed count"));
        SendMessageA(spacing_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Every N meters"));
        SendMessageA(spacing_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Both ends"));
        SendMessageA(spacing_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Emitter end"));
        SendMessageA(spacing_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Target end"));
        SendMessageA(spacing_combo, CB_SETCURSEL,
            g_rope_dlg_deco.spacing_mode > rope_curve::deco_spacing_mode_max
                ? 0 : g_rope_dlg_deco.spacing_mode, 0);

        HWND orient_combo = GetDlgItem(hdlg, IDC_ROPE_DECO_ORIENT);
        SendMessageA(orient_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Follow curve"));
        SendMessageA(orient_combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("Upright"));
        SendMessageA(orient_combo, CB_SETCURSEL,
            g_rope_dlg_deco.orient_mode > 1 ? 0 : g_rope_dlg_deco.orient_mode, 0);

        for (int i = 0; i < rope_curve::deco_max_meshes; i++) {
            SetDlgItemTextA(hdlg, rope_deco_mesh_idcs[i], g_rope_dlg_deco.meshes[i].c_str());
        }
        rope_update_fx_labels(hdlg);
        CheckDlgButton(hdlg, IDC_ROPE_DECO_RANDOM,
            g_rope_dlg_deco.random_order ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemInt(hdlg, IDC_ROPE_DECO_COUNT, static_cast<UINT>(g_rope_dlg_deco.count), FALSE);
        alpine_dlg_set_float_field(hdlg, IDC_ROPE_DECO_SPACING, g_rope_dlg_deco.spacing);

        alpine_spinner_init_int(hdlg, IDC_ROPE_DECO_COUNT, IDC_ROPE_DECO_COUNT_SPIN, 1,
                                rope_curve::deco_min_count, rope_curve::deco_max_count);
        alpine_spinner_init(hdlg, IDC_ROPE_DECO_SPACING, IDC_ROPE_DECO_SPACING_SPIN, 0.05f,
                            rope_curve::deco_min_spacing, rope_curve::deco_max_spacing, 2);

        rope_deco_update_state(hdlg);
        return TRUE;
    }
    case WM_COMMAND: {
        for (int i = 0; i < rope_curve::deco_max_meshes; i++) {
            if (LOWORD(wp) != rope_deco_browse_idcs[i]) continue;
            char current[MAX_PATH] = {};
            GetDlgItemTextA(hdlg, rope_deco_mesh_idcs[i], current, sizeof(current));
            std::string chosen = current;
            // Decorations are static geometry only, so this field takes no animation.
            if (alpine_browse_mesh(hdlg, chosen, ALPINE_MESH_V3M)) {
                SetDlgItemTextA(hdlg, rope_deco_mesh_idcs[i], chosen.c_str());
            }
            return TRUE;
        }
        for (int i = 0; i < rope_curve::deco_max_meshes; i++) {
            if (LOWORD(wp) != rope_deco_fx_idcs[i]) continue;
            rope_emitter_edit_slot_fx(hdlg, i);
            return TRUE;
        }
        switch (LOWORD(wp)) {
        case IDC_ROPE_DECO_SPACING_MODE:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                rope_deco_update_state(hdlg);
                rope_deco_refresh_preview(hdlg);
            }
            break;
        case IDC_ROPE_DECO_COUNT:
        case IDC_ROPE_DECO_SPACING:
            if (HIWORD(wp) == EN_CHANGE) {
                rope_deco_refresh_preview(hdlg);
            }
            break;
        case IDOK:
            for (int i = 0; i < rope_curve::deco_max_meshes; i++) {
                char mesh_buf[MAX_PATH] = {};
                GetDlgItemTextA(hdlg, rope_deco_mesh_idcs[i], mesh_buf, sizeof(mesh_buf));
                std::string name = mesh_buf;
                // Same cap the loader applies, so a name the game would drop is rejected here.
                if (name.size() > rfl_mesh_name_max_len) name.clear();
                g_rope_dlg_deco.meshes[i] = name;
            }
            {
                const LRESULT sel = SendDlgItemMessageA(hdlg, IDC_ROPE_DECO_SPACING_MODE,
                                                        CB_GETCURSEL, 0, 0);
                g_rope_dlg_deco.spacing_mode =
                    (sel > 0 && sel <= rope_curve::deco_spacing_mode_max)
                        ? static_cast<uint8_t>(sel)
                        : 0;
            }
            g_rope_dlg_deco.orient_mode =
                SendDlgItemMessageA(hdlg, IDC_ROPE_DECO_ORIENT, CB_GETCURSEL, 0, 0) == 1 ? 1 : 0;
            g_rope_dlg_deco.count = alpine_dlg_get_int_field(hdlg, IDC_ROPE_DECO_COUNT);
            g_rope_dlg_deco.spacing = alpine_dlg_get_float_field(hdlg, IDC_ROPE_DECO_SPACING);
            g_rope_dlg_deco.random_order =
                IsDlgButtonChecked(hdlg, IDC_ROPE_DECO_RANDOM) == BST_CHECKED;
            EndDialog(hdlg, IDOK);
            return TRUE;
        case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY:
        if (alpine_spinner_handle_notify(hdlg, lp)) return TRUE;
        break;
    }
    return FALSE;
}

static void rope_emitter_edit_decorations(HWND hdlg)
{
    // Only the mesh list and the placement rules wait for this popup's OK. The per-slot FX popups
    // nested inside it write the FX staging as they close, and Apply to all slots writes all six at
    // once, so without a snapshot a Cancel here would discard the list edits while keeping the FX
    // edits made alongside them.
    DedRopeSlotFx saved_fx[rope_curve::deco_max_meshes];
    for (int i = 0; i < rope_curve::deco_max_meshes; i++) {
        saved_fx[i] = g_rope_dlg_fx[i];
    }

    const INT_PTR result = DialogBoxParam(
        reinterpret_cast<HINSTANCE>(&__ImageBase),
        MAKEINTRESOURCE(IDD_ALPINE_ROPE_DECORATIONS),
        hdlg,
        RopeDecorationsDialogProc,
        0
    );
    if (result != IDOK) {
        for (int i = 0; i < rope_curve::deco_max_meshes; i++) {
            g_rope_dlg_fx[i] = saved_fx[i];
        }
    }
    rope_deco_preview_from_staging();
    if (g_rope_preview.active) {
        redraw_all_viewports();
    }
}

static INT_PTR CALLBACK RopeEmitterDialogProc(HWND hdlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        if (g_selected_rope_emitters.empty()) {
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        auto* rope = g_selected_rope_emitters[0];

        SetDlgItemTextA(hdlg, IDC_ROPE_SCRIPT_NAME, rope->script_name.c_str());
        SetDlgItemInt(hdlg, IDC_ROPE_TARGET_UID, static_cast<UINT>(rope->target_uid), TRUE);

        alpine_dlg_set_float_field(hdlg, IDC_ROPE_DANGLE_LENGTH, rope->dangle_length);
        alpine_dlg_set_float_field(hdlg, IDC_ROPE_SLACK, rope->slack);
        alpine_dlg_set_float_field(hdlg, IDC_ROPE_WEIGHT, rope->weight);
        alpine_dlg_set_float_field(hdlg, IDC_ROPE_THICKNESS, rope->thickness);
        SetDlgItemInt(hdlg, IDC_ROPE_SEGMENTS, static_cast<UINT>(rope->segments), FALSE);
        alpine_dlg_set_float_field(hdlg, IDC_ROPE_UV_TILES, rope->uv_tiles_per_meter);
        alpine_dlg_set_float_field(hdlg, IDC_ROPE_SWAY_AMPLITUDE, rope->sway_amplitude);
        alpine_dlg_set_float_field(hdlg, IDC_ROPE_SWAY_SPEED, rope->sway_speed);

        g_rope_color_r = rope->color_r;
        g_rope_color_g = rope->color_g;
        g_rope_color_b = rope->color_b;
        rope_emitter_update_color_controls(hdlg);
        SetDlgItemInt(hdlg, IDC_ROPE_COLOR_A, rope->color_a, FALSE);
        SetDlgItemTextA(hdlg, IDC_ROPE_BITMAP, rope->bitmap.c_str());
        rope_emitter_update_bitmap_preview(hdlg, true);

        // Script name and target UID are per-rope identity; writing one over a whole selection
        // would collapse them, so a multi-select cannot edit them at all. The decoration set is the
        // same case - one shared staging - and its greying lives in update_state, but the label has
        // to say whose decorations are on display.
        if (g_selected_rope_emitters.size() > 1) {
            EnableWindow(GetDlgItem(hdlg, IDC_ROPE_SCRIPT_NAME), FALSE);
            EnableWindow(GetDlgItem(hdlg, IDC_ROPE_TARGET_UID), FALSE);
            SetDlgItemTextA(hdlg, IDC_ROPE_DECORATIONS, "Decorations (first selected)");
        }

        CheckDlgButton(hdlg, IDC_ROPE_DYNAMIC,
            (rope->flags & ded_rope_flag_dynamic) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_ROPE_GLOW,
            (rope->flags & ded_rope_flag_glow) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_ROPE_SWAY,
            (rope->flags & ded_rope_flag_sway) ? BST_CHECKED : BST_UNCHECKED);
        CheckDlgButton(hdlg, IDC_ROPE_INITIALLY_ON, rope->initially_on ? BST_CHECKED : BST_UNCHECKED);

        // The decoration list and its placement rules have no controls on this panel: they are
        // staged here and edited through the Decorations popup, which writes back to the staging.
        for (int i = 0; i < rope_curve::deco_max_meshes; i++) {
            g_rope_dlg_deco.meshes[i] = rope->deco_meshes[i];
            g_rope_dlg_fx[i] = rope->deco_fx[i];
        }
        g_rope_dlg_deco.spacing_mode = rope->deco_spacing_mode;
        g_rope_dlg_deco.count = rope->deco_count;
        g_rope_dlg_deco.spacing = rope->deco_spacing;
        g_rope_dlg_deco.random_order = rope->deco_random_order;
        g_rope_dlg_deco.orient_mode = rope->deco_orient_mode;

        CheckDlgButton(hdlg, IDC_ROPE_DECORATIONS,
            rope->decorations_enabled ? BST_CHECKED : BST_UNCHECKED);
        rope_emitter_update_state(hdlg);

        // Ranges match rope_clamp_properties, so a spinner cannot reach a value the OK path would
        // clamp away. The target UID is identity, not a tunable, so it keeps a plain edit.
        alpine_spinner_init(hdlg, IDC_ROPE_SLACK, IDC_ROPE_SLACK_SPIN, 0.05f, 0.0f, rope_curve::rope_max_slack, 2);
        alpine_spinner_init(hdlg, IDC_ROPE_DANGLE_LENGTH, IDC_ROPE_DANGLE_LENGTH_SPIN, 0.05f,
                            rope_curve::rope_min_dangle, rope_curve::rope_max_dangle, 2);
        alpine_spinner_init_int(hdlg, IDC_ROPE_SEGMENTS, IDC_ROPE_SEGMENTS_SPIN, 1,
                                rope_curve::rope_min_segments, rope_curve::rope_max_segments);
        alpine_spinner_init(hdlg, IDC_ROPE_THICKNESS, IDC_ROPE_THICKNESS_SPIN, 0.005f,
                            rope_curve::rope_min_thickness, rope_curve::rope_max_thickness, 3);
        alpine_spinner_init(hdlg, IDC_ROPE_UV_TILES, IDC_ROPE_UV_TILES_SPIN, 0.1f, 0.0f,
                            rope_curve::rope_max_uv_tiles, 2);
        alpine_spinner_init(hdlg, IDC_ROPE_WEIGHT, IDC_ROPE_WEIGHT_SPIN, 0.05f, rope_curve::rope_min_weight,
                            rope_curve::rope_max_weight, 2);
        alpine_spinner_init(hdlg, IDC_ROPE_SWAY_AMPLITUDE, IDC_ROPE_SWAY_AMPLITUDE_SPIN, 0.01f,
                            0.0f, rope_curve::rope_max_sway_amplitude, 2);
        alpine_spinner_init(hdlg, IDC_ROPE_SWAY_SPEED, IDC_ROPE_SWAY_SPEED_SPIN, 0.05f, 0.0f,
                            rope_curve::rope_max_sway_speed, 2);

        rope_emitter_capture_preview(hdlg);
        g_rope_preview.active = true;

        return TRUE;
    }
    case WM_COMMAND: {
        switch (LOWORD(wp)) {
        case IDC_ROPE_DECORATIONS:
            rope_emitter_update_state(hdlg);
            rope_emitter_refresh_preview(hdlg);
            return TRUE;
        case IDC_ROPE_DECO_EDIT:
            rope_emitter_edit_decorations(hdlg);
            return TRUE;
        case IDC_ROPE_DYNAMIC:
        case IDC_ROPE_SWAY:
            rope_emitter_update_state(hdlg);
            return TRUE;
        case IDC_ROPE_TARGET_UID:
            if (HIWORD(wp) == EN_CHANGE) {
                rope_emitter_update_state(hdlg);
                rope_emitter_refresh_preview(hdlg);
            }
            break;
        case IDC_ROPE_SLACK:
        case IDC_ROPE_DANGLE_LENGTH:
        case IDC_ROPE_SEGMENTS:
        case IDC_ROPE_THICKNESS:
            if (HIWORD(wp) == EN_CHANGE) {
                rope_emitter_refresh_preview(hdlg);
            }
            break;
        case IDC_ROPE_COLOR_VALUE:
            if (HIWORD(wp) == EN_CHANGE) {
                rope_emitter_refresh_preview(hdlg);
            }
            else if (HIWORD(wp) == EN_KILLFOCUS) {
                rope_emitter_read_color_text(hdlg, g_rope_color_r, g_rope_color_g, g_rope_color_b);
                rope_emitter_update_color_controls(hdlg);
            }
            break;
        case IDC_ROPE_COLOR_CHANGE:
            rope_emitter_pick_color(hdlg);
            rope_emitter_refresh_preview(hdlg);
            return TRUE;
        case IDC_ROPE_BITMAP:
            if (HIWORD(wp) == EN_CHANGE) {
                rope_emitter_update_bitmap_preview(hdlg, false);
            }
            break;
        case IDC_ROPE_BITMAP_BROWSE: {
            // Same modal-loop dance as rope_fx_browse_bitmap.
            EnableWindow(hdlg, FALSE);
            int picked = texture_browser_pick("Effects", g_rope_bitmap_preview_handle);
            EnableWindow(hdlg, TRUE);
            SetActiveWindow(hdlg);
            if (picked >= 0) {
                const char* name = bm_get_filename(picked);
                SetDlgItemTextA(hdlg, IDC_ROPE_BITMAP, name ? name : "");
                rope_emitter_update_bitmap_preview(hdlg, true);
            }
            return TRUE;
        }
        case IDOK: {
            const bool single = g_selected_rope_emitters.size() == 1;

            char buf[256] = {};
            GetDlgItemTextA(hdlg, IDC_ROPE_SCRIPT_NAME, buf, sizeof(buf));
            char bmp_buf[256] = {};
            GetDlgItemTextA(hdlg, IDC_ROPE_BITMAP, bmp_buf, sizeof(bmp_buf));
            // Same cap the loader applies, so a name the game would drop is rejected here.
            std::string bitmap = bmp_buf;
            if (rfl_name_over_long(bitmap)) bitmap.clear();

            BOOL translated = FALSE;
            int target_uid = static_cast<int>(GetDlgItemInt(hdlg, IDC_ROPE_TARGET_UID, &translated, TRUE));
            if (!translated) target_uid = -1;

            float dangle_length = alpine_dlg_get_float_field(hdlg, IDC_ROPE_DANGLE_LENGTH);
            float slack = alpine_dlg_get_float_field(hdlg, IDC_ROPE_SLACK);
            float weight = alpine_dlg_get_float_field(hdlg, IDC_ROPE_WEIGHT);
            float thickness = alpine_dlg_get_float_field(hdlg, IDC_ROPE_THICKNESS);
            int segments = alpine_dlg_get_int_field(hdlg, IDC_ROPE_SEGMENTS);
            float uv_tiles = alpine_dlg_get_float_field(hdlg, IDC_ROPE_UV_TILES);
            float sway_amplitude = alpine_dlg_get_float_field(hdlg, IDC_ROPE_SWAY_AMPLITUDE);
            float sway_speed = alpine_dlg_get_float_field(hdlg, IDC_ROPE_SWAY_SPEED);

            // The text field is authoritative for RGB: it may not have lost focus yet.
            rope_emitter_read_color_text(hdlg, g_rope_color_r, g_rope_color_g, g_rope_color_b);
            uint8_t r = g_rope_color_r;
            uint8_t g = g_rope_color_g;
            uint8_t b = g_rope_color_b;
            uint8_t a = static_cast<uint8_t>(
                std::min(GetDlgItemInt(hdlg, IDC_ROPE_COLOR_A, nullptr, FALSE), 255u));

            uint32_t flags = 0;
            if (IsDlgButtonChecked(hdlg, IDC_ROPE_DYNAMIC) == BST_CHECKED) flags |= ded_rope_flag_dynamic;
            if (IsDlgButtonChecked(hdlg, IDC_ROPE_GLOW) == BST_CHECKED) flags |= ded_rope_flag_glow;
            if (IsDlgButtonChecked(hdlg, IDC_ROPE_SWAY) == BST_CHECKED) flags |= ded_rope_flag_sway;
            bool initially_on = IsDlgButtonChecked(hdlg, IDC_ROPE_INITIALLY_ON) == BST_CHECKED;

            // Compacted, and each slot's effects move with its name: what the wire unit and both
            // loaders assume.
            bool decorations_enabled = IsDlgButtonChecked(hdlg, IDC_ROPE_DECORATIONS) == BST_CHECKED;
            std::string deco_meshes[rope_curve::deco_max_meshes];
            DedRopeSlotFx deco_fx[rope_curve::deco_max_meshes];
            int deco_kept = 0;
            for (int i = 0; i < rope_curve::deco_max_meshes; i++) {
                if (g_rope_dlg_deco.meshes[i].empty()) continue;
                deco_meshes[deco_kept] = g_rope_dlg_deco.meshes[i];
                deco_fx[deco_kept] = g_rope_dlg_fx[i];
                deco_kept++;
            }
            uint8_t deco_spacing_mode =
                g_rope_dlg_deco.spacing_mode > rope_curve::deco_spacing_mode_max
                    ? 0
                    : g_rope_dlg_deco.spacing_mode;
            uint8_t deco_orient_mode = g_rope_dlg_deco.orient_mode > 1 ? 0 : g_rope_dlg_deco.orient_mode;
            int deco_count = g_rope_dlg_deco.count;
            float deco_spacing = g_rope_dlg_deco.spacing;
            bool deco_random_order = g_rope_dlg_deco.random_order;

            for (auto* rope : g_selected_rope_emitters) {
                if (single) {
                    rope->script_name.assign_0(buf);
                    rope->target_uid = target_uid;
                }
                rope->bitmap = bitmap;
                rope->dangle_length = dangle_length;
                rope->slack = slack;
                rope->weight = weight;
                rope->thickness = thickness;
                rope->segments = segments;
                rope->uv_tiles_per_meter = uv_tiles;
                rope->sway_amplitude = sway_amplitude;
                rope->sway_speed = sway_speed;
                rope->color_r = r;
                rope->color_g = g;
                rope->color_b = b;
                rope->color_a = a;
                rope->flags = flags;
                rope->initially_on = initially_on;
                // The whole decoration set comes from one shared staging loaded off the first
                // selected rope, so writing it over the selection would replace every other rope's
                // decorations with the first one's. Single-select only, like the two identity fields.
                if (single) {
                    rope->decorations_enabled = decorations_enabled;
                    for (int i = 0; i < rope_curve::deco_max_meshes; i++) {
                        rope->deco_meshes[i] = deco_meshes[i];
                        rope->deco_fx[i] = deco_fx[i];
                    }
                    rope->deco_spacing_mode = deco_spacing_mode;
                    rope->deco_count = deco_count;
                    rope->deco_spacing = deco_spacing;
                    rope->deco_random_order = deco_random_order;
                    rope->deco_orient_mode = deco_orient_mode;
                }
                rope_clamp_properties(rope);
            }
            EndDialog(hdlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY:
        if (alpine_spinner_handle_notify(hdlg, lp)) return TRUE;
        break;
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
        if (dis && dis->CtlID == IDC_ROPE_BITMAP_PREVIEW) {
            alpine_dlg_draw_bitmap_preview(dis->hwndItem, dis->rcItem, g_rope_bitmap_preview_handle);
            return TRUE;
        }
        break;
    }
    }
    return FALSE;
}

void ShowRopeEmitterPropertiesDialog(CDedLevel* level)
{
    auto& sel = level->selection;
    g_selected_rope_emitters.clear();
    for (int i = 0; i < sel.get_size(); i++) {
        DedObject* obj = sel[i];
        if (obj && obj->type == DedObjectType::DED_ROPE_EMITTER) {
            g_selected_rope_emitters.push_back(static_cast<DedRopeEmitter*>(obj));
        }
    }

    if (!g_selected_rope_emitters.empty()) {
        DialogBoxParam(
            reinterpret_cast<HINSTANCE>(&__ImageBase),
            MAKEINTRESOURCE(IDD_ALPINE_ROPE_PROPERTIES),
            GetActiveWindow(),
            RopeEmitterDialogProc,
            0
        );
        // Objects are only written by IDOK, so a cancelled dialog has nothing to restore.
        g_rope_preview.active = false;
    }

    g_selected_rope_emitters.clear();
}

// ─── Object Lifecycle ───────────────────────────────────────────────────────

void PlaceNewRopeEmitterObject()
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto* rope = new DedRopeEmitter();
    memset(static_cast<DedObject*>(rope), 0, sizeof(DedObject));
    rope->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
    rope->type = DedObjectType::DED_ROPE_EMITTER;

    rope->script_name.assign_0("Rope Emitter");

    auto* viewport = get_active_viewport();
    if (viewport && viewport->view_data) {
        rope->pos = viewport->view_data->camera_pos;
        rope->orient = viewport->view_data->camera_orient;
    }
    else {
        rope->orient.rvec = {1.0f, 0.0f, 0.0f};
        rope->orient.uvec = {0.0f, 1.0f, 0.0f};
        rope->orient.fvec = {0.0f, 0.0f, 1.0f};
    }

    rope->uid = generate_uid();

    level->GetAlpineLevelProperties().rope_emitter_objects.push_back(rope);
    level->master_objects.add(static_cast<DedObject*>(rope));

    level->clear_selection();
    level->add_to_selection(static_cast<DedObject*>(rope));
    level->update_console_display();
}

DedRopeEmitter* CloneRopeEmitterObject(DedRopeEmitter* source, bool add_to_level)
{
    if (!source) return nullptr;

    auto* rope = new DedRopeEmitter();
    memset(static_cast<DedObject*>(rope), 0, sizeof(DedObject));
    rope->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
    rope->type = DedObjectType::DED_ROPE_EMITTER;

    rope->pos = source->pos;
    rope->orient = source->orient;
    rope->script_name.assign_0(source->script_name.c_str());

    rope->target_uid = source->target_uid;
    rope->dangle_length = source->dangle_length;
    rope->slack = source->slack;
    rope->weight = source->weight;
    rope->thickness = source->thickness;
    rope->segments = source->segments;
    rope->uv_tiles_per_meter = source->uv_tiles_per_meter;
    rope->color_r = source->color_r;
    rope->color_g = source->color_g;
    rope->color_b = source->color_b;
    rope->color_a = source->color_a;
    rope->bitmap = source->bitmap;
    rope->sway_amplitude = source->sway_amplitude;
    rope->sway_speed = source->sway_speed;
    rope->flags = source->flags;
    rope->initially_on = source->initially_on;
    rope->decorations_enabled = source->decorations_enabled;
    for (int i = 0; i < rope_curve::deco_max_meshes; i++) {
        rope->deco_meshes[i] = source->deco_meshes[i];
        rope->deco_fx[i] = source->deco_fx[i];
    }
    rope->deco_spacing_mode = source->deco_spacing_mode;
    rope->deco_count = source->deco_count;
    rope->deco_spacing = source->deco_spacing;
    rope->deco_random_order = source->deco_random_order;
    rope->deco_orient_mode = source->deco_orient_mode;

    rope->uid = generate_uid();

    if (add_to_level) {
        auto* level = CDedLevel::Get();
        if (level) {
            level->GetAlpineLevelProperties().rope_emitter_objects.push_back(rope);
            level->master_objects.add(static_cast<DedObject*>(rope));
        }
    }

    return rope;
}

void DeleteRopeEmitterObject(DedRopeEmitter* rope)
{
    if (!rope) return;
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto& ropes = level->GetAlpineLevelProperties().rope_emitter_objects;
    auto it = std::find(ropes.begin(), ropes.end(), rope);
    if (it != ropes.end()) {
        ropes.erase(it);
    }
    alpine_remove_from_groups(level, static_cast<DedObject*>(rope));
    level->master_objects.remove_by_value(static_cast<DedObject*>(rope));
    DestroyDedRopeEmitter(rope);
}

// ─── Rendering ──────────────────────────────────────────────────────────────

void rope_emitter_render(CDedLevel* level)
{
    auto& ropes = level->GetAlpineLevelProperties().rope_emitter_objects;
    if (ropes.empty()) return;

    rope_emitter_load_icon();

    const float cam_param = gr_cam_param;
    static std::vector<rope_curve::Vec3> solved;

    // One index per repaint, so the preview still tracks an anchor being dragged without each
    // rope re-walking master_objects.
    static std::unordered_map<int, DedObject*> uid_index;
    rope_emitter_build_uid_index(level, uid_index);

    // Selection membership, so a multi-select previews on every rope the dialog will write to.
    const bool preview_all = g_rope_preview.active && !g_selected_rope_emitters.empty();
    static std::vector<Vector3> staged_points;

    for (auto* rope : ropes) {
        if (rope->hidden_in_editor) continue;

        const bool selected = is_object_selected(level, rope);
        const bool preview = preview_all &&
            std::find(g_selected_rope_emitters.begin(), g_selected_rope_emitters.end(), rope) !=
                g_selected_rope_emitters.end();

        int r = 0xff, g = 0x00, b = 0x00; // selection always wins, as elsewhere in the family
        if (preview) {
            // While the dialog is open the staged tint wins over the selection red, which is the
            // only way a color edit is visible at all. Floored as below so black still reads.
            r = std::max<int>(g_rope_preview.color_r, 0x40);
            g = std::max<int>(g_rope_preview.color_g, 0x40);
            b = std::max<int>(g_rope_preview.color_b, 0x40);
        }
        else if (!selected) {
            // Dimmed rope tint, floored so a near-black rope still reads against the grid.
            r = std::max<int>(rope->color_r * 3 / 5, 0x40);
            g = std::max<int>(rope->color_g * 3 / 5, 0x40);
            b = std::max<int>(rope->color_b * 3 / 5, 0x40);
        }

        // The lookup runs every repaint; the solve behind it only runs when the quantized inputs
        // actually changed.
        const int32_t target_uid = (preview && g_rope_preview.stage_target)
                                       ? g_rope_preview.target_uid
                                       : rope->target_uid;
        Vector3 target_pos{};
        bool has_target = false;
        if (target_uid != -1) {
            auto it = uid_index.find(target_uid);
            if (it != uid_index.end()) {
                target_pos = it->second->pos;
                has_target = true;
            }
        }

        // The staged solve bypasses the cache outright: its key is the rope's own fields, which
        // the dialog has not written yet.
        float length = 0.0f;
        if (preview) {
            staged_points.clear();
            if (rope_emitter_solve_preview(rope->pos, g_rope_preview.slack,
                                           g_rope_preview.dangle_length, g_rope_preview.segments,
                                           has_target, target_pos, solved, length)) {
                staged_points.reserve(solved.size());
                for (const auto& p : solved) {
                    staged_points.push_back(Vector3{p.x, p.y, p.z});
                }
            }
            else {
                length = 0.0f;
            }
        }
        else {
            int32_t key[ded_rope_preview_key_len];
            rope_emitter_preview_key(rope, has_target, target_pos, key);
            if (!rope->preview_valid || memcmp(key, rope->preview_key, sizeof(key)) != 0) {
                rope->preview_points.clear();
                rope->preview_length = 0.0f;
                if (rope_emitter_solve_preview(rope->pos, rope->slack, rope->dangle_length,
                                               rope->segments, has_target, target_pos, solved,
                                               rope->preview_length)) {
                    rope->preview_points.reserve(solved.size());
                    for (const auto& p : solved) {
                        rope->preview_points.push_back(Vector3{p.x, p.y, p.z});
                    }
                }
                memcpy(rope->preview_key, key, sizeof(key));
                rope->preview_valid = true;
            }
            length = rope->preview_length;
        }

        const auto& points = preview ? staged_points : rope->preview_points;
        for (std::size_t i = 0; i + 1 < points.size(); i++) {
            draw_3d_line(points[i].x, points[i].y, points[i].z,
                         points[i + 1].x, points[i + 1].y, points[i + 1].z, r, g, b);
        }

        if (preview && g_rope_preview.decorations_enabled) {
            rope_emitter_draw_deco_markers(points, length, g_rope_preview.deco_spacing_mode,
                                           g_rope_preview.deco_count, g_rope_preview.deco_spacing,
                                           g_rope_preview.thickness);
        }
        else if (!preview && selected && rope->decorations_enabled) {
            rope_emitter_draw_deco_markers(points, length, rope->deco_spacing_mode,
                                           rope->deco_count, rope->deco_spacing, rope->thickness);
        }

        // The straight chord to the far anchor shows how much slack the curve is carrying.
        if (selected && has_target) {
            draw_3d_line(rope->pos.x, rope->pos.y, rope->pos.z,
                         target_pos.x, target_pos.y, target_pos.z, 0, 255, 255);
        }

        set_draw_color(r, g, b, 0xff);
        if (g_rope_emitter_icon_handle >= 0) {
            gr_set_bitmap(g_rope_emitter_icon_handle, -1);
        }
        gr_render_billboard(&rope->pos, 0, 0.25f, cam_param);
    }
}

void rope_emitter_pick(CDedLevel* level, int param1, int param2)
{
    auto& ropes = level->GetAlpineLevelProperties().rope_emitter_objects;
    for (auto* rope : ropes) {
        if (rope->hidden_in_editor) continue;
        bool hit = level->hit_test_point(param1, param2, &rope->pos);
        if (hit) {
            level->select_object(static_cast<DedObject*>(rope));
        }
    }
}

DedRopeEmitter* rope_emitter_click_pick(CDedLevel* level, float click_x, float click_y)
{
    return alpine_click_pick_point(level->GetAlpineLevelProperties().rope_emitter_objects,
                                   click_x, click_y, alpine_click_pick_radius_sq);
}

void rope_emitter_tree_populate(EditorTreeCtrl* tree, int master_groups, CDedLevel* level)
{
    auto& ropes = level->GetAlpineLevelProperties().rope_emitter_objects;

    char buf[64];
    snprintf(buf, sizeof(buf), "Rope Emitters (%d)", static_cast<int>(ropes.size()));
    int parent = tree->insert_item(buf, master_groups, 0xffff0002);

    for (auto* rope : ropes) {
        const char* name = rope->script_name.c_str();
        if (!name || name[0] == '\0') {
            name = "(unnamed rope emitter)";
        }
        int child = tree->insert_item(name, parent, 0xffff0002);
        tree->set_item_data(child, rope->uid);
    }
}

void rope_emitter_tree_add_object_type(EditorTreeCtrl* tree)
{
    tree->insert_item("Rope Emitter", 0xffff0000, 0xffff0002);
}

bool rope_emitter_copy_object(DedObject* source)
{
    if (!source || source->type != DedObjectType::DED_ROPE_EMITTER) return false;
    auto* staged = CloneRopeEmitterObject(static_cast<DedRopeEmitter*>(source), false);
    if (staged) {
        g_rope_emitter_clipboard.push_back(staged);
        return true;
    }
    return false;
}

void rope_emitter_paste_objects(CDedLevel* level)
{
    for (auto* staged : g_rope_emitter_clipboard) {
        auto* clone = CloneRopeEmitterObject(staged, true);
        if (clone) {
            level->add_to_selection(static_cast<DedObject*>(clone));
        }
    }
}

void rope_emitter_clear_clipboard()
{
    for (auto* rope : g_rope_emitter_clipboard) {
        DestroyDedRopeEmitter(rope);
    }
    g_rope_emitter_clipboard.clear();
}

void rope_emitter_handle_delete_or_cut(DedObject* obj)
{
    if (!obj || obj->type != DedObjectType::DED_ROPE_EMITTER) return;
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto& ropes = level->GetAlpineLevelProperties().rope_emitter_objects;
    auto it = std::find(ropes.begin(), ropes.end(), static_cast<DedRopeEmitter*>(obj));
    if (it != ropes.end()) {
        ropes.erase(it);
    }
}

void rope_emitter_handle_delete_selection(CDedLevel* level)
{
    alpine_compact_selection<DedRopeEmitter>(level, DedObjectType::DED_ROPE_EMITTER,
                                             DeleteRopeEmitterObject);
}

void rope_emitter_ensure_uid(int& uid)
{
    auto* level = CDedLevel::Get();
    if (!level) return;
    alpine_ensure_uid(level->GetAlpineLevelProperties().rope_emitter_objects, uid);
}
