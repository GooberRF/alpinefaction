#include <algorithm>
#include <cmath>
#include <common/utils/list-utils.h>
#include <xlog/xlog.h>
#include "gr_d3d11_outline.h"
#include "gr_d3d11.h"
#include "gr_d3d11_state.h"
#include "gr_d3d11_context.h"
#include "gr_d3d11_shader.h"
#include "gr_d3d11_mesh.h"
#include "gr_d3d11_vertex.h"
#include "../../misc/alpine_settings.h"
#include "../../multi/multi.h"
#include "../../multi/gametype.h"
#include "../../multi/bagman.h"
#include "../../multi/salvage.h"
#include "../../hud/multi_spectate.h"
#include "../../rf/multi.h"
#include "../../rf/player/player.h"
#include "../../rf/entity.h"
#include "../../rf/character.h"
#include "../../rf/vmesh.h"
#include "../../rf/vfx.h"
#include "../../rf/gr/gr.h"
#include "../../rf/os/frametime.h"

namespace gr::d3d11
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
        DF_GR_D3D11_CHECK_HR(ctx->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        memcpy(mapped.pData, &data, sizeof(T));
        ctx->Unmap(buffer, 0);
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
        vfx_queue_.clear();
        xray_forced_.clear();
        flushed_cis_.clear();
        current_character_outline_ = nullptr;
        next_stencil_ref_ = 1;

        // Save the main-scene camera for use by flush_forced_xray().
        // begin_frame() runs inside setup_3d() after update_view_proj_transform(),
        // so render_context_ already has the main scene projection and the engine
        // globals still hold the eye gr_setup_3d was just called with.
        // Both halves matter: gr_setup_3d writes rf::gr::eye_pos / eye_matrix as
        // well as the projection scales, and gameplay_render_frame's closing
        // gr_setup_3d re-binds the world FOV with the *unoffset* camera position
        // (the scene itself renders from camera_pos - 0.14 * camera_forward).
        saved_projection_ = render_context_.projection();
        saved_eye_pos_ = rf::gr::eye_pos;
        saved_eye_orient_ = rf::gr::eye_matrix;

        // Only active in multiplayer
        if (!rf::is_multi) {
            return;
        }

        bool is_spectating = multi_spectate_is_spectating();

        // The objective carrier is outlined green through walls for everybody, no
        // toggle and no team distinction: whoever holds the bag or the salvage flag
        // is what the whole server is chasing.
        auto outline_objective_carrier = [&](rf::Player* carrier) {
            if (!carrier || next_stencil_ref_ > 255) {
                return;
            }
            rf::Entity* entity = rf::entity_from_handle(carrier->entity_handle);
            if (!entity || rf::entity_is_dying(entity)) {
                return;
            }
            if (!entity->vmesh || entity->vmesh->type != rf::MESH_TYPE_CHARACTER) {
                return;
            }
            auto* ci = static_cast<rf::CharacterInstance*>(entity->vmesh->instance);
            if (!ci) {
                return;
            }

            OutlineInfo info{};
            info.r = 0.0f;
            info.g = 1.0f;
            info.b = 0.0f;
            info.a = 1.0f;
            info.xray = true;
            info.stencil_ref = next_stencil_ref_++;
            ci_map_.emplace(ci, info);

            if (ci->base_character
                && ci->base_character->num_character_meshes > 0
                && ci->base_character->character_meshes[0].mesh) {
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
        };

        // Outline the bag carrier player.
        if (gt_is_bagman_any() && !bagman_viewer_is_carrier_first_person()) {
            outline_objective_carrier(g_bagman_info.carrier);
        }

        // Outline the salvage flag carrier player, the same way.
        if (gt_is_salvage() && !salvage_viewer_is_carrier_first_person()) {
            outline_objective_carrier(g_salvage_info.carrier);
        }

        // Cache the bagman bag outlines for this frame.
        bagman_pickup_xray_ = ForcedV3dXrayEntry{};
        bagman_carrier_xray_ = ForcedV3dXrayEntry{};
        if (gt_is_bagman_any()) {
            rf::VifLodMesh* lod = nullptr;
            rf::Vector3 bp{};
            rf::Matrix3 bo{};
            if (bagman_query_pickup_bag_outline(&lod, &bp, &bo)) {
                bagman_pickup_xray_.lod_mesh = lod;
                bagman_pickup_xray_.pos = bp;
                bagman_pickup_xray_.orient = bo;
            }
            if (bagman_query_carrier_bag_outline(&lod, &bp, &bo)) {
                bagman_carrier_xray_.lod_mesh = lod;
                bagman_carrier_xray_.pos = bp;
                bagman_carrier_xray_.orient = bo;
            }
        }

        // Queue the salvage flag outline for this frame. Unlike the bag there is no
        // natural-render path to piggyback on: the flag is a .vfx mesh drawn by
        // gr_poly, which never reaches render_v3d_vif, so nothing queues it while the
        // object pass runs. It is drawn by flush_vfx(), from an explicit drain point
        // once the scene is complete (normally just before the fpgun renders — see
        // Renderer::flush_outlines_before_fpgun), which rebinds the scene camera
        // explicitly because the engine has moved on by then. Its stencil ref
        // must be its own — a shared ref would let the flag and the carrier erase
        // each other's silhouettes.
        if (gt_is_salvage() && next_stencil_ref_ <= 255) {
            rf::VMesh* flag_vmesh = nullptr;
            rf::Vector3 fp{};
            rf::Matrix3 fo{};
            if (salvage_query_flag_outline(&flag_vmesh, &fp, &fo)) {
                QueuedVfxOutline entry{};
                entry.vmesh = flag_vmesh;
                entry.pos = fp;
                entry.orient = fo;
                entry.info.r = 0.0f;
                entry.info.g = 1.0f;
                entry.info.b = 0.0f;
                entry.info.a = 1.0f;
                entry.info.xray = true;
                entry.info.stencil_ref = next_stencil_ref_++;
                vfx_queue_.push_back(std::move(entry));
            }
        }

        if (is_spectating) {
            // Spectator outlines: client toggle only, no server permission needed
            if (!g_alpine_game_config.outlines_spectator) {
                return;
            }
        }
        else {
            // Spawned player outlines: requires client toggle AND server permission
            if (!g_alpine_game_config.try_outlines) {
                return;
            }
            if (!rf::is_server) {
                auto& server_info = get_af_server_info();
                if (!server_info.has_value() || !server_info->allow_outlines) {
                    return;
                }
            }
        }

        // Determine xray permission (spectators always allowed, servers always allowed)
        bool xray_allowed = true;
        if (!is_spectating && !rf::is_server) {
            auto& server_info = get_af_server_info();
            if (!server_info.has_value() || !server_info->allow_outlines_xray) {
                xray_allowed = false;
            }
        }

        bool is_team_mode = multi_is_team_game_type();
        const bool is_salvage = gt_is_salvage();
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

            if (gt_is_bagman_any() && g_bagman_info.carrier == &player) {
                continue;
            }

            // Same for the salvage flag carrier: the green objective outline above
            // must not be replaced by a team-coloured one here.
            if (is_salvage && g_salvage_info.carrier == &player) {
                continue;
            }

            // Get entity
            rf::Entity* entity = rf::entity_from_handle(player.entity_handle);
            if (!entity) {
                continue;
            }

            // Skip dying/dead entities
            if (rf::entity_is_dying(entity)) {
                continue;
            }

            // Get CharacterInstance from vmesh
            if (!entity->vmesh || entity->vmesh->type != rf::MESH_TYPE_CHARACTER) {
                continue;
            }
            auto* ci = static_cast<rf::CharacterInstance*>(entity->vmesh->instance);
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

    void OutlineRenderer::maybe_queue_bag_outline(
        rf::VifLodMesh* lod_mesh, int lod_index,
        const rf::Vector3& pos, const rf::Matrix3& orient)
    {
        // Match the rendered lod against either cached bag (pickup or carrier)
        // and queue a green xray outline. Marks the matched entry as naturally
        // rendered so flush_forced_xray doesn't double it on portal-cull
        // recovery.
        ForcedV3dXrayEntry* match = nullptr;
        if (bagman_pickup_xray_.lod_mesh == lod_mesh) {
            match = &bagman_pickup_xray_;
        }
        else if (bagman_carrier_xray_.lod_mesh == lod_mesh) {
            match = &bagman_carrier_xray_;
        }
        if (!match) return;
        if (next_stencil_ref_ > 255) return;

        QueuedV3dOutline entry{};
        entry.lod_mesh = lod_mesh;
        entry.lod_index = lod_index;
        entry.pos = pos;
        entry.orient = orient;
        entry.info.r = 0.0f;
        entry.info.g = 1.0f;
        entry.info.b = 0.0f;
        entry.info.a = 1.0f;
        entry.info.xray = true;
        entry.info.stencil_ref = next_stencil_ref_++;
        v3d_queue_.push_back(std::move(entry));
        match->naturally_rendered = true;
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

    void OutlineRenderer::refresh_vfx_transforms()
    {
        // begin_frame() runs from setup_3d, before item_render advances the pickup
        // spin, so the transform sampled there is a frame stale. Re-sample it at draw
        // time. render_vfx_outline still prefers the engine's own per-chunk cache when
        // the two agree — that is the exact pair the engine drew with — and falls back
        // to this one when the item was culled and the cache never refreshed.
        if (vfx_queue_.empty()) {
            return;
        }
        // The salvage flag is the only thing that ever queues a .vfx outline, so an
        // entry the live query no longer backs is stale by definition — the flag was
        // taken, captured or destroyed between begin_frame() and here, and its
        // rf::VMesh* may already be freed. Drop those rather than dereference them
        // at draw time.
        rf::VMesh* flag_vmesh = nullptr;
        rf::Vector3 fp{};
        rf::Matrix3 fo{};
        if (!gt_is_salvage() || !salvage_query_flag_outline(&flag_vmesh, &fp, &fo)) {
            vfx_queue_.clear();
            return;
        }
        std::erase_if(vfx_queue_, [flag_vmesh](const QueuedVfxOutline& outline) {
            return outline.vmesh != flag_vmesh;
        });
        for (auto& outline : vfx_queue_) {
            outline.pos = fp;
            outline.orient = fo;
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

    void OutlineRenderer::flush_vfx()
    {
        if (vfx_queue_.empty()) {
            return;
        }

        // The scene camera is already torn down by the time 2D begins — the fpgun's
        // gr_setup_3d rebound both the projection and rf::gr::eye_pos / eye_matrix — so
        // this needs the same explicit rebind of the saved scene camera that
        // flush_forced_xray() performs, for exactly the same reason.
        Projection current_proj = render_context_.projection();
        render_context_.update_view_proj_transform(saved_projection_, saved_eye_pos_, saved_eye_orient_);

        refresh_vfx_transforms();
        for (const auto& outline : vfx_queue_) {
            render_vfx_outline(outline);
        }
        vfx_queue_.clear();

        // Restore whatever projection the caller had bound and drop the cached mode:
        // the outline passes set depth/stencil/blend states that bypass set_mode().
        render_context_.update_view_proj_transform(current_proj);
        render_context_.invalidate_mode();
    }

    void OutlineRenderer::flush_forced_xray(MeshRenderer& mesh_renderer)
    {
        // Force-queue xray outlines for players culled by the portal renderer.
        // Called from flip() so these render after the main scene is complete.
        // Xray outlines don't need depth data (DepthEnable=FALSE on both passes)
        // so they can render after zbuffer_clear without issues.
        queue_unrendered_xray_outlines();

        // A bag mesh needs a forced outline when it has a cached entry.
        const bool need_forced_pickup =
            bagman_pickup_xray_.lod_mesh && !bagman_pickup_xray_.naturally_rendered;
        const bool need_forced_carrier =
            bagman_carrier_xray_.lod_mesh && !bagman_carrier_xray_.naturally_rendered;

        if (queue_.empty() && v3d_queue_.empty() && vfx_queue_.empty()
            && !need_forced_pickup && !need_forced_carrier) {
            xray_forced_.clear();
            return;
        }

        // Rebind the camera the scene was rendered with. Restoring the projection
        // alone is not enough: ViewProjTransformBuffer::update() rebuilds the view
        // matrix from rf::gr::eye_pos / eye_matrix as they stand at call time, and by
        // flip() the engine has moved them. gr_setup_3d assigns both globals on every
        // call, and gameplay_render_frame's closing call (0x00432935) passes the raw
        // camera position while the scene itself was rendered from
        // camera_pos - 0.14 * camera_forward — a camera 0.14 units ahead of the
        // scene's, which scales everything radially about the screen centre by a few
        // percent. That reads as an outline that is displaced and "stretched by FOV",
        // worst at the screen edges and on nearby objects.
        Projection current_proj = render_context_.projection();
        render_context_.update_view_proj_transform(saved_projection_, saved_eye_pos_, saved_eye_orient_);

        // Characters first: this recomputes their bones (frozen while culled),
        // which the carrier bag attach transform depends on.
        for (const auto& outline : queue_) {
            rf::ci_update_bone_transforms(const_cast<rf::CharacterInstance*>(outline.ci));
            render_outline(outline, mesh_renderer);
        }
        queue_.clear();

        // Force-queue culled bag outlines with freshly-computed transforms.
        auto push_forced_bag = [&](rf::VifLodMesh* lod,
            const rf::Vector3& pos, const rf::Matrix3& orient) {
            if (!lod || next_stencil_ref_ > 255) return;
            QueuedV3dOutline entry{};
            entry.lod_mesh = lod;
            entry.lod_index = 0;
            entry.pos = pos;
            entry.orient = orient;
            entry.info.r = 0.0f;
            entry.info.g = 1.0f;
            entry.info.b = 0.0f;
            entry.info.a = 1.0f;
            entry.info.xray = true;
            entry.info.stencil_ref = next_stencil_ref_++;
            v3d_queue_.push_back(std::move(entry));
        };
        if (need_forced_pickup) {
            // Tick spin (the stock increment in item_render is skipped while
            // culled) then re-query for the post-tick orient.
            bagman_tick_pickup_spin();
            rf::VifLodMesh* lod = nullptr;
            rf::Vector3 p{};
            rf::Matrix3 o{};
            if (bagman_query_pickup_bag_outline(&lod, &p, &o)) {
                push_forced_bag(lod, p, o);
            }
        }
        if (need_forced_carrier) {
            rf::VifLodMesh* lod = nullptr;
            rf::Vector3 p{};
            rf::Matrix3 o{};
            if (bagman_query_carrier_bag_outline(&lod, &p, &o)) {
                push_forced_bag(lod, p, o);
            }
        }
        // Everything cached for this frame has now been turned into a draw, so drop the
        // bag entries too. That makes a second call in the same frame a no-op, which is
        // what lets the screenshot path flush early and flip() flush again harmlessly.
        bagman_pickup_xray_ = ForcedV3dXrayEntry{};
        bagman_carrier_xray_ = ForcedV3dXrayEntry{};

        for (const auto& outline : v3d_queue_) {
            render_v3d_outline(outline, mesh_renderer);
        }
        v3d_queue_.clear();

        // VFX (.vfx) outlines. Normally already drained before the fpgun renders, or
        // failing that as the HUD starts drawing, either of which leaves this a no-op;
        // it stays as the safety net for a frame that reaches neither. The salvage flag
        // has no natural render path that could queue it mid-scene, and the scene-phase
        // hooks that merely *looked* like "the end of the world pass" fire at points
        // where the draw is immediately overwritten (see Renderer::zbuffer_clear).
        // Running here under the restored scene camera above lands them on the same
        // pixels the engine drew the flag on.
        refresh_vfx_transforms();
        for (const auto& outline : vfx_queue_) {
            render_vfx_outline(outline);
        }
        vfx_queue_.clear();

        // Restore the fpgun projection
        render_context_.update_view_proj_transform(current_proj);
        render_context_.invalidate_mode();

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
        // Xray outlines disable depth test so stencil is written even behind geometry.
        bool depth_disabled = outline.info.xray;
        auto stencil_mark = depth_disabled
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
        auto depth_stencil = depth_disabled
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
        bool depth_disabled = outline.info.xray;
        auto stencil_mark = depth_disabled
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
        auto depth_stencil = depth_disabled
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

    void OutlineRenderer::render_vfx_outline(const QueuedVfxOutline& outline)
    {
        auto* ctx = render_context_.device_context();

        rf::VMesh* vmesh = outline.vmesh;
        if (!vmesh || vmesh->type != rf::MESH_TYPE_ANIM_FX) {
            return;
        }
        auto* vfx_inst = static_cast<rf::VfxInstance*>(vmesh->instance);
        if (!vfx_inst) {
            return;
        }
        // Both instance gates the engine's own vfx render applies (FUN_0054d0a0): the
        // readiness bit and the separate bit that suppresses the triangle-mesh chunks,
        // either of which the force-render bit overrides.
        if ((vfx_inst->flags & 8) == 0 && (vfx_inst->flags & (1 | 4)) != 0) {
            return;
        }

        auto* vfx_geo = static_cast<rf::VfxGeo*>(vmesh->mesh);
        if (!vfx_geo || vfx_geo->num_sfxo_chunks < 1 || !vfx_geo->sfxo_chunks) {
            return;
        }

        rf::VfxSfxoRenderObj* sfxo_instances = vfx_inst->sfxo_instances;
        if (!sfxo_instances) {
            return;
        }

        // A chunk contributes to the silhouette only if the engine would draw it as a
        // triangle mesh. FUN_0053ee90 first gates on the render object being active and
        // not hidden (flags bit 31), then switches on render_type and sends chunks with
        // (render_flags & 0x801) down the facing/glow path instead — a camera-aligned
        // sprite has no silhouette worth extruding. Missing the hidden-chunk gate is
        // what let helper geometry the engine never draws inflate the hull.
        auto chunk_is_drawable = [&](int c) {
            const rf::VfxSfxoChunk& chunk = vfx_geo->sfxo_chunks[c];
            if (chunk.render_type != 0 || (chunk.render_flags & 0x801) != 0) {
                return false;
            }
            if (chunk.num_faces < 1 || !chunk.faces) {
                return false;
            }
            const rf::VfxSfxoRenderObj& render_obj = sfxo_instances[c];
            if (render_obj.active == 0 || (render_obj.flags & 0x80000000u) != 0) {
                return false;
            }
            return render_obj.vertex_positions != nullptr;
        };

        // The engine draws every SFXO chunk through gr::start_instance with the
        // transform FUN_0053ee90 cached on the render object, then hands
        // vertex_positions straight to gr::rotate_vertex — so a vertex lands at
        // render_pos + render_orient * v, with no per-chunk offset and no scale.
        // Reusing that cached pair rather than the item transform sampled back in
        // begin_frame() puts the hull on exactly the pixels the engine drew: by the
        // time this runs, item_render has already advanced the pickup spin and
        // salvage_move_carried_flag has already placed the carried flag.
        // A portal-culled item is never dispatched, so the cache can be stale; when it
        // no longer agrees with where the item is now, fall back to the queried pair.
        constexpr float max_cached_transform_drift_sq = 1.0f;
        auto chunk_transform = [&](int c, rf::Vector3* out_pos, rf::Matrix3* out_orient) {
            const rf::VfxSfxoRenderObj& render_obj = sfxo_instances[c];
            const rf::Vector3 drift = render_obj.render_pos - outline.pos;
            if (drift.len_sq() <= max_cached_transform_drift_sq) {
                *out_pos = render_obj.render_pos;
                *out_orient = render_obj.render_orient;
            }
            else {
                *out_pos = outline.pos;
                *out_orient = outline.orient;
            }
        };

        // Count total vertices across all drawable SFXO chunks (un-indexed soup)
        int total_verts = 0;
        for (int c = 0; c < vfx_geo->num_sfxo_chunks; ++c) {
            if (!chunk_is_drawable(c)) continue;
            total_verts += vfx_geo->sfxo_chunks[c].num_faces * 3;
        }
        if (total_verts == 0) {
            return;
        }
        // Sanity cap on a value derived entirely from file data. A corrupt or hostile
        // .vfx can claim an absurd face count; DF_GR_D3D11_CHECK_HR turns the
        // CreateBuffer failure that would follow into a fatal error, so skip the
        // outline instead. Far above any real flag mesh — the stock one is ~200 verts.
        constexpr int max_vfx_outline_verts = 65536;
        if (total_verts > max_vfx_outline_verts) {
            static bool warned = false;
            if (!warned) {
                warned = true;
                xlog::warn("vfx outline: skipping mesh with {} verts (cap {})",
                    total_verts, max_vfx_outline_verts);
            }
            return;
        }

        // Ensure dynamic VB is large enough (grow with 2x factor to avoid frequent reallocation)
        if (!vfx_outline_vb_ || vfx_outline_vb_capacity_ < total_verts) {
            vfx_outline_vb_.release();
            vfx_outline_vb_capacity_ = std::max(total_verts, vfx_outline_vb_capacity_ * 2);
            D3D11_BUFFER_DESC vb_desc{};
            vb_desc.ByteWidth = vfx_outline_vb_capacity_ * sizeof(GpuVertex);
            vb_desc.Usage = D3D11_USAGE_DYNAMIC;
            vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            DF_GR_D3D11_CHECK_HR(device_->CreateBuffer(&vb_desc, nullptr, &vfx_outline_vb_));
        }

        // Fill the VB once; both passes draw it. vertex_positions is decompressed by
        // the engine every frame, so the current animation frame comes for free.
        D3D11_MAPPED_SUBRESOURCE mapped;
        DF_GR_D3D11_CHECK_HR(ctx->Map(vfx_outline_vb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        auto* gpu_verts = static_cast<GpuVertex*>(mapped.pData);

        int vert_idx = 0;
        for (int c = 0; c < vfx_geo->num_sfxo_chunks; ++c) {
            if (!chunk_is_drawable(c)) continue;

            const rf::VfxSfxoChunk& chunk = vfx_geo->sfxo_chunks[c];
            const rf::Vector3* positions = sfxo_instances[c].vertex_positions;
            const rf::VfxSubObject* faces = chunk.faces;
            const int num_verts = chunk.num_vertices;

            // Each chunk carries its own copy of the object transform, so the fill has
            // to bake the vertices into world space here rather than lean on a single
            // model matrix. The draw below binds an identity model transform to match.
            rf::Vector3 chunk_pos{};
            rf::Matrix3 chunk_orient{};
            chunk_transform(c, &chunk_pos, &chunk_orient);

            // Centre of this chunk's own vertices, used below as the origin for the
            // outward term of the extrusion direction. Per chunk rather than per mesh:
            // the pole and the cloth want to push away from their own middles, and a
            // shared centre would tilt the pole's directions toward the cloth. The
            // bounding-box centre is used instead of the mean so a chunk with lopsided
            // vertex density does not drag the origin off to one side.
            rf::Vector3 chunk_centre{};
            if (positions && num_verts > 0) {
                rf::Vector3 lo = positions[0];
                rf::Vector3 hi = positions[0];
                for (int v = 1; v < num_verts; ++v) {
                    const rf::Vector3& p = positions[v];
                    lo.x = std::min(lo.x, p.x);
                    lo.y = std::min(lo.y, p.y);
                    lo.z = std::min(lo.z, p.z);
                    hi.x = std::max(hi.x, p.x);
                    hi.y = std::max(hi.y, p.y);
                    hi.z = std::max(hi.z, p.z);
                }
                chunk_centre = chunk_orient.transform_vector((lo + hi) * 0.5f) + chunk_pos;
            }
            else {
                chunk_centre = chunk_pos;
            }

            for (int i = 0; i < chunk.num_faces; ++i) {
                rf::Vector3 corners[3]{};
                for (int j = 0; j < 3; ++j) {
                    rf::Vector3 local{};
                    int vi = faces[i].vertex_indices[j];
                    if (vi >= 0 && vi < num_verts) {
                        local = positions[vi];
                    }
                    corners[j] = chunk_orient.transform_vector(local);
                    corners[j] += chunk_pos;
                }

                // Flat per-face normal. The outline VS extrudes along NORMAL, and a
                // zero normal means no extrusion at all, so the hull collapses onto the
                // stencil mark and is fully rejected. Smooth (averaged) normals are
                // unusable on this mesh: the flag cloth is a doubled two-sided sheet
                // whose opposing layers cancel to roughly zero. A per-face normal cannot
                // cancel, and the shader's 2D-normalized 1px extrusion keeps the
                // resulting hard-edge cracks around a pixel.
                rf::Vector3 norm = (corners[1] - corners[0]).cross(corners[2] - corners[0]);
                const float len_sq = norm.len_sq();
                if (len_sq > 1e-16f) {
                    norm /= std::sqrt(len_sq);
                }
                else {
                    // Degenerate face: leave the normal zeroed rather than picking an
                    // arbitrary direction. It has no area to outline anyway.
                    norm = rf::Vector3{0.0f, 0.0f, 0.0f};
                }

                for (int j = 0; j < 3; ++j) {
                    // Blend an outward-from-the-chunk-centre term into the direction the
                    // shader extrudes along. A pure face normal fails on the cloth: it is
                    // a flat doubled sheet, so every face normal is perpendicular to the
                    // sheet plane, and viewed face-on those all project to a near-zero
                    // screen direction. The shader's `len > 0.0001f` guard then skips the
                    // extrusion entirely and the cloth outline disappears, reappearing
                    // only as the sheet turns edge-on. The corner-outward term is always
                    // in the sheet plane, so it survives projection from any angle, and
                    // on border faces it points out of the silhouette, which is exactly
                    // where the outline is wanted. Interior faces get shells nudged
                    // sideways inside the pass-1 stencil mask, so they cost nothing.
                    // Weight 0.6 keeps the blended direction within ~31 degrees of the
                    // true face normal, so an ordinary silhouette face still extrudes
                    // ~0.86 of the requested pixel outward; that also bounds the error on
                    // the pole, where the outward term near the tips is dominated by the
                    // along-axis component. The shader renormalises in 2D, so only the
                    // direction of this vector matters, not its length.
                    constexpr float outward_blend = 0.6f;
                    rf::Vector3 dir = norm;
                    rf::Vector3 outward = corners[j] - chunk_centre;
                    const float outward_len_sq = outward.len_sq();
                    if (outward_len_sq > 1e-12f) {
                        dir = norm + outward * (outward_blend / std::sqrt(outward_len_sq));
                        const float dir_len_sq = dir.len_sq();
                        if (dir_len_sq > 1e-12f) {
                            dir /= std::sqrt(dir_len_sq);
                        }
                        else {
                            dir = norm;
                        }
                    }

                    GpuVertex& gv = gpu_verts[vert_idx++];
                    gv = {};
                    gv.x = corners[j].x;
                    gv.y = corners[j].y;
                    gv.z = corners[j].z;
                    gv.norm = float3{dir.x, dir.y, dir.z};
                }
            }
        }
        ctx->Unmap(vfx_outline_vb_, 0);

        render_context_.set_primitive_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        // b0 world_mat, the same cbuffer prepare_v3d_for_draw updates for the v3d path.
        // The fill already applied each chunk's own transform, so this is identity.
        static const rf::Matrix3 identity_orient{
            rf::Vector3{1.0f, 0.0f, 0.0f},
            rf::Vector3{0.0f, 1.0f, 0.0f},
            rf::Vector3{0.0f, 0.0f, 1.0f},
        };
        render_context_.set_model_transform(rf::Vector3{0.0f, 0.0f, 0.0f}, identity_orient);

        // RenderContext has no non-indexed draw wrapper, so the VB is bound and drawn
        // on the raw context. Both passes reuse this binding; the cache is re-synced
        // after the last draw.
        UINT stride = sizeof(GpuVertex);
        UINT vb_offset = 0;
        ID3D11Buffer* vb = vfx_outline_vb_;
        ctx->IASetVertexBuffers(0, 1, &vb, &stride, &vb_offset);

        // ---- Pass 1: Stencil mark ----
        bool depth_disabled = outline.info.xray;
        auto stencil_mark = depth_disabled
            ? state_manager_.get_outline_stencil_mark_xray_state()
            : state_manager_.get_outline_stencil_mark_state();
        render_context_.set_depth_stencil_state(stencil_mark, outline.info.stencil_ref);
        render_context_.set_blend_state(state_manager_.get_no_color_write_blend_state());
        render_context_.set_cull_mode(D3D11_CULL_BACK);

        auto& std_vs = shader_manager_.get_vertex_shader(VertexShaderId::standard);
        render_context_.set_vertex_shader(std_vs);
        render_context_.set_pixel_shader(shader_manager_.get_pixel_shader(PixelShaderId::outline));

        ctx->Draw(total_verts, 0);

        // ---- Pass 2: Outline ----
        auto depth_stencil = depth_disabled
            ? state_manager_.get_outline_xray_state()
            : state_manager_.get_outline_depth_test_state();
        render_context_.set_depth_stencil_state(depth_stencil, outline.info.stencil_ref);

        render_context_.set_blend_state(
            state_manager_.lookup_blend_state(rf::gr::ALPHA_BLEND_ALPHA));

        render_context_.set_cull_mode(D3D11_CULL_FRONT);

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

        ctx->Draw(total_verts, 0);

        // Reset cull mode back to normal
        render_context_.set_cull_mode(D3D11_CULL_BACK);

        // Re-sync the RenderContext vertex buffer cache with what is actually bound,
        // so the next cached set_vertex_buffer() rebinds instead of no-opping.
        render_context_.set_vertex_buffer(vfx_outline_vb_, sizeof(GpuVertex), 0);
    }
}
