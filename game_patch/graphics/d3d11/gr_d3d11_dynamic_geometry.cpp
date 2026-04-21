#include <cassert>
#include "gr_d3d11.h"
#include "gr_d3d11_dynamic_geometry.h"
#include "gr_d3d11_shader.h"
#include "gr_d3d11_context.h"

namespace gr::d3d11
{
    constexpr int batch_max_vertex = 6000;
    constexpr int batch_max_index = 10000;

    DynamicGeometryRenderer::DynamicGeometryRenderer(ComPtr<ID3D11Device> device, ShaderManager& shader_manager, RenderContext& render_context) :
        device_{device}, render_context_(render_context),
        vertex_ring_buffer_{batch_max_vertex, D3D11_BIND_VERTEX_BUFFER, device_, render_context.device_context()},
        index_ring_buffer_{batch_max_index, D3D11_BIND_INDEX_BUFFER, device_, render_context.device_context()}
    {
        vertex_shader_ = shader_manager.get_vertex_shader(VertexShaderId::transformed);
        std_pixel_shader_ = shader_manager.get_pixel_shader(PixelShaderId::standard);
        std_pixel_shader_no_gas_ = shader_manager.get_pixel_shader(PixelShaderId::standard_no_gas);
        ui_pixel_shader_ = shader_manager.get_pixel_shader(PixelShaderId::ui);
    }

    void DynamicGeometryRenderer::set_pre_flush_callback(std::function<void()> callback)
    {
        pre_flush_callback_ = std::move(callback);
    }

    void DynamicGeometryRenderer::set_cull_mode(D3D11_CULL_MODE cull_mode)
    {
        if (cull_mode_ != cull_mode) {
            flush();
            cull_mode_ = cull_mode;
        }
    }

    void DynamicGeometryRenderer::flush()
    {
        flush_impl(true);
    }

    void DynamicGeometryRenderer::flush_impl(bool run_pre_callback)
    {
        auto [start_vertex, num_vertex] = vertex_ring_buffer_.submit();
        if (num_vertex == 0) {
            return;
        }
        // Invoke pre-flush callback before drawing batched content.
        // Used to flush outlines so they render behind transparent effects
        // (smoke, particles) that were batched in the dyn_geo renderer.
        // Guard against reentrance in case the callback indirectly triggers a flush.
        if (run_pre_callback && pre_flush_callback_ && !in_pre_flush_callback_) {
            in_pre_flush_callback_ = true;
            pre_flush_callback_();
            in_pre_flush_callback_ = false;
        }
        auto [start_index, num_index] = index_ring_buffer_.submit();

        //xlog::trace("Drawing dynamic geometry num_vertex {} num_index {} texture {}", num_vertex, num_index, rf::bm::get_filename(state_.textures[0]));

        render_context_.set_vertex_buffer(vertex_ring_buffer_.get_buffer(), sizeof(GpuTransformedVertex));
        render_context_.set_index_buffer(index_ring_buffer_.get_buffer());
        render_context_.set_vertex_shader(vertex_shader_);
        render_context_.set_pixel_shader(state_.pixel_shader);
        render_context_.set_primitive_topology(state_.primitive_topology);
        render_context_.set_mode(state_.mode);
        render_context_.set_textures(state_.textures[0], state_.textures[1]);
        render_context_.set_cull_mode(cull_mode_);

        render_context_.draw_indexed(num_index, start_index, start_vertex);
    }

    static inline bool mode_uses_vertex_color(rf::gr::Mode mode)
    {
        if (mode.get_texture_source() == rf::gr::TEXTURE_SOURCE_NONE) {
            return true;
        }
        return mode.get_color_source() != rf::gr::COLOR_SOURCE_TEXTURE;
    }

    static inline bool mode_uses_vertex_alpha(rf::gr::Mode mode)
    {
        if (mode.get_texture_source() == rf::gr::TEXTURE_SOURCE_NONE) {
            return true;
        }
        return mode.get_alpha_source() != rf::gr::ALPHA_SOURCE_TEXTURE;
    }

    static inline rf::Color get_vertex_color_from_screen(rf::gr::Mode mode)
    {
        rf::Color color{255, 255, 255, 255};
        if (mode_uses_vertex_color(mode)) {
            color.red = rf::gr::screen.current_color.red;
            color.green = rf::gr::screen.current_color.green;
            color.blue = rf::gr::screen.current_color.blue;
        }
        if (mode_uses_vertex_alpha(mode)) {
            color.alpha = rf::gr::screen.current_color.alpha;
        }
        return color;
    }

    inline std::array<float, 4> DynamicGeometryRenderer::convert_pos(const rf::gr::Vertex& v, bool is_3d)
    {
        rf::Vector3 ndc{
            ((v.sx - rf::gr::screen.offset_x) / rf::gr::screen.clip_width * 2.0f - 1.0f),
            ((v.sy - rf::gr::screen.offset_y) / rf::gr::screen.clip_height * -2.0f + 1.0f),
            v.sw,
        };
        // Set w to depth in camera space (needed for 3D rendering)
        float w = is_3d ? render_context_.projection().unproject_z(v.sw) : 1.0f;
        return {
            ndc.x * w,
            ndc.y * w,
            ndc.z * w,
            w,
        };
    }

    void DynamicGeometryRenderer::add_poly(int nv, const rf::gr::Vertex **vertices, int vertex_attributes, const std::array<int, 2>& tex_handles, rf::gr::Mode mode)
    {
        int num_index = (nv - 2) * 3;
        if (nv > batch_max_vertex || num_index > batch_max_index) {
            xlog::error("too many vertices/indices needed in dynamic geometry renderer");
            return;
        }

        std::array<int, 2> normalized_tex_handles = normalize_texture_handles_for_mode(mode, tex_handles);

        State new_state{
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
            normalized_tex_handles,
            mode,
            render_context_.has_gas_regions() ? std_pixel_shader_ : std_pixel_shader_no_gas_,
        };
        auto [gpu_verts, gpu_ind_ptr, base_vertex] = setup(nv, num_index, new_state);

        bool use_vert_color = mode_uses_vertex_color(mode);
        bool use_vert_alpha = mode_uses_vertex_alpha(mode);
        rf::Color color{255, 255, 255, 255};
        if (use_vert_color && !(vertex_attributes & rf::gr::TMAP_FLAG_RGB)) {
            color.red = rf::gr::screen.current_color.red;
            color.green = rf::gr::screen.current_color.green;
            color.blue = rf::gr::screen.current_color.blue;
        }
        if (use_vert_alpha && !(vertex_attributes & rf::gr::TMAP_FLAG_ALPHA)) {
            color.alpha = rf::gr::screen.current_color.alpha;
        }
        for (int i = 0; i < nv; ++i) {
            const rf::gr::Vertex& in_vert = *vertices[i];
            GpuTransformedVertex& out_vert = gpu_verts[i];
            auto [x, y, z, w] = convert_pos(in_vert, true);
            out_vert.x = x;
            out_vert.y = y;
            out_vert.z = z;
            out_vert.w = w;

            if (use_vert_color && (vertex_attributes & rf::gr::TMAP_FLAG_RGB)) {
                color.red = in_vert.r;
                color.green = in_vert.g;
                color.blue = in_vert.b;
            }
            if (use_vert_alpha && (vertex_attributes & rf::gr::TMAP_FLAG_ALPHA)) {
                color.alpha = in_vert.a;
            }
            out_vert.diffuse = pack_color(color);
            out_vert.u0 = in_vert.u1;
            out_vert.v0 = in_vert.v1;
            //xlog::info("vert[{}]: pos <{:.2f} {:.2f} {:.2f}> uv <{:.2f} {:.2f}>", i, out_vert.x, out_vert.y, in_vert.sw, out_vert.u0, out_vert.v0);

            if (i >= 2) {
                *(gpu_ind_ptr++) = base_vertex;
                *(gpu_ind_ptr++) = base_vertex + i - 1;
                *(gpu_ind_ptr++) = base_vertex + i;
            }
        }
    }

    void DynamicGeometryRenderer::line(const rf::gr::Vertex **vertices, rf::gr::Mode mode, bool is_3d)
    {
        constexpr int num_verts = 2;
        constexpr int num_inds = 2;
        State new_state{
            D3D11_PRIMITIVE_TOPOLOGY_LINELIST,
            {-1, -1},
            mode,
            is_3d ? (render_context_.has_gas_regions() ? std_pixel_shader_ : std_pixel_shader_no_gas_) : ui_pixel_shader_,
        };
        auto [gpu_verts, gpu_ind_ptr, base_vertex] = setup(num_verts, num_inds, new_state);

        rf::Color color = get_vertex_color_from_screen(mode);
        int diffuse = pack_color(color);

        for (int i = 0; i < num_verts; ++i) {
            const rf::gr::Vertex& in_vert = *vertices[i];
            GpuTransformedVertex& out_vert = gpu_verts[i];
            auto [x, y, z, w] = convert_pos(in_vert, is_3d);
            out_vert.x = x;
            out_vert.y = y;
            out_vert.z = z;
            out_vert.w = w;
            out_vert.diffuse = diffuse;
        }
        *(gpu_ind_ptr++) = base_vertex;
        *(gpu_ind_ptr++) = base_vertex + 1;
    }

    void DynamicGeometryRenderer::line_3d(const rf::gr::Vertex& v0, const rf::gr::Vertex& v1, rf::gr::Mode mode)
    {
        const rf::gr::Vertex* verts_ptrs[] = {&v0, &v1};
        line(verts_ptrs, mode, true);
    }

    void DynamicGeometryRenderer::line_2d(float x1, float y1, float x2, float y2, rf::gr::Mode mode)
    {
        rf::gr::Vertex verts[2];
        verts[0].sx = x1 + 0.5f;
        verts[0].sy = y1 + 0.5f;
        verts[0].sw = 1.0f;
        verts[1].sx = x2 + 0.5f;
        verts[1].sy = y2 + 0.5f;
        verts[1].sw = 1.0f;
        const rf::gr::Vertex* verts_ptrs[] = {&verts[0], &verts[1]};
        line(verts_ptrs, mode, false);
    }

    void DynamicGeometryRenderer::bitmap(int bm_handle, float x, float y, float w, float h, float sx, float sy, float sw, float sh, bool flip_x, bool flip_y, rf::gr::Mode mode)
    {
        //xlog::trace("Drawing bitmap");
        int bm_w, bm_h;
        rf::bm::get_dimensions(bm_handle, &bm_w, &bm_h);

        // For some reason original implementation do not allow UVs > 1
        sw = std::min(sw, bm_w - sx);
        sh = std::min(sh, bm_h - sy);
        if (sw <= 0.0f || sh <= 0.0f) {
            return;
        }

        float sx_left = x / rf::gr::screen.clip_width * 2.0f - 1.0f;
        float sx_right = (x + w) / rf::gr::screen.clip_width * 2.0f - 1.0f;
        float sy_top = y / rf::gr::screen.clip_height * -2.0f + 1.0f;
        float sy_bottom = (y + h) / rf::gr::screen.clip_height * -2.0f + 1.0f;
        float u_left = sx / bm_w;
        float u_right = (sx + sw) / bm_w;
        float v_top = sy / bm_h;
        float v_bottom = (sy + sh) / bm_h;

        // Make sure wrapped texel is not used in case of scaling with filtering enabled
        if (w != sw) {
            u_left += 0.5f / bm_w;
            u_right -= 0.5f / bm_w;
        }
        if (h != sh) {
            v_top += 0.5f / bm_h;
            v_bottom -= 0.5f / bm_h;
        }

        if (flip_x) {
            std::swap(u_left, u_right);
        }
        if (flip_y) {
            std::swap(v_top, v_bottom);
        }

        constexpr int num_verts = 4;
        constexpr int num_inds = 6;

        State new_state{
            D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
            {bm_handle, -1},
            mode,
            ui_pixel_shader_,
        };
        auto [gpu_verts, gpu_ind_ptr, base_vertex] = setup(num_verts, num_inds, new_state);

        rf::Color color = get_vertex_color_from_screen(mode);
        int diffuse = pack_color(color);

        for (int i = 0; i < num_verts; ++i) {
            GpuTransformedVertex& gpu_vert = gpu_verts[i];
            gpu_vert.x = (i == 0 || i == 3) ? sx_left : sx_right;
            gpu_vert.y = (i == 0 || i == 1) ? sy_top : sy_bottom;
            gpu_vert.z = 1.0f;
            gpu_vert.w = 1.0f;
            gpu_vert.diffuse = diffuse;
            gpu_vert.u0 = (i == 0 || i == 3) ? u_left : u_right;
            gpu_vert.v0 = (i == 0 || i == 1) ? v_top : v_bottom;
        }
        *(gpu_ind_ptr++) = base_vertex;
        *(gpu_ind_ptr++) = base_vertex + 1;
        *(gpu_ind_ptr++) = base_vertex + 2;
        *(gpu_ind_ptr++) = base_vertex;
        *(gpu_ind_ptr++) = base_vertex + 2;
        *(gpu_ind_ptr++) = base_vertex + 3;
    }
}
