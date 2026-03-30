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
}

namespace df::gr::d3d11
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

    // Pre-stored data for xray players so we can queue outlines even if
    // the portal renderer culled them (different room/portal).
    struct ForcedXrayEntry
    {
        rf::VifLodMesh* lod_mesh;
        rf::Vector3 pos;
        rf::Matrix3 orient;
        const rf::CharacterInstance* ci;
        OutlineInfo info;
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
        void flush_forced_xray(MeshRenderer& mesh_renderer);

    private:
        void queue_unrendered_xray_outlines();
        void render_outline(const QueuedOutline& outline, MeshRenderer& mesh_renderer);
        void render_v3d_outline(const QueuedV3dOutline& outline, MeshRenderer& mesh_renderer);

        ID3D11Device* device_;
        ShaderManager& shader_manager_;
        StateManager& state_manager_;
        RenderContext& render_context_;

        // Outline constant buffers
        ComPtr<ID3D11Buffer> outline_vs_params_buffer_;
        ComPtr<ID3D11Buffer> outline_ps_color_buffer_;

        // Per-frame data
        std::unordered_map<const rf::CharacterInstance*, OutlineInfo> ci_map_;
        std::vector<QueuedOutline> queue_;
        std::vector<QueuedV3dOutline> v3d_queue_;
        std::vector<ForcedXrayEntry> xray_forced_;
        std::unordered_set<const rf::CharacterInstance*> flushed_cis_; // CIs already rendered by flush()
        Projection saved_projection_; // main scene projection saved at begin_frame
        UINT next_stencil_ref_ = 1;
        int last_begin_frame_ = -1; // frame_count of last begin_frame to run once per frame
        const OutlineInfo* current_character_outline_ = nullptr; // outline info of last rendered character
    };
}
