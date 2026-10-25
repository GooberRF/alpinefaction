#pragma once

#include <vector>
#include <optional>
#include <memory>
#include <span>
#include <d3d11.h>
#include <common/ComPtr.h>
#include "gr_d3d11_shader.h"
#include "gr_d3d11_terrain.h"
#include <gr_d3d11_engine.h>

namespace gr::d3d11
{
    class StateManager;
    class DynamicGeometryRenderer;
    class RenderContext;
    class AfLightmapRenderer;
    class GRenderCacheBuilder;
    class RoomRenderCache;
    class GRenderCache;

    enum class FaceRenderType { opaque, alpha, liquid };

    // How the level geometry is drawn: the editor's View menu display modes. The game always draws normal.
    enum class SolidDisplayMode { normal, textures_only, room_colors, see_through };

    // What the level caches hold: the editor's View options. The game keeps the defaults.
    struct SolidCacheOptions
    {
        SolidDisplayMode display_mode = SolidDisplayMode::normal;
        bool invisible_faces_drawn = false;
        bool portal_faces_drawn = false;
        // Faces it returns true for are left out
        bool (*hides_face)(const rf::GFace& face) = nullptr;
        bool level_decals_hidden = false;
        // Also kept for SolidRenderer::render_overlay: the edges of the faces, and the portal faces left out
        bool edges_built = false;
        bool portal_planes_built = false;

        bool operator==(const SolidCacheOptions&) const = default;
    };

    // What SolidRenderer::render_overlay draws of the caches: lines, or triangles
    enum class SolidOverlay { edges, portal_planes };

    // A terrain batch's crater texture for its top and underside faces, which have none.
    constexpr int no_crater_texture = -2;

    // The mode a level decal draws in
    inline rf::gr::Mode determine_decal_mode(const rf::GDecal* decal)
    {
        if (decal->flags & rf::DF_SELF_ILLUMINATED) {
            return rf::gr_decal_self_illuminated_mode;
        }
        if (decal->flags & rf::DF_TILING_U) {
            return rf::gr::decal_tiling_u_mode;
        }
        if (decal->flags & rf::DF_TILING_V) {
            return rf::gr::decal_tiling_v_mode;
        }
        return rf::gr::decal_mode;
    }

    class SolidRenderer
    {
    public:
        SolidRenderer(ComPtr<ID3D11Device> device, ShaderManager& shader_manager, StateManager& state_manager, DynamicGeometryRenderer& dyn_geo_renderer, RenderContext& render_context, AfLightmapRenderer& af_lightmap_renderer);
        ~SolidRenderer();
        void render_solid(rf::GSolid* solid, rf::GRoom** rooms, int num_rooms);
        void render_movable_solid(rf::GSolid* solid, const rf::Vector3& pos, const rf::Matrix3& orient, bool include_alpha);
        bool movable_solid_has_alpha(rf::GSolid* solid);
        void render_movable_solid_alpha(rf::GSolid* solid, const rf::Vector3& pos, const rf::Matrix3& orient);
        void render_sky_room(rf::GRoom *room, rf::Vector3& out_sky_transform_pos, rf::Matrix3& out_sky_transform_orient);
        void render_alpha_detail(rf::GRoom *room, rf::GSolid *solid);
        void render_room_liquid_surface(rf::GSolid* solid, rf::GRoom* room);
        void clear_cache();
        // Drops the cache a detail room's geo_cache points at, leaving it to be rebuilt.
        void release_detail_room_cache(rf::GRoom* room);
        void reset_cache_after_boolean();
        // Shapes the caches built afterwards, so a change is followed by clear_cache()
        void set_cache_options(const SolidCacheOptions& options)
        {
            cache_options_ = options;
        }
        const SolidCacheOptions& cache_options() const
        {
            return cache_options_;
        }
        // The editor's View > Show Back Faces; the game culls them
        void set_two_sided(bool two_sided)
        {
            cull_mode_ = two_sided ? D3D11_CULL_NONE : D3D11_CULL_BACK;
        }
        // The editor's extras over what render_solid / render_sky_room draw of these rooms, as the cache options
        // built them, with the shaders and states the caller bound. The edges of faces with an id in
        // hidden_edge_face_ids (sorted, unique) are left out.
        void render_overlay(SolidOverlay what, rf::GSolid* solid, rf::GRoom** rooms, int num_rooms,
                            std::span<const int> hidden_edge_face_ids);
        void render_sky_room_overlay(SolidOverlay what, rf::GRoom* room);
        void release_terrain_gpu()
        {
            terrain_renderer_.release();
        }
        void page_in_solid(rf::GSolid* solid);
        void page_in_movable_solid(rf::GSolid* solid)
        {
            get_or_create_movable_solid_cache(solid);
        }
        // Terrain chunks whose decorations the last render_solid pass found in view
        const std::vector<AlpineTerrainRoomRef>& decoration_chunks() const
        {
            return decoration_chunks_;
        }
        // Of those, the ones `room` claimed, while `solid` is still on the pass that found them: its object pass
        // draws their soft edges
        const std::vector<AlpineTerrainRoomRef>& room_decoration_chunks(const rf::GSolid* solid, const rf::GRoom* room);

    private:
        void before_render(const rf::Vector3& pos, const rf::Matrix3& orient);
        void after_render();
        void render_room_faces(rf::GSolid* solid, rf::GRoom* room, FaceRenderType render_type);
        void render_detail(rf::GSolid* solid, rf::GRoom* room, bool alpha);
        void render_terrain(GRenderCache& cache);
        void render_dynamic_decals(rf::GRoom** rooms, int num_rooms);
        void render_alpha_detail_dynamic_decals(rf::GRoom* detail_room);
        void render_movable_solid_dynamic_decals(rf::GSolid* solid, const rf::Vector3& pos, const rf::Matrix3& orient, bool opaque_faces, bool alpha_faces);
        void before_render_decals();
        void after_render_decals();
        RoomRenderCache* get_or_create_normal_room_cache(rf::GSolid* solid, rf::GRoom* room);
        GRenderCache* get_or_create_detail_room_cache(rf::GSolid* solid, rf::GRoom* room);
        GRenderCache* get_or_create_movable_solid_cache(rf::GSolid* solid);
        bool claim_terrain_chunk(std::vector<int>& stamps, const rf::GRoom* room);
        bool terrain_chunk_drawn(const rf::GRoom* room) const;
        void collect_decoration_chunk(const rf::GRoom* room, const rf::GRoom* portal_room);

        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext> context_;
        VertexShaderAndLayout vertex_shader_;
        ComPtr<ID3D11PixelShader> pixel_shader_;
        ComPtr<ID3D11PixelShader> pixel_shader_no_gas_;
        ComPtr<ID3D11PixelShader> terrain_pixel_shader_;
        ComPtr<ID3D11PixelShader> terrain_pixel_shader_no_gas_;
        TerrainRenderer terrain_renderer_;
        // Terrain whose inputs are bound this pass (-1 for none), and the crater texture bound with it
        int bound_terrain_ = -1;
        int bound_crater_texture_ = no_crater_texture;
        // render_solid pass number, and per room_index the pass that last drew each terrain chunk
        // and its decals: a chunk is drawn once per pass, whichever room of the pass lists it first.
        int terrain_pass_ = 0;
        std::vector<int> terrain_drawn_;
        std::vector<int> terrain_decals_drawn_;
        std::vector<int> terrain_decorations_seen_;
        std::vector<AlpineTerrainRoomRef> decoration_chunks_;
        // Per decoration_chunks_ entry, the normal room of the pass that claimed it
        std::vector<const rf::GRoom*> decoration_chunk_rooms_;
        // The GSolid::current_frame of the pass that found them: the sky room drains before the pass's render_solid
        int decoration_chunks_frame_ = 0;
        std::vector<AlpineTerrainRoomRef> room_decoration_chunks_;
        // Sorted rooms that hold a dynamic decal, gathered by each render_dynamic_decals call
        std::vector<rf::GRoom*> dynamic_decal_rooms_;
        DynamicGeometryRenderer& dyn_geo_renderer_;
        RenderContext& render_context_;
        AfLightmapRenderer& af_lightmap_renderer_;
        std::vector<std::unique_ptr<RoomRenderCache>> room_cache_;
        std::vector<std::unique_ptr<GRenderCache>> detail_render_cache_;
        std::unordered_map<rf::GSolid*, std::unique_ptr<GRenderCache>> mover_render_cache_;
        std::vector<rf::GRoom*> geo_cache_rooms_;
        SolidCacheOptions cache_options_;
        D3D11_CULL_MODE cull_mode_ = D3D11_CULL_BACK;
    };
}
