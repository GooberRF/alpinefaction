#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <type_traits>
#include <xlog/xlog.h>
#include "../../misc/alpine_terrain_decorations.h"
#include "../../rf/gr/gr.h"
#include "../../rf/gr/gr_light.h"
#include "../../rf/v3d.h"
#include "../../rf/vmesh.h"
#include "gr_d3d11.h"
#include "gr_d3d11_context.h"
#include "gr_d3d11_decoration.h"
#include "gr_d3d11_mesh.h"

namespace at = alpine_terrain;

namespace
{
    // Mirror of DecorationBuffer (b4) in standard_vs.hlsl
    struct alignas(16) DecorationBufferData
    {
        std::array<float, 3> submesh_center;
        float draw_distance;
        float fade_band;
        std::array<float, 3> pad;
    };
    static_assert(offsetof(DecorationBufferData, draw_distance) == 12);
    static_assert(offsetof(DecorationBufferData, fade_band) == 16);
    static_assert(sizeof(DecorationBufferData) == 32);

    constexpr UINT cbuffer_slot = 4;
    constexpr UINT instance_slot = 1;
    // Instances shrink away over the last fade_band_fraction of the draw distance, at least min_fade_band
    constexpr float min_fade_band = 2.0f;
    constexpr float fade_band_fraction = 0.1f;

    // The level of detail stock LOD selection (0x0052FA40) picks at this apparent distance.
    int select_lod(const rf::VifLodMesh& lod_mesh, float apparent_distance)
    {
        const int levels = std::clamp(lod_mesh.num_levels, 1, static_cast<int>(std::size(lod_mesh.meshes)));
        if (rf::vif_lod_full_detail || levels < 2) {
            return 0;
        }
        const int min_lod = std::clamp(rf::vif_min_lod, 0, levels - 1);
        for (int lod = levels - 1; lod > min_lod; lod--) {
            if (lod_mesh.distances[lod] <= apparent_distance) {
                return lod;
            }
        }
        return min_lod;
    }
}

namespace gr::d3d11
{
    DecorationRenderer::DecorationRenderer(ComPtr<ID3D11Device> device, ShaderManager& shader_manager,
                                           RenderContext& render_context, MeshRenderer& mesh_renderer) :
        device_{std::move(device)}, render_context_{render_context},
        mesh_renderer_{mesh_renderer}
    {
        vertex_shader_ = shader_manager.get_vertex_shader(VertexShaderId::decoration);
        pixel_shader_ = shader_manager.get_pixel_shader(PixelShaderId::decoration);
        pixel_shader_no_gas_ = shader_manager.get_pixel_shader(PixelShaderId::decoration_no_gas);
        CD3D11_BUFFER_DESC desc{
            sizeof(DecorationBufferData),
            D3D11_BIND_CONSTANT_BUFFER,
            D3D11_USAGE_DYNAMIC,
            D3D11_CPU_ACCESS_WRITE,
        };
        shaders_ok_ = vertex_shader_.vertex_shader && vertex_shader_.input_layout && pixel_shader_ &&
                      pixel_shader_no_gas_ && SUCCEEDED(device_->CreateBuffer(&desc, nullptr, &cbuffer_));
        if (!shaders_ok_) {
            xlog::warn("[AlpineTerrain] Terrain decorations are not drawn: their shaders could not be loaded");
        }
    }

    ID3D11Buffer* DecorationRenderer::instance_buffer(int terrain)
    {
        auto& all = alpine_terrain_decorations_get_all();
        TerrainDecorations& td = all[static_cast<std::size_t>(terrain)];
        if (instance_buffers_.size() < all.size()) {
            instance_buffers_.resize(all.size());
        }
        ComPtr<ID3D11Buffer>& buffer = instance_buffers_[static_cast<std::size_t>(terrain)];
        if (!buffer) {
            CD3D11_BUFFER_DESC desc{
                static_cast<UINT>(td.inst.size() * sizeof(GpuDecorationInstance)),
                D3D11_BIND_VERTEX_BUFFER,
                D3D11_USAGE_DEFAULT,
            };
            D3D11_SUBRESOURCE_DATA init{td.inst.data(), 0, 0};
            if (td.inst.empty() || FAILED(device_->CreateBuffer(&desc, &init, &buffer))) {
                xlog::warn("[AlpineTerrain] Terrain {} draws no decorations: their buffer could not be created",
                           alpine_terrain_get_all()[static_cast<std::size_t>(terrain)].uid);
                td.chunks.clear();
                return nullptr;
            }
            td.dirty_chunks.clear();
        }
        for (std::uint32_t c : td.dirty_chunks) {
            const DecorationChunk& chunk = td.chunks[c];
            for (std::uint32_t d = 0; d < at::max_decorations; d++) {
                if (chunk.count[d] == 0) continue;
                constexpr UINT stride = sizeof(GpuDecorationInstance);
                const D3D11_BOX box{chunk.first[d] * stride, 0, 0, (chunk.first[d] + chunk.count[d]) * stride, 1, 1};
                render_context_.device_context()->UpdateSubresource(buffer, 0, &box, &td.inst[chunk.first[d]], 0, 0);
            }
        }
        td.dirty_chunks.clear();
        return buffer;
    }

    void DecorationRenderer::set_submesh(const rf::Vector3& center, float draw_distance)
    {
        const float state[4] = {center.x, center.y, center.z, draw_distance};
        if (cbuffer_valid_ && std::equal(std::begin(state), std::end(state), std::begin(cbuffer_state_))) {
            return;
        }
        std::copy(std::begin(state), std::end(state), std::begin(cbuffer_state_));
        cbuffer_valid_ = true;

        DecorationBufferData data{};
        data.submesh_center = {center.x, center.y, center.z};
        data.draw_distance = draw_distance;
        data.fade_band = std::max(min_fade_band, fade_band_fraction * draw_distance);
        D3D11_MAPPED_SUBRESOURCE mapped;
        DF_GR_D3D11_CHECK_HR(render_context_.device_context()->Map(cbuffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        std::memcpy(mapped.pData, &data, sizeof(data));
        render_context_.device_context()->Unmap(cbuffer_, 0);
    }

    void DecorationRenderer::render(rf::GSolid* solid, const std::vector<AlpineTerrainRoomRef>& chunks)
    {
        if (chunks.empty() || !shaders_ok_) {
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        auto& all = alpine_terrain_decorations_get_all();
        DecorationFrameStats& stats = alpine_terrain_decorations_frame_stats();
        const rf::Vector3 eye = rf::gr::eye_pos;
        // Linear in the distance, so one engine call scales them all
        const float apparent_per_meter = rf::gr::get_apparent_distance_from_camera(eye + rf::Vector3{1.0f, 0.0f, 0.0f});

        sorted_chunks_.assign(chunks.begin(), chunks.end());
        std::sort(sorted_chunks_.begin(), sorted_chunks_.end(), [](const auto& a, const auto& b) {
            return a.terrain != b.terrain ? a.terrain < b.terrain : a.chunk < b.chunk;
        });

        RenderContext::ScopedPicmipActive picmip_scope{render_context_, true};
        render_context_.set_sky_room(false);
        render_context_.set_draw_room_uid(-1);
        // Dynamic lights only: the baked light in each instance holds the static ones. A sun scale above zero
        // uploads the sun, which the shader scales per instance.
        rf::gr::light_filter_set_solid(solid, 1, 0);
        render_context_.update_lights(false, nullptr, 1.0f);
        render_context_.set_vertex_shader(vertex_shader_);
        render_context_.set_pixel_shader(render_context_.has_gas_regions() ? pixel_shader_ : pixel_shader_no_gas_);
        render_context_.set_primitive_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        render_context_.bind_vs_cbuffer(cbuffer_slot, cbuffer_);

        for (std::size_t begin = 0, end = 0; begin < sorted_chunks_.size(); begin = end) {
            const int terrain = sorted_chunks_[begin].terrain;
            while (end < sorted_chunks_.size() && sorted_chunks_[end].terrain == terrain) {
                end++;
            }
            ID3D11Buffer* buffer = instance_buffer(terrain);
            if (!buffer) continue;
            const TerrainDecorations& td = all[static_cast<std::size_t>(terrain)];
            render_context_.set_vertex_buffer(buffer, sizeof(GpuDecorationInstance), instance_slot);
            chunk_distance_.clear();
            for (std::size_t i = begin; i < end; i++) {
                chunk_distance_.push_back(std::sqrt(td.chunks[sorted_chunks_[i].chunk].dist_sq(eye)));
            }
            stats.visible_chunks += static_cast<std::uint32_t>(end - begin);

            for (std::uint32_t d = 0; d < at::max_decorations; d++) {
                if (td.mesh_slot[d] < 0) continue;
                auto* v3d = static_cast<rf::V3d*>(alpine_terrain_decorations_mesh(td.mesh_slot[d]).mesh->instance);
                if (!v3d || !v3d->meshes) continue;
                const float draw_distance = td.draw_distance[d];
                for (int m = 0; m < v3d->num_meshes; m++) {
                    rf::V3dMesh& submesh = v3d->meshes[m];
                    rf::VifLodMesh* lod_mesh = submesh.vu;
                    if (!lod_mesh || lod_mesh->num_levels < 1) continue;
                    set_submesh(lod_mesh->center, draw_distance);
                    auto* materials = reinterpret_cast<rf::MeshMaterial*>(submesh.materials);
                    for (std::size_t i = begin; i < end; i++) {
                        const DecorationChunk& chunk = td.chunks[sorted_chunks_[i].chunk];
                        const float distance = chunk_distance_[i - begin];
                        if (chunk.count[d] == 0 || distance > draw_distance) continue;
                        const int lod = select_lod(*lod_mesh, distance * apparent_per_meter);
                        const auto* batches = mesh_renderer_.bind_v3d_buffers(
                            lod_mesh, lod, submesh.num_materials > 0 ? materials : nullptr, submesh.num_materials);
                        if (!batches) continue;
                        const int* tex_handles = lod_mesh->meshes[lod]->tex_handles;
                        constexpr int max_textures = std::extent_v<decltype(rf::VifMesh::tex_handles)>;
                        for (const auto& b : *batches) {
                            render_context_.set_cull_mode(b.double_sided ? D3D11_CULL_NONE : D3D11_CULL_BACK);
                            const bool textured = b.texture_index >= 0 && b.texture_index < max_textures;
                            const int texture = textured ? tex_handles[b.texture_index] : -1;
                            const float self_illum =
                                b.mode.get_color_source() == rf::gr::COLOR_SOURCE_TEXTURE ? 1.0f : b.self_illumination;
                            render_context_.set_mode(b.mode, {255, 255, 255, 255}, false, true, self_illum);
                            render_context_.set_textures(texture, -1);
                            render_context_.draw_indexed_instanced(b.num_indices, static_cast<int>(chunk.count[d]),
                                                                   b.start_index, b.base_vertex,
                                                                   static_cast<int>(chunk.first[d]));
                            stats.draws++;
                        }
                        if (m == 0) {
                            stats.instances += chunk.count[d];
                        }
                    }
                }
            }
        }

        rf::gr::light_filter_reset();
        render_context_.update_lights();
        stats.cpu_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }

    void DecorationRenderer::release()
    {
        instance_buffers_.clear();
    }
}
