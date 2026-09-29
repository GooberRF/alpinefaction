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

    // Terrain mesh decorations, instanced per terrain chunk, drawn in the opaque world pass.
    class DecorationRenderer
    {
    public:
        DecorationRenderer(ComPtr<ID3D11Device> device, ShaderManager& shader_manager, RenderContext& render_context,
                           MeshRenderer& mesh_renderer);
        void render(rf::GSolid* solid, const std::vector<AlpineTerrainRoomRef>& chunks);
        void release();

    private:
        ID3D11Buffer* instance_buffer(int terrain);
        void set_submesh(const rf::Vector3& center, float draw_distance);

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
        float cbuffer_state_[4] = {};
        bool cbuffer_valid_ = false;
        bool shaders_ok_ = false;
    };
}
