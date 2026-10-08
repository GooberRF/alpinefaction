#pragma once

#include <vector>
#include <d3d11.h>
#include <common/ComPtr.h>
#include "gr_d3d11_shader.h"
#include "../../misc/alpine_terrain.h"

namespace rf
{
    struct GSolid;
    struct Vector3;
}

namespace gr::d3d11
{
    class RenderContext;
    class MeshRenderer;

    // Every decoration draws with the opaque world (core). Blended materials of a layer with soft edges draw what
    // the core cut away after the opaque objects of the room that claimed the chunk (edge).
    enum class DecorationPass { core, edge };

    // Terrain mesh decorations, instanced per terrain chunk.
    class DecorationRenderer
    {
    public:
        DecorationRenderer(ComPtr<ID3D11Device> device, ShaderManager& shader_manager, RenderContext& render_context,
                           MeshRenderer& mesh_renderer);
        // alpha_to_coverage: the target is multisampled, so hard edges can be anti-aliased
        void render(rf::GSolid* solid, const std::vector<AlpineTerrainRoomRef>& chunks, DecorationPass pass,
                    bool alpha_to_coverage = false);
        // Whether any of `chunks` has a layer with soft edges, which the edge pass draws
        bool has_soft_edges(const std::vector<AlpineTerrainRoomRef>& chunks) const;
        void release();

    private:
        ID3D11Buffer* instance_buffer(int terrain);
        void set_draw_params(const rf::Vector3& center, float draw_distance, bool dither_fade, float alpha_pass);

        ComPtr<ID3D11Device> device_;
        RenderContext& render_context_;
        MeshRenderer& mesh_renderer_;
        VertexShaderAndLayout vertex_shader_;
        ComPtr<ID3D11PixelShader> pixel_shader_;
        ComPtr<ID3D11PixelShader> pixel_shader_no_gas_;
        ComPtr<ID3D11Buffer> cbuffer_;
        // By terrain index, created on first draw
        std::vector<ComPtr<ID3D11Buffer>> instance_buffers_;
        std::vector<AlpineTerrainRoomRef> sorted_chunks_;
        std::vector<float> chunk_distance_;
        float cbuffer_state_[6] = {};
        bool cbuffer_valid_ = false;
        bool shaders_ok_ = false;
    };
}
