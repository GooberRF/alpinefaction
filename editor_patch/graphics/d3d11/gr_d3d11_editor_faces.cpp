#include <iterator>
#include <common/gr_d3d11/gr_d3d11_af_lightmap.h>
#include <common/gr_d3d11/gr_d3d11_context.h>
#include <common/gr_d3d11/gr_d3d11_dynamic_geometry.h>
#include "gr_d3d11_editor_faces.h"

namespace gr::d3d11
{
    namespace
    {
        constexpr int face_batch_max_vertex = 6000;
        constexpr int face_batch_max_index = 10000;
        constexpr const char* lightmap_vertex_shader_filename = "editor_lightmap_vs.bin";

        // editor_lightmap_vs.hlsl's input
        const D3D11_INPUT_ELEMENT_DESC lightmap_vertex_layout[] = {
            {"POSITIONT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT,
             D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, D3D11_APPEND_ALIGNED_ELEMENT, D3D11_INPUT_PER_VERTEX_DATA,
             0},
        };
    }

    EditorFaceRenderer::EditorFaceRenderer(ComPtr<ID3D11Device> device, ShaderManager& shader_manager,
                                           RenderContext& render_context, DynamicGeometryRenderer& dyn_geo_renderer,
                                           AfLightmapRenderer& af_lightmap_renderer) :
        render_context_{render_context},
        dyn_geo_renderer_{dyn_geo_renderer},
        af_lightmap_renderer_{af_lightmap_renderer},
        vertex_ring_buffer_{face_batch_max_vertex, D3D11_BIND_VERTEX_BUFFER, device, render_context.device_context()},
        index_ring_buffer_{face_batch_max_index, D3D11_BIND_INDEX_BUFFER, device, render_context.device_context()},
        vertex_shader_{shader_manager.load_vertex_shader(lightmap_vertex_shader_filename, lightmap_vertex_layout,
                                                         std::size(lightmap_vertex_layout))},
        pixel_shader_{shader_manager.get_pixel_shader(PixelShaderId::standard_no_gas)}
    {
    }

    void EditorFaceRenderer::add_face(int nv, const EditorLightmapVertex* vertices, int bm_handle, int lm_handle,
                                      rf::gr::Mode mode)
    {
        const int num_index = (nv - 2) * 3;
        if (nv < 3 || nv > face_batch_max_vertex || num_index > face_batch_max_index) {
            return;
        }
        // Whatever the dynamic geometry holds was drawn before this face
        dyn_geo_renderer_.flush();

        const State state{bm_handle, lm_handle, mode};
        if (state_ != state || vertex_ring_buffer_.is_full(nv) || index_ring_buffer_.is_full(num_index)) {
            flush();
            state_ = state;
        }
        const auto base_vertex = static_cast<rf::ushort>(vertex_ring_buffer_.get_pos());
        EditorLightmapVertex* gpu_verts = vertex_ring_buffer_.alloc(nv);
        rf::ushort* gpu_inds = index_ring_buffer_.alloc(num_index);
        for (int i = 0; i < nv; ++i) {
            gpu_verts[i] = vertices[i];
            if (i >= 2) {
                *(gpu_inds++) = base_vertex;
                *(gpu_inds++) = static_cast<rf::ushort>(base_vertex + i - 1);
                *(gpu_inds++) = static_cast<rf::ushort>(base_vertex + i);
            }
        }
    }

    void EditorFaceRenderer::flush()
    {
        const auto [start_vertex, num_vertex] = vertex_ring_buffer_.submit();
        if (num_vertex == 0) {
            return;
        }
        const auto [start_index, num_index] = index_ring_buffer_.submit();

        render_context_.set_vertex_buffer(vertex_ring_buffer_.get_buffer(), sizeof(EditorLightmapVertex));
        render_context_.set_index_buffer(index_ring_buffer_.get_buffer());
        render_context_.set_vertex_shader(vertex_shader_);
        render_context_.set_pixel_shader(pixel_shader_);
        render_context_.set_primitive_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        render_context_.set_mode(state_.mode);
        render_context_.set_textures(state_.bm_handle, state_.lm_handle);
        render_context_.set_cull_mode(D3D11_CULL_NONE);
        af_lightmap_renderer_.bind();
        render_context_.draw_indexed(num_index, start_index, start_vertex);
    }
}
