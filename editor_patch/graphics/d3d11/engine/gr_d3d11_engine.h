#pragma once

// RED's engine shim for the shared Direct3D 11 core in common/gr_d3d11 (see gr_d3d11_bindings.h). The rf::
// names come from vtypes.h and this folder's rf_*.h; the Alpine runtime the editor does not have yet binds to
// neutral values.

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <source_location>
#include <string>
#include <vector>
#include "../../../vtypes.h"
#include "../../../level.h"
#include "../../../textures.h"
#include "../../../alpine_lightmaps.h"
#include "../../../editor_settings.h"
#include "../gr_d3d11_editor_hooks.h"
#include "../gr_d3d11_editor_lightmaps.h"
#include <common/alpine_dir_light.h>
#include <common/alpine_gas_region.h>
#include <common/bitmap/bm_format.h>
#include <common/lighting/alpine_sun_state.h>
#include <common/lightmap/af_lightmap_runtime.h>
#include <common/terrain/alpine_terrain.h>
#include <common/terrain/alpine_terrain_decorations_runtime.h>
#include <common/terrain/alpine_terrain_runtime.h>
#include <common/utils/color-utils.h>

// ─── The rf:: views against the editor's own declarations of the same engine objects ───

static_assert(sizeof(rf::Vector3) == sizeof(Vector3));
static_assert(offsetof(rf::Vector3, x) == offsetof(Vector3, x) && offsetof(rf::Vector3, z) == offsetof(Vector3, z));
static_assert(sizeof(rf::Matrix3) == sizeof(Matrix3));
static_assert(offsetof(rf::Matrix3, uvec) == offsetof(Matrix3, uvec));
static_assert(offsetof(rf::Matrix3, fvec) == offsetof(Matrix3, fvec));
static_assert(sizeof(rf::Plane) == sizeof(Plane) && offsetof(rf::Plane, offset) == offsetof(Plane, dist));
static_assert(sizeof(rf::Color) == sizeof(Color));
static_assert(offsetof(rf::Color, red) == offsetof(Color, r) && offsetof(rf::Color, green) == offsetof(Color, g));
static_assert(offsetof(rf::Color, blue) == offsetof(Color, b) && offsetof(rf::Color, alpha) == offsetof(Color, a));
static_assert(sizeof(rf::VArray<int>) == sizeof(VArray<int>));
static_assert(offsetof(VArray<int>, size) == 0 && offsetof(VArray<int>, data_ptr) == 8);

static_assert(sizeof(rf::gr::Vertex) == sizeof(GrVertex));
static_assert(offsetof(rf::gr::Vertex, sx) == offsetof(GrVertex, screen_x));
static_assert(offsetof(rf::gr::Vertex, sy) == offsetof(GrVertex, screen_y));
static_assert(offsetof(rf::gr::Vertex, sw) == offsetof(GrVertex, rhw));
static_assert(offsetof(rf::gr::Vertex, u1) == offsetof(GrVertex, u));
static_assert(offsetof(rf::gr::Vertex, v1) == offsetof(GrVertex, v));
static_assert(offsetof(rf::gr::Vertex, r) == offsetof(GrVertex, r) && offsetof(rf::gr::Vertex, a) == offsetof(GrVertex, a));
static_assert(sizeof(rf::gr::Screen) == sizeof(red::GrScreen));
static_assert(offsetof(rf::gr::Screen, offset_x) == offsetof(red::GrScreen, offset_x));
static_assert(offsetof(rf::gr::Screen, offset_y) == offsetof(red::GrScreen, offset_y));
static_assert(offsetof(rf::gr::Screen, clip_width) == offsetof(red::GrScreen, clip_width));
static_assert(offsetof(rf::gr::Screen, clip_height) == offsetof(red::GrScreen, clip_height));
static_assert(offsetof(rf::gr::Screen, clip_left) == offsetof(red::GrScreen, clip_left));
static_assert(offsetof(rf::gr::Screen, clip_top) == offsetof(red::GrScreen, clip_top));
static_assert(offsetof(rf::gr::Screen, current_color) == offsetof(red::GrScreen, current_color));
static_assert(offsetof(rf::gr::Screen, current_texture_1) == offsetof(red::GrScreen, current_bitmap));
static_assert(offsetof(rf::gr::Screen, fog_mode) == offsetof(red::GrScreen, fog_mode));
static_assert(offsetof(rf::gr::Screen, fog_near) == offsetof(red::GrScreen, fog_near));
static_assert(offsetof(rf::gr::Screen, fog_far) == offsetof(red::GrScreen, fog_far));
static_assert(offsetof(rf::gr::Screen, depthbuffer_type) == offsetof(red::GrScreen, zbuffer_mode));
static_assert(sizeof(rf::gr::LockInfo) == sizeof(GrLockInfo));
static_assert(offsetof(rf::gr::LockInfo, format) == offsetof(GrLockInfo, format));
static_assert(offsetof(rf::gr::LockInfo, data) == offsetof(GrLockInfo, data));
static_assert(offsetof(rf::gr::LockInfo, stride_in_bytes) == offsetof(GrLockInfo, stride_in_bytes));
static_assert(offsetof(rf::gr::LockInfo, mode) == offsetof(GrLockInfo, mode));
static_assert(rf::gr::TMAP_FLAG_RGB == tmap_rgb && rf::gr::TMAP_FLAG_ALPHA == tmap_alpha);
static_assert(sizeof(rf::gr::Light) == sizeof(GrLight));
static_assert(offsetof(rf::gr::Light, type) == offsetof(GrLight, type));
static_assert(offsetof(rf::gr::Light, vec) == offsetof(GrLight, vec));
static_assert(offsetof(rf::gr::Light, vec2) == offsetof(GrLight, vec2));
static_assert(offsetof(rf::gr::Light, rad_2) == offsetof(GrLight, rad_2));
static_assert(offsetof(rf::gr::Light, r) == offsetof(GrLight, r) && offsetof(rf::gr::Light, b) == offsetof(GrLight, b));
static_assert(offsetof(rf::gr::Light, shadow_condition) == offsetof(GrLight, shadow_condition));
static_assert(offsetof(rf::gr::Light, local_vec) == offsetof(GrLight, local_vec));
static_assert(offsetof(rf::gr::Light, local_vec2) == offsetof(GrLight, local_vec2));
static_assert(rf::gr::LT_SPOT == ::LT_SPOT && rf::gr::LT_TUBE == ::LT_TUBE);

static_assert(sizeof(rf::bm::BitmapEntry) == sizeof(BitmapEntry));
static_assert(offsetof(rf::bm::BitmapEntry, handle) == offsetof(BitmapEntry, handle));
static_assert(offsetof(rf::bm::BitmapEntry, bm_type) == offsetof(BitmapEntry, bm_type));
static_assert(offsetof(rf::bm::BitmapEntry, format) == offsetof(BitmapEntry, format));
static_assert(offsetof(rf::bm::BitmapEntry, num_frames) == offsetof(BitmapEntry, num_frames));
static_assert(offsetof(rf::bm::BitmapEntry, dynamic) == offsetof(BitmapEntry, pad_5E));
static_assert(rf::bm::TYPE_USER == BitmapEntry::TYPE_USER);
static_assert(rf::bm::FORMAT_8_PALETTED == BitmapEntry::FORMAT_8_PALETTED);
static_assert(rf::bm::FORMAT_565_RGB == BitmapEntry::FORMAT_565_RGB);
static_assert(rf::bm::FORMAT_4444_ARGB == BitmapEntry::FORMAT_4444_ARGB);
static_assert(rf::bm::FORMAT_1555_ARGB == BitmapEntry::FORMAT_1555_ARGB);
static_assert(rf::bm::FORMAT_888_RGB == BitmapEntry::FORMAT_888_RGB);
static_assert(rf::bm::FORMAT_8888_ARGB == BitmapEntry::FORMAT_8888_ARGB);

static_assert(offsetof(rf::GSolid, face_list) == offsetof(GSolid, face_list_head));
static_assert(offsetof(rf::GSolid, all_rooms) == offsetof(GSolid, all_rooms));
static_assert(offsetof(rf::GSolid, surfaces) == offsetof(GSolid, surfaces));
static_assert(offsetof(rf::GSolid, texture_movers) == offsetof(GSolid, texture_movers));
static_assert(sizeof(rf::GRoom) == sizeof(GRoom));
static_assert(offsetof(rf::GRoom, is_sky) == offsetof(GRoom, is_sky));
static_assert(offsetof(rf::GRoom, geo_cache) == offsetof(GRoom, geo_cache));
static_assert(offsetof(rf::GRoom, bbox_min) == offsetof(GRoom, bbox_min));
static_assert(offsetof(rf::GRoom, bbox_max) == offsetof(GRoom, bbox_max));
static_assert(offsetof(rf::GRoom, room_index) == offsetof(GRoom, room_index));
static_assert(offsetof(rf::GRoom, uid) == offsetof(GRoom, uid));
static_assert(offsetof(rf::GRoom, face_list) == offsetof(GRoom, face_list_head));
static_assert(offsetof(rf::GRoom, detail_rooms) == offsetof(GRoom, detail_rooms));
static_assert(offsetof(rf::GRoom, room_to_render_with) == offsetof(GRoom, room_to_render_with));
static_assert(offsetof(rf::GRoom, decals) == offsetof(GRoom, _decals_and_runtime));
static_assert(offsetof(rf::GRoom, liquid_type) == offsetof(GRoom, liquid_type));
static_assert(offsetof(rf::GRoom, contains_liquid) == offsetof(GRoom, contains_liquid));
static_assert(offsetof(rf::GRoom, liquid_depth) == offsetof(GRoom, liquid_depth));
static_assert(offsetof(rf::GRoom, liquid_color) == offsetof(GRoom, liquid_color));
static_assert(offsetof(rf::GRoom, liquid_visibility) == offsetof(GRoom, liquid_visibility));
static_assert(offsetof(rf::GRoom, liquid_alpha) == offsetof(GRoom, liquid_alpha));
static_assert(sizeof(rf::GFaceAttributes) == sizeof(GFaceAttributes));
static_assert(offsetof(rf::GFaceAttributes, group_id) == offsetof(GFaceAttributes, group_id));
static_assert(offsetof(rf::GFaceAttributes, bitmap_id) == offsetof(GFaceAttributes, bitmap_id));
static_assert(offsetof(rf::GFaceAttributes, portal_id) == offsetof(GFaceAttributes, portal_id));
static_assert(offsetof(rf::GFaceAttributes, surface_index) == offsetof(GFaceAttributes, surface_index));
static_assert(sizeof(rf::GFace) == sizeof(GFace));
static_assert(offsetof(rf::GFace, plane) == offsetof(GFace, plane));
static_assert(offsetof(rf::GFace, attributes) == offsetof(GFace, flags));
static_assert(offsetof(rf::GFace, attributes) + offsetof(rf::GFaceAttributes, bitmap_id) == offsetof(GFace, bitmap_id));
static_assert(offsetof(rf::GFace, attributes) + offsetof(rf::GFaceAttributes, surface_index)
              == offsetof(GFace, surface_index));
static_assert(offsetof(rf::GFace, edge_loop) == offsetof(GFace, edge_loop));
static_assert(offsetof(rf::GFace, which_room) == offsetof(GFace, which_room));
static_assert(offsetof(rf::GFace, decal_list) == offsetof(GFace, decal_list));
static_assert(offsetof(rf::GFace, next) == offsetof(GFace, next_solid));
static_assert(offsetof(rf::GFace, next) + sizeof(rf::GFace*) * rf::FACE_LIST_BBOX == offsetof(GFace, next_bbox));
static_assert(offsetof(rf::GFace, next) + sizeof(rf::GFace*) * rf::FACE_LIST_ROOM == offsetof(GFace, next_room));
static_assert(rf::FACE_SHOW_SKY == ::FACE_SHOW_SKY && rf::FACE_LIQUID == ::FACE_LIQUID);
static_assert(rf::FACE_SEE_THRU == ::FACE_SEE_THRU && rf::FACE_INVISIBLE == ::FACE_INVISIBLE);
static_assert(sizeof(rf::GFaceVertex) == sizeof(GFaceVertex));
static_assert(offsetof(rf::GFaceVertex, texture_u) == offsetof(GFaceVertex, u));
static_assert(offsetof(rf::GFaceVertex, texture_v) == offsetof(GFaceVertex, v));
static_assert(offsetof(rf::GFaceVertex, lightmap_u) == offsetof(GFaceVertex, lm_u));
static_assert(offsetof(rf::GFaceVertex, lightmap_v) == offsetof(GFaceVertex, lm_v));
static_assert(offsetof(rf::GFaceVertex, next) == offsetof(GFaceVertex, next));
static_assert(sizeof(rf::GVertex) == sizeof(GVertex));
static_assert(offsetof(rf::GVertex, rotated_pos) == offsetof(GVertex, rotated_pos));
static_assert(offsetof(rf::GVertex, last_frame) == offsetof(GVertex, last_frame));
static_assert(offsetof(rf::GVertex, clip_codes) == offsetof(GVertex, clip_codes));
static_assert(offsetof(rf::GVertex, adjacent_faces) == offsetof(GVertex, face_refs));
static_assert(offsetof(rf::GSurface, lightmap) == offsetof(GSurface, lightmap));
static_assert(offsetof(rf::GSurface, xstart) == offsetof(GSurface, xstart));
static_assert(offsetof(rf::GSurface, ystart) == offsetof(GSurface, ystart));
static_assert(offsetof(rf::GSurface, width) == offsetof(GSurface, width));
static_assert(offsetof(rf::GSurface, height) == offsetof(GSurface, height));
static_assert(offsetof(rf::GSurface, uv_scale) == offsetof(GSurface, uv_scale_x));
static_assert(offsetof(rf::GSurface, uv_add) == offsetof(GSurface, uv_add_x));
static_assert(offsetof(rf::GSurface, u_coefficient) == offsetof(GSurface, u_coefficient));
static_assert(offsetof(rf::GSurface, v_coefficient) == offsetof(GSurface, v_coefficient));
static_assert(offsetof(rf::GLightmap, w) == offsetof(GLightmap, w) && offsetof(rf::GLightmap, h) == offsetof(GLightmap, h));
static_assert(sizeof(rf::GLightmap) == sizeof(GLightmap));
static_assert(offsetof(rf::GLightmap, bm_handle) == offsetof(GLightmap, bm_handle));
static_assert(sizeof(rf::GTextureMover) == sizeof(GTextureMover));
static_assert(offsetof(rf::GTextureMover, u_pan_speed) == offsetof(GTextureMover, u_pan_speed));
static_assert(offsetof(rf::GTextureMover, v_pan_speed) == offsetof(GTextureMover, v_pan_speed));
static_assert(offsetof(rf::GTextureMover, faces) == offsetof(GTextureMover, faces));

static_assert(sizeof(rf::VMesh) == sizeof(EditorVMesh));
static_assert(offsetof(rf::VMesh, instance) == offsetof(EditorVMesh, instance));
static_assert(sizeof(rf::V3d) == sizeof(EditorV3d));
static_assert(offsetof(rf::V3d, num_meshes) == offsetof(EditorV3d, num_meshes));
static_assert(offsetof(rf::V3d, meshes) == offsetof(EditorV3d, meshes));
static_assert(sizeof(rf::V3dMesh) == sizeof(EditorV3dMesh));
static_assert(offsetof(rf::V3dMesh, materials) == offsetof(EditorV3dMesh, materials));
static_assert(offsetof(rf::V3dMesh, vu) == offsetof(EditorV3dMesh, lod_mesh));
// render_cache is the game's field past the stock structure
static_assert(offsetof(rf::VifLodMesh, render_cache) == sizeof(EditorVifLodMesh));
static_assert(offsetof(rf::VifLodMesh, meshes) == offsetof(EditorVifLodMesh, meshes));
static_assert(offsetof(rf::VifLodMesh, distances) == offsetof(EditorVifLodMesh, distances));
static_assert(offsetof(rf::VifMesh, render_cache) == sizeof(EditorVifMesh));
static_assert(offsetof(rf::VifMesh, chunks) == offsetof(EditorVifMesh, chunks));
static_assert(offsetof(rf::VifMesh, num_chunks) == offsetof(EditorVifMesh, num_chunks));
static_assert(offsetof(rf::VifMesh, tex_ids) == offsetof(EditorVifMesh, tex_ids));
static_assert(offsetof(rf::VifMesh, tex_handles) == offsetof(EditorVifMesh, tex_handles));
static_assert(offsetof(rf::VifMesh, num_textures_handles) == offsetof(EditorVifMesh, num_texture_handles));
static_assert(offsetof(rf::VifMesh, num_original_vecs) == offsetof(EditorVifMesh, num_original_vecs));
static_assert(sizeof(rf::VifChunk) == sizeof(EditorVifChunk));
static_assert(offsetof(rf::VifChunk, vecs) == offsetof(EditorVifChunk, vecs));
static_assert(offsetof(rf::VifChunk, norms) == offsetof(EditorVifChunk, norms));
static_assert(offsetof(rf::VifChunk, uvs) == offsetof(EditorVifChunk, uvs));
static_assert(offsetof(rf::VifChunk, faces) == offsetof(EditorVifChunk, faces));
static_assert(offsetof(rf::VifChunk, same_vertex_offsets) == offsetof(EditorVifChunk, same_vertex_offsets));
static_assert(offsetof(rf::VifChunk, wi) == offsetof(EditorVifChunk, wi));
static_assert(offsetof(rf::VifChunk, texture_idx) == offsetof(EditorVifChunk, texture_idx));
static_assert(offsetof(rf::VifChunk, orig_map) == offsetof(EditorVifChunk, orig_map));
static_assert(offsetof(rf::VifChunk, num_vecs) == offsetof(EditorVifChunk, num_vecs));
static_assert(offsetof(rf::VifChunk, num_faces) == offsetof(EditorVifChunk, num_faces));
static_assert(sizeof(rf::VifFace) == sizeof(EditorVifFace) && offsetof(rf::VifFace, flags) == offsetof(EditorVifFace, flags));
static_assert(sizeof(rf::TexMap) == sizeof(EditorTextureMap));
static_assert(sizeof(rf::MeshMaterial) == sizeof(EditorMeshMaterial));
static_assert(offsetof(rf::MeshMaterial, use_additive_blending) == offsetof(EditorMeshMaterial, use_additive_blending));
static_assert(offsetof(rf::MeshMaterial, diffuse_color) == offsetof(EditorMeshMaterial, diffuse_color));
static_assert(offsetof(rf::MeshMaterial, texture_maps) == offsetof(EditorMeshMaterial, texture_maps));
static_assert(offsetof(rf::MeshMaterial, specular_level) == offsetof(EditorMeshMaterial, specular_level));
static_assert(offsetof(rf::MeshMaterial, reflection_amount) == offsetof(EditorMeshMaterial, reflection_amount));
static_assert(offsetof(rf::MeshMaterial, refl_tex_handle) == offsetof(EditorMeshMaterial, refl_tex_handle));
static_assert(offsetof(rf::MeshMaterial, self_illumination) == offsetof(EditorMeshMaterial, self_illumination));
static_assert(sizeof(rf::MeshRenderParams) == sizeof(EditorRenderParams));
static_assert(offsetof(rf::MeshRenderParams, alpha) == offsetof(EditorRenderParams, field_08));
static_assert(offsetof(rf::MeshRenderParams, self_illum) == offsetof(EditorRenderParams, diffuse_color));
static_assert(offsetof(rf::MeshRenderParams, alt_tex) == offsetof(EditorRenderParams, field_10));
static_assert(offsetof(rf::MeshRenderParams, vertex_colors) == offsetof(EditorRenderParams, field_1C));
static_assert(offsetof(rf::MeshRenderParams, powerup_bitmaps) == offsetof(EditorRenderParams, field_20));
static_assert(offsetof(rf::MeshRenderParams, ambient_color) == offsetof(EditorRenderParams, ambient_color));
static_assert(rf::MRF_CUSTOM_AMBIENT_COLOR == ERF_CUSTOM_AMBIENT);
static_assert(sizeof(rf::Character) == sizeof(EditorCharacter));
static_assert(offsetof(rf::Character, num_bones) == offsetof(EditorCharacter, num_bones));
static_assert(offsetof(rf::Character, num_anims) == offsetof(EditorCharacter, num_actions));
static_assert(offsetof(rf::Character, animations) == offsetof(EditorCharacter, actions));
static_assert(offsetof(rf::Character, anim_is_state) == offsetof(EditorCharacter, action_is_state));
static_assert(offsetof(rf::Character, character_meshes) == offsetof(EditorCharacter, character_meshes));
static_assert(sizeof(rf::CharacterInstance) == sizeof(EditorCharacterInstance));
static_assert(offsetof(rf::CharacterInstance, holding_last_action_frame) == offsetof(EditorCharacterInstance, action_held));
static_assert(sizeof(rf::Skeleton) == sizeof(EditorAnimSkeleton));
static_assert(sizeof(rf::VfxSfxoChunk) == sizeof(EditorVfxChunk));
static_assert(offsetof(rf::VfxSfxoChunk, radius) == offsetof(EditorVfxChunk, radius));
static_assert(offsetof(rf::VfxSfxoChunk, num_vertices) == offsetof(EditorVfxChunk, num_vertices));
static_assert(offsetof(rf::VfxSfxoChunk, ribbon_width) == offsetof(EditorVfxChunk, ribbon_width));
static_assert(sizeof(rf::VfxSfxoRenderObj) == sizeof(EditorVfxPart));
static_assert(offsetof(rf::VfxSfxoRenderObj, pivot) == offsetof(EditorVfxPart, pivot));
static_assert(offsetof(rf::VfxSfxoRenderObj, width) == offsetof(EditorVfxPart, width));
static_assert(offsetof(rf::VfxSfxoRenderObj, height) == offsetof(EditorVfxPart, height));
static_assert(offsetof(rf::VfxSfxoRenderObj, vertex_positions) == offsetof(EditorVfxPart, vertex_positions));
static_assert(offsetof(rf::ParticleEmitter, min_spawn_delay) == offsetof(EditorParticleEmitter, min_spawn_delay));
static_assert(offsetof(rf::ParticleEmitter, max_spawn_delay) == offsetof(EditorParticleEmitter, max_spawn_delay));
static_assert(offsetof(rf::ParticleEmitter, emitter_flags) == offsetof(EditorParticleEmitter, emitter_flags));
static_assert(offsetof(rf::ParticleEmitter, spawn_timer) == offsetof(EditorParticleEmitter, spawn_timer));
static_assert(offsetof(rf::Particle, vel) == offsetof(EditorParticle, velocity));

namespace timer
{
    // Elapsed time since the first call, in 1/scale seconds
    inline int64_t get_i64(const int scale)
    {
        static const int64_t freq = [] {
            LARGE_INTEGER value{};
            QueryPerformanceFrequency(&value);
            return value.QuadPart;
        }();
        static const int64_t base = [] {
            LARGE_INTEGER value{};
            QueryPerformanceCounter(&value);
            return value.QuadPart;
        }();
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const int64_t elapsed = now.QuadPart - base;
        return (elapsed / freq) * scale + (elapsed % freq) * scale / freq;
    }
}

#include <common/gr_d3d11/gr_d3d11_bindings.h>

namespace gr::d3d11::engine
{
    inline int colorblind_mode()
    {
        return 0;
    }

    // The game's default (AlpineGameSettings::dynamic_light_ndotl)
    inline float dynamic_light_ndotl()
    {
        return 1.0f;
    }

    inline int picmip()
    {
        return 1;
    }

    inline int underwater_fx()
    {
        return 0;
    }

    inline bool nearest_texture_filtering()
    {
        return false;
    }

    inline bool anisotropic_filtering()
    {
        return true;
    }

    inline uint32_t sample_count()
    {
        return 1;
    }

    inline bool precache_rooms()
    {
        return false;
    }

    inline std::optional<uint32_t> thermal_entity_color_override()
    {
        return std::nullopt;
    }

    // The open level's compiled geometry
    inline rf::GSolid* level_solid()
    {
        CDedLevel* const level = CDedLevel::Get();
        return level ? reinterpret_cast<rf::GSolid*>(level->solid) : nullptr;
    }

    // RED holds every level in the current format
    inline bool rfl_version_minimum([[maybe_unused]] int version)
    {
        return true;
    }

    inline rf::Color level_ambient_light()
    {
        const CDedLevel* const level = CDedLevel::Get();
        if (!level) {
            return {0, 0, 0, 255};
        }
        const ::Color& c = level->ambient_color;
        return {c.r, c.g, c.b, c.a};
    }

    // The game's single-player value
    inline float static_mesh_light_scale()
    {
        CDedLevel* const level = CDedLevel::Get();
        if (level) {
            const AlpineLevelProperties& level_props = level->GetAlpineLevelProperties();
            if (level_props.override_static_mesh_ambient_light_modifier) {
                return level_props.static_mesh_ambient_light_modifier;
            }
        }
        return 2.0f;
    }

    inline bool is_sky_fix_level()
    {
        return false;
    }

    // The game's: the level fog's near clip from RFL 306 on. RED zeroes an older level's when it loads it.
    inline float fog_near_clip()
    {
        return rf::gr::screen.fog_near;
    }

    inline float level_default_far_clip()
    {
        return gr_far_clip_dist;
    }

    inline float level_dry_far_clip()
    {
        return gr_far_clip_dist;
    }

    inline DistanceFog level_distance_fog()
    {
        return {rf::Color{0, 0, 0, 0}, 0.0f, 0.0f};
    }

    inline rf::GRoom* camera_room()
    {
        return nullptr;
    }

    inline rf::Vector3 camera_pos()
    {
        return {};
    }

    inline rf::MoverBrush* mover_brush_list()
    {
        return nullptr;
    }

    inline SunLightState sun_state()
    {
        return {};
    }

    inline const std::vector<GasRegionInfo>& gas_regions()
    {
        static const std::vector<GasRegionInfo> none;
        return none;
    }

    inline const std::vector<AlpineDirLight>& dir_lights()
    {
        static const std::vector<AlpineDirLight> none;
        return none;
    }

    inline uint32_t dir_light_generation()
    {
        return 0;
    }

    inline AfLightmapConstants af_lightmap_constants()
    {
        return editor_af_lightmap_constants();
    }

    inline int af_lightmap_synthesized_page_bm()
    {
        return lightmap_synthesized_page_bm();
    }

    inline bool af_lightmap_face_setup(rf::GSolid* solid, int surface_index, AfLightmapFace& out)
    {
        return editor_af_lightmap_face_setup(solid, surface_index, out);
    }

    inline void af_lightmap_face_texel(const AfLightmapFace& face, const rf::Vector3& pos, float& out_u, float& out_v)
    {
        editor_af_lightmap_face_texel(face, pos, out_u, out_v);
    }

    inline bool af_lightmap_overflow_face(const rf::GSolid* solid, const rf::GFace* face, AfOverflowFace& out)
    {
        return editor_af_lightmap_overflow_face(solid, face, out);
    }

    inline bool af_lightmap_overflow_texel(const AfOverflowFace& face, const rf::Vector3& pos, float& out_u,
                                           float& out_v)
    {
        return editor_af_lightmap_overflow_texel(face, pos, out_u, out_v);
    }

    inline bool af_lightmap_terrain_chart([[maybe_unused]] int terrain_index, [[maybe_unused]] AfTerrainChart& out)
    {
        return false;
    }

    inline const std::vector<AlpineTerrain>& terrains()
    {
        static const std::vector<AlpineTerrain> none;
        return none;
    }

    inline const AlpineTerrainRoomRef* terrain_find_room([[maybe_unused]] const rf::GRoom* room)
    {
        return nullptr;
    }

    inline bool terrain_is_separate_chunk([[maybe_unused]] const rf::GRoom* parent,
                                          [[maybe_unused]] const rf::GRoom* detail_room)
    {
        return false;
    }

    inline alpine_terrain::GridView terrain_grid(const AlpineTerrain& terrain)
    {
        return alpine_terrain::make_grid_view(terrain.header, terrain.heights.data(),
                                              terrain.weights.empty() ? nullptr : terrain.weights.data(),
                                              terrain.holes.data(), terrain.diag.data(), terrain.layers.data());
    }

    inline alpine_terrain::FaceKind terrain_face_kind(const alpine_terrain::GridView& grid, const rf::GFace& face)
    {
        return alpine_terrain::face_kind(grid, [&](auto&& visit) {
            int n = 0;
            for (const rf::GFaceVertex* fv = face.edge_loop; fv && fv->vertex && n < rf::max_face_vertices; n++) {
                visit(fv->vertex->pos.x, fv->vertex->pos.y, fv->vertex->pos.z);
                fv = fv->next;
                if (fv == face.edge_loop) {
                    break;
                }
            }
        });
    }

    inline bool decorations_active()
    {
        return false;
    }

    inline std::vector<TerrainDecorations>& decorations()
    {
        static std::vector<TerrainDecorations> none;
        return none;
    }

    inline const DecorationChunk* decoration_chunk([[maybe_unused]] const AlpineTerrainRoomRef& ref)
    {
        return nullptr;
    }

    inline const DecorationMesh& decoration_mesh([[maybe_unused]] int slot)
    {
        static const DecorationMesh none{nullptr, 0.0f};
        return none;
    }

    inline DecorationFrameStats& decoration_frame_stats()
    {
        static DecorationFrameStats stats;
        return stats;
    }

    // View > Fullbright Meshes
    inline bool character_meshes_fullbright()
    {
        return g_editor_settings.fullbright_meshes;
    }

    inline bool static_meshes_fullbright()
    {
        return g_editor_settings.fullbright_meshes;
    }

    inline void mesh_blend_ambient(const float (&lightmap)[3], float (&out)[3])
    {
        out[0] = lightmap[0];
        out[1] = lightmap[1];
        out[2] = lightmap[2];
    }

    inline float mesh_sun_scale([[maybe_unused]] const float* ambient)
    {
        return 0.0f;
    }

    inline bool is_monitor_screen_bitmap([[maybe_unused]] int bm_handle)
    {
        return false;
    }

    inline bool vehicle_tread_scroll_for_draw([[maybe_unused]] int& config_out, [[maybe_unused]] float& u_out,
                                              [[maybe_unused]] float& v_out)
    {
        return false;
    }

    inline bool vehicle_is_tread_bitmap([[maybe_unused]] int bm_handle, [[maybe_unused]] int config_index)
    {
        return false;
    }

    inline void vehicle_tread_resolve_mesh_bitmaps([[maybe_unused]] int config_index,
                                                   [[maybe_unused]] const void* mesh_key,
                                                   [[maybe_unused]] const int* tex_handles,
                                                   [[maybe_unused]] int num_tex_handles)
    {
    }

    inline int live_feed_bitmap([[maybe_unused]] int bm_handle)
    {
        return -1;
    }

    inline bool bitmap_user_mipmap([[maybe_unused]] int bm_handle)
    {
        return false;
    }

    inline bool read_file(const char* filename, std::vector<std::byte>& out)
    {
        // RED's File::open only looks a file up; open_mode opens the stream
        rf::File file;
        if (file.open_mode(filename) != 0) {
            return false;
        }
        const int size = file.get_size();
        out.resize(static_cast<std::size_t>(size > 0 ? size : 0));
        const int bytes_read = size > 0 ? file.read(out.data(), out.size()) : 0;
        file.close();
        return bytes_read == size;
    }

    [[noreturn]] inline void fatal_error(const std::string& text, const std::source_location& loc)
    {
        editor_d3d11_fatal_error(text, loc);
    }
}
