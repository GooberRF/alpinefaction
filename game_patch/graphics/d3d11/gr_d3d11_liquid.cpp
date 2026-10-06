#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include "gr_d3d11.h"
#include "gr_d3d11_liquid.h"
#include "gr_d3d11_shader.h"
#include "../../misc/alpine_settings.h"
#include "../../os/os.h"
#include "../../rf/geometry.h"
#include "../../rf/gr/gr.h"
#include "../../rf/level.h"
#include "../../rf/player/camera.h"
#include "../../rf/player/player.h"

namespace gr::d3d11
{
    namespace
    {
        // -> liq_params.xyzw: extinction per unit, per-channel absorb hi/lo, in-scatter depth falloff
        constexpr float liquid_sigma_k = 3.0f;
        constexpr float liquid_absorb_hi = 1.6f;
        constexpr float liquid_absorb_lo = 0.7f;
        constexpr float liquid_depth_darken = 0.04f;

        constexpr float liquid_blend_tau = 0.35f;
        constexpr float liquid_blend_max_dt = 0.1f;

        constexpr UINT scene_depth_slot = 6;        // t6 Texture2D

        // Same type group as the depth buffer's DXGI_FORMAT_D24_UNORM_S8_UINT, which is what
        // CopyResource requires, and the only way to get a shader resource view on it.
        constexpr DXGI_FORMAT scene_depth_format = DXGI_FORMAT_R24G8_TYPELESS;
        constexpr DXGI_FORMAT scene_depth_srv_format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        constexpr DXGI_FORMAT scene_depth_dsv_format = DXGI_FORMAT_D24_UNORM_S8_UINT;

        // slack so adjacent rooms leave no seam; the top face is contracted instead (liquid_surface_epsilon)
        constexpr float liquid_box_epsilon = 0.05f;

        // Keeps the liquid surface polygon outside the volume, so a grazing ray cannot tint it
        constexpr float liquid_surface_epsilon = 0.01f;

        // Far clip the engine applies with the eye dry, as 0x00431A00 computes it: the level fog
        // range where it is used, else the room cull distance. The projection's own z_far is not
        // the same thing - in skyroom levels it stays at the fog range while the cull is 275.
        float above_water_far_clip()
        {
            if (!rf::level.has_skyroom && rf::level.distance_fog_far_clip > 0.0f) {
                return rf::level.distance_fog_far_clip;
            }
            return rf::gr::default_wfar;
        }

        float aabb_distance(const rf::Vector3& bbox_min, const rf::Vector3& bbox_max, const rf::Vector3& p)
        {
            float dx = std::max({bbox_min.x - p.x, 0.0f, p.x - bbox_max.x});
            float dy = std::max({bbox_min.y - p.y, 0.0f, p.y - bbox_max.y});
            float dz = std::max({bbox_min.z - p.z, 0.0f, p.z - bbox_max.z});
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        struct LiquidCandidate
        {
            rf::GRoom* room;
            float dist;
        };
    }

    void SceneDepthCapture::reset(ID3D11Device* device, ID3D11DeviceContext* device_context,
                                  ID3D11Texture2D* depth_texture)
    {
        depth_texture_ = depth_texture;
        release_copy();
        copy_failed_ = false;
        multisampled_ = false;
        resolve_capable_ = false;
        if (!depth_texture) {
            return;
        }
        depth_texture->GetDesc(&depth_desc_);
        multisampled_ = depth_desc_.SampleDesc.Count > 1;
        // The shader-resource bind flag is only requested where the feature level allows an SRV on
        // a multisampled depth buffer, so it doubles as the resolve-path gate.
        resolve_capable_ = multisampled_ && (depth_desc_.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;

        if (!stand_in_srv_) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = 1;
            desc.Height = 1;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = scene_depth_format;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
            D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
            srv_desc.Format = scene_depth_srv_format;
            srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv_desc.Texture2D.MipLevels = 1;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &stand_in_texture_))
                || FAILED(device->CreateShaderResourceView(stand_in_texture_, &srv_desc, &stand_in_srv_))) {
                xlog::warn("Liquid: no stand-in view for the scene depth slot");
                stand_in_texture_.release();
                stand_in_srv_.release();
            }
        }
        bind(device_context);
    }

    void SceneDepthCapture::release_copy()
    {
        copy_texture_.release();
        copy_srv_.release();
        copy_dsv_.release();
        depth_srv_.release();
        resolve_vs_.release();
        resolve_ps_.release();
        resolve_depth_state_.release();
        resolve_rasterizer_state_.release();
    }

    bool SceneDepthCapture::create_resolve_objects(ID3D11Device* device, ShaderManager& shader_manager)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
        dsv_desc.Format = scene_depth_dsv_format;
        dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        if (FAILED(device->CreateDepthStencilView(copy_texture_, &dsv_desc, &copy_dsv_))) {
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC ms_srv_desc{};
        ms_srv_desc.Format = scene_depth_srv_format;
        ms_srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        if (FAILED(device->CreateShaderResourceView(depth_texture_, &ms_srv_desc, &depth_srv_))) {
            return false;
        }

        resolve_vs_ = shader_manager.load_vertex_shader_only(
            get_vertex_shader_filename(VertexShaderId::gamma));
        resolve_ps_ = shader_manager.get_pixel_shader(PixelShaderId::depth_resolve);
        if (!resolve_vs_ || !resolve_ps_) {
            return false;
        }

        CD3D11_DEPTH_STENCIL_DESC ds_desc{D3D11_DEFAULT};
        ds_desc.DepthEnable = TRUE;
        ds_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        ds_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
        ds_desc.StencilEnable = FALSE;
        if (FAILED(device->CreateDepthStencilState(&ds_desc, &resolve_depth_state_))) {
            return false;
        }

        CD3D11_RASTERIZER_DESC rast_desc{D3D11_DEFAULT};
        rast_desc.CullMode = D3D11_CULL_NONE;
        return SUCCEEDED(device->CreateRasterizerState(&rast_desc, &resolve_rasterizer_state_));
    }

    bool SceneDepthCapture::ensure(ID3D11Device* device, ID3D11DeviceContext* device_context,
                                   ShaderManager& shader_manager)
    {
        if (copy_srv_) {
            return true;
        }
        if (!depth_texture_ || copy_failed_ || (multisampled_ && !resolve_capable_)) {
            return false;
        }
        // Depth-stencil bindable as well as readable: the copy stays the same kind of resource as
        // the depth buffer it is copied from, which is what CopyResource is happiest with. Under
        // MSAA it is instead the target of the resolve pass, which needs the same bind flags.
        D3D11_TEXTURE2D_DESC desc = depth_desc_;
        desc.Format = scene_depth_format;
        desc.SampleDesc = {1, 0};
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DEPTH_STENCIL;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = scene_depth_srv_format;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv_desc.Texture2D.MipLevels = 1;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &copy_texture_))
            || FAILED(device->CreateShaderResourceView(copy_texture_, &srv_desc, &copy_srv_))
            || (multisampled_ && !create_resolve_objects(device, shader_manager))) {
            xlog::warn("Liquid: scene depth copy unavailable, surfaces keep their far-clip fade");
            release_copy();
            copy_failed_ = true;
            return false;
        }
        bind(device_context);
        return true;
    }

    bool SceneDepthCapture::capture(ID3D11DeviceContext* device_context)
    {
        if (!copy_srv_) {
            return false;
        }
        if (!multisampled_) {
            device_context->CopyResource(copy_texture_, depth_texture_);
            return false;
        }

        // The copy is about to become a depth target, so drop its own binding first; the MSAA
        // depth buffer likewise has to leave the OM before it can be read as an SRV.
        ID3D11ShaderResourceView* null_srv = nullptr;
        device_context->PSSetShaderResources(scene_depth_slot, 1, &null_srv);
        device_context->OMSetRenderTargets(0, nullptr, copy_dsv_);

        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(depth_desc_.Width);
        viewport.Height = static_cast<float>(depth_desc_.Height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        device_context->RSSetViewports(1, &viewport);
        device_context->RSSetState(resolve_rasterizer_state_);
        device_context->OMSetDepthStencilState(resolve_depth_state_, 0);
        device_context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        device_context->IASetInputLayout(nullptr);
        device_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        device_context->VSSetShader(resolve_vs_, nullptr, 0);
        device_context->PSSetShader(resolve_ps_, nullptr, 0);
        ID3D11ShaderResourceView* depth_srv = depth_srv_;
        device_context->PSSetShaderResources(0, 1, &depth_srv);

        device_context->Draw(3, 0);

        device_context->PSSetShaderResources(0, 1, &null_srv);
        device_context->OMSetRenderTargets(0, nullptr, nullptr);
        bind(device_context);
        return true;
    }

    void SceneDepthCapture::bind(ID3D11DeviceContext* device_context)
    {
        ID3D11ShaderResourceView* view = copy_srv_ ? copy_srv_.get() : stand_in_srv_.get();
        device_context->PSSetShaderResources(scene_depth_slot, 1, &view);
    }

    LiquidFxRenderer::LiquidFxRenderer(ID3D11Device* device)
    {
        LiquidBufferData init_data{};
        D3D11_SUBRESOURCE_DATA subres_data{&init_data, 0, 0};
        CD3D11_BUFFER_DESC desc{
            sizeof(LiquidBufferData),
            D3D11_BIND_CONSTANT_BUFFER,
            D3D11_USAGE_DYNAMIC,
            D3D11_CPU_ACCESS_WRITE,
        };
        DF_GR_D3D11_CHECK_HR(device->CreateBuffer(&desc, &subres_data, &buffer_));
    }

    void LiquidFxRenderer::upload(ID3D11DeviceContext* device_context, const LiquidBufferData& data)
    {
        D3D11_MAPPED_SUBRESOURCE mapped_subres;
        DF_GR_D3D11_CHECK_HR(
            device_context->Map(buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_subres)
        );
        std::memcpy(mapped_subres.pData, &data, sizeof(data));
        device_context->Unmap(buffer_, 0);
        disabled_uploaded_ = false;
    }

    void LiquidFxRenderer::write_disabled(ID3D11DeviceContext* device_context)
    {
        if (disabled_uploaded_) {
            return;
        }
        LiquidBufferData disabled{};
        upload(device_context, disabled);
        disabled_uploaded_ = true;
    }

    void LiquidFxRenderer::rewrite(ID3D11DeviceContext* device_context)
    {
        upload(device_context, data_);
    }

    bool LiquidFxRenderer::background_color(rf::Vector3& out) const
    {
        if (state_.mode == 0 || !state_.eye_room_liquid) {
            return false;
        }
        const float depth_below = std::max(state_.blended_surface_y - eye_pos_.y, 0.0f);
        const float darken = std::exp(-depth_below * liquid_depth_darken);
        out = {
            state_.blended_color.x * darken,
            state_.blended_color.y * darken,
            state_.blended_color.z * darken,
        };
        return true;
    }

    void LiquidFxRenderer::snap_blend()
    {
        state_.blended_color = state_.color;
        state_.blended_alpha = state_.alpha;
        state_.blended_visibility = state_.visibility;
        state_.blended_surface_y = state_.surface_y;
        state_.blended_over_fog_color = state_.over_fog_color;
        state_.blended_over_fog_far = state_.over_fog_far;
    }

    Projection LiquidFxRenderer::update(ID3D11DeviceContext* device_context, const Projection& projection,
                                        const rf::Vector3& eye_pos, const rf::Matrix3& eye_orient,
                                        float scene_depth_mode)
    {
        const int prev_mode = state_.mode;
        eye_pos_ = eye_pos;

        if (!rf::level.geometry) {
            state_ = LiquidState{};
            data_ = {};
            write_disabled(device_context);
            return projection;
        }

        rf::Camera* cam = rf::local_player ? rf::local_player->cam : nullptr;
        rf::GRoom* cam_room = cam && cam->camera_entity ? rf::camera_get_room(cam) : nullptr;
        const float dry_far_clip = std::min(above_water_far_clip(), projection.z_far());

        // The room the liquid appearance comes from. The camera's own room wins; a dry camera
        // takes the nearest liquid room in range instead, so water seen through a portal is
        // fogged like water the camera is standing in.
        rf::GRoom* liquid_room = cam_room && cam_room->contains_liquid ? cam_room : nullptr;
        const bool eye_room_liquid = liquid_room != nullptr;
        if (!liquid_room && g_alpine_game_config.underwater_fx >= 2) {
            float best_dist = std::numeric_limits<float>::max();
            auto& all_rooms = rf::level.geometry->all_rooms;
            for (int i = 0; i < all_rooms.size(); ++i) {
                rf::GRoom* room = all_rooms[i];
                if (!room || room->uid < 0 || !room->contains_liquid || room->liquid_type <= 0) {
                    continue;
                }
                float dist = aabb_distance(room->bbox_min, room->bbox_max, eye_pos);
                if (dist <= dry_far_clip && dist < best_dist) {
                    best_dist = dist;
                    liquid_room = room;
                }
            }
        }

        if (liquid_room) {
            // Keeps mode != 0 in step with the shader's liq_mode > 0.5
            state_.mode = liquid_room->liquid_type > 0 ? liquid_room->liquid_type : 0;
            state_.surface_y = liquid_room->bbox_min.y + liquid_room->liquid_depth;
            state_.color = {
                liquid_room->liquid_color.red / 255.0f,
                liquid_room->liquid_color.green / 255.0f,
                liquid_room->liquid_color.blue / 255.0f,
            };
            state_.alpha = std::clamp(liquid_room->liquid_alpha, 0, 255) / 255.0f;
            state_.visibility = liquid_room->liquid_visibility;
            state_.eye_under = eye_room_liquid && eye_pos.y <= state_.surface_y;
            state_.eye_room_liquid = eye_room_liquid;
        }
        else {
            state_ = LiquidState{};
        }

        // Level fog for the above-surface part of a submerged view; the liquid fog has replaced
        // it in rf::gr::screen by the time the scene draws. Enable test mirrors 0x004318C0.
        if (rf::level.distance_fog_far_clip > 0.0f) {
            state_.over_fog_color = {
                rf::level.distance_fog_color.red / 255.0f,
                rf::level.distance_fog_color.green / 255.0f,
                rf::level.distance_fog_color.blue / 255.0f,
            };
            state_.over_fog_far = rf::level.distance_fog_far_clip;
        }
        else {
            state_.over_fog_color = {0.0f, 0.0f, 0.0f};
            state_.over_fog_far = 0.0f;
        }

        // Submerged, a level far clip shorter than the engine's cull distance depth-clips geometry
        // into a hole instead of fogging it out. Decided here so far_clip below targets it.
        Projection out_projection = projection;
        if (g_alpine_game_config.underwater_fx >= 2 && state_.eye_under
            && std::isfinite(rf::gr::default_wfar)
            && projection.z_far() < rf::gr::default_wfar
            && rf::gr::default_wfar > projection.z_near()) {
            out_projection = Projection{projection.scale_x(), projection.scale_y(),
                                        projection.z_near(), rf::gr::default_wfar};
        }

        // Entering liquid from dry snaps: there is nothing meaningful to fade from.
        const int64_t now_ms = timer::get_i64(1000);
        const float dt = last_update_ms_ >= 0
            ? std::clamp(static_cast<float>(now_ms - last_update_ms_) / 1000.0f, 0.0f, liquid_blend_max_dt)
            : 0.0f;
        last_update_ms_ = now_ms;
        if (state_.mode != 0) {
            if (prev_mode == 0) {
                snap_blend();
            }
            else {
                const float k = 1.0f - std::exp(-dt / liquid_blend_tau);
                auto mix = [k](float from, float to) { return from + (to - from) * k; };
                state_.blended_color = {
                    mix(state_.blended_color.x, state_.color.x),
                    mix(state_.blended_color.y, state_.color.y),
                    mix(state_.blended_color.z, state_.color.z),
                };
                state_.blended_alpha = mix(state_.blended_alpha, state_.alpha);
                state_.blended_visibility = mix(state_.blended_visibility, state_.visibility);
                state_.blended_surface_y = mix(state_.blended_surface_y, state_.surface_y);
                state_.blended_over_fog_color = {
                    mix(state_.blended_over_fog_color.x, state_.over_fog_color.x),
                    mix(state_.blended_over_fog_color.y, state_.over_fog_color.y),
                    mix(state_.blended_over_fog_color.z, state_.over_fog_color.z),
                };
                // Easing toward 0 would pass through arbitrarily short fog ranges
                state_.blended_over_fog_far = state_.over_fog_far > 0.0f
                    ? mix(state_.blended_over_fog_far, state_.over_fog_far)
                    : 0.0f;
            }
        }

        if (g_alpine_game_config.underwater_fx < 2 || state_.mode == 0) {
            data_ = {};
            write_disabled(device_context);
            return out_projection;
        }

        LiquidBufferData data{};
        data.eye_pos = {eye_pos.x, eye_pos.y, eye_pos.z};
        data.mode = static_cast<float>(state_.mode);

        data.cam_right = {eye_orient.rvec.x, eye_orient.rvec.y, eye_orient.rvec.z};
        data.cam_up = {eye_orient.uvec.x, eye_orient.uvec.y, eye_orient.uvec.z};
        data.cam_forward = {eye_orient.fvec.x, eye_orient.fvec.y, eye_orient.fvec.z};
        data.proj_sx = out_projection.scale_x();
        data.proj_sy = out_projection.scale_y();
        const auto origin = viewport_origin();
        data.viewport_x = origin[0];
        data.viewport_y = origin[1];
        data.viewport_w = static_cast<float>(rf::gr::screen.clip_width);
        data.viewport_h = static_cast<float>(rf::gr::screen.clip_height);

        data.surface_y = state_.surface_y;
        data.visibility = state_.blended_visibility;
        data.eye_under = state_.eye_under ? 1.0f : 0.0f;
        data.color = {state_.blended_color.x, state_.blended_color.y, state_.blended_color.z};
        data.over_fog_color = {
            state_.blended_over_fog_color.x,
            state_.blended_over_fog_color.y,
            state_.blended_over_fog_color.z,
        };
        data.over_fog_far = state_.blended_over_fog_far > 1e-3f
            ? state_.blended_over_fog_far
            : std::numeric_limits<float>::infinity();
        data.params = {liquid_sigma_k, liquid_absorb_hi, liquid_absorb_lo, liquid_depth_darken};
        data.dark_surface_y = state_.blended_surface_y;
        // Nearest of the room/object cull and the depth-clip plane. Recomputed rather than read
        // from gr_far_clip_dist, which 0x00431D3F only sets later in the frame. Submerged the
        // widened liquid clip is what cuts; dry it is the engine's own far clip, which is also
        // what the surface fade in the shader has to target.
        data.far_clip = state_.eye_under
            ? std::min(liquid_far_clip(state_.visibility), out_projection.z_far())
            : dry_far_clip;

        // Every nearby liquid room of the reference room's type, nearest first with the reference
        // room pinned to slot 0. The shader sums the ray's time through all of them, so the fogged
        // length no longer stops at the walls of a single room.
        LiquidCandidate candidates[max_liquid_volumes];
        int num_volumes = 0;
        if (liquid_room) {
            candidates[num_volumes++] = {liquid_room, 0.0f};
            auto& all_rooms = rf::level.geometry->all_rooms;
            for (int i = 0; i < all_rooms.size(); ++i) {
                rf::GRoom* room = all_rooms[i];
                if (!room || room == liquid_room || room->uid < 0 || !room->contains_liquid
                    || room->liquid_type != state_.mode) {
                    continue;
                }
                float dist = aabb_distance(room->bbox_min, room->bbox_max, eye_pos);
                if (dist > data.far_clip) {
                    continue;
                }
                if (num_volumes == max_liquid_volumes
                    && dist >= candidates[max_liquid_volumes - 1].dist) {
                    continue;
                }
                int slot = std::min(num_volumes, max_liquid_volumes - 1);
                while (slot > 1 && candidates[slot - 1].dist > dist) {
                    candidates[slot] = candidates[slot - 1];
                    --slot;
                }
                candidates[slot] = {room, dist};
                num_volumes = std::min(num_volumes + 1, max_liquid_volumes);
            }
        }
        for (int i = 0; i < num_volumes; ++i) {
            rf::GRoom* room = candidates[i].room;
            const float room_surface_y = room->bbox_min.y + room->liquid_depth;
            auto& dst = data.volumes[i];
            dst.bbox_min = {
                room->bbox_min.x - liquid_box_epsilon,
                room->bbox_min.y - liquid_box_epsilon,
                room->bbox_min.z - liquid_box_epsilon,
            };
            dst.bbox_max = {
                room->bbox_max.x + liquid_box_epsilon,
                std::min(room_surface_y, room->bbox_max.y) - liquid_surface_epsilon,
                room->bbox_max.z + liquid_box_epsilon,
            };
        }
        data.num_volumes = static_cast<float>(num_volumes);
        data.depth_sz = out_projection.scale_z();
        data.depth_tz = out_projection.translate_z();
        data.depth_mode = scene_depth_mode;

        data_ = data;
        rewrite(device_context);
        return out_projection;
    }
}
