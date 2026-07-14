#include <cstring>
#include <cmath>
#include <algorithm>
#include <xlog/xlog.h>
#include <common/utils/list-utils.h>
#include <common/utils/string-utils.h>
#include "gr_d3d11.h"
#include "gr_d3d11_entity_shadow.h"
#include "gr_d3d11_context.h"
#include "gr_d3d11_shader.h"
#include "gr_d3d11_mesh.h"
#include "../../rf/entity.h"
#include "../../rf/corpse.h"
#include "../../rf/item.h"
#include "../../rf/player/player.h"
#include "../../rf/vmesh.h"
#include "../../rf/v3d.h"
#include "../../rf/vfx.h"
#include "../../rf/character.h"
#include "../../rf/gr/gr.h"
#include "../../rf/multi.h"
#include "../../rf/os/frametime.h"
#include "../../misc/alpine_settings.h"
#include "../../hud/multi_spectate.h"

namespace gr::d3d11
{
    // ViewProjTransform buffer data layout (must match b1 layout used by shadow VS)
    struct alignas(16) ShadowViewProjData
    {
        GpuMatrix4x3 view_mat;
        GpuMatrix4x4 proj_mat;
    };
    static_assert(sizeof(ShadowViewProjData) % 16 == 0);

    static void normalize_vec3(float& x, float& y, float& z)
    {
        float len = std::sqrt(x * x + y * y + z * z);
        if (len > 0.0001f) {
            x /= len;
            y /= len;
            z /= len;
        }
    }

    static void cross(float ax, float ay, float az, float bx, float by, float bz,
                      float& rx, float& ry, float& rz)
    {
        rx = ay * bz - az * by;
        ry = az * bx - ax * bz;
        rz = ax * by - ay * bx;
    }

    static float dot3(float ax, float ay, float az, float bx, float by, float bz)
    {
        return ax * bx + ay * by + az * bz;
    }

    // Build an orthographic projection matrix (standard Z: near=0, far=1)
    // GpuMatrix4x4 is column-major: [col][row]
    // Translations go in row 3 of each column (4th element), matching existing Projection::matrix() layout
    static GpuMatrix4x4 build_ortho_matrix(float left, float right, float bottom, float top, float near_z, float far_z)
    {
        float w = right - left;
        float h = top - bottom;
        float d = far_z - near_z;
        return {{
            {2.0f / w,   0.0f,       0.0f,          -(right + left) / w},   // column 0
            {0.0f,       2.0f / h,   0.0f,          -(top + bottom) / h},   // column 1
            {0.0f,       0.0f,       1.0f / d,      -near_z / d},           // column 2
            {0.0f,       0.0f,       0.0f,           1.0f},                 // column 3
        }};
    }

    // GpuMatrix4x3 is column-major: [col][row], 3 columns of 4 floats each
    // Same layout as build_view_matrix in gr_d3d11_transform.h
    static GpuMatrix4x3 build_lookat_view_matrix(
        float eye_x, float eye_y, float eye_z,
        float at_x, float at_y, float at_z,
        float up_x, float up_y, float up_z)
    {
        float fwd_x = at_x - eye_x;
        float fwd_y = at_y - eye_y;
        float fwd_z = at_z - eye_z;
        normalize_vec3(fwd_x, fwd_y, fwd_z);

        float right_x, right_y, right_z;
        cross(up_x, up_y, up_z, fwd_x, fwd_y, fwd_z, right_x, right_y, right_z);
        normalize_vec3(right_x, right_y, right_z);

        float real_up_x, real_up_y, real_up_z;
        cross(fwd_x, fwd_y, fwd_z, right_x, right_y, right_z, real_up_x, real_up_y, real_up_z);

        float tx = -dot3(right_x, right_y, right_z, eye_x, eye_y, eye_z);
        float ty = -dot3(real_up_x, real_up_y, real_up_z, eye_x, eye_y, eye_z);
        float tz = -dot3(fwd_x, fwd_y, fwd_z, eye_x, eye_y, eye_z);

        // Column-major: [col][row]
        return {{
            {right_x,   right_y,   right_z,   tx},   // column 0: right + translation.x
            {real_up_x, real_up_y, real_up_z, ty},   // column 1: up + translation.y
            {fwd_x,     fwd_y,     fwd_z,     tz},   // column 2: forward + translation.z
        }};
    }

    // Multiply 4x3 view by 4x4 proj, both column-major [col][row]
    // view is 3 columns of 4 rows (implicit 4th column = {0,0,0,1})
    // proj is 4 columns of 4 rows
    // result is 4 columns of 4 rows
    static GpuMatrix4x4 multiply_view_proj(const GpuMatrix4x3& view, const GpuMatrix4x4& proj)
    {
        // In column-major, mathematical matrix M has M_math[row][col] = storage[col][row]
        // We need VP = V * P in mathematical terms
        // VP_math[r][c] = sum_k(V_math[r][k] * P_math[k][c])
        // storage[c][r] = sum_k(V_storage[k][r] * P_storage[c][k])
        // For V: col k, row r. V has 3 columns; col 3 is implicit {0,0,0,1}
        GpuMatrix4x4 result{};
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    float v_kr = (k < 3) ? view[k][r] : ((r == 3) ? 1.0f : 0.0f);
                    sum += v_kr * proj[c][k];
                }
                result[c][r] = sum;
            }
        }
        return result;
    }

    EntityShadowRenderer::EntityShadowRenderer(ID3D11Device* device, ShaderManager& shader_manager, MeshRenderer& mesh_renderer)
        : device_{device}, shader_manager_{shader_manager}, mesh_renderer_{mesh_renderer}
    {
        int quality = std::clamp(g_alpine_game_config.shadow_quality, 0, num_shadow_quality_presets - 1);
        current_quality_ = quality;
        int resolution = shadow_quality_presets[quality].resolution;
        if (resolution > 0) {
            current_resolution_ = resolution;
            create_resources(current_resolution_);
        }
    }

    EntityShadowRenderer::~EntityShadowRenderer() = default;

    void EntityShadowRenderer::create_resources(int resolution)
    {
        D3D11_TEXTURE2D_DESC tex_desc{};
        tex_desc.Width = resolution;
        tex_desc.Height = resolution;
        tex_desc.MipLevels = 1;
        tex_desc.ArraySize = 1;
        tex_desc.Format = DXGI_FORMAT_R32_TYPELESS;
        tex_desc.SampleDesc.Count = 1;
        tex_desc.Usage = D3D11_USAGE_DEFAULT;
        tex_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        DF_GR_D3D11_CHECK_HR(device_->CreateTexture2D(&tex_desc, nullptr, &shadow_map_texture_));

        D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
        dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
        dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        DF_GR_D3D11_CHECK_HR(device_->CreateDepthStencilView(shadow_map_texture_, &dsv_desc, &shadow_dsv_));

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;
        DF_GR_D3D11_CHECK_HR(device_->CreateShaderResourceView(shadow_map_texture_, &srv_desc, &shadow_srv_));

        D3D11_SAMPLER_DESC samp_desc{};
        samp_desc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
        samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
        samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        samp_desc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
        samp_desc.BorderColor[0] = 1.0f;
        samp_desc.BorderColor[1] = 1.0f;
        samp_desc.BorderColor[2] = 1.0f;
        samp_desc.BorderColor[3] = 1.0f;
        DF_GR_D3D11_CHECK_HR(device_->CreateSamplerState(&samp_desc, &shadow_comparison_sampler_));

        // Point sampler for reading shadow map depth values (no comparison).
        // POINT gives true per-texel depths without bilinear blending, which is
        // critical for projection fade: bilinear would blend caster depth with
        // cleared background (1.0) at shadow edges, producing unreliable values.
        D3D11_SAMPLER_DESC depth_samp_desc{};
        depth_samp_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        depth_samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
        depth_samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
        depth_samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
        depth_samp_desc.BorderColor[0] = 1.0f;
        depth_samp_desc.BorderColor[1] = 1.0f;
        depth_samp_desc.BorderColor[2] = 1.0f;
        depth_samp_desc.BorderColor[3] = 1.0f;
        DF_GR_D3D11_CHECK_HR(device_->CreateSamplerState(&depth_samp_desc, &shadow_depth_sampler_));

        D3D11_RASTERIZER_DESC rast_desc{};
        rast_desc.FillMode = D3D11_FILL_SOLID;
        rast_desc.CullMode = D3D11_CULL_NONE;
        rast_desc.FrontCounterClockwise = FALSE;
        rast_desc.DepthBias = 0;
        rast_desc.DepthBiasClamp = 0.0f;
        rast_desc.SlopeScaledDepthBias = 0.0f;
        rast_desc.DepthClipEnable = TRUE;
        DF_GR_D3D11_CHECK_HR(device_->CreateRasterizerState(&rast_desc, &shadow_rasterizer_state_));

        D3D11_DEPTH_STENCIL_DESC ds_desc{};
        ds_desc.DepthEnable = TRUE;
        ds_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ds_desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        ds_desc.StencilEnable = FALSE;
        DF_GR_D3D11_CHECK_HR(device_->CreateDepthStencilState(&ds_desc, &shadow_depth_stencil_state_));

        D3D11_BUFFER_DESC cb_desc{};
        cb_desc.ByteWidth = sizeof(ShadowConstantBuffer);
        cb_desc.Usage = D3D11_USAGE_DYNAMIC;
        cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        DF_GR_D3D11_CHECK_HR(device_->CreateBuffer(&cb_desc, nullptr, &shadow_cbuffer_));

        D3D11_BUFFER_DESC vp_cb_desc{};
        vp_cb_desc.ByteWidth = sizeof(ShadowViewProjData);
        vp_cb_desc.Usage = D3D11_USAGE_DYNAMIC;
        vp_cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        vp_cb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        DF_GR_D3D11_CHECK_HR(device_->CreateBuffer(&vp_cb_desc, nullptr, &shadow_vp_cbuffer_));

        xlog::info("Entity shadow map created: {}x{}", resolution, resolution);
    }

    void EntityShadowRenderer::apply_quality(int quality)
    {
        quality = std::clamp(quality, 0, num_shadow_quality_presets - 1);
        if (quality == current_quality_) return;
        current_quality_ = quality;

        int resolution = shadow_quality_presets[quality].resolution;
        if (resolution == 0) {
            // Blob shadows — release shadow map to free GPU memory
            shadow_map_texture_.release();
            shadow_dsv_.release();
            shadow_srv_.release();
            return;
        }

        // If per-device resources (cbuffer, samplers, states) were never created
        // (e.g. started at quality 0), do a full create_resources
        if (!shadow_cbuffer_) {
            current_resolution_ = resolution;
            create_resources(current_resolution_);
            return;
        }

        if (resolution == current_resolution_ && shadow_map_texture_) {
            // Same resolution, resources already exist
            return;
        }

        current_resolution_ = resolution;

        shadow_map_texture_.release();
        shadow_dsv_.release();
        shadow_srv_.release();

        D3D11_TEXTURE2D_DESC tex_desc{};
        tex_desc.Width = current_resolution_;
        tex_desc.Height = current_resolution_;
        tex_desc.MipLevels = 1;
        tex_desc.ArraySize = 1;
        tex_desc.Format = DXGI_FORMAT_R32_TYPELESS;
        tex_desc.SampleDesc.Count = 1;
        tex_desc.Usage = D3D11_USAGE_DEFAULT;
        tex_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        DF_GR_D3D11_CHECK_HR(device_->CreateTexture2D(&tex_desc, nullptr, &shadow_map_texture_));

        D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
        dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
        dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        DF_GR_D3D11_CHECK_HR(device_->CreateDepthStencilView(shadow_map_texture_, &dsv_desc, &shadow_dsv_));

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = DXGI_FORMAT_R32_FLOAT;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;
        DF_GR_D3D11_CHECK_HR(device_->CreateShaderResourceView(shadow_map_texture_, &srv_desc, &shadow_srv_));

        xlog::info("Shadow map resized: {}x{}", current_resolution_, current_resolution_);
    }

    void EntityShadowRenderer::build_shadow_view_proj(ID3D11DeviceContext* context, const rf::Vector3& camera_pos)
    {
        current_camera_pos_ = camera_pos;

        float ld_x = light_dir_x;
        float ld_y = light_dir_y;
        float ld_z = light_dir_z;
        normalize_vec3(ld_x, ld_y, ld_z);

        float up_x = 0.0f, up_y = 1.0f, up_z = 0.0f;
        if (std::abs(ld_y) > 0.99f) {
            up_x = 0.0f;
            up_y = 0.0f;
            up_z = 1.0f;
        }

        // Build light-space axes (right, up, forward) for texel snapping
        float fwd_x = ld_x, fwd_y = ld_y, fwd_z = ld_z;
        float right_x, right_y, right_z;
        cross(up_x, up_y, up_z, fwd_x, fwd_y, fwd_z, right_x, right_y, right_z);
        normalize_vec3(right_x, right_y, right_z);
        float real_up_x, real_up_y, real_up_z;
        cross(fwd_x, fwd_y, fwd_z, right_x, right_y, right_z, real_up_x, real_up_y, real_up_z);

        // Ortho extents cover the shadow fade area plus margin
        int dist_preset = std::clamp(g_alpine_game_config.shadow_distance, 0, num_shadow_distance_presets - 1);
        float fade_end = shadow_distance_presets[dist_preset].fade_end;
        float extent = fade_end * 1.2f;
        float depth_range = fade_end * 4.0f;

        // Snap the shadow frustum center to texel boundaries to prevent shadow swimming
        // World-space size of one shadow map texel
        float texel_world_size = (extent * 2.0f) / static_cast<float>(current_resolution_);

        // Project camera position onto light-space right and up axes
        float cam_right = dot3(camera_pos.x, camera_pos.y, camera_pos.z, right_x, right_y, right_z);
        float cam_up = dot3(camera_pos.x, camera_pos.y, camera_pos.z, real_up_x, real_up_y, real_up_z);

        // Snap to nearest texel
        cam_right = std::floor(cam_right / texel_world_size) * texel_world_size;
        cam_up = std::floor(cam_up / texel_world_size) * texel_world_size;

        // Reconstruct snapped center in world space (keep forward component from original camera pos)
        float cam_fwd = dot3(camera_pos.x, camera_pos.y, camera_pos.z, fwd_x, fwd_y, fwd_z);
        float snapped_x = right_x * cam_right + real_up_x * cam_up + fwd_x * cam_fwd;
        float snapped_y = right_y * cam_right + real_up_y * cam_up + fwd_y * cam_fwd;
        float snapped_z = right_z * cam_right + real_up_z * cam_up + fwd_z * cam_fwd;

        float far_offset = fade_end * 2.0f;
        float eye_x = snapped_x - ld_x * far_offset;
        float eye_y = snapped_y - ld_y * far_offset;
        float eye_z = snapped_z - ld_z * far_offset;

        GpuMatrix4x3 view_mat = build_lookat_view_matrix(
            eye_x, eye_y, eye_z,
            snapped_x, snapped_y, snapped_z,
            up_x, up_y, up_z
        );

        GpuMatrix4x4 proj_mat = build_ortho_matrix(-extent, extent, -extent, extent, 0.0f, depth_range);

        current_depth_range_ = depth_range;
        shadow_vp_matrix_ = multiply_view_proj(view_mat, proj_mat);

        ShadowViewProjData vp_data;
        vp_data.view_mat = view_mat;
        vp_data.proj_mat = proj_mat;

        D3D11_MAPPED_SUBRESOURCE mapped;
        DF_GR_D3D11_CHECK_HR(context->Map(shadow_vp_cbuffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        std::memcpy(mapped.pData, &vp_data, sizeof(vp_data));
        context->Unmap(shadow_vp_cbuffer_, 0);
    }

    void EntityShadowRenderer::generate_shadow_map(ID3D11DeviceContext* context, RenderContext& render_context, const rf::Vector3& camera_pos)
    {
        // Only generate once per frame — render_solid is called multiple times
        // per frame for different room/portal groups
        if (rf::frame_count == last_frame_) return;
        last_frame_ = rf::frame_count;

        // Respect the stock game's ShowShadows toggle
        if (rf::local_player && !rf::local_player->settings.shadows_enabled) return;

        int quality = std::clamp(g_alpine_game_config.shadow_quality, 0, num_shadow_quality_presets - 1);
        if (shadow_quality_presets[quality].resolution == 0) return; // blob shadows only

        // Check if quality changed (e.g. via console command)
        if (quality != current_quality_) {
            apply_quality(quality);
            last_shadow_render_frame_ = -1; // force re-render after texture recreation
        }

        current_camera_pos_ = camera_pos;

        // Quick pre-scan: skip entire shadow pass if no casters are in range.
        // Mirrors the same filters used by the actual render functions to avoid
        // false positives (e.g. local player entity is always at distance 0).
        {
            int dp = std::clamp(g_alpine_game_config.shadow_distance, 0, num_shadow_distance_presets - 1);
            float fade_end = shadow_distance_presets[dp].fade_end;
            float fade_end_sq = fade_end * fade_end;
            bool found_caster = false;

            // Determine which entities to skip (matches render_entity_shadow)
            int local_entity_handle = -1;
            if (rf::local_player) {
                local_entity_handle = rf::local_player->entity_handle;
            }
            int spectate_entity_handle = -1;
            if (multi_spectate_is_first_person()) {
                rf::Player* spectate_target = multi_spectate_get_target_player();
                if (spectate_target) {
                    spectate_entity_handle = spectate_target->entity_handle;
                }
            }

            for (auto& entity : DoublyLinkedList{rf::entity_list}) {
                if (entity.handle == local_entity_handle) continue;
                if (entity.handle == spectate_entity_handle) continue;
                if (entity.entity_flags2 & rf::EF2_NO_SHADOW) continue;
                if (entity.obj_flags & (rf::OF_DELAYED_DELETE | rf::OF_HIDDEN)) continue;
                if (!entity.vmesh) continue;
                float dx = entity.pos.x - camera_pos.x;
                float dy = entity.pos.y - camera_pos.y;
                float dz = entity.pos.z - camera_pos.z;
                if (dx * dx + dy * dy + dz * dz <= fade_end_sq) { found_caster = true; break; }
            }

            if (!found_caster && g_alpine_game_config.shadow_corpses) {
                for (auto& corpse : DoublyLinkedList{rf::corpse_list}) {
                    if (corpse.obj_flags & (rf::OF_DELAYED_DELETE | rf::OF_HIDDEN)) continue;
                    if (!corpse.vmesh) continue;
                    float dx = corpse.pos.x - camera_pos.x;
                    float dy = corpse.pos.y - camera_pos.y;
                    float dz = corpse.pos.z - camera_pos.z;
                    if (dx * dx + dy * dy + dz * dz <= fade_end_sq) { found_caster = true; break; }
                }
            }

            if (!found_caster && g_alpine_game_config.shadow_items) {
                for (auto& item : DoublyLinkedList{rf::item_list}) {
                    if (item.obj_flags & (rf::OF_DELAYED_DELETE | rf::OF_HIDDEN)) continue;
                    if (!item.vmesh) continue;
                    float dx = item.pos.x - camera_pos.x;
                    float dy = item.pos.y - camera_pos.y;
                    float dz = item.pos.z - camera_pos.z;
                    if (dx * dx + dy * dy + dz * dz <= fade_end_sq) { found_caster = true; break; }
                }
            }

            last_frame_had_casters_ = found_caster;
            if (!found_caster) return;
        }

        // Frame lag: reuse the cached shadow map (and its VP matrix) if rendered recently enough
        int frame_lag = std::clamp(g_alpine_game_config.shadow_frame_lag, 1, 30);
        if (last_shadow_render_frame_ >= 0 &&
            (rf::frame_count - last_shadow_render_frame_) < frame_lag) {
            return;
        }
        last_shadow_render_frame_ = rf::frame_count;

        build_shadow_view_proj(context, camera_pos);

        // Unbind shadow SRV to avoid resource hazard
        ID3D11ShaderResourceView* null_srv = nullptr;
        context->PSSetShaderResources(2, 1, &null_srv);

        // Set shadow viewport (shared by both maps)
        D3D11_VIEWPORT shadow_vp{};
        shadow_vp.Width = static_cast<float>(current_resolution_);
        shadow_vp.Height = static_cast<float>(current_resolution_);
        shadow_vp.MinDepth = 0.0f;
        shadow_vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &shadow_vp);

        // Set shadow-specific states (shared by both passes)
        context->RSSetState(shadow_rasterizer_state_);
        context->OMSetDepthStencilState(shadow_depth_stencil_state_, 0);
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context->PSSetShader(nullptr, nullptr, 0);

        // Bind shadow VP constant buffer at b1
        ID3D11Buffer* vp_cbuffers[] = { shadow_vp_cbuffer_ };
        context->VSSetConstantBuffers(1, 1, vp_cbuffers);

        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // Render all shadow casters (entities, corpses, items) to the shadow map.
        ID3D11RenderTargetView* null_rtv = nullptr;
        context->OMSetRenderTargets(1, &null_rtv, shadow_dsv_);
        context->ClearDepthStencilView(shadow_dsv_, D3D11_CLEAR_DEPTH, 1.0f, 0);

        render_entity_shadow(context, render_context);
        if (g_alpine_game_config.shadow_corpses) {
            render_corpse_shadow(context, render_context);
        }
        if (g_alpine_game_config.shadow_items) {
            render_item_shadow(context, render_context);
        }

        // Unbind shadow DSV so it can be bound as SRV later
        context->OMSetRenderTargets(1, &null_rtv, nullptr);

        // Invalidate render context cached state so the main pass re-binds everything
        render_context.invalidate_cached_state();
    }

    void EntityShadowRenderer::render_entity_shadow(ID3D11DeviceContext* context, RenderContext& render_context)
    {
        const auto& shadow_standard_vs = shader_manager_.get_vertex_shader(VertexShaderId::shadow_standard);
        const auto& shadow_character_vs = shader_manager_.get_vertex_shader(VertexShaderId::shadow_character);

        // Get local player entity handle to skip self-shadow
        int local_entity_handle = -1;
        if (rf::local_player) {
            local_entity_handle = rf::local_player->entity_handle;
        }

        // Get spectated player entity handle to skip their shadow in first-person spectate
        int spectate_entity_handle = -1;
        if (multi_spectate_is_first_person()) {
            rf::Player* spectate_target = multi_spectate_get_target_player();
            if (spectate_target) {
                spectate_entity_handle = spectate_target->entity_handle;
            }
        }

        int dp = std::clamp(g_alpine_game_config.shadow_distance, 0, num_shadow_distance_presets - 1);
        float fade_end = shadow_distance_presets[dp].fade_end;
        float fade_end_sq = fade_end * fade_end;

        for (auto& entity : DoublyLinkedList{rf::entity_list}) {
            if (entity.handle == local_entity_handle) continue;
            if (entity.handle == spectate_entity_handle) continue;
            if (entity.entity_flags2 & rf::EF2_NO_SHADOW) continue;
            if (entity.obj_flags & (rf::OF_DELAYED_DELETE | rf::OF_HIDDEN)) continue;
            if (!entity.vmesh) continue;

            float dx = entity.pos.x - current_camera_pos_.x;
            float dy = entity.pos.y - current_camera_pos_.y;
            float dz = entity.pos.z - current_camera_pos_.z;
            float dist_sq = dx * dx + dy * dy + dz * dz;
            if (dist_sq > fade_end_sq) continue;

            rf::VMesh* vmesh = entity.vmesh;
            if (vmesh->type == rf::MESH_TYPE_CHARACTER) {
                auto* ci = static_cast<rf::CharacterInstance*>(vmesh->instance);
                if (!ci || !ci->base_character) continue;
                if (ci->base_character->num_character_meshes < 1) continue;

                rf::V3dMesh* v3d_mesh = ci->base_character->character_meshes[0].mesh;
                if (!v3d_mesh || !v3d_mesh->vu) continue;

                mesh_renderer_.draw_shadow_character_mesh(v3d_mesh->vu, entity.pos, entity.orient, ci, shadow_character_vs, context);
                ID3D11Buffer* vp_cb[] = { shadow_vp_cbuffer_ };
                context->VSSetConstantBuffers(1, 1, vp_cb);
            }
            else if (vmesh->type == rf::MESH_TYPE_STATIC) {
                auto* v3d = static_cast<rf::V3d*>(vmesh->instance);
                if (!v3d || v3d->num_meshes < 1 || !v3d->meshes) continue;

                for (int m = 0; m < v3d->num_meshes; ++m) {
                    rf::VifLodMesh* lod_mesh = v3d->meshes[m].vu;
                    if (!lod_mesh) continue;
                    mesh_renderer_.draw_shadow_v3d_mesh(lod_mesh, entity.pos, entity.orient, shadow_standard_vs, context);
                }
                ID3D11Buffer* vp_cb[] = { shadow_vp_cbuffer_ };
                context->VSSetConstantBuffers(1, 1, vp_cb);
            }
        }
    }

    void EntityShadowRenderer::render_corpse_shadow(ID3D11DeviceContext* context, RenderContext& render_context)
    {
        const auto& shadow_standard_vs = shader_manager_.get_vertex_shader(VertexShaderId::shadow_standard);
        const auto& shadow_character_vs = shader_manager_.get_vertex_shader(VertexShaderId::shadow_character);

        int dp = std::clamp(g_alpine_game_config.shadow_distance, 0, num_shadow_distance_presets - 1);
        float fade_end = shadow_distance_presets[dp].fade_end;
        float fade_end_sq = fade_end * fade_end;

        for (auto& corpse : DoublyLinkedList{rf::corpse_list}) {
            if (corpse.obj_flags & (rf::OF_DELAYED_DELETE | rf::OF_HIDDEN)) continue;
            if (!corpse.vmesh) continue;

            float dx = corpse.pos.x - current_camera_pos_.x;
            float dy = corpse.pos.y - current_camera_pos_.y;
            float dz = corpse.pos.z - current_camera_pos_.z;
            float dist_sq = dx * dx + dy * dy + dz * dz;
            if (dist_sq > fade_end_sq) continue;

            rf::VMesh* vmesh = corpse.vmesh;
            if (vmesh->type == rf::MESH_TYPE_CHARACTER) {
                auto* ci = static_cast<rf::CharacterInstance*>(vmesh->instance);
                if (!ci || !ci->base_character) continue;
                if (ci->base_character->num_character_meshes < 1) continue;

                rf::V3dMesh* v3d_mesh = ci->base_character->character_meshes[0].mesh;
                if (!v3d_mesh || !v3d_mesh->vu) continue;

                mesh_renderer_.draw_shadow_character_mesh(v3d_mesh->vu, corpse.pos, corpse.orient, ci, shadow_character_vs, context);
                ID3D11Buffer* vp_cb[] = { shadow_vp_cbuffer_ };
                context->VSSetConstantBuffers(1, 1, vp_cb);
            }
            else if (vmesh->type == rf::MESH_TYPE_STATIC) {
                auto* v3d = static_cast<rf::V3d*>(vmesh->instance);
                if (!v3d || v3d->num_meshes < 1 || !v3d->meshes) continue;

                for (int m = 0; m < v3d->num_meshes; ++m) {
                    rf::VifLodMesh* lod_mesh = v3d->meshes[m].vu;
                    if (!lod_mesh) continue;
                    mesh_renderer_.draw_shadow_v3d_mesh(lod_mesh, corpse.pos, corpse.orient, shadow_standard_vs, context);
                }
                ID3D11Buffer* vp_cb[] = { shadow_vp_cbuffer_ };
                context->VSSetConstantBuffers(1, 1, vp_cb);
            }
        }
    }

    void EntityShadowRenderer::render_item_shadow(ID3D11DeviceContext* context, RenderContext& render_context)
    {
        const auto& shadow_standard_vs = shader_manager_.get_vertex_shader(VertexShaderId::shadow_standard);

        int dp = std::clamp(g_alpine_game_config.shadow_distance, 0, num_shadow_distance_presets - 1);
        float fade_end = shadow_distance_presets[dp].fade_end;
        float fade_end_sq = fade_end * fade_end;

        // Determine which player's carried CTF flag should have its shadow hidden
        rf::Player* first_person_player = nullptr;
        if (multi_spectate_is_first_person()) {
            first_person_player = multi_spectate_get_target_player();
        }
        else if (rf::local_player) {
            first_person_player = rf::local_player;
        }

        for (auto& item : DoublyLinkedList{rf::item_list}) {
            if (item.obj_flags & (rf::OF_DELAYED_DELETE | rf::OF_HIDDEN)) continue;
            if (!item.vmesh) continue;

            // Skip item classes that shouldn't cast shadows (banners, uniforms, effects, etc.)
            if (item.info) {
                const char* cls = item.info->cls_name;
                if (string_iequals(cls, "CTF Banner Red") ||
                    string_iequals(cls, "CTF Banner Blue") ||
                    string_iequals(cls, "Doctor Uniform") ||
                    string_iequals(cls, "Brainstem") ||
                    string_iequals(cls, "keycard") ||
                    string_iequals(cls, "Demo_K000"))
                    continue;
            }

            // Skip CTF flag shadow if carried by the first-person player (self or spectated)
            if (first_person_player &&
                ((&item == rf::ctf_red_flag_item && rf::multi_ctf_get_red_flag_player() == first_person_player) ||
                 (&item == rf::ctf_blue_flag_item && rf::multi_ctf_get_blue_flag_player() == first_person_player))) {
                continue;
            }

            float dx = item.pos.x - current_camera_pos_.x;
            float dy = item.pos.y - current_camera_pos_.y;
            float dz = item.pos.z - current_camera_pos_.z;
            float dist_sq = dx * dx + dy * dy + dz * dz;
            if (dist_sq > fade_end_sq) continue;

            // Spinning items use a fresh orientation from spin_angle (matching stock renderer)
            rf::Matrix3 orient;
            if (item.info && (item.info->flags & rf::IIF_SPINS_IN_MULTI)) {
                orient.set_from_angles(0.0f, 0.0f, -item.spin_angle);
            } else {
                orient = item.orient;
            }

            rf::VMesh* vmesh = item.vmesh;
            if (vmesh->type == rf::MESH_TYPE_STATIC) {
                auto* v3d = static_cast<rf::V3d*>(vmesh->instance);
                if (!v3d || v3d->num_meshes < 1 || !v3d->meshes) continue;

                for (int m = 0; m < v3d->num_meshes; ++m) {
                    rf::VifLodMesh* lod_mesh = v3d->meshes[m].vu;
                    if (!lod_mesh) continue;
                    mesh_renderer_.draw_shadow_v3d_mesh(lod_mesh, item.pos, orient, shadow_standard_vs, context);
                }
                ID3D11Buffer* vp_cb[] = { shadow_vp_cbuffer_ };
                context->VSSetConstantBuffers(1, 1, vp_cb);
            }
            else if (vmesh->type == rf::MESH_TYPE_ANIM_FX) {
                auto* vfx_inst = static_cast<rf::VfxInstance*>(vmesh->instance);
                if (!vfx_inst) continue;
                // Match FUN_0054d0a0 flag check: skip if bit 2 set and bit 3 not set
                if ((vfx_inst->flags & 4) != 0 && (vfx_inst->flags & 8) == 0) continue;

                auto* vfx_geo = static_cast<rf::VfxGeo*>(vmesh->mesh);
                if (!vfx_geo || vfx_geo->num_sfxo_chunks < 1 || !vfx_geo->sfxo_chunks) continue;

                rf::VfxSfxoRenderObj* sfxo_instances = vfx_inst->sfxo_instances;
                if (!sfxo_instances) continue;

                // Count total vertices across all active SFXO chunks
                int total_verts = 0;
                for (int c = 0; c < vfx_geo->num_sfxo_chunks; ++c) {
                    rf::VfxSfxoRenderObj& render_obj = sfxo_instances[c];
                    if (!render_obj.active || !render_obj.vertex_positions) continue;
                    total_verts += vfx_geo->sfxo_chunks[c].num_faces * 3;
                }
                if (total_verts == 0) continue;

                // Ensure dynamic VB is large enough (grow with 2x factor to avoid frequent reallocation)
                if (!vfx_shadow_vb_ || vfx_shadow_vb_capacity_ < total_verts) {
                    vfx_shadow_vb_.release();
                    vfx_shadow_vb_capacity_ = std::max(total_verts, vfx_shadow_vb_capacity_ * 2);
                    D3D11_BUFFER_DESC vb_desc{};
                    vb_desc.ByteWidth = vfx_shadow_vb_capacity_ * sizeof(GpuVertex);
                    vb_desc.Usage = D3D11_USAGE_DYNAMIC;
                    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
                    vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                    DF_GR_D3D11_CHECK_HR(device_->CreateBuffer(&vb_desc, nullptr, &vfx_shadow_vb_));
                }

                // Fill VB with triangle positions from all active SFXO chunks
                D3D11_MAPPED_SUBRESOURCE mapped;
                DF_GR_D3D11_CHECK_HR(context->Map(vfx_shadow_vb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
                auto* gpu_verts = static_cast<GpuVertex*>(mapped.pData);

                int vert_idx = 0;
                for (int c = 0; c < vfx_geo->num_sfxo_chunks; ++c) {
                    rf::VfxSfxoRenderObj& render_obj = sfxo_instances[c];
                    if (!render_obj.active || !render_obj.vertex_positions) continue;

                    const rf::VfxSfxoChunk& chunk = vfx_geo->sfxo_chunks[c];
                    const rf::Vector3* positions = render_obj.vertex_positions;
                    const rf::VfxSubObject* faces = chunk.faces;
                    int num_verts = chunk.num_vertices;

                    for (int i = 0; i < chunk.num_faces; ++i) {
                        for (int j = 0; j < 3; ++j) {
                            int vi = faces[i].vertex_indices[j];
                            GpuVertex& gv = gpu_verts[vert_idx++];
                            gv = {};
                            if (vi >= 0 && vi < num_verts) {
                                gv.x = positions[vi].x;
                                gv.y = positions[vi].y;
                                gv.z = positions[vi].z;
                            }
                        }
                    }
                }
                context->Unmap(vfx_shadow_vb_, 0);

                // Set shadow VS and input layout
                context->IASetInputLayout(shadow_standard_vs.input_layout);
                context->VSSetShader(shadow_standard_vs.vertex_shader, nullptr, 0);

                // Set model transform at b0
                render_context.model_transform_cbuffer().update(item.pos, orient, context);
                ID3D11Buffer* model_cb = render_context.model_transform_cbuffer();
                context->VSSetConstantBuffers(0, 1, &model_cb);

                // Bind VFX dynamic VB and draw (bypasses render_context state cache)
                UINT stride = sizeof(GpuVertex);
                UINT vb_offset = 0;
                ID3D11Buffer* vb = vfx_shadow_vb_;
                context->IASetVertexBuffers(0, 1, &vb, &stride, &vb_offset);
                context->Draw(total_verts, 0);

                // Invalidate render_context VB cache so subsequent static mesh draws rebind correctly
                render_context.set_vertex_buffer(vfx_shadow_vb_, sizeof(GpuVertex), 0);

                // Restore shadow VP at b1
                ID3D11Buffer* vp_cb[] = { shadow_vp_cbuffer_ };
                context->VSSetConstantBuffers(1, 1, vp_cb);
            }
        }
    }

    void EntityShadowRenderer::bind_shadow_resources(ID3D11DeviceContext* context)
    {
        // No resources created (quality 0 from startup) — nothing to bind
        if (!shadow_cbuffer_) return;

        int quality = std::clamp(g_alpine_game_config.shadow_quality, 0, num_shadow_quality_presets - 1);
        bool shadows_active = shadow_quality_presets[quality].resolution > 0;

        // Respect the stock game's ShowShadows toggle
        if (rf::local_player && !rf::local_player->settings.shadows_enabled) {
            shadows_active = false;
        }

        // No casters were found in the pre-scan — disable shadow sampling
        if (!last_frame_had_casters_) {
            shadows_active = false;
        }

        // Compute normalized light direction for PS normal bias
        float ld_x = light_dir_x, ld_y = light_dir_y, ld_z = light_dir_z;
        normalize_vec3(ld_x, ld_y, ld_z);

        int dist_preset = std::clamp(g_alpine_game_config.shadow_distance, 0, num_shadow_distance_presets - 1);

        ShadowConstantBuffer data{};
        data.shadow_vp_mat = shadow_vp_matrix_;
        data.shadow_strength = shadow_strength;
        data.shadow_fade_start = shadow_distance_presets[dist_preset].fade_start;
        data.shadow_fade_end = shadow_distance_presets[dist_preset].fade_end;
        data.shadow_enabled = shadows_active ? 1.0f : 0.0f;
        data.shadow_light_dir[0] = ld_x;
        data.shadow_light_dir[1] = ld_y;
        data.shadow_light_dir[2] = ld_z;
        data.shadow_normal_offset = 0.08f;
        data.shadow_texel_size = 1.0f / static_cast<float>(current_resolution_);
        data.shadow_depth_range = current_depth_range_;
        data.shadow_projection_fade_start = shadow_projection_fade_start;
        data.shadow_projection_fade_end = shadow_projection_fade_end;
        const auto& preset = shadow_quality_presets[quality];
        data.shadow_pcf_taps = static_cast<float>(preset.pcf_taps);
        data.shadow_debug = debug_enabled ? 1.0f : 0.0f;
        data.shadow_soft_edges = preset.soft_edges ? 1.0f : 0.0f;

        D3D11_MAPPED_SUBRESOURCE mapped;
        DF_GR_D3D11_CHECK_HR(context->Map(shadow_cbuffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        std::memcpy(mapped.pData, &data, sizeof(data));
        context->Unmap(shadow_cbuffer_, 0);

        ID3D11Buffer* ps_cb = shadow_cbuffer_;
        context->PSSetConstantBuffers(3, 1, &ps_cb);

        if (shadows_active) {
            // Always bind the comparison sampler
            ID3D11SamplerState* sampler = shadow_comparison_sampler_;
            context->PSSetSamplers(2, 1, &sampler);

            ID3D11SamplerState* depth_sampler = shadow_depth_sampler_;
            context->PSSetSamplers(3, 1, &depth_sampler);

            // Bind shadow map at t2
            ID3D11ShaderResourceView* srv = shadow_srv_;
            context->PSSetShaderResources(2, 1, &srv);
        }
    }

    void EntityShadowRenderer::unbind_shadow_resources(ID3D11DeviceContext* context)
    {
        ID3D11ShaderResourceView* null_srv = nullptr;
        context->PSSetShaderResources(2, 1, &null_srv);
    }

    void EntityShadowRenderer::disable_shadow_rendering(ID3D11DeviceContext* context)
    {
        if (!shadow_cbuffer_) return;

        // Set shadow_enabled to 0 in the constant buffer so the pixel shader
        // skips shadow sampling entirely (no null SRV reads, no debug warnings)
        ShadowConstantBuffer data{};
        data.shadow_enabled = 0.0f;

        D3D11_MAPPED_SUBRESOURCE mapped;
        DF_GR_D3D11_CHECK_HR(context->Map(shadow_cbuffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        std::memcpy(mapped.pData, &data, sizeof(data));
        context->Unmap(shadow_cbuffer_, 0);
    }

    bool EntityShadowRenderer::debug_enabled = false;

    void EntityShadowRenderer::render_debug_overlay(ID3D11DeviceContext* context)
    {
        if (!debug_enabled) return;

        // Only allow in single-player or as listen server host
        if (rf::is_multi && !rf::is_server) return;

        if (!shadow_srv_) return;

        // Lazy-init debug resources
        if (!debug_vb_) {
            D3D11_BUFFER_DESC vb_desc{};
            vb_desc.ByteWidth = 6 * sizeof(GpuTransformedVertex); // 1 quad * 6 verts
            vb_desc.Usage = D3D11_USAGE_DYNAMIC;
            vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            DF_GR_D3D11_CHECK_HR(device_->CreateBuffer(&vb_desc, nullptr, &debug_vb_));
        }
        if (!debug_blend_state_) {
            D3D11_BLEND_DESC bd{};
            bd.RenderTarget[0].BlendEnable = TRUE;
            bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
            bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            DF_GR_D3D11_CHECK_HR(device_->CreateBlendState(&bd, &debug_blend_state_));
        }
        if (!debug_no_depth_state_) {
            D3D11_DEPTH_STENCIL_DESC ds{};
            ds.DepthEnable = FALSE;
            ds.StencilEnable = FALSE;
            DF_GR_D3D11_CHECK_HR(device_->CreateDepthStencilState(&ds, &debug_no_depth_state_));
        }

        float sw = static_cast<float>(rf::gr::screen_width());
        float sh = static_cast<float>(rf::gr::screen_height());

        // Overlay size and position (top-left corner)
        float quad_size = 512.0f;
        float margin = 8.0f;
        float x0 = margin, y0 = margin;
        float x1 = x0 + quad_size, y1 = y0 + quad_size;

        // Convert pixel coords to clip space
        float cx0 = (x0 / sw) * 2.0f - 1.0f;
        float cy0 = 1.0f - (y0 / sh) * 2.0f;
        float cx1 = (x1 / sw) * 2.0f - 1.0f;
        float cy1 = 1.0f - (y1 / sh) * 2.0f;
        int diffuse = static_cast<int>(0xCCFFFFFF);

        D3D11_MAPPED_SUBRESOURCE mapped;
        DF_GR_D3D11_CHECK_HR(context->Map(debug_vb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        auto* verts = static_cast<GpuTransformedVertex*>(mapped.pData);
        verts[0] = { cx0, cy0, 0.0f, 1.0f, diffuse, 0.0f, 0.0f };
        verts[1] = { cx1, cy0, 0.0f, 1.0f, diffuse, 1.0f, 0.0f };
        verts[2] = { cx0, cy1, 0.0f, 1.0f, diffuse, 0.0f, 1.0f };
        verts[3] = { cx1, cy0, 0.0f, 1.0f, diffuse, 1.0f, 0.0f };
        verts[4] = { cx1, cy1, 0.0f, 1.0f, diffuse, 1.0f, 1.0f };
        verts[5] = { cx0, cy1, 0.0f, 1.0f, diffuse, 0.0f, 1.0f };
        context->Unmap(debug_vb_, 0);

        // Set render state
        const auto& vs = shader_manager_.get_vertex_shader(VertexShaderId::transformed);
        const auto& ps = shader_manager_.get_pixel_shader(PixelShaderId::shadow_debug);
        context->IASetInputLayout(vs.input_layout);
        context->VSSetShader(vs.vertex_shader, nullptr, 0);
        context->PSSetShader(ps, nullptr, 0);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->OMSetBlendState(debug_blend_state_, nullptr, 0xFFFFFFFF);
        context->OMSetDepthStencilState(debug_no_depth_state_, 0);

        UINT stride = sizeof(GpuTransformedVertex);
        UINT offset = 0;
        ID3D11Buffer* vb = debug_vb_;
        context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);

        // Bind point sampler at s0 for the debug PS
        ID3D11SamplerState* sampler = shadow_depth_sampler_;
        context->PSSetSamplers(0, 1, &sampler);

        // Draw shadow map quad
        ID3D11ShaderResourceView* srv = shadow_srv_;
        context->PSSetShaderResources(0, 1, &srv);
        context->Draw(6, 0);

        // Clean up SRV at t0
        ID3D11ShaderResourceView* null_srv = nullptr;
        context->PSSetShaderResources(0, 1, &null_srv);
    }
}
