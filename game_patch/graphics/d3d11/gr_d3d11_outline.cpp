#include <common/utils/list-utils.h>
#include <xlog/xlog.h>
#include "gr_d3d11_outline.h"
#include "gr_d3d11.h"
#include "gr_d3d11_state.h"
#include "gr_d3d11_context.h"
#include "gr_d3d11_shader.h"
#include "gr_d3d11_mesh.h"
#include "../../misc/alpine_settings.h"
#include "../../multi/multi.h"
#include "../../multi/gametype.h"
#include "../../hud/multi_spectate.h"
#include "../../rf/multi.h"
#include "../../rf/player/player.h"
#include "../../rf/entity.h"
#include "../../rf/character.h"
#include "../../rf/vmesh.h"
#include "../../rf/gr/gr.h"
#include "../../rf/os/frametime.h"
#include "../../multi/server.h"

using namespace rf;

namespace df::gr::d3d11
{
    // VS cbuffer b4: { float2 screen_resolution, float outline_thickness, float padding }
    struct OutlineVSParams
    {
        float screen_width;
        float screen_height;
        float outline_thickness;
        float padding;
    };
    static_assert(sizeof(OutlineVSParams) == 16);

    // PS cbuffer b2: { float4 outline_color }
    struct OutlinePSColor
    {
        float r, g, b, a;
    };
    static_assert(sizeof(OutlinePSColor) == 16);

    static ComPtr<ID3D11Buffer> create_dynamic_cbuffer(ID3D11Device* device, UINT size)
    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = size;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

        ComPtr<ID3D11Buffer> buffer;
        check_hr(
            device->CreateBuffer(&desc, nullptr, &buffer),
            []() { xlog::error("Failed to create outline cbuffer"); }
        );
        return buffer;
    }

    template<typename T>
    static void update_cbuffer(ID3D11DeviceContext* ctx, ID3D11Buffer* buffer, const T& data)
    {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = ctx->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (SUCCEEDED(hr)) {
            memcpy(mapped.pData, &data, sizeof(T));
            ctx->Unmap(buffer, 0);
        }
    }

    OutlineRenderer::OutlineRenderer(ID3D11Device* device, ShaderManager& shader_manager, StateManager& state_manager, RenderContext& render_context)
        : device_{device}
        , shader_manager_{shader_manager}
        , state_manager_{state_manager}
        , render_context_{render_context}
    {
        outline_vs_params_buffer_ = create_dynamic_cbuffer(device, sizeof(OutlineVSParams));
        outline_ps_color_buffer_ = create_dynamic_cbuffer(device, sizeof(OutlinePSColor));
    }

    void OutlineRenderer::begin_frame()
    {
        // Only run once per game frame. The fpgun's setup_3d triggers a second
        // begin_frame which would rebuild xray_forced_ and cause double rendering.
        if (last_begin_frame_ == rf::frame_count) {
            return;
        }
        last_begin_frame_ = rf::frame_count;

        ci_map_.clear();
        queue_.clear();
        v3d_queue_.clear();
        xray_forced_.clear();
        flushed_cis_.clear();
        current_character_outline_ = nullptr;
        next_stencil_ref_ = 1;

        // Save the main-scene projection for use by flush_forced_xray().
        // begin_frame() runs inside setup_3d() after update_view_proj_transform(),
        // so render_context_ already has the main scene projection.
        saved_projection_ = render_context_.projection();

        // Only active in multiplayer
        if (!rf::is_multi) {
            return;
        }

        // Check client setting
        if (!g_alpine_game_config.try_outlines) {
            // Even if outlines are off, spectator-only mode might be active
            if (!g_alpine_game_config.outlines_spectator_only) {
                return;
            }
        }

        // Spectator-only mode check
        bool is_spectating = multi_spectate_is_spectating();
        if (g_alpine_game_config.outlines_spectator_only && !is_spectating) {
            if (!g_alpine_game_config.try_outlines) {
                return;
            }
        }

        // Check server permissions (spectators always allowed)
        if (!is_spectating) {
            auto& server_info = get_af_server_info();
            if (server_info.has_value() && !server_info->allow_outlines) {
                return;
            }
            if (!server_info.has_value()) {
                return;
            }
        }

        // Determine xray permission
        bool xray_allowed = true;
        if (!is_spectating) {
            auto& server_info = get_af_server_info();
            if (!server_info.has_value() || !server_info->allow_outlines_xray) {
                xray_allowed = false;
            }
        }

        bool is_team_mode = multi_is_team_game_type();
        rf::Player* local_player = rf::local_player;
        if (!local_player) {
            return;
        }

        int local_team = local_player->team;

        // When spectating in first-person view, skip the spectated player's mesh
        rf::Player* spectate_target = is_spectating ? multi_spectate_get_target_player() : nullptr;

        // Iterate all players, build CI map
        for (rf::Player& player : SinglyLinkedList{rf::player_list}) {
            // Skip local player (don't outline yourself)
            if (&player == local_player) {
                continue;
            }

            // Skip the player we are spectating (their mesh is our first-person view)
            if (spectate_target && &player == spectate_target) {
                continue;
            }

            // Get entity
            rf::Entity* entity = rf::entity_from_handle(player.entity_handle);
            if (!entity) {
                continue;
            }

            // Get CharacterInstance from vmesh
            if (!entity->vmesh || entity->vmesh->type != rf::MESH_TYPE_CHARACTER) {
                continue;
            }
            auto* ci = static_cast<const rf::CharacterInstance*>(entity->vmesh->instance);
            if (!ci) {
                continue;
            }

            // Assign stencil ref (1-255)
            if (next_stencil_ref_ > 255) {
                break; // max 255 outlined characters
            }

            // Determine if this player is an enemy or teammate
            bool is_enemy = true;
            if (is_team_mode && !is_spectating) {
                is_enemy = (player.team != local_team);
            }

            // Determine color
            float r, g, b, a;

            if (is_spectating) {
                // Spectator: all players visible, use team colors
                if (is_team_mode) {
                    uint32_t color = (player.team == 0)
                        ? g_alpine_game_config.outlines_color_team_r
                        : g_alpine_game_config.outlines_color_team_b;
                    std::tie(r, g, b, a) = extract_normalized_color_components(color);
                } else {
                    std::tie(r, g, b, a) = extract_normalized_color_components(g_alpine_game_config.outlines_color);
                }
            }
            else if (!is_team_mode) {
                // FFA/DM: all are enemies
                uint32_t color = g_alpine_game_config.outlines_color_enemy.value_or(g_alpine_game_config.outlines_color);
                std::tie(r, g, b, a) = extract_normalized_color_components(color);
            }
            else if (is_enemy) {
                // Team mode, enemy
                uint32_t default_enemy_color = (player.team == 0)
                    ? g_alpine_game_config.outlines_color_team_r
                    : g_alpine_game_config.outlines_color_team_b;
                uint32_t color = g_alpine_game_config.outlines_color_enemy.value_or(default_enemy_color);
                std::tie(r, g, b, a) = extract_normalized_color_components(color);
            }
            else {
                // Team mode, teammate
                uint32_t default_team_color = (local_team == 0)
                    ? g_alpine_game_config.outlines_color_team_r
                    : g_alpine_game_config.outlines_color_team_b;
                uint32_t color = g_alpine_game_config.outlines_color_team.value_or(default_team_color);
                std::tie(r, g, b, a) = extract_normalized_color_components(color);
            }

            // Determine xray
            bool xray = false;
            if (is_spectating) {
                xray = true;
            }
            else if (!is_enemy && g_alpine_game_config.try_outlines_team_xray && xray_allowed) {
                xray = true;
            }

            OutlineInfo info{};
            info.r = r;
            info.g = g;
            info.b = b;
            info.a = a;
            info.xray = xray;
            info.stencil_ref = next_stencil_ref_++;

            ci_map_.emplace(ci, info);

            // Store data for xray players so we can queue their outlines even if
            // the portal renderer culls them (entity in a different room/portal).
            if (xray && ci->base_character &&
                ci->base_character->num_character_meshes > 0 &&
                ci->base_character->character_meshes[0].mesh) {
                ForcedXrayEntry forced{};
                forced.lod_mesh = ci->base_character->character_meshes[0].mesh->vu;
                forced.pos = entity->pos;
                forced.orient = entity->orient;
                forced.ci = ci;
                forced.info = info;
                if (forced.lod_mesh) {
                    xray_forced_.push_back(forced);
                }
            }
        }

    }

    const OutlineInfo* OutlineRenderer::lookup(const rf::CharacterInstance* ci) const
    {
        auto it = ci_map_.find(ci);
        if (it != ci_map_.end()) {
            return &it->second;
        }
        return nullptr;
    }

    void OutlineRenderer::queue(QueuedOutline entry)
    {
        queue_.push_back(std::move(entry));
    }

    void OutlineRenderer::queue_v3d(QueuedV3dOutline entry)
    {
        v3d_queue_.push_back(std::move(entry));
    }

    const OutlineInfo* OutlineRenderer::current_character_outline() const
    {
        return current_character_outline_;
    }

    void OutlineRenderer::set_current_character_outline(const OutlineInfo* info)
    {
        current_character_outline_ = info;
    }

    void OutlineRenderer::queue_unrendered_xray_outlines()
    {
        if (xray_forced_.empty()) {
            return;
        }

        // Queue forced xray outlines for players not already rendered by flush()
        for (const auto& forced : xray_forced_) {
            if (flushed_cis_.find(forced.ci) == flushed_cis_.end()) {
                QueuedOutline entry{};
                entry.lod_mesh = forced.lod_mesh;
                entry.lod_index = 0;
                entry.pos = forced.pos;
                entry.orient = forced.orient;
                entry.ci = forced.ci;
                entry.info = forced.info;
                queue_.push_back(std::move(entry));
            }
        }
    }

    void OutlineRenderer::flush(MeshRenderer& mesh_renderer)
    {
        if (queue_.empty() && v3d_queue_.empty()) {
            return;
        }

        for (const auto& outline : queue_) {
            flushed_cis_.insert(outline.ci);
            render_outline(outline, mesh_renderer);
        }

        for (const auto& outline : v3d_queue_) {
            render_v3d_outline(outline, mesh_renderer);
        }

        queue_.clear();
        v3d_queue_.clear();

        // Outline rendering sets custom depth/stencil/blend states that bypass set_mode().
        // Invalidate the cached mode so the next set_mode() call from normal rendering
        // forces a full state reset (depth test re-enabled, correct blend, etc.).
        render_context_.invalidate_mode();
    }

    void OutlineRenderer::flush_forced_xray(MeshRenderer& mesh_renderer)
    {
        // Force-queue xray outlines for players culled by the portal renderer.
        // Called from flip() so these render after the main scene is complete.
        // Xray outlines don't need depth data (DepthEnable=FALSE on both passes)
        // so they can render after zbuffer_clear without issues.
        queue_unrendered_xray_outlines();

        if (!queue_.empty()) {
            // The fpgun's setup_3d may have overwritten the view/projection cbuffer
            // with a different FOV. Restore the main-scene projection so outlines
            // render at the correct screen positions.
            Projection current_proj = render_context_.projection();
            render_context_.update_view_proj_transform(saved_projection_);

            for (const auto& outline : queue_) {
                render_outline(outline, mesh_renderer);
            }
            queue_.clear();

            // Restore the fpgun projection
            render_context_.update_view_proj_transform(current_proj);
            render_context_.invalidate_mode();
        }

        xray_forced_.clear();
    }

    void OutlineRenderer::render_outline(const QueuedOutline& outline, MeshRenderer& mesh_renderer)
    {
        auto* ctx = render_context_.device_context();

        // Prepare mesh: set model transform, bone transforms, bind vertex/index buffers
        const auto* batches = mesh_renderer.prepare_character_for_draw(
            outline.lod_mesh, outline.lod_index,
            outline.pos, outline.orient, outline.ci);
        if (!batches || batches->empty()) {
            return;
        }
        render_context_.set_primitive_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // ---- Pass 1: Stencil mark ----
        // Write stencil ref where character pixels are drawn. No color write.
        // Xray outlines disable depth test so stencil is written even behind walls.
        auto stencil_mark = outline.info.xray
            ? state_manager_.get_outline_stencil_mark_xray_state()
            : state_manager_.get_outline_stencil_mark_state();
        render_context_.set_depth_stencil_state(stencil_mark, outline.info.stencil_ref);
        render_context_.set_blend_state(state_manager_.get_no_color_write_blend_state());
        render_context_.set_cull_mode(D3D11_CULL_BACK);

        // Use the regular character vertex shader for the stencil mark pass
        auto& char_vs = shader_manager_.get_vertex_shader(VertexShaderId::character);
        render_context_.set_vertex_shader(char_vs);
        // Any pixel shader is fine since color write is disabled; use the outline PS for simplicity
        render_context_.set_pixel_shader(shader_manager_.get_pixel_shader(PixelShaderId::outline));

        for (const auto& batch : *batches) {
            render_context_.draw_indexed(batch.num_indices, batch.start_index, batch.base_vertex);
        }

        // ---- Pass 2: Outline ----
        // Draw inflated mesh with stencil NOT_EQUAL test
        auto depth_stencil = outline.info.xray
            ? state_manager_.get_outline_xray_state()
            : state_manager_.get_outline_depth_test_state();
        render_context_.set_depth_stencil_state(depth_stencil, outline.info.stencil_ref);

        // Alpha blending for semi-transparent outlines
        render_context_.set_blend_state(
            state_manager_.lookup_blend_state(rf::gr::ALPHA_BLEND_ALPHA));

        // Front-face culling (render backfaces only) for cleaner silhouette
        render_context_.set_cull_mode(D3D11_CULL_FRONT);

        // Set outline vertex shader with extrusion
        auto& outline_vs = shader_manager_.get_vertex_shader(VertexShaderId::outline_character);
        render_context_.set_vertex_shader(outline_vs);
        render_context_.set_pixel_shader(shader_manager_.get_pixel_shader(PixelShaderId::outline));

        // Update VS params cbuffer (b4): screen resolution + thickness
        OutlineVSParams vs_params{};
        vs_params.screen_width = static_cast<float>(rf::gr::screen.max_w);
        vs_params.screen_height = static_cast<float>(rf::gr::screen.max_h);
        vs_params.outline_thickness = 1.0f;
        vs_params.padding = 0.0f;
        update_cbuffer(ctx, outline_vs_params_buffer_, vs_params);

        ID3D11Buffer* vs_b4[] = { outline_vs_params_buffer_ };
        ctx->VSSetConstantBuffers(4, 1, vs_b4);

        // Update PS color cbuffer (b2): outline color
        OutlinePSColor ps_color{};
        ps_color.r = outline.info.r;
        ps_color.g = outline.info.g;
        ps_color.b = outline.info.b;
        ps_color.a = outline.info.a;
        update_cbuffer(ctx, outline_ps_color_buffer_, ps_color);

        ID3D11Buffer* ps_b2[] = { outline_ps_color_buffer_ };
        ctx->PSSetConstantBuffers(2, 1, ps_b2);

        for (const auto& batch : *batches) {
            render_context_.draw_indexed(batch.num_indices, batch.start_index, batch.base_vertex);
        }

        // Reset cull mode back to normal
        render_context_.set_cull_mode(D3D11_CULL_BACK);
    }

    void OutlineRenderer::render_v3d_outline(const QueuedV3dOutline& outline, MeshRenderer& mesh_renderer)
    {
        auto* ctx = render_context_.device_context();

        // Prepare static mesh: set model transform, bind vertex/index buffers
        const auto* batches = mesh_renderer.prepare_v3d_for_draw(
            outline.lod_mesh, outline.lod_index,
            outline.pos, outline.orient);
        if (!batches || batches->empty()) {
            return;
        }
        render_context_.set_primitive_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // ---- Pass 1: Stencil mark ----
        auto stencil_mark = outline.info.xray
            ? state_manager_.get_outline_stencil_mark_xray_state()
            : state_manager_.get_outline_stencil_mark_state();
        render_context_.set_depth_stencil_state(stencil_mark, outline.info.stencil_ref);
        render_context_.set_blend_state(state_manager_.get_no_color_write_blend_state());
        render_context_.set_cull_mode(D3D11_CULL_BACK);

        // Use the regular standard vertex shader for the stencil mark pass
        auto& std_vs = shader_manager_.get_vertex_shader(VertexShaderId::standard);
        render_context_.set_vertex_shader(std_vs);
        render_context_.set_pixel_shader(shader_manager_.get_pixel_shader(PixelShaderId::outline));

        for (const auto& batch : *batches) {
            render_context_.draw_indexed(batch.num_indices, batch.start_index, batch.base_vertex);
        }

        // ---- Pass 2: Outline ----
        auto depth_stencil = outline.info.xray
            ? state_manager_.get_outline_xray_state()
            : state_manager_.get_outline_depth_test_state();
        render_context_.set_depth_stencil_state(depth_stencil, outline.info.stencil_ref);

        render_context_.set_blend_state(
            state_manager_.lookup_blend_state(rf::gr::ALPHA_BLEND_ALPHA));

        render_context_.set_cull_mode(D3D11_CULL_FRONT);

        // Set outline standard vertex shader with extrusion (no bone transforms)
        auto& outline_vs = shader_manager_.get_vertex_shader(VertexShaderId::outline_standard);
        render_context_.set_vertex_shader(outline_vs);
        render_context_.set_pixel_shader(shader_manager_.get_pixel_shader(PixelShaderId::outline));

        // Update VS params cbuffer (b4): screen resolution + thickness
        OutlineVSParams vs_params{};
        vs_params.screen_width = static_cast<float>(rf::gr::screen.max_w);
        vs_params.screen_height = static_cast<float>(rf::gr::screen.max_h);
        vs_params.outline_thickness = 1.0f;
        vs_params.padding = 0.0f;
        update_cbuffer(ctx, outline_vs_params_buffer_, vs_params);

        ID3D11Buffer* vs_b4[] = { outline_vs_params_buffer_ };
        ctx->VSSetConstantBuffers(4, 1, vs_b4);

        // Update PS color cbuffer (b2): outline color
        OutlinePSColor ps_color{};
        ps_color.r = outline.info.r;
        ps_color.g = outline.info.g;
        ps_color.b = outline.info.b;
        ps_color.a = outline.info.a;
        update_cbuffer(ctx, outline_ps_color_buffer_, ps_color);

        ID3D11Buffer* ps_b2[] = { outline_ps_color_buffer_ };
        ctx->PSSetConstantBuffers(2, 1, ps_b2);

        for (const auto& batch : *batches) {
            render_context_.draw_indexed(batch.num_indices, batch.start_index, batch.base_vertex);
        }

        // Reset cull mode back to normal
        render_context_.set_cull_mode(D3D11_CULL_BACK);
    }
}
