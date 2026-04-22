#pragma once

#include <optional>
#include <d3d11.h>
#include <common/ComPtr.h>
#include "gr_d3d11_transform.h"
#include "gr_d3d11_vertex.h"
#include "gr_d3d11_shader.h"
#include "gr_d3d11_texture.h"
#include "gr_d3d11_state.h"
#include "../../misc/alpine_settings.h"
#include "../../rf/gr/gr.h"
#include "gr_d3d11_mesh.h"

namespace gr::d3d11
{

    class StateManager;
    class ShaderManager;
    class TextureManager;

    class ModelTransformBuffer
    {
    public:
        ModelTransformBuffer(ID3D11Device* device);

        void update(const rf::Vector3& pos, const rf::Matrix3& orient, ID3D11DeviceContext* device_context)
        {
            if (current_model_pos_ != pos || current_model_orient_ != orient) {
                current_model_pos_ = pos;
                current_model_orient_ = orient;
                update_buffer(device_context);
            }
        }

        operator ID3D11Buffer*() const
        {
            return buffer_;
        }

    private:
        void update_buffer(ID3D11DeviceContext* device_context);

        ComPtr<ID3D11Buffer> buffer_;
        rf::Vector3 current_model_pos_;
        rf::Matrix3 current_model_orient_;
    };

    class ViewProjTransformBuffer
    {
    public:
        ViewProjTransformBuffer(ID3D11Device* device);

        void update(const Projection& proj, ID3D11DeviceContext* device_context);

        operator ID3D11Buffer*() const
        {
            return buffer_;
        }

    private:
        ComPtr<ID3D11Buffer> buffer_;
    };

    class LightsBuffer
    {
    public:
        LightsBuffer(ID3D11Device* device);
        void update(ID3D11DeviceContext* device_context, bool force_neutral = false, const float* ambient_override = nullptr);

        operator ID3D11Buffer*() const
        {
            return buffer_;
        }

    private:
        ComPtr<ID3D11Buffer> buffer_;
    };

    class RenderModeBuffer
    {
    public:
        RenderModeBuffer(ID3D11Device* device);

        void update(rf::gr::Mode mode, rf::Color color, bool lightmap_only, bool dynamic_lighting, float self_illumination, bool apply_light_scale, bool emissive_override, ID3D11DeviceContext* device_context)
        {
            bool alpha_test = mode.get_zbuffer_type() == rf::gr::ZBUFFER_TYPE_FULL_ALPHA_TEST;
            bool fog_allowed = mode.get_fog_type() != rf::gr::FOG_NOT_ALLOWED;
            int colorblind_mode = g_alpine_game_config.colorblind_mode;
            float dynamic_light_ndotl = g_alpine_game_config.dynamic_light_ndotl;
            float pixel_light_overbright = g_level_pixel_light_overbright;
            float alpha_test_threshold = g_alpha_test_threshold;
            if (force_update_ || current_alpha_test_ != alpha_test || current_fog_allowed_ != fog_allowed || current_color_ != color || current_colorblind_mode_ != colorblind_mode || current_lightmap_only_ != lightmap_only || current_dynamic_lighting_ != dynamic_lighting || current_self_illumination_ != self_illumination || current_apply_light_scale_ != apply_light_scale || current_emissive_override_ != emissive_override || current_dynamic_light_ndotl_ != dynamic_light_ndotl || current_pixel_light_overbright_ != pixel_light_overbright || current_alpha_test_threshold_ != alpha_test_threshold) {
                current_alpha_test_ = alpha_test;
                current_fog_allowed_ = fog_allowed;
                current_color_ = color;
                current_colorblind_mode_ = colorblind_mode;
                current_lightmap_only_ = lightmap_only;
                current_dynamic_lighting_ = dynamic_lighting;
                current_self_illumination_ = self_illumination;
                current_apply_light_scale_ = apply_light_scale;
                current_emissive_override_ = emissive_override;
                current_dynamic_light_ndotl_ = dynamic_light_ndotl;
                current_pixel_light_overbright_ = pixel_light_overbright;
                current_alpha_test_threshold_ = alpha_test_threshold;
                force_update_ = false;
                update_buffer(device_context);
            }
        }

        operator ID3D11Buffer*() const
        {
            return buffer_;
        }

        void handle_fog_change()
        {
            if (current_fog_allowed_) {
                force_update_ = true;
            }
        }

    private:
        void update_buffer(ID3D11DeviceContext* device_context);

        ComPtr<ID3D11Buffer> buffer_;
        bool current_alpha_test_ = false;
        bool current_fog_allowed_ = false;
        bool force_update_ = true;
        rf::Color current_color_{255, 255, 255};
        int current_colorblind_mode_ = 0;
        bool current_lightmap_only_ = false;
        bool current_dynamic_lighting_ = false;
        float current_self_illumination_ = 0.0f;
        bool current_apply_light_scale_ = true;
        bool current_emissive_override_ = false;
        float current_dynamic_light_ndotl_ = 0.0f;
        float current_pixel_light_overbright_ = 0.5f;
        float current_alpha_test_threshold_ = 1.0f / 255.0f;
    };

    class PerFrameBuffer
    {
    public:
        PerFrameBuffer(ID3D11Device* device);
        void update(ID3D11DeviceContext* device_context);

        operator ID3D11Buffer*() const
        {
            return buffer_;
        }

    private:
        ComPtr<ID3D11Buffer> buffer_;
    };

    class TextureScaleBuffer
    {
    public:
        TextureScaleBuffer(ID3D11Device* device);

        void update(float u_scale, float v_scale, ID3D11DeviceContext* device_context)
        {
            if (current_u_scale_ != u_scale || current_v_scale_ != v_scale) {
                current_u_scale_ = u_scale;
                current_v_scale_ = v_scale;
                update_buffer(device_context);
            }
        }

        operator ID3D11Buffer*() const
        {
            return buffer_;
        }

    private:
        void update_buffer(ID3D11DeviceContext* device_context);

        ComPtr<ID3D11Buffer> buffer_;
        float current_u_scale_ = 1.0f;
        float current_v_scale_ = 1.0f;
    };

    class GasRegionBuffer
    {
    public:
        static constexpr int max_gas_regions = 32;

        GasRegionBuffer(ID3D11Device* device);
        void update(ID3D11DeviceContext* device_context, const Projection& projection);

        bool has_gas_regions() const
        {
            return current_gas_count_ > 0;
        }

        operator ID3D11Buffer*() const
        {
            return buffer_;
        }

    private:
        ComPtr<ID3D11Buffer> buffer_;
        int current_gas_count_ = 0;
    };

    class RenderContext
    {
    public:
        RenderContext(
            ComPtr<ID3D11Device> device,
            ComPtr<ID3D11DeviceContext> device_context,
            StateManager& state_manager,
            ShaderManager& shader_manager,
            TextureManager& texture_manager
        );

        ID3D11Device* device() const
        {
            return device_;
        }

        ID3D11DeviceContext* device_context() const
        {
            return device_context_;
        }

        void set_textures(int tex_handle0, int tex_handle1 = -1)
        {
            if (current_tex_handles_[0] != tex_handle0 || current_tex_handles_[1] != tex_handle1) {
                current_tex_handles_[0] = tex_handle0;
                current_tex_handles_[1] = tex_handle1;

                if (pow2_tex_active_) {
                    // Combined lookup: get SRV and UV scale in a single cache access
                    auto [srv, u_scale, v_scale] = texture_manager_.lookup_texture_with_scale(tex_handle0);
                    ID3D11ShaderResourceView* shader_resources[] = {
                        srv ? srv : texture_manager_.get_white_texture(),
                        get_lightmap_texture_view(tex_handle1),
                    };
                    device_context_->PSSetShaderResources(0, std::size(shader_resources), shader_resources);

                    if (!suppress_texture_uv_scale_) {
                        texture_scale_cbuffer_.update(u_scale, v_scale, device_context_);
                    }
                }
                else {
                    ID3D11ShaderResourceView* shader_resources[] = {
                        get_diffuse_texture_view(tex_handle0),
                        get_lightmap_texture_view(tex_handle1),
                    };
                    device_context_->PSSetShaderResources(0, std::size(shader_resources), shader_resources);
                }
            }
        }

        void set_suppress_texture_uv_scale(bool suppress)
        {
            if (pow2_tex_active_) {
                suppress_texture_uv_scale_ = suppress;
                if (suppress) {
                    texture_scale_cbuffer_.update(1.0f, 1.0f, device_context_);
                }
                else {
                    // Invalidate so next set_textures() re-evaluates the scale
                    current_tex_handles_ = {-2, -2};
                }
            }
        }

        bool is_pow2_tex_active() const
        {
            return pow2_tex_active_;
        }

        void set_pow2_tex_active(bool active)
        {
            pow2_tex_active_ = active;
            suppress_texture_uv_scale_ = false;
            if (active) {
                // Invalidate cached texture handles so the next set_textures() call
                // updates the scale buffer for the currently bound texture
                current_tex_handles_ = {-2, -2};
            }
            else {
                // Reset scale to (1,1) when p2t is disabled
                texture_scale_cbuffer_.update(1.0f, 1.0f, device_context_);
            }
        }

        void set_picmip_active(bool active)
        {
            // Clamp to false when r_picmip is disabled
            picmip_active_ = active && g_alpine_game_config.picmip > 1;
        }

        bool picmip_active() const { return picmip_active_; }

        // RAII: set picmip_active for the lifetime of the guard, restore on destruction.
        // Use at the top of geometry-rendering functions (mesh/CSG draws) so r_picmip's
        // MinLOD clamp bites on those draws and not on sprite/UI draws that follow.
        class ScopedPicmipActive
        {
        public:
            ScopedPicmipActive(RenderContext& ctx, bool active)
                : ctx_{ctx}
                , prev_{ctx.picmip_active_}
            {
                ctx_.set_picmip_active(active);
            }
            ~ScopedPicmipActive() { ctx_.picmip_active_ = prev_; }
            ScopedPicmipActive(const ScopedPicmipActive&) = delete;
            ScopedPicmipActive& operator=(const ScopedPicmipActive&) = delete;
        private:
            RenderContext& ctx_;
            bool prev_;
        };

        void set_mode(rf::gr::Mode mode, rf::Color color = {255, 255, 255, 255}, bool lightmap_only = false, bool dynamic_lighting = false, float self_illumination = 0.0f, bool apply_light_scale = true, bool emissive_override = false)
        {
            render_mode_cbuffer_.update(mode, color, lightmap_only, dynamic_lighting, self_illumination, apply_light_scale, emissive_override, device_context_);
            if (!current_mode_ || current_mode_.value() != mode || current_picmip_active_ != picmip_active_) {
                if (!current_mode_ || current_mode_.value().get_texture_source() != mode.get_texture_source() || current_picmip_active_ != picmip_active_) {
                    std::array<ID3D11SamplerState*, 2> sampler_states = {
                        state_manager_.lookup_sampler_state(mode.get_texture_source(), 0, picmip_active_),
                        // Slot 1 is the lightmap, r_picmip should scale diffuse textures only
                        state_manager_.lookup_sampler_state(mode.get_texture_source(), 1, false),
                    };
                    set_sampler_states(sampler_states);
                    current_picmip_active_ = picmip_active_;
                }
                if (!current_mode_ || current_mode_.value().get_alpha_blend() != mode.get_alpha_blend()) {
                    ID3D11BlendState* blend_state =
                        state_manager_.lookup_blend_state(mode.get_alpha_blend());
                    set_blend_state(blend_state);
                }
                if (!current_mode_ || current_mode_.value().get_zbuffer_type() != mode.get_zbuffer_type()) {
                    ID3D11DepthStencilState* depth_stencil_state =
                        state_manager_.lookup_depth_stencil_state(mode.get_zbuffer_type());
                    set_depth_stencil_state(depth_stencil_state);
                }
                current_mode_.emplace(mode);
            }
        }

        void set_sampler_states(std::array<ID3D11SamplerState*, 2> sampler_states)
        {
            if (current_sampler_states_ != sampler_states) {
                current_sampler_states_ = sampler_states;
                device_context_->PSSetSamplers(0, sampler_states.size(), sampler_states.data());
            }
        }

        void set_blend_state(ID3D11BlendState* blend_state)
        {
            if (current_blend_state_ != blend_state) {
                current_blend_state_ = blend_state;
                device_context_->OMSetBlendState(blend_state, nullptr, 0xffffffff);
            }
        }

        void set_depth_stencil_state(ID3D11DepthStencilState* depth_stencil_state, UINT stencil_ref = 0)
        {
            if (current_depth_stencil_state_ != depth_stencil_state || current_stencil_ref_ != stencil_ref) {
                current_depth_stencil_state_ = depth_stencil_state;
                current_stencil_ref_ = stencil_ref;
                device_context_->OMSetDepthStencilState(depth_stencil_state, stencil_ref);
            }
        }

        // Invalidate cached render mode so the next set_mode() call forces a full
        // state reset. Call this after directly setting depth/blend/sampler states
        // outside of set_mode() (e.g., outline rendering) to prevent stale caches.
        void invalidate_mode()
        {
            current_mode_.reset();
            // Re-bind constant buffers so that slots overwritten by the outline
            // renderer (VS b4 = outline params, PS b2 = outline color) are
            // restored to the correct buffers before normal rendering resumes.
            bind_cbuffers();
        }

        void set_rasterizer_state(ID3D11RasterizerState* rasterizer_state)
        {
            if (current_rasterizer_state_ != rasterizer_state) {
                current_rasterizer_state_ = rasterizer_state;
                device_context_->RSSetState(rasterizer_state);
            }
        }

        void set_render_target(ID3D11RenderTargetView* render_target_view, ID3D11DepthStencilView* depth_stencil_view)
        {
            render_target_view_ = render_target_view;
            depth_stencil_view_ = depth_stencil_view;
            ID3D11RenderTargetView* render_targets[] = { render_target_view };
            device_context_->OMSetRenderTargets(std::size(render_targets), render_targets, depth_stencil_view);
        }

        void bind_vs_cbuffer(int index, ID3D11Buffer* cbuffer)
        {
            ID3D11Buffer* vs_cbuffers[] = { cbuffer };
            device_context_->VSSetConstantBuffers(index, std::size(vs_cbuffers), vs_cbuffers);
        }

        void clear();
        void zbuffer_clear();
        void set_clip();

        void update_view_proj_transform(Projection proj)
        {
            projection_ = proj;
            view_proj_transform_cbuffer_.update(projection_, device_context_);
        }

        void update_per_frame_constants()
        {
            per_frame_buffer_.update(device_context_);
            gas_region_buffer_.update(device_context_, projection_);
        }

        bool has_gas_regions() const
        {
            return gas_region_buffer_.has_gas_regions();
        }

        void fog_set()
        {
            render_mode_cbuffer_.handle_fog_change();
        }

        void set_vertex_buffer(ID3D11Buffer* vertex_buffer, UINT stride, UINT slot = 0)
        {
            assert(slot < vertex_buffer_slots);
            if (current_vertex_buffers_[slot] != vertex_buffer) {
                current_vertex_buffers_[slot] = vertex_buffer;
                UINT offsets[] = { 0 };
                ID3D11Buffer* vertex_buffers[] = { vertex_buffer };
                device_context_->IASetVertexBuffers(slot, std::size(vertex_buffers), vertex_buffers, &stride, offsets);
            }
        }

        void set_index_buffer(ID3D11Buffer* index_buffer)
        {
            if (index_buffer != current_index_buffer_) {
                current_index_buffer_ = index_buffer;
                device_context_->IASetIndexBuffer(index_buffer, DXGI_FORMAT_R16_UINT, 0);
            }
        }

        void set_primitive_topology(D3D11_PRIMITIVE_TOPOLOGY primitive_topology)
        {
            if (current_primitive_topology_ != primitive_topology) {
                current_primitive_topology_ = primitive_topology;
                device_context_->IASetPrimitiveTopology(primitive_topology);
            }
        }

        void set_input_layout(ID3D11InputLayout* input_layout)
        {
            if (current_input_layout_ != input_layout) {
                current_input_layout_ = input_layout;
                device_context_->IASetInputLayout(input_layout);
            }
        }

        void set_vertex_shader(ID3D11VertexShader* vertex_shader)
        {
            if (current_vertex_shader_ != vertex_shader) {
                current_vertex_shader_ = vertex_shader;
                device_context_->VSSetShader(vertex_shader, nullptr, 0);
            }
        }

        void set_vertex_shader(const VertexShaderAndLayout& vertex_shader_and_layout)
        {
            set_input_layout(vertex_shader_and_layout.input_layout);
            set_vertex_shader(vertex_shader_and_layout.vertex_shader);
        }

        void set_pixel_shader(ID3D11PixelShader* pixel_shader)
        {
            if (current_pixel_shader_ != pixel_shader) {
                current_pixel_shader_ = pixel_shader;
                device_context_->PSSetShader(pixel_shader, nullptr, 0);
            }
        }

        void set_cull_mode(D3D11_CULL_MODE cull_mode)
        {
            if (current_cull_mode_ != cull_mode || zbias_changed_ || depth_clip_enabled_changed_) {
                current_cull_mode_ = cull_mode;
                zbias_changed_ = false;
                depth_clip_enabled_changed_ = false;
                set_rasterizer_state(state_manager_.lookup_rasterizer_state(current_cull_mode_, zbias_, depth_clip_enabled_));
            }
        }

        void set_model_transform(const rf::Vector3& pos, const rf::Matrix3& orient)
        {
            model_transform_cbuffer_.update(pos, orient, device_context_);
        }

        void set_zbias(int zbias)
        {
            if (zbias_ != zbias) {
                zbias_ = zbias;
                zbias_changed_ = true;
            }
        }

        void set_depth_clip_enabled(bool depth_clip_enabled)
        {
            if (depth_clip_enabled_ != depth_clip_enabled) {
                depth_clip_enabled_ = depth_clip_enabled;
                depth_clip_enabled_changed_ = true;
            }
        }

        void update_lights(bool force_neutral = false, const float* ambient_override = nullptr)
        {
            lights_buffer_.update(device_context_, force_neutral, ambient_override);
        }

        void draw_indexed(int index_count, int index_start_location, int base_vertex_location)
        {
            device_context_->DrawIndexed(index_count, index_start_location, base_vertex_location);
        }

        const Projection& projection() const
        {
            return projection_;
        }

        void invalidate_cached_state()
        {
            for (auto& vb : current_vertex_buffers_) vb = nullptr;
            current_index_buffer_ = nullptr;
            current_input_layout_ = nullptr;
            current_vertex_shader_ = nullptr;
            current_pixel_shader_ = nullptr;
            current_primitive_topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
            current_tex_handles_ = {-2, -2};
            current_cull_mode_ = D3D11_CULL_NONE;
            current_mode_.reset();
            current_picmip_active_ = false;
            current_sampler_states_ = {nullptr, nullptr};
            current_blend_state_ = nullptr;
            current_depth_stencil_state_ = nullptr;
            current_rasterizer_state_ = nullptr;
            zbias_ = 0;
            zbias_changed_ = true;
            depth_clip_enabled_ = true;
            depth_clip_enabled_changed_ = true;
            // Re-bind RenderContext's own constant buffers (restores b1 VP after shadow pass etc.)
            bind_cbuffers();
        }

        ModelTransformBuffer& model_transform_cbuffer()
        {
            return model_transform_cbuffer_;
        }

        ViewProjTransformBuffer& view_proj_transform_cbuffer()
        {
            return view_proj_transform_cbuffer_;
        }

    private:
        void bind_cbuffers();

        ID3D11ShaderResourceView* get_diffuse_texture_view(int tex_handle)
        {
            if (tex_handle != -1) {
                return texture_manager_.lookup_texture(tex_handle);
            }
            return texture_manager_.get_white_texture();
        }

        ID3D11ShaderResourceView* get_lightmap_texture_view(int tex_handle)
        {
            if (tex_handle != -1) {
                return texture_manager_.lookup_texture(tex_handle);
            }
            return texture_manager_.get_gray_texture();
        }

        static constexpr int vertex_buffer_slots = 2;

        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11DeviceContext> device_context_;
        StateManager& state_manager_;
        ShaderManager& shader_manager_;
        TextureManager& texture_manager_;
        ModelTransformBuffer model_transform_cbuffer_;
        ViewProjTransformBuffer view_proj_transform_cbuffer_;
        LightsBuffer lights_buffer_;
        RenderModeBuffer render_mode_cbuffer_;
        PerFrameBuffer per_frame_buffer_;
        TextureScaleBuffer texture_scale_cbuffer_;
        GasRegionBuffer gas_region_buffer_;

        ID3D11RenderTargetView* render_target_view_ = nullptr;
        ID3D11DepthStencilView* depth_stencil_view_ = nullptr;
        ID3D11Buffer* current_vertex_buffers_[vertex_buffer_slots] = {};
        ID3D11Buffer* current_index_buffer_ = nullptr;
        ID3D11InputLayout* current_input_layout_ = nullptr;
        ID3D11VertexShader* current_vertex_shader_ = nullptr;
        ID3D11PixelShader* current_pixel_shader_ = nullptr;
        D3D11_PRIMITIVE_TOPOLOGY current_primitive_topology_ = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
        std::array<int, 2> current_tex_handles_ = {-2, -2};
        D3D11_CULL_MODE current_cull_mode_ = D3D11_CULL_NONE;
        std::optional<rf::gr::Mode> current_mode_;
        bool picmip_active_ = false;
        bool current_picmip_active_ = false;
        std::array<ID3D11SamplerState*, 2> current_sampler_states_ = {nullptr, nullptr};
        ID3D11BlendState* current_blend_state_ = nullptr;
        ID3D11DepthStencilState* current_depth_stencil_state_ = nullptr;
        UINT current_stencil_ref_ = 0;
        ID3D11RasterizerState* current_rasterizer_state_ = nullptr;
        int zbias_ = 0;
        bool zbias_changed_ = true;
        bool depth_clip_enabled_ = true;
        bool depth_clip_enabled_changed_ = true;
        Projection projection_;
        bool pow2_tex_active_ = false;
        bool suppress_texture_uv_scale_ = false;
    };
}
