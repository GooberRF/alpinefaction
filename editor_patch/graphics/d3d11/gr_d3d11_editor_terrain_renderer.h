#pragma once

#include <memory>
#include <unordered_map>
#include <d3d11.h>
#include <common/ComPtr.h>
#include <common/gr_d3d11/gr_d3d11_shader.h>
#include <common/gr_d3d11/gr_d3d11_terrain.h>
#include "gr_d3d11_editor_terrain.h"

namespace gr::d3d11
{
    class RenderContext;
    class ShaderManager;
    class AfLightmapRenderer;

    // The terrain preview's terrains, each chunk built by alpine_terrain::emit_chunk as Build Geometry builds it, and
    // drawn with the terrain shader (editor_terrain_draw)
    class EditorTerrainRenderer
    {
    public:
        EditorTerrainRenderer(ComPtr<ID3D11Device> device, ShaderManager& shader_manager, RenderContext& render_context,
                              AfLightmapRenderer& af_lightmap_renderer);
        ~EditorTerrainRenderer();

        bool draw(const EditorTerrainDraw& draw, rf::gr::Mode mode, D3D11_CULL_MODE cull_mode);
        void cells_changed(const void* key, const TerrainCellRect* cells);
        void heights_changed(const void* key, const TerrainCellRect* verts);
        void rebind_grid(const void* key, const void* old_grid, const void* new_grid);
        void textures_reloaded();
        void forget(const void* key);

    private:
        struct Terrain;

        bool sync(Terrain& t, const EditorTerrainDraw& draw);
        bool build_chunk(const alpine_terrain::GridView& view, std::uint32_t edge, std::uint32_t index, Terrain& t);

        ComPtr<ID3D11Device> device_;
        RenderContext& render_context_;
        AfLightmapRenderer& af_lightmap_renderer_;
        TerrainRenderer terrain_renderer_;
        VertexShaderAndLayout vertex_shader_;
        ComPtr<ID3D11PixelShader> pixel_shader_;
        ComPtr<ID3D11PixelShader> pixel_shader_no_gas_;
        std::unordered_map<const void*, std::unique_ptr<Terrain>> terrains_;
    };
}
