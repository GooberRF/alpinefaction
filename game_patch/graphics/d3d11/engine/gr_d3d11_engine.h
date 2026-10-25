#pragma once

// The game's engine shim for the shared Direct3D 11 core in common/gr_d3d11 (see gr_d3d11_bindings.h)

#include <cassert>
#include <patch_common/MemUtils.h>
#include "../../../rf/bmpman.h"
#include "../../../rf/character.h"
#include "../../../rf/file/file.h"
#include "../../../rf/geometry.h"
#include "../../../rf/gr/gr.h"
#include "../../../rf/gr/gr_light.h"
#include "../../../rf/level.h"
#include "../../../rf/math/matrix.h"
#include "../../../rf/math/quaternion.h"
#include "../../../rf/math/vector.h"
#include "../../../rf/misc.h"
#include "../../../rf/mover.h"
#include "../../../rf/multi.h"
#include "../../../rf/os/frametime.h"
#include "../../../rf/os/os.h"
#include "../../../rf/particle_emitter.h"
#include "../../../rf/player/camera.h"
#include "../../../rf/player/player.h"
#include "../../../rf/v3d.h"
#include "../../../rf/vfx.h"
#include "../../../rf/vmesh.h"
#include "../../../bmpman/atx.h"
#include "../../../bmpman/bmpman.h"
#include "../../../main/main.h"
#include "../../../misc/alpine_options.h"
#include "../../../misc/alpine_settings.h"
#include "../../../misc/alpine_terrain.h"
#include "../../../misc/alpine_terrain_decorations.h"
#include "../../../misc/level.h"
#include "../../../misc/misc.h"
#include "../../../multi/vehicles/vehicle_render.h"
#include "../../../object/alpine_dir_light.h"
#include "../../../object/object.h"
#include "../../../os/os.h"
#include "../../af_lightmap.h"
#include "../../gr.h"

#include <common/gr_d3d11/gr_d3d11_bindings.h>

namespace gr::d3d11::engine
{
    inline int colorblind_mode()
    {
        return g_alpine_game_config.colorblind_mode;
    }

    inline float dynamic_light_ndotl()
    {
        return g_alpine_game_config.dynamic_light_ndotl;
    }

    inline int picmip()
    {
        return g_alpine_game_config.picmip;
    }

    inline int underwater_fx()
    {
        return g_alpine_game_config.underwater_fx;
    }

    inline bool nearest_texture_filtering()
    {
        return g_alpine_game_config.nearest_texture_filtering;
    }

    inline bool anisotropic_filtering()
    {
        return g_game_config.anisotropic_filtering;
    }

    inline uint32_t sample_count()
    {
        return g_alpine_game_config.sample_count;
    }

    inline bool precache_rooms()
    {
        return g_alpine_game_config.precache_rooms;
    }

    inline std::optional<uint32_t> thermal_entity_color_override()
    {
        return g_alpine_game_config.thermal_entity_color_override;
    }

    inline rf::GSolid* level_solid()
    {
        return rf::level.geometry;
    }

    inline bool rfl_version_minimum(int version)
    {
        return ::rfl_version_minimum(version);
    }

    inline rf::Color level_ambient_light()
    {
        return rf::level.ambient_light;
    }

    inline float static_mesh_light_scale()
    {
        const auto& level_props = AlpineLevelProperties::instance();
        if (level_props.override_static_mesh_ambient_light_modifier) {
            return level_props.static_mesh_ambient_light_modifier;
        }
        return rf::is_multi ? 3.2f : 2.0f;
    }

    inline bool is_sky_fix_level()
    {
        return ::is_sky_fix_level(rf::level.filename);
    }

    inline float fog_near_clip()
    {
        return gr_fog_near_clip();
    }

    inline float level_default_far_clip()
    {
        return ::level_default_far_clip();
    }

    inline float level_dry_far_clip()
    {
        return ::level_dry_far_clip();
    }

    inline DistanceFog level_distance_fog()
    {
        if (rf::level.distance_fog_far_clip > 0.0f) {
            return {
                rf::level.distance_fog_color,
                rfl_version_minimum(306) ? rf::level.distance_fog_near_clip : 0.0f,
                rf::level.distance_fog_far_clip,
            };
        }
        return {rf::Color{0, 0, 0, 0}, 0.0f, 0.0f};
    }

    inline rf::GRoom* camera_room()
    {
        rf::Camera* cam = rf::local_player ? rf::local_player->cam : nullptr;
        return cam && cam->camera_entity ? rf::camera_get_room(cam) : nullptr;
    }

    inline rf::Vector3 camera_pos()
    {
        rf::Camera* cam = rf::local_player ? rf::local_player->cam : nullptr;
        return cam ? rf::camera_get_pos(cam) : rf::Vector3{};
    }

    inline rf::MoverBrush* mover_brush_list()
    {
        return &rf::mover_brush_list;
    }

    inline SunLightState sun_state()
    {
        return gr_get_sun_state();
    }

    inline const std::vector<GasRegionInfo>& gas_regions()
    {
        return gas_region_get_all();
    }

    inline const std::vector<AlpineDirLight>& dir_lights()
    {
        return alpine_dir_light_get_all();
    }

    inline uint32_t dir_light_generation()
    {
        return alpine_dir_light_generation();
    }

    inline AfLightmapConstants af_lightmap_constants()
    {
        return ::af_lightmap_constants();
    }

    inline int af_lightmap_synthesized_page_bm()
    {
        return ::af_lightmap_synthesized_page_bm();
    }

    inline bool af_lightmap_face_setup(rf::GSolid* solid, int surface_index, AfLightmapFace& out)
    {
        return ::af_lightmap_face_setup(solid, surface_index, out);
    }

    inline void af_lightmap_face_texel(const AfLightmapFace& face, const rf::Vector3& pos, float& out_u, float& out_v)
    {
        ::af_lightmap_face_texel(face, pos, out_u, out_v);
    }

    inline bool af_lightmap_overflow_face(const rf::GSolid* solid, const rf::GFace* face, AfOverflowFace& out)
    {
        return ::af_lightmap_overflow_face(solid, face, out);
    }

    inline bool af_lightmap_overflow_texel(const AfOverflowFace& face, const rf::Vector3& pos, float& out_u,
                                           float& out_v)
    {
        return ::af_lightmap_overflow_texel(face, pos, out_u, out_v);
    }

    inline bool af_lightmap_terrain_chart(int terrain_index, AfTerrainChart& out)
    {
        return ::af_lightmap_terrain_chart(terrain_index, out);
    }

    inline const std::vector<AlpineTerrain>& terrains()
    {
        return alpine_terrain_get_all();
    }

    inline const AlpineTerrainRoomRef* terrain_find_room(const rf::GRoom* room)
    {
        return alpine_terrain_find_room(room);
    }

    inline bool terrain_is_separate_chunk(const rf::GRoom* parent, const rf::GRoom* detail_room)
    {
        return alpine_terrain_is_separate_chunk(parent, detail_room);
    }

    inline alpine_terrain::GridView terrain_grid(const AlpineTerrain& terrain)
    {
        return alpine_terrain_grid(terrain);
    }

    inline alpine_terrain::FaceKind terrain_face_kind(const alpine_terrain::GridView& grid, const rf::GFace& face)
    {
        return alpine_terrain_face_kind(grid, face);
    }

    inline bool decorations_active()
    {
        return alpine_terrain_decorations_active();
    }

    inline std::vector<TerrainDecorations>& decorations()
    {
        return alpine_terrain_decorations_get_all();
    }

    inline const DecorationChunk* decoration_chunk(const AlpineTerrainRoomRef& ref)
    {
        return alpine_terrain_decorations_chunk(ref);
    }

    inline const DecorationMesh& decoration_mesh(int slot)
    {
        return alpine_terrain_decorations_mesh(slot);
    }

    inline DecorationFrameStats& decoration_frame_stats()
    {
        return alpine_terrain_decorations_frame_stats();
    }

    inline bool character_meshes_fullbright()
    {
        return g_character_meshes_are_fullbright;
    }

    inline bool static_meshes_fullbright()
    {
        return false;
    }

    inline void mesh_blend_ambient(const float (&lightmap)[3], float (&out)[3])
    {
        gr_mesh_blend_ambient(lightmap, out);
    }

    inline float mesh_sun_scale(const float* ambient)
    {
        return gr_sun_get_mesh_scale(ambient);
    }

    inline bool is_monitor_screen_bitmap(int bm_handle)
    {
        return ::is_monitor_screen_bitmap(bm_handle);
    }

    inline bool vehicle_tread_scroll_for_draw(int& config_out, float& u_out, float& v_out)
    {
        return ::vehicle_tread_scroll_for_draw(config_out, u_out, v_out);
    }

    inline bool vehicle_is_tread_bitmap(int bm_handle, int config_index)
    {
        return ::vehicle_is_tread_bitmap(bm_handle, config_index);
    }

    inline void vehicle_tread_resolve_mesh_bitmaps(int config_index, const void* mesh_key, const int* tex_handles,
                                                   int num_tex_handles)
    {
        ::vehicle_tread_resolve_mesh_bitmaps(config_index, mesh_key, tex_handles, num_tex_handles);
    }

    inline int live_feed_bitmap(int bm_handle)
    {
        return atx_lookup_live_feed(bm_handle);
    }

    inline bool bitmap_user_mipmap(int bm_handle)
    {
        return bm_is_user_mipmap(bm_handle);
    }

    inline bool read_file(const char* filename, std::vector<std::byte>& out)
    {
        rf::File file;
        if (file.open(filename) != 0) {
            return false;
        }
        const int size = file.size();
        out.resize(static_cast<std::size_t>(size));
        [[maybe_unused]] int bytes_read = file.read(out.data(), size);
        assert(bytes_read == size);
        return true;
    }

    [[noreturn]] inline void fatal_error(const std::string& text, const std::source_location& loc)
    {
        rf::fatal_error(text, loc);
    }
}
