#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <string>
#include <vector>
#include <xlog/xlog.h>
#include "gr_d3d11.h"
#include "gr_d3d11_caustics.h"
#include "../../misc/alpine_settings.h"
#include "../../rf/bmpman.h"
#include "../../rf/geometry.h"
#include "../../rf/gr/gr.h"
#include "../../rf/level.h"
#include "../../rf/os/frametime.h"
#include "../../rf/player/camera.h"
#include "../../rf/player/player.h"

namespace gr::d3d11
{
    namespace
    {
        constexpr float caustic_intensity = 5.0f;
        constexpr float caustic_scale = 0.2f;
        constexpr float caustic_speed = 0.015f;
        constexpr float caustic_floor = 0.02f;
        constexpr float caustic_exponent = 1.25f;
        constexpr float caustic_depth_fade = 100.0f;
        constexpr float caustic_above_water = 0.6f;
        constexpr float caustic_drift = 0.05f;
        constexpr float caustic_wall_stretch = 0.6f;

        // Fallback view distance used to cull far water rooms when fog is disabled
        constexpr float caustics_no_fog_range = 300.0f;

        // Liquid colors left at (or near) black would kill the effect entirely
        constexpr float caustics_min_tint = 0.35f;

        constexpr int liquid_type_water = 1;

        float aabb_distance(const rf::Vector3& bbox_min, const rf::Vector3& bbox_max, const rf::Vector3& p)
        {
            float dx = std::max({bbox_min.x - p.x, 0.0f, p.x - bbox_max.x});
            float dy = std::max({bbox_min.y - p.y, 0.0f, p.y - bbox_max.y});
            float dz = std::max({bbox_min.z - p.z, 0.0f, p.z - bbox_max.z});
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        struct CausticCandidate
        {
            rf::Vector3 bbox_min;
            rf::Vector3 bbox_max;
            float surface_y;
            rf::Color color;
            float dist;
            int room_uid;
        };
    }

    struct alignas(16) CausticVolumeGPUData
    {
        std::array<float, 3> bbox_min; float surface_y;
        std::array<float, 3> bbox_max; float room_uid;
        std::array<float, 3> color;    float _pad1;
    };
    static_assert(sizeof(CausticVolumeGPUData) == 48);
    static_assert(sizeof(CausticVolumeGPUData) % 16 == 0);

    struct alignas(16) CausticsBufferData
    {
        int num_caustic_volumes; float caustic_time; float caustic_intensity; float caustic_scale;
        float caustic_speed; float caustic_floor; float caustic_exponent; float caustic_depth_fade;
        float caustic_above_water; float caustic_drift; float caustic_wall_stretch; float _hdr_pad;
        CausticVolumeGPUData volumes[CausticsRenderer::max_caustic_volumes];
    };
    static_assert(sizeof(CausticsBufferData) == 816);
    static_assert(sizeof(CausticsBufferData) % 16 == 0);
    static_assert(offsetof(CausticsBufferData, volumes) == 48);

    CausticsRenderer::CausticsRenderer(ID3D11Device* device) : device_{device}
    {
        CD3D11_BUFFER_DESC desc{
            sizeof(CausticsBufferData),
            D3D11_BIND_CONSTANT_BUFFER,
            D3D11_USAGE_DYNAMIC,
            D3D11_CPU_ACCESS_WRITE,
        };
        DF_GR_D3D11_CHECK_HR(device_->CreateBuffer(&desc, nullptr, &buffer_));
    }

    void CausticsRenderer::write_disabled(ID3D11DeviceContext* device_context)
    {
        D3D11_MAPPED_SUBRESOURCE mapped_subres;
        DF_GR_D3D11_CHECK_HR(
            device_context->Map(buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_subres)
        );
        std::memset(mapped_subres.pData, 0, sizeof(CausticsBufferData));
        device_context->Unmap(buffer_, 0);
    }

    bool CausticsRenderer::build_texture()
    {
        constexpr int pixels_per_frame = frame_size * frame_size;

        // mip 0 for every frame, then the CPU-generated chain
        std::vector<std::vector<rf::ubyte>> subresources;
        subresources.resize(static_cast<size_t>(num_frames) * mip_levels);

        for (int frame = 0; frame < num_frames; ++frame) {
            std::string name = std::format("af_caust{:02}.png", frame);
            int handle = rf::bm::load(name.c_str(), -1, false);
            if (handle == -1) {
                xlog::error("Caustics: failed to load {}", name);
                return false;
            }

            int w = 0, h = 0;
            rf::bm::get_dimensions(handle, &w, &h);
            if (w != frame_size || h != frame_size) {
                xlog::error("Caustics: {} is {}x{}, expected {}x{}", name, w, h, frame_size, frame_size);
                rf::bm::release(handle);
                return false;
            }

            rf::ubyte* bits = nullptr;
            rf::ubyte* pal = nullptr;
            rf::bm::Format fmt = rf::bm::lock(handle, &bits, &pal);
            int stride = 0;
            if (fmt == rf::bm::FORMAT_888_RGB || fmt == rf::bm::FORMAT_888_BGR) {
                stride = 3;
            }
            else if (fmt == rf::bm::FORMAT_8888_ARGB) {
                stride = 4;
            }
            if (stride == 0 || bits == nullptr) {
                xlog::error("Caustics: unsupported format {} for {}", static_cast<int>(fmt), name);
                if (bits) {
                    rf::bm::unlock(handle);
                }
                rf::bm::release(handle);
                return false;
            }

            auto& mip0 = subresources[static_cast<size_t>(frame) * mip_levels];
            mip0.resize(pixels_per_frame);
            for (int i = 0; i < pixels_per_frame; ++i) {
                mip0[i] = bits[static_cast<size_t>(i) * stride];
            }
            rf::bm::unlock(handle);
            rf::bm::release(handle);

            int src_size = frame_size;
            for (int mip = 1; mip < mip_levels; ++mip) {
                const auto& src = subresources[static_cast<size_t>(frame) * mip_levels + mip - 1];
                int dst_size = src_size / 2;
                auto& dst = subresources[static_cast<size_t>(frame) * mip_levels + mip];
                dst.resize(static_cast<size_t>(dst_size) * dst_size);
                for (int y = 0; y < dst_size; ++y) {
                    for (int x = 0; x < dst_size; ++x) {
                        int sx = x * 2;
                        int sy = y * 2;
                        int sum = src[static_cast<size_t>(sy) * src_size + sx]
                                + src[static_cast<size_t>(sy) * src_size + sx + 1]
                                + src[static_cast<size_t>(sy + 1) * src_size + sx]
                                + src[static_cast<size_t>(sy + 1) * src_size + sx + 1];
                        dst[static_cast<size_t>(y) * dst_size + x] = static_cast<rf::ubyte>((sum + 2) / 4);
                    }
                }
                src_size = dst_size;
            }
        }

        std::vector<D3D11_SUBRESOURCE_DATA> init_data;
        init_data.resize(subresources.size());
        for (int frame = 0; frame < num_frames; ++frame) {
            int size = frame_size;
            for (int mip = 0; mip < mip_levels; ++mip) {
                size_t idx = static_cast<size_t>(frame) * mip_levels + mip;
                init_data[idx].pSysMem = subresources[idx].data();
                init_data[idx].SysMemPitch = static_cast<UINT>(size);
                init_data[idx].SysMemSlicePitch = 0;
                size /= 2;
            }
        }

        D3D11_TEXTURE2D_DESC tex_desc{};
        tex_desc.Width = frame_size;
        tex_desc.Height = frame_size;
        tex_desc.MipLevels = mip_levels;
        tex_desc.ArraySize = num_frames;
        tex_desc.Format = DXGI_FORMAT_R8_UNORM;
        tex_desc.SampleDesc.Count = 1;
        tex_desc.Usage = D3D11_USAGE_IMMUTABLE;
        tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = device_->CreateTexture2D(&tex_desc, init_data.data(), &texture_);
        if (FAILED(hr)) {
            xlog::error("Caustics: CreateTexture2D failed: {:x}", static_cast<unsigned>(hr));
            return false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
        srv_desc.Format = DXGI_FORMAT_R8_UNORM;
        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        srv_desc.Texture2DArray.MostDetailedMip = 0;
        srv_desc.Texture2DArray.MipLevels = mip_levels;
        srv_desc.Texture2DArray.FirstArraySlice = 0;
        srv_desc.Texture2DArray.ArraySize = num_frames;
        hr = device_->CreateShaderResourceView(texture_, &srv_desc, &srv_);
        if (FAILED(hr)) {
            xlog::error("Caustics: CreateShaderResourceView failed: {:x}", static_cast<unsigned>(hr));
            texture_.release();
            return false;
        }

        D3D11_SAMPLER_DESC samp_desc{};
        samp_desc.Filter = D3D11_FILTER_ANISOTROPIC;
        samp_desc.MaxAnisotropy = 4;
        samp_desc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        samp_desc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        samp_desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        samp_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        samp_desc.MinLOD = 0.0f;
        samp_desc.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device_->CreateSamplerState(&samp_desc, &sampler_);
        if (FAILED(hr)) {
            xlog::error("Caustics: CreateSamplerState failed: {:x}", static_cast<unsigned>(hr));
            srv_.release();
            texture_.release();
            return false;
        }

        return true;
    }

    void CausticsRenderer::update(ID3D11DeviceContext* device_context)
    {
        if (g_alpine_game_config.underwater_fx < 1 || build_failed_ || !rf::level.geometry) {
            write_disabled(device_context);
            return;
        }

        const rf::Vector3& eye_pos = rf::gr::eye_pos;
        const float max_dist = rf::gr::screen.fog_mode ? rf::gr::screen.fog_far : caustics_no_fog_range;

        // Nearest-first insertion into a fixed slot array, so a per-frame allocation is never needed
        CausticCandidate candidates[max_caustic_volumes];
        int num_candidates = 0;
        auto& all_rooms = rf::level.geometry->all_rooms;
        for (int i = 0; i < all_rooms.size(); ++i) {
            rf::GRoom* room = all_rooms[i];
            if (!room || room->uid < 0 || !room->contains_liquid
                || room->liquid_type != liquid_type_water) {
                continue;
            }
            float dist = aabb_distance(room->bbox_min, room->bbox_max, eye_pos);
            if (dist > max_dist) {
                continue;
            }
            if (num_candidates == max_caustic_volumes && dist >= candidates[max_caustic_volumes - 1].dist) {
                continue;
            }
            int slot = std::min(num_candidates, max_caustic_volumes - 1);
            while (slot > 0 && candidates[slot - 1].dist > dist) {
                candidates[slot] = candidates[slot - 1];
                --slot;
            }
            candidates[slot] = {
                room->bbox_min,
                room->bbox_max,
                room->bbox_min.y + room->liquid_depth,
                room->liquid_color,
                dist,
                room->uid,
            };
            num_candidates = std::min(num_candidates + 1, max_caustic_volumes);
        }

        if (num_candidates == 0) {
            write_disabled(device_context);
            return;
        }

        if (!build_attempted_) {
            build_attempted_ = true;
            if (!build_texture()) {
                build_failed_ = true;
            }
        }
        if (build_failed_) {
            write_disabled(device_context);
            return;
        }

        CausticsBufferData data{};
        data.num_caustic_volumes = num_candidates;
        data.caustic_time = rf::frametime_total_milliseconds / 1000.0f;
        data.caustic_intensity = caustic_intensity;
        data.caustic_scale = caustic_scale;
        data.caustic_speed = caustic_speed;
        data.caustic_floor = caustic_floor;
        data.caustic_exponent = caustic_exponent;
        data.caustic_depth_fade = caustic_depth_fade;
        data.caustic_drift = caustic_drift;
        data.caustic_wall_stretch = caustic_wall_stretch;

        for (int i = 0; i < num_candidates; ++i) {
            const auto& src = candidates[i];
            auto& dst = data.volumes[i];
            dst.bbox_min = {src.bbox_min.x, src.bbox_min.y, src.bbox_min.z};
            dst.bbox_max = {src.bbox_max.x, src.bbox_max.y, src.bbox_max.z};
            dst.surface_y = src.surface_y;
            dst.room_uid = static_cast<float>(src.room_uid);
            dst.color = {
                std::max(src.color.red / 255.0f, caustics_min_tint),
                std::max(src.color.green / 255.0f, caustics_min_tint),
                std::max(src.color.blue / 255.0f, caustics_min_tint),
            };
        }

        rf::Camera* cam = rf::local_player ? rf::local_player->cam : nullptr;
        rf::GRoom* cam_room = cam && cam->camera_entity ? rf::camera_get_room(cam) : nullptr;
        bool underwater = false;
        if (cam_room && cam_room->contains_liquid && cam_room->liquid_type == liquid_type_water) {
            underwater = rf::camera_get_pos(cam).y <= cam_room->bbox_min.y + cam_room->liquid_depth;
        }
        data.caustic_above_water = underwater ? 1.0f : caustic_above_water;

        D3D11_MAPPED_SUBRESOURCE mapped_subres;
        DF_GR_D3D11_CHECK_HR(
            device_context->Map(buffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_subres)
        );
        std::memcpy(mapped_subres.pData, &data, sizeof(data));
        device_context->Unmap(buffer_, 0);

        ID3D11ShaderResourceView* srv = srv_;
        device_context->PSSetShaderResources(3, 1, &srv);
        ID3D11SamplerState* samp = sampler_;
        device_context->PSSetSamplers(4, 1, &samp);
    }
}
