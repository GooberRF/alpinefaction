#pragma once

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <d3d11.h>
#include <common/ComPtr.h>
#include "gr_d3d11_transform.h"
#include "../../rf/math/vector.h"
#include "../../rf/math/matrix.h"

namespace rf
{
    struct VifLodMesh;
    struct CharacterInstance;
    struct VMesh;
}

namespace gr::d3d11
{
    class ShaderManager;
    class StateManager;
    class RenderContext;
    class MeshRenderer;

    struct OutlineInfo
    {
        float r, g, b, a;   // normalized outline color
        bool xray;           // render through geometry (also bypasses portal culling)
        UINT stencil_ref;    // unique per character (1-255)
    };

    struct QueuedOutline
    {
        rf::VifLodMesh* lod_mesh;
        int lod_index;
        rf::Vector3 pos;
        rf::Matrix3 orient;
        const rf::CharacterInstance* ci;
        OutlineInfo info;
    };

    struct QueuedV3dOutline
    {
        rf::VifLodMesh* lod_mesh;
        int lod_index;
        rf::Vector3 pos;
        rf::Matrix3 orient;
        OutlineInfo info;
    };

    // A .vfx (MESH_TYPE_ANIM_FX) outline. VFX geometry has no VifLodMesh and never
    // reaches render_v3d_vif, so it cannot go through MeshRenderer at all: the mesh
    // is walked into a private dynamic vertex buffer and drawn as a triangle soup.
    struct QueuedVfxOutline
    {
        rf::VMesh* vmesh;
        rf::Vector3 pos;
        rf::Matrix3 orient;
        OutlineInfo info;
    };

    // Pre-stored data for xray players so we can queue outlines even if
    // the portal renderer culled them (different room/portal).
    struct ForcedXrayEntry
    {
        rf::VifLodMesh* lod_mesh;
        rf::Vector3 pos;
        rf::Matrix3 orient;
        rf::CharacterInstance* ci;
        OutlineInfo info;
    };

    // v3d analog of ForcedXrayEntry for a static mesh
    struct ForcedV3dXrayEntry
    {
        rf::VifLodMesh* lod_mesh = nullptr;
        rf::Vector3 pos{};
        rf::Matrix3 orient{};
        bool naturally_rendered = false;
    };

    class OutlineRenderer
    {
    public:
        OutlineRenderer(ID3D11Device* device, ShaderManager& shader_manager, StateManager& state_manager, RenderContext& render_context);

        void begin_frame();
        const OutlineInfo* lookup(const rf::CharacterInstance* ci) const;
        void queue(QueuedOutline entry);
        void queue_v3d(QueuedV3dOutline entry);
        const OutlineInfo* current_character_outline() const;
        void set_current_character_outline(const OutlineInfo* info);
        void flush(MeshRenderer& mesh_renderer);
        // Drains vfx_queue_ only. Called where the character/v3d queues drain, as 2D
        // rendering begins, so the .vfx outline lands under the HUD instead of over it.
        void flush_vfx();
        void flush_forced_xray(MeshRenderer& mesh_renderer);
        void maybe_queue_bag_outline(
            rf::VifLodMesh* lod_mesh, int lod_index,
            const rf::Vector3& pos, const rf::Matrix3& orient);

    private:
        void queue_unrendered_xray_outlines();
        void refresh_vfx_transforms();
        void render_outline(const QueuedOutline& outline, MeshRenderer& mesh_renderer);
        void render_v3d_outline(const QueuedV3dOutline& outline, MeshRenderer& mesh_renderer);
        void render_vfx_outline(const QueuedVfxOutline& outline);

        ID3D11Device* device_;
        ShaderManager& shader_manager_;
        StateManager& state_manager_;
        RenderContext& render_context_;

        // Outline constant buffers
        ComPtr<ID3D11Buffer> outline_vs_params_buffer_;
        ComPtr<ID3D11Buffer> outline_ps_color_buffer_;

        // Dynamic vertex buffer for VFX outline geometry (un-indexed triangle soup,
        // rebuilt every frame). Owned here on purpose: the shadow renderer has its own
        // and runs earlier in the frame.
        ComPtr<ID3D11Buffer> vfx_outline_vb_;
        int vfx_outline_vb_capacity_ = 0;

        // Per-frame data
        std::unordered_map<const rf::CharacterInstance*, OutlineInfo> ci_map_;
        std::vector<QueuedOutline> queue_;
        std::vector<QueuedV3dOutline> v3d_queue_;
        std::vector<QueuedVfxOutline> vfx_queue_;
        std::vector<ForcedXrayEntry> xray_forced_;
        std::unordered_set<const rf::CharacterInstance*> flushed_cis_; // CIs already rendered by flush()
        Projection saved_projection_; // main scene projection saved at begin_frame
        // The camera the main scene was rendered from, saved alongside the
        // projection. gr_setup_3d writes rf::gr::eye_pos / eye_matrix on every call
        // and runs several more times before the frame is presented, so anything
        // drawn from flip() has to bind these explicitly.
        rf::Vector3 saved_eye_pos_{};
        rf::Matrix3 saved_eye_orient_{};
        UINT next_stencil_ref_ = 1;
        int last_begin_frame_ = -1; // frame_count of last begin_frame to run once per frame
        const OutlineInfo* current_character_outline_ = nullptr; // outline info of last rendered character
        ForcedV3dXrayEntry bagman_pickup_xray_{};
        ForcedV3dXrayEntry bagman_carrier_xray_{};
    };
}
