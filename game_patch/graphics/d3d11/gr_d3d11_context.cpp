#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>
#include <xlog/xlog.h>
#include "../../rf/gr/gr_light.h"
#include "../../rf/os/frametime.h"
#include "../../rf/multi.h"
#include "../../misc/level.h"
#include "gr_d3d11.h"
#include "gr_d3d11_context.h"
#include "gr_d3d11_texture.h"
#include "gr_d3d11_state.h"
#include "gr_d3d11_shader.h"

namespace gr::d3d11
{
    RenderContext::RenderContext(
        ComPtr<ID3D11Device> device, ComPtr<ID3D11DeviceContext> device_context,
        StateManager& state_manager, ShaderManager& shader_manager,
        TextureManager& texture_manager
    ) :
        device_{std::move(device)}, device_context_{std::move(device_context)},
        state_manager_{state_manager}, shader_manager_{shader_manager}, texture_manager_{texture_manager},
        model_transform_cbuffer_{device_},
        view_proj_transform_cbuffer_{device_},
        lights_buffer_{device_},
        render_mode_cbuffer_{device_},
        per_frame_buffer_{device_},
        texture_scale_cbuffer_{device_},
        gas_region_buffer_{device_}
    {
        bind_cbuffers();
    }

    void RenderContext::bind_cbuffers()
    {
        ID3D11Buffer* vs_cbuffers[] = {
            model_transform_cbuffer_,
            view_proj_transform_cbuffer_,
            per_frame_buffer_,
            nullptr,
        };
        device_context_->VSSetConstantBuffers(0, std::size(vs_cbuffers), vs_cbuffers);

        ID3D11Buffer* ps_cbuffers[] = {
            render_mode_cbuffer_,
            lights_buffer_,
            texture_scale_cbuffer_,
        };
        device_context_->PSSetConstantBuffers(0, std::size(ps_cbuffers), ps_cbuffers);

        // Gas region buffer at b4 (b3 is used by shadow renderer)
        ID3D11Buffer* gas_cbuffer = gas_region_buffer_;
        device_context_->PSSetConstantBuffers(4, 1, &gas_cbuffer);
    }

    void RenderContext::clear()
    {
        // Note: original code clears clip rect only but it is not trivial in D3D11
        if (render_target_view_) {
            float clear_color[4] = {
                rf::gr::screen.current_color.red / 255.0f,
                rf::gr::screen.current_color.green / 255.0f,
                rf::gr::screen.current_color.blue / 255.0f,
                1.0f,
            };
            device_context_->ClearRenderTargetView(render_target_view_, clear_color);
        }
    }

    void RenderContext::zbuffer_clear()
    {
        // Note: original code clears clip rect only but it is not trivial in D3D11
        if (rf::gr::screen.depthbuffer_type != rf::gr::DEPTHBUFFER_NONE && depth_stencil_view_) {
            float depth = rf::gr::screen.depthbuffer_type == rf::gr::DEPTHBUFFER_Z ? 0.0f : 1.0f;
            device_context_->ClearDepthStencilView(depth_stencil_view_, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, depth, 0);
        }
    }

    void RenderContext::set_clip()
    {
        D3D11_VIEWPORT vp;
        vp.TopLeftX = static_cast<float>(rf::gr::screen.clip_left + rf::gr::screen.offset_x);
        vp.TopLeftY = static_cast<float>(rf::gr::screen.clip_top + rf::gr::screen.offset_y);
        vp.Width = static_cast<float>(rf::gr::screen.clip_width);
        vp.Height = static_cast<float>(rf::gr::screen.clip_height);
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        device_context_->RSSetViewports(1, &vp);
    }

    struct alignas(16) ModelTransformBufferData
    {
        // model to world
        GpuMatrix4x3 world_mat;
    };
    static_assert(sizeof(ModelTransformBufferData) % 16 == 0);

    ModelTransformBuffer::ModelTransformBuffer(ID3D11Device* device) :
        current_model_pos_{NAN, NAN, NAN},
        current_model_orient_{{NAN, NAN, NAN}, {NAN, NAN, NAN}, {NAN, NAN, NAN}}
    {
        CD3D11_BUFFER_DESC desc{
            sizeof(ModelTransformBufferData),
            D3D11_BIND_CONSTANT_BUFFER,
            D3D11_USAGE_DYNAMIC,
            D3D11_CPU_ACCESS_WRITE,
        };
        DF_GR_D3D11_CHECK_HR(device->CreateBuffer(&desc, nullptr, &buffer_));
    }

    void ModelTransformBuffer::update_buffer(ID3D11DeviceContext* device_context)
    {
        ModelTransformBufferData data;
        data.world_mat = build_world_matrix(current_model_pos_, current_model_orient_);

        D3D11_MAPPED_SUBRESOURCE mapped_subres;
        DF_GR_D3D11_CHECK_HR(
            device_context->Map(buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_subres)
        );
        std::memcpy(mapped_subres.pData, &data, sizeof(data));
        device_context->Unmap(buffer_, 0);
    }

    struct alignas(16) ViewProjTransformBufferData
    {
        GpuMatrix4x3 view_mat;
        GpuMatrix4x4 proj_mat;
    };
    static_assert(sizeof(ViewProjTransformBufferData) % 16 == 0);

    ViewProjTransformBuffer::ViewProjTransformBuffer(ID3D11Device* device)
    {
        CD3D11_BUFFER_DESC desc{
            sizeof(ViewProjTransformBufferData),
            D3D11_BIND_CONSTANT_BUFFER,
            D3D11_USAGE_DYNAMIC,
            D3D11_CPU_ACCESS_WRITE,
        };
        DF_GR_D3D11_CHECK_HR(device->CreateBuffer(&desc, nullptr, &buffer_));
    }

    void ViewProjTransformBuffer::update(const Projection& proj, ID3D11DeviceContext* device_context)
    {
        ViewProjTransformBufferData data;
        data.view_mat = build_view_matrix(rf::gr::eye_pos, rf::gr::eye_matrix);
        data.proj_mat = proj.matrix();

        D3D11_MAPPED_SUBRESOURCE mapped_subres;
        DF_GR_D3D11_CHECK_HR(
            device_context->Map(buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_subres)
        );
        std::memcpy(mapped_subres.pData, &data, sizeof(data));
        device_context->Unmap(buffer_, 0);
    }

    struct alignas(16) PerFrameBufferData
    {
        float time;
    };
    static_assert(sizeof(PerFrameBufferData) % 16 == 0);

    PerFrameBuffer::PerFrameBuffer(ID3D11Device* device)
    {
        CD3D11_BUFFER_DESC desc{
            sizeof(PerFrameBufferData),
            D3D11_BIND_CONSTANT_BUFFER,
            D3D11_USAGE_DYNAMIC,
            D3D11_CPU_ACCESS_WRITE,
        };
        DF_GR_D3D11_CHECK_HR(device->CreateBuffer(&desc, nullptr, &buffer_));
    }

    void PerFrameBuffer::update(ID3D11DeviceContext* device_context)
    {
        PerFrameBufferData data{};
        data.time = rf::frametime_total_milliseconds / 1000.0f;

        D3D11_MAPPED_SUBRESOURCE mapped_subres;
        DF_GR_D3D11_CHECK_HR(
            device_context->Map(buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_subres)
        );
        std::memcpy(mapped_subres.pData, &data, sizeof(data));
        device_context->Unmap(buffer_, 0);
    }

    struct LightsBufferData
    {
        static constexpr int max_point_lights = 32; // max dynamic lights in a scene, matches stock game DX9 maximum

        struct PointLight
        {
            std::array<float, 3> pos;
            float radius;
            std::array<float, 3> color;       // rgb
            float light_type;                 // 0=omni, 1=spot, 2=tube
            std::array<float, 3> spot_dir;    // spotlight direction (0,0,0 for omni)
            float spot_fov1_dot;              // -cos(fov1/2): inner cone threshold (negated)
            float spot_fov2_dot;              // -cos(fov2/2): outer cone threshold (negated)
            float spot_atten;                 // spotlight distance attenuation modifier
            float spot_sq_falloff;            // 1.0 if squared cone falloff, 0.0 for linear
            float atten_algo;                 // distance attenuation: 0=linear, 1=squared, 2=cosine, 3=sqrt
            std::array<float, 3> pos2;        // tube light second endpoint
            float _pad;
        };

        std::array<float, 3> ambient_light;
        float num_point_lights;
        PointLight point_lights[max_point_lights];
    };
    static_assert(sizeof(LightsBufferData::PointLight) % 16 == 0);

    LightsBuffer::LightsBuffer(ID3D11Device* device)
    {
        CD3D11_BUFFER_DESC desc{
            sizeof(LightsBufferData),
            D3D11_BIND_CONSTANT_BUFFER,
            D3D11_USAGE_DYNAMIC,
            D3D11_CPU_ACCESS_WRITE,
        };
        DF_GR_D3D11_CHECK_HR(device->CreateBuffer(&desc, nullptr, &buffer_));
    }

    void LightsBuffer::update(ID3D11DeviceContext* device_context, bool force_neutral, const float* ambient_override)
    {
        D3D11_MAPPED_SUBRESOURCE mapped_subres;
        DF_GR_D3D11_CHECK_HR(
            device_context->Map(buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_subres)
        );

        LightsBufferData data{};
        if (!force_neutral) {
            for (int i = 0; i < std::min(rf::gr::num_relevant_lights, LightsBufferData::max_point_lights); ++i) {
                LightsBufferData::PointLight& gpu_light = data.point_lights[i];
                // Make sure radius is never 0 because we divide by it
                gpu_light.radius = 0.0001f;
            }

            if (ambient_override) {
                data.ambient_light = {ambient_override[0], ambient_override[1], ambient_override[2]};
            } else {
                rf::gr::light_get_ambient(&data.ambient_light[0], &data.ambient_light[1], &data.ambient_light[2]);
            }

            int num_point_lights = std::min(rf::gr::num_relevant_lights, LightsBufferData::max_point_lights);
            data.num_point_lights = static_cast<float>(num_point_lights);

            int gpu_index = 0;
            for (int i = 0; i < num_point_lights; ++i) {
                rf::gr::Light* light = rf::gr::relevant_lights[i];
                // Skip negative-intensity lights at upload time as a safety net
                if (light->r < 0.0f || light->g < 0.0f || light->b < 0.0f) {
                    continue;
                }
                LightsBufferData::PointLight& gpu_light = data.point_lights[gpu_index];
                gpu_light.pos = {light->vec.x, light->vec.y, light->vec.z};
                gpu_light.color = {light->r, light->g, light->b};
                gpu_light.radius = light->rad_2;
                gpu_light.pos2 = {0.0f, 0.0f, 0.0f};
                gpu_light._pad = 0.0f;
                if (light->type == rf::gr::LT_SPOT) {
                    gpu_light.light_type = 1.0f;
                    gpu_light.spot_dir = {light->spotlight_dir.x, light->spotlight_dir.y, light->spotlight_dir.z};
                    gpu_light.spot_fov1_dot = light->spotlight_fov1_dot;
                    gpu_light.spot_fov2_dot = light->spotlight_fov2_dot;
                    gpu_light.spot_atten = light->spotlight_atten;
                    gpu_light.spot_sq_falloff = light->use_squared_fov_falloff ? 1.0f : 0.0f;
                } else if (light->type == rf::gr::LT_TUBE) {
                    gpu_light.light_type = 2.0f;
                    gpu_light.pos2 = {light->vec2.x, light->vec2.y, light->vec2.z};
                    gpu_light.spot_dir = {0.0f, 0.0f, 0.0f};
                    gpu_light.spot_fov1_dot = 0.0f;
                    gpu_light.spot_fov2_dot = 0.0f;
                    gpu_light.spot_atten = 0.0f;
                    gpu_light.spot_sq_falloff = 0.0f;
                } else {
                    gpu_light.light_type = 0.0f;
                    gpu_light.spot_dir = {0.0f, 0.0f, 0.0f};
                    gpu_light.spot_fov1_dot = 0.0f;
                    gpu_light.spot_fov2_dot = 0.0f;
                    gpu_light.spot_atten = 0.0f;
                    gpu_light.spot_sq_falloff = 0.0f;
                }
                gpu_light.atten_algo = static_cast<float>(light->attenuation_algorithm);
                gpu_index++;
            }
            data.num_point_lights = static_cast<float>(gpu_index);
        }
        else {
            data.ambient_light = {1.0f, 1.0f, 1.0f};
            data.num_point_lights = 0.0f;
        }

        std::memcpy(mapped_subres.pData, &data, sizeof(data));

        device_context->Unmap(buffer_, 0);
    }

    struct alignas(16) RenderModeBufferData
    {
        std::array<float, 4> current_color;
        float alpha_test;
        float fog_far;
        float colorblind_mode;
        float disable_textures;
        std::array<float, 3> fog_color;
        float use_dynamic_lighting;
        float self_illumination;
        float light_scale;
        float dynamic_light_ndotl;
        float pixel_light_overbright;
        float emissive_override;
        float gas_fog_allowed;
        float _pad[2];
    };
    static_assert(sizeof(RenderModeBufferData) % 16 == 0);

    RenderModeBuffer::RenderModeBuffer(ID3D11Device* device)
    {
        CD3D11_BUFFER_DESC desc{
            sizeof(RenderModeBufferData),
            D3D11_BIND_CONSTANT_BUFFER,
            D3D11_USAGE_DYNAMIC,
            D3D11_CPU_ACCESS_WRITE,
        };
        DF_GR_D3D11_CHECK_HR(device->CreateBuffer(&desc, nullptr, &buffer_));
    }

    struct alignas(16) TextureScaleBufferData
    {
        std::array<float, 2> tex0_uv_scale;
        std::array<float, 2> pad;
    };
    static_assert(sizeof(TextureScaleBufferData) % 16 == 0);

    TextureScaleBuffer::TextureScaleBuffer(ID3D11Device* device)
    {
        TextureScaleBufferData init_data{};
        init_data.tex0_uv_scale = {1.0f, 1.0f};
        init_data.pad = {0.0f, 0.0f};
        D3D11_SUBRESOURCE_DATA subres_data{&init_data, 0, 0};
        CD3D11_BUFFER_DESC desc{
            sizeof(TextureScaleBufferData),
            D3D11_BIND_CONSTANT_BUFFER,
            D3D11_USAGE_DYNAMIC,
            D3D11_CPU_ACCESS_WRITE,
        };
        DF_GR_D3D11_CHECK_HR(device->CreateBuffer(&desc, &subres_data, &buffer_));
    }

    void TextureScaleBuffer::update_buffer(ID3D11DeviceContext* device_context)
    {
        TextureScaleBufferData data{};
        data.tex0_uv_scale = {current_u_scale_, current_v_scale_};
        data.pad = {0.0f, 0.0f};

        D3D11_MAPPED_SUBRESOURCE mapped_subres;
        DF_GR_D3D11_CHECK_HR(
            device_context->Map(buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_subres)
        );
        std::memcpy(mapped_subres.pData, &data, sizeof(data));
        device_context->Unmap(buffer_, 0);
    }

    struct alignas(16) GasRegionGPUData
    {
        std::array<float, 3> center;  float density;        // 1 float4
        std::array<float, 3> color;   float shape;          // 1 float4 (0=sphere, 1=box)
        std::array<float, 3> extents; float _pad0;          // 1 float4
        std::array<float, 3> orient_r0; float _pad1;        // 1 float4 (transpose row 0)
        std::array<float, 3> orient_r1; float _pad2;        // 1 float4 (transpose row 1)
        std::array<float, 3> orient_r2; float _pad3;        // 1 float4 (transpose row 2)
    };
    static_assert(sizeof(GasRegionGPUData) == 96);
    static_assert(sizeof(GasRegionGPUData) % 16 == 0);

    struct alignas(16) GasRegionBufferData
    {
        std::array<float, 3> eye_pos; int num_gas_regions;          // 1 float4
        // Camera params for reconstructing world pos from screen pos + view depth
        std::array<float, 3> cam_right;   float proj_scale_x;      // 1 float4
        std::array<float, 3> cam_up;      float proj_scale_y;      // 1 float4
        std::array<float, 3> cam_forward; float viewport_w;        // 1 float4
        float viewport_h; float _header_pad[3];                     // 1 float4
        GasRegionGPUData regions[GasRegionBuffer::max_gas_regions];
    };
    static_assert(sizeof(GasRegionBufferData) % 16 == 0);

    GasRegionBuffer::GasRegionBuffer(ID3D11Device* device)
    {
        CD3D11_BUFFER_DESC desc{
            sizeof(GasRegionBufferData),
            D3D11_BIND_CONSTANT_BUFFER,
            D3D11_USAGE_DYNAMIC,
            D3D11_CPU_ACCESS_WRITE,
        };
        DF_GR_D3D11_CHECK_HR(device->CreateBuffer(&desc, nullptr, &buffer_));
    }

    void GasRegionBuffer::update(ID3D11DeviceContext* device_context, const Projection& projection)
    {
        const auto& gas_regions = gas_region_get_all();

        // Count enabled regions to allow early-out and shader variant selection
        int enabled_count = 0;
        for (const auto& r : gas_regions) {
            if (r.enabled) ++enabled_count;
        }
        current_gas_count_ = enabled_count;

        if (enabled_count == 0) {
            // Write num_gas_regions=0 so stale gas data never renders if the shader
            // variant selection and buffer update are momentarily out of sync.
            // WRITE_DISCARD invalidates the entire buffer, but the shader checks
            // num_gas_regions before accessing any region data.
            D3D11_MAPPED_SUBRESOURCE mapped_subres;
            DF_GR_D3D11_CHECK_HR(
                device_context->Map(buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_subres)
            );
            std::memset(mapped_subres.pData, 0, sizeof(int) * 4); // zero first float4 (includes num_gas_regions)
            device_context->Unmap(buffer_, 0);
            return;
        }

        GasRegionBufferData data{};
        data.eye_pos = {rf::gr::eye_pos.x, rf::gr::eye_pos.y, rf::gr::eye_pos.z};

        // Camera orientation for world pos reconstruction from pre-transformed vertices
        const auto& m = rf::gr::eye_matrix;
        data.cam_right = {m.rvec.x, m.rvec.y, m.rvec.z};
        data.cam_up = {m.uvec.x, m.uvec.y, m.uvec.z};
        data.cam_forward = {m.fvec.x, m.fvec.y, m.fvec.z};
        data.proj_scale_x = projection.scale_x();
        data.proj_scale_y = projection.scale_y();
        data.viewport_w = static_cast<float>(rf::gr::screen.clip_width);
        data.viewport_h = static_cast<float>(rf::gr::screen.clip_height);

        int gpu_index = 0;
        for (size_t i = 0; i < gas_regions.size() && gpu_index < max_gas_regions; i++) {
            const auto& src = gas_regions[i];
            if (!src.enabled) {
                continue;
            }
            auto& dst = data.regions[gpu_index];

            dst.center = {src.pos.x, src.pos.y, src.pos.z};
            dst.density = src.density;
            dst.color = {src.color.red / 255.0f, src.color.green / 255.0f, src.color.blue / 255.0f};
            dst.shape = (src.shape == 2) ? 1.0f : 0.0f; // 0=sphere, 1=box

            if (src.shape == 1) { // sphere
                dst.extents = {src.radius, src.radius, src.radius};
            } else { // box: RFL stores height, width, depth
                dst.extents = {src.width / 2.0f, src.height / 2.0f, src.depth / 2.0f};
            }

            // Store orient transpose (inverse for orthonormal) for world-to-local transform
            // Matrix3 columns: rvec (local X), uvec (local Y), fvec (local Z) in world space
            // Transpose rows = original columns
            const auto& o = src.orient;
            dst.orient_r0 = {o.rvec.x, o.rvec.y, o.rvec.z};
            dst.orient_r1 = {o.uvec.x, o.uvec.y, o.uvec.z};
            dst.orient_r2 = {o.fvec.x, o.fvec.y, o.fvec.z};

            gpu_index++;
        }
        data.num_gas_regions = gpu_index;

        // Sort regions front-to-back by signed distance from camera to nearest
        // surface so the pixel shader's sequential compositing layers correctly.
        // Signed distance: positive = camera outside (distance to surface),
        // negative = camera inside (penetration depth). This gives a natural
        // tiebreak when inside multiple overlapping regions: deeper inside sorts first.
        auto signed_surface_dist = [&](const GasRegionGPUData& r) -> float {
            if (r.shape < 0.5f) {
                // Sphere: dist_to_center - radius (negative when inside)
                float dist_sq = 0.0f;
                for (int k = 0; k < 3; k++) {
                    float v = r.center[k] - data.eye_pos[k];
                    dist_sq += v * v;
                }
                return std::sqrt(dist_sq) - r.extents[0];
            } else {
                // OBB: project camera into local space. If outside, return distance
                // to nearest surface. If inside, return negative penetration depth.
                float delta[3];
                for (int k = 0; k < 3; k++)
                    delta[k] = data.eye_pos[k] - r.center[k];
                const float* rows[3] = {r.orient_r0.data(), r.orient_r1.data(), r.orient_r2.data()};
                float outside_dist_sq = 0.0f;
                float min_penetration = std::numeric_limits<float>::max();
                bool is_inside = true;
                for (int axis = 0; axis < 3; axis++) {
                    float proj = delta[0] * rows[axis][0] + delta[1] * rows[axis][1] + delta[2] * rows[axis][2];
                    float excess = std::abs(proj) - r.extents[axis];
                    if (excess > 0.0f) {
                        is_inside = false;
                        outside_dist_sq += excess * excess;
                    } else {
                        min_penetration = std::min(min_penetration, -excess);
                    }
                }
                return is_inside ? -min_penetration : std::sqrt(outside_dist_sq);
            }
        };
        std::sort(data.regions, data.regions + gpu_index,
            [&](const GasRegionGPUData& a, const GasRegionGPUData& b) {
                return signed_surface_dist(a) < signed_surface_dist(b);
            });

        D3D11_MAPPED_SUBRESOURCE mapped_subres;
        DF_GR_D3D11_CHECK_HR(
            device_context->Map(buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_subres)
        );
        std::memcpy(mapped_subres.pData, &data, sizeof(data));
        device_context->Unmap(buffer_, 0);
    }

    void RenderModeBuffer::update_buffer(ID3D11DeviceContext* device_context)
    {
        RenderModeBufferData data{};
        data.current_color = {
            current_color_.red / 255.0f,
            current_color_.green / 255.0f,
            current_color_.blue / 255.0f,
            current_color_.alpha / 255.0f,
        };
        data.alpha_test = current_alpha_test_ ? current_alpha_test_threshold_ : 0.0f;
        if (!current_fog_allowed_ || !rf::gr::screen.fog_mode) {
            data.fog_far = std::numeric_limits<float>::infinity();
            data.fog_color = {0.0f, 0.0f, 0.0f};
        }
        else {
            data.fog_far = rf::gr::screen.fog_far;
            data.fog_color = {
                static_cast<float>(rf::gr::screen.fog_color.red) / 255.0f,
                static_cast<float>(rf::gr::screen.fog_color.green) / 255.0f,
                static_cast<float>(rf::gr::screen.fog_color.blue) / 255.0f,
            };
        }
        data.colorblind_mode = static_cast<float>(current_colorblind_mode_);
        data.disable_textures = current_lightmap_only_ ? 1.0f : 0.0f;
        data.use_dynamic_lighting = current_dynamic_lighting_ ? 1.0f : 0.0f;
        data.self_illumination = current_self_illumination_;
        // Mesh lighting scale: matches stock vmesh_update_lighting_data behavior.
        // Stock game applies this scale only to static meshes (clutter/items via
        // vmesh_update_lighting_data), NOT to character meshes (skeletal/v3c).
        // When apply_light_scale is false (character meshes), use 1.0 to skip scaling.
        if (current_apply_light_scale_) {
            const auto& level_props = AlpineLevelProperties::instance();
            if (level_props.override_static_mesh_ambient_light_modifier) {
                data.light_scale = level_props.static_mesh_ambient_light_modifier;
            } else {
                data.light_scale = rf::is_multi ? 3.2f : 2.0f;
            }
        } else {
            data.light_scale = 1.0f;
        }
        data.dynamic_light_ndotl = g_alpine_game_config.dynamic_light_ndotl;
        data.pixel_light_overbright = g_level_pixel_light_overbright;
        data.emissive_override = current_emissive_override_ ? 1.0f : 0.0f;
        data.gas_fog_allowed = current_fog_allowed_ ? 1.0f : 0.0f;

        D3D11_MAPPED_SUBRESOURCE mapped_subres;
        DF_GR_D3D11_CHECK_HR(
            device_context->Map(buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_subres)
        );
        std::memcpy(mapped_subres.pData, &data, sizeof(data));
        device_context->Unmap(buffer_, 0);
    }
}
