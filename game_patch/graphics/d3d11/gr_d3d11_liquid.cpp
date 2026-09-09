#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include "gr_d3d11.h"
#include "gr_d3d11_liquid.h"
#include "../../misc/alpine_settings.h"
#include "../../os/os.h"
#include "../../rf/geometry.h"
#include "../../rf/gr/gr.h"
#include "../../rf/level.h"
#include "../../rf/misc.h"
#include "../../rf/player/camera.h"
#include "../../rf/player/player.h"

namespace gr::d3d11
{
    namespace
    {
        // Underwater fog tunables. sigma_k / liquid_visibility is the base extinction in 1/world unit;
        // absorb_hi/lo scale it per channel (dark channels of the liquid color die first); depth_darken
        // is the in-scatter falloff with depth below the surface, also in 1/world unit.
        constexpr float liquid_sigma_k = 3.0f;
        constexpr float liquid_absorb_hi = 1.6f;
        constexpr float liquid_absorb_lo = 0.7f;
        constexpr float liquid_depth_darken = 0.04f;

        // Colour/visibility cross-fade when the camera moves between liquid rooms
        constexpr float liquid_blend_tau = 0.35f;
        constexpr float liquid_blend_max_dt = 0.1f;

        // Slack on the uploaded volume boxes so adjacent rooms leave no seam
        constexpr float liquid_box_epsilon = 0.05f;

        // The top face instead sits just below the surface, so the liquid surface polygon itself
        // is outside the volume: inside it, a grazing ray picks up a real path length and tints
        // an opaque surface (very visible on lava, whose visibility is short)
        constexpr float liquid_surface_epsilon = 0.01f;

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
    }

    void LiquidFxRenderer::write_disabled(ID3D11DeviceContext* device_context)
    {
        LiquidBufferData disabled{};
        upload(device_context, disabled);
    }

    void LiquidFxRenderer::rewrite(ID3D11DeviceContext* device_context)
    {
        upload(device_context, data_);
    }

    bool LiquidFxRenderer::background_color(rf::Vector3& out) const
    {
        if (state_.mode == 0) {
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

    void LiquidFxRenderer::update(ID3D11DeviceContext* device_context, const Projection& projection,
                                  const rf::Vector3& eye_pos, const rf::Matrix3& eye_orient)
    {
        const int prev_mode = state_.mode;
        eye_pos_ = eye_pos;

        rf::Camera* cam = rf::local_player ? rf::local_player->cam : nullptr;
        rf::GRoom* cam_room = cam && cam->camera_entity ? rf::camera_get_room(cam) : nullptr;
        if (cam_room && cam_room->contains_liquid) {
            state_.mode = cam_room->liquid_type;
            state_.room_uid = cam_room->uid;
            state_.surface_y = cam_room->bbox_min.y + cam_room->liquid_depth;
            state_.bbox_min = cam_room->bbox_min;
            state_.bbox_max = cam_room->bbox_max;
            state_.color = {
                cam_room->liquid_color.red / 255.0f,
                cam_room->liquid_color.green / 255.0f,
                cam_room->liquid_color.blue / 255.0f,
            };
            state_.alpha = cam_room->liquid_alpha / 255.0f;
            state_.visibility = cam_room->liquid_visibility;
            // <= matches GRoom::liquid_contains_point (0x004CE080)
            state_.eye_under = eye_pos.y <= state_.surface_y;
        }
        else {
            // Nothing may survive the room we left: a respawn or teleport out of water must not
            // leave a stale surface plane, tint or under-water edge behind.
            state_ = LiquidState{};
        }

        // Level distance fog for the above-surface part of a submerged view. Taken from the level
        // rather than rf::gr::screen because the liquid fog has replaced it there by the time the
        // scene draws; the enable test mirrors 0x004321C4.
        if (rf::level.distance_fog_far_clip > 0.0f && !rf::level.has_skyroom) {
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

        // Cross-fade into the new room's look. Entering liquid from dry snaps, because there is
        // nothing meaningful to fade from; leaving it needs nothing, the reset above covers it.
        const int64_t now_ms = timer::get_i64(1000);
        const float dt = last_update_ms_ >= 0
            ? std::min(static_cast<float>(now_ms - last_update_ms_) / 1000.0f, liquid_blend_max_dt)
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
                state_.blended_over_fog_far = mix(state_.blended_over_fog_far, state_.over_fog_far);
            }
        }

        if (g_alpine_game_config.underwater_fx < 2 || state_.mode == 0) {
            data_ = {};
            rewrite(device_context);
            return;
        }

        LiquidBufferData data{};
        data.eye_pos = {eye_pos.x, eye_pos.y, eye_pos.z};
        data.mode = static_cast<float>(state_.mode);

        data.cam_right = {eye_orient.rvec.x, eye_orient.rvec.y, eye_orient.rvec.z};
        data.cam_up = {eye_orient.uvec.x, eye_orient.uvec.y, eye_orient.uvec.z};
        data.cam_forward = {eye_orient.fvec.x, eye_orient.fvec.y, eye_orient.fvec.z};
        data.proj_sx = projection.scale_x();
        data.proj_sy = projection.scale_y();
        data.viewport_w = static_cast<float>(rf::gr::screen.clip_width);
        data.viewport_h = static_cast<float>(rf::gr::screen.clip_height);

        data.surface_y = state_.surface_y;
        data.visibility = state_.blended_visibility;
        data.eye_under = state_.eye_under ? 1.0f : 0.0f;
        data.color = {state_.blended_color.x, state_.blended_color.y, state_.blended_color.z};
        data.alpha = state_.blended_alpha;
        data.over_fog_color = {
            state_.blended_over_fog_color.x,
            state_.blended_over_fog_color.y,
            state_.blended_over_fog_color.z,
        };
        data.over_fog_far = state_.blended_over_fog_far > 0.0f
            ? state_.blended_over_fog_far
            : std::numeric_limits<float>::infinity();
        data.params = {liquid_sigma_k, liquid_absorb_hi, liquid_absorb_lo, liquid_depth_darken};
        data.dark_surface_y = state_.blended_surface_y;
        // Nearest of the two far planes: the engine's room/object cull (gr_set_far_clip
        // 0x00518060) and the projection plane DepthClipEnable cuts triangles at. The shader
        // fades to the in-scatter colour before it so neither cut is visible.
        const float engine_far = rf::gr_use_far_clip ? rf::gr_far_clip_dist : rf::gr::default_wfar;
        data.far_clip = std::min(engine_far, projection.z_far());

        // Every nearby liquid room of the camera room's type, nearest first with the camera room
        // pinned to slot 0. The shader sums the ray's time through all of them, so the fogged
        // length no longer stops at the walls of whichever room the camera happens to be in.
        LiquidCandidate candidates[max_liquid_volumes];
        int num_volumes = 0;
        if (cam_room) {
            candidates[num_volumes++] = {cam_room, 0.0f};
            if (rf::level.geometry) {
                auto& all_rooms = rf::level.geometry->all_rooms;
                for (int i = 0; i < all_rooms.size(); ++i) {
                    rf::GRoom* room = all_rooms[i];
                    if (!room || room == cam_room || room->uid < 0 || !room->contains_liquid
                        || room->liquid_type != state_.mode) {
                        continue;
                    }
                    float dist = aabb_distance(room->bbox_min, room->bbox_max, rf::gr::eye_pos);
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
            dst.surface_y = room_surface_y;
        }
        data.num_volumes = static_cast<float>(num_volumes);

        data_ = data;
        rewrite(device_context);
    }
}
