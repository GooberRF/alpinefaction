#pragma once

// What the shared Direct3D 11 core needs from the binary it is built into. Each binary's
// <gr_d3d11_engine.h> includes its engine and Alpine declarations, then this header, then an inline
// definition of every function below. Core files include <gr_d3d11_engine.h>, never this header.
//
// The core also uses, by name, the engine declarations in namespace rf (gr, bm, geometry, vmesh,
// vfx, particles, lights) and these Alpine types and helpers, which the shim provides:
//   AlpineTerrain, AlpineTerrainRoomRef, alpine_terrain::*, alpine_dir_light::*, TerrainDecorations, DecorationChunk,
//   DecorationMesh, DecorationFrameStats, GpuDecorationInstance, AlpineDirLight, GasRegionInfo,
//   SunLightState, AfLightmapFace, AfOverflowFace, AfLightmapConstants, AfTerrainChart,
//   the bm_* format helpers, extract_color_components, timer::get_i64.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <source_location>
#include <string>
#include <vector>

namespace gr::d3d11::engine
{
    // Settings
    int colorblind_mode();
    float dynamic_light_ndotl();
    int picmip();
    int underwater_fx();
    bool nearest_texture_filtering();
    bool anisotropic_filtering();
    uint32_t sample_count();
    bool precache_rooms();
    std::optional<uint32_t> thermal_entity_color_override();

    // Level
    rf::GSolid* level_solid();
    bool rfl_version_minimum(int version);
    rf::Color level_ambient_light();
    // The light scale a static mesh's custom ambient gets
    float static_mesh_light_scale();
    bool is_sky_fix_level();
    float fog_near_clip();
    float level_default_far_clip();
    float level_dry_far_clip();

    struct DistanceFog
    {
        rf::Color color;
        float near_clip;
        // 0 when the level has no distance fog
        float far_clip;
    };
    DistanceFog level_distance_fog();

    // The room the view camera is in, null with no camera
    rf::GRoom* camera_room();
    // Zero with no camera
    rf::Vector3 camera_pos();

    // Null when the binary has no mover brushes
    rf::MoverBrush* mover_brush_list();

    SunLightState sun_state();
    const std::vector<GasRegionInfo>& gas_regions();
    const std::vector<AlpineDirLight>& dir_lights();
    uint32_t dir_light_generation();

    // Alpine lightmap atlas charts
    AfLightmapConstants af_lightmap_constants();
    int af_lightmap_synthesized_page_bm();
    bool af_lightmap_face_setup(rf::GSolid* solid, int surface_index, AfLightmapFace& out);
    void af_lightmap_face_texel(const AfLightmapFace& face, const rf::Vector3& pos, float& out_u, float& out_v);
    bool af_lightmap_overflow_face(const rf::GSolid* solid, const rf::GFace* face, AfOverflowFace& out);
    bool af_lightmap_overflow_texel(const AfOverflowFace& face, const rf::Vector3& pos, float& out_u, float& out_v);
    bool af_lightmap_terrain_chart(int terrain_index, AfTerrainChart& out);

    // Terrain and its decorations
    const std::vector<AlpineTerrain>& terrains();
    const AlpineTerrainRoomRef* terrain_find_room(const rf::GRoom* room);
    bool terrain_is_separate_chunk(const rf::GRoom* parent, const rf::GRoom* detail_room);
    alpine_terrain::GridView terrain_grid(const AlpineTerrain& terrain);
    alpine_terrain::FaceKind terrain_face_kind(const alpine_terrain::GridView& grid, const rf::GFace& face);
    bool decorations_active();
    std::vector<TerrainDecorations>& decorations();
    const DecorationChunk* decoration_chunk(const AlpineTerrainRoomRef& ref);
    const DecorationMesh& decoration_mesh(int slot);
    DecorationFrameStats& decoration_frame_stats();

    // Meshes
    bool character_meshes_fullbright();
    // Static (v3d) meshes drawn unlit, as character meshes are while character_meshes_fullbright is set
    bool static_meshes_fullbright();
    // The mesh ambient for a custom ambient (a lightmap colour 0..1)
    void mesh_blend_ambient(const float (&lightmap)[3], float (&out)[3]);
    // The sun's scale on a mesh lit by `ambient`, or by the level ambient when null
    float mesh_sun_scale(const float* ambient);
    bool is_monitor_screen_bitmap(int bm_handle);
    bool vehicle_tread_scroll_for_draw(int& config_out, float& u_out, float& v_out);
    bool vehicle_is_tread_bitmap(int bm_handle, int config_index);
    void vehicle_tread_resolve_mesh_bitmaps(int config_index, const void* mesh_key, const int* tex_handles,
                                            int num_tex_handles);

    // Bitmaps: the render target an ATX live feed shows instead of the bitmap, -1 for none (called per draw)
    int live_feed_bitmap(int bm_handle);
    bool bitmap_user_mipmap(int bm_handle);

    // Reads a whole packfile or loose file; false when it cannot be opened
    bool read_file(const char* filename, std::vector<std::byte>& out);
    [[noreturn]] void fatal_error(const std::string& text, const std::source_location& loc);
}
