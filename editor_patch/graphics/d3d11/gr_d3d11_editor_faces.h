#pragma once

#include <d3d11.h>
#include <common/ComPtr.h>
#include <common/gr_d3d11/gr_d3d11_buffer.h>
#include <common/gr_d3d11/gr_d3d11_shader.h>
#include <gr_d3d11_engine.h>

namespace gr::d3d11
{
    class AfLightmapRenderer;
    class DynamicGeometryRenderer;
    class RenderContext;

    // A lightmapped level or brush face (stock 0x004E1C90), pre-transformed like the dynamic geometry
    struct EditorLightmapVertex
    {
        float x;
        float y;
        float z;
        float w;
        int diffuse;
        float u0;
        float v0;
        float u1;
        float v1;
        // As GpuVertex::lm_chart: an Alpine atlas chart record, and u1/v1 its texel coordinates; -1 for none
        float lm_chart;
    };
    static_assert(sizeof(EditorLightmapVertex) == 40);

    // Batches lightmapped faces sharing a texture, lightmap and mode. Drawing order against the dynamic geometry
    // is kept by flushing it before a face is queued, and this batch before it draws (its pre-flush callback).
    class EditorFaceRenderer
    {
    public:
        EditorFaceRenderer(ComPtr<ID3D11Device> device, ShaderManager& shader_manager, RenderContext& render_context,
                           DynamicGeometryRenderer& dyn_geo_renderer, AfLightmapRenderer& af_lightmap_renderer);
        void add_face(int nv, const EditorLightmapVertex* vertices, int bm_handle, int lm_handle, rf::gr::Mode mode);
        void flush();

        bool has_pending() const
        {
            return vertex_ring_buffer_.get_pos() > 0;
        }

    private:
        struct State
        {
            int bm_handle = -1;
            int lm_handle = -1;
            rf::gr::Mode mode;

            bool operator==(const State& other) const = default;
        };

        RenderContext& render_context_;
        DynamicGeometryRenderer& dyn_geo_renderer_;
        AfLightmapRenderer& af_lightmap_renderer_;
        RingBuffer<EditorLightmapVertex> vertex_ring_buffer_;
        RingBuffer<rf::ushort> index_ring_buffer_;
        VertexShaderAndLayout vertex_shader_;
        ComPtr<ID3D11PixelShader> pixel_shader_;
        State state_;
    };
}
