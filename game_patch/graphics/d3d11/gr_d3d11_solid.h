#pragma once

#include <vector>
#include <optional>
#include <memory>
#include <d3d11.h>
#include <common/ComPtr.h>
#include "gr_d3d11_shader.h"

namespace rf
{
    struct GRoom;
    struct GSolid;
    struct GDecal;
}

namespace gr::d3d11
{
    class StateManager;
    class DynamicGeometryRenderer;
    class RenderContext;
    class GRenderCacheBuilder;
    class RoomRenderCache;
    class GRenderCache;

    enum class FaceRenderType { opaque, alpha, liquid };

    // A terrain batch's crater texture for its top and underside faces, which have none.
    constexpr int no_crater_texture = -2;

    class SolidRenderer
    {
    public:
        SolidRenderer(ComPtr<ID3D11Device> device, ShaderManager& shader_manager, StateManager& state_manager, DynamicGeometryRenderer& dyn_geo_renderer, RenderContext& render_context);
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
        void page_in_solid(rf::GSolid* solid);
        void page_in_movable_solid(rf::GSolid* solid)
        {
            get_or_create_movable_solid_cache(solid);
        }

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

        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext> context_;
        VertexShaderAndLayout vertex_shader_;
        ComPtr<ID3D11PixelShader> pixel_shader_;
        ComPtr<ID3D11PixelShader> pixel_shader_no_gas_;
        ComPtr<ID3D11PixelShader> terrain_pixel_shader_;
        ComPtr<ID3D11PixelShader> terrain_pixel_shader_no_gas_;
        // Terrain whose inputs are bound this pass (-1 for none), and the crater texture bound with it
        int bound_terrain_ = -1;
        int bound_crater_texture_ = no_crater_texture;
        // render_solid pass number, and per room_index the pass that last drew each terrain chunk
        // and its decals: a chunk is drawn once per pass, whichever room of the pass lists it first.
        int terrain_pass_ = 0;
        std::vector<int> terrain_drawn_;
        std::vector<int> terrain_decals_drawn_;
        // Sorted rooms that hold a dynamic decal, gathered by each render_dynamic_decals call
        std::vector<rf::GRoom*> dynamic_decal_rooms_;
        DynamicGeometryRenderer& dyn_geo_renderer_;
        RenderContext& render_context_;
        std::vector<std::unique_ptr<RoomRenderCache>> room_cache_;
        std::vector<std::unique_ptr<GRenderCache>> detail_render_cache_;
        std::unordered_map<rf::GSolid*, std::unique_ptr<GRenderCache>> mover_render_cache_;
        std::vector<rf::GRoom*> geo_cache_rooms_;
    };
}
