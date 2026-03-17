#pragma once

#include <d3d11.h>
#include <common/ComPtr.h>
#include "gr_d3d11_transform.h"

namespace df::gr::d3d11
{
    class ShaderManager;
    class MeshRenderer;
    class RenderContext;
    // Shadow distance presets: camera distance at which shadows fade
    struct ShadowDistancePreset
    {
        float fade_start;
        float fade_end;
    };

    // Shadow quality presets: resolution=0 means blob shadows (no shadow map)
    struct ShadowQualityPreset
    {
        int resolution;        // shadow map resolution (0 = blob shadows only)
        int pcf_taps;          // PCF sample count (5, 9, 12, or 16)
        bool soft_edges;       // disable PCF early-out for softer shadow edges
    };

    class EntityShadowRenderer
    {
    public:
        // Shadow distance presets (camera distance fade, in game units)
        // Medium matches stock RF blob shadow distances (7.5 start, 13.0 end)
        static constexpr ShadowDistancePreset shadow_distance_presets[] = {
            {5.0f,    8.0f},    // 0: lowest
            {6.0f,   10.0f},    // 1: low
            {7.5f,   13.0f},    // 2: medium (stock blob shadow distances)
            {15.0f,  25.0f},    // 3: high
            {30.0f,  50.0f},    // 4: very_high
            {60.0f, 100.0f},    // 5: maximum
        };

        // Shadow quality presets (resolution + PCF taps)
        // Quality 0 = stock blob shadows (no d3d11 shadow map)
        static constexpr ShadowQualityPreset shadow_quality_presets[] = {
            {0,    0,  false},   // 0: lowest (stock blob shadows)
            {1024, 5,  false},   // 1: low
            {2048, 9,  false},   // 2: medium
            {4096, 12, false},   // 3: high
            {8192, 16, false},   // 4: very_high
            {8192, 16, true},    // 5: maximum (soft edges - no PCF early-out)
        };

        static constexpr int num_shadow_distance_presets = sizeof(shadow_distance_presets) / sizeof(shadow_distance_presets[0]);
        static constexpr int num_shadow_quality_presets = sizeof(shadow_quality_presets) / sizeof(shadow_quality_presets[0]);

        static constexpr const char* preset_names[] = {"Lowest", "Low", "Medium", "High", "Very High", "Maximum"};

        // Shadow darkness (0.0 = fully dark shadow, 1.0 = no shadow)
        static constexpr float shadow_strength = 0.5f;

        // Shadow projection distance fade (in game units)
        // Shadows fade as the receiving surface gets further from the caster
        static constexpr float shadow_projection_fade_start = 1.0f;
        static constexpr float shadow_projection_fade_end = 3.0f;

        // Static light direction (nearly overhead, slight offset)
        static constexpr float light_dir_x = 0.15f;
        static constexpr float light_dir_y = -1.0f;
        static constexpr float light_dir_z = 0.1f;

        EntityShadowRenderer(ID3D11Device* device, ShaderManager& shader_manager, MeshRenderer& mesh_renderer);
        ~EntityShadowRenderer();

        void create_resources(int resolution);
        void generate_shadow_map(ID3D11DeviceContext* context, RenderContext& render_context, const rf::Vector3& camera_pos);
        void bind_shadow_resources(ID3D11DeviceContext* context);
        void unbind_shadow_resources(ID3D11DeviceContext* context);

        void apply_quality(int quality);
        void render_debug_overlay(ID3D11DeviceContext* context);

        static bool debug_enabled;

    private:
        struct ShadowConstantBuffer
        {
            GpuMatrix4x4 shadow_vp_mat;
            float shadow_strength;
            float shadow_fade_start;
            float shadow_fade_end;
            float shadow_enabled;
            float shadow_light_dir[3];
            float shadow_normal_offset;
            float shadow_texel_size;
            float shadow_depth_range;
            float shadow_projection_fade_start;
            float shadow_projection_fade_end;
            float shadow_pcf_taps;
            float shadow_debug;
            float shadow_soft_edges;
            float pad;
        };
        static_assert(sizeof(ShadowConstantBuffer) % 16 == 0);

        void build_shadow_view_proj(ID3D11DeviceContext* context, const rf::Vector3& camera_pos);
        void render_entity_shadow(ID3D11DeviceContext* context, RenderContext& render_context);
        void render_corpse_shadow(ID3D11DeviceContext* context, RenderContext& render_context);
        void render_item_shadow(ID3D11DeviceContext* context, RenderContext& render_context);
        ID3D11Device* device_;
        ShaderManager& shader_manager_;
        MeshRenderer& mesh_renderer_;

        ComPtr<ID3D11Texture2D> shadow_map_texture_;
        ComPtr<ID3D11DepthStencilView> shadow_dsv_;
        ComPtr<ID3D11ShaderResourceView> shadow_srv_;

        ComPtr<ID3D11SamplerState> shadow_comparison_sampler_;
        ComPtr<ID3D11SamplerState> shadow_depth_sampler_;
        ComPtr<ID3D11RasterizerState> shadow_rasterizer_state_;
        ComPtr<ID3D11DepthStencilState> shadow_depth_stencil_state_;
        ComPtr<ID3D11Buffer> shadow_cbuffer_;
        ComPtr<ID3D11Buffer> shadow_vp_cbuffer_;

        int current_quality_ = 2;
        int current_resolution_ = 1024;
        int last_frame_ = -1;

        // Dynamic vertex buffer for VFX (MESH_TYPE_ANIM_FX) shadow geometry
        ComPtr<ID3D11Buffer> vfx_shadow_vb_;
        int vfx_shadow_vb_capacity_ = 0;

        // Cached shadow VP matrix for the current frame
        GpuMatrix4x4 shadow_vp_matrix_;
        rf::Vector3 current_camera_pos_;
        float current_depth_range_ = 200.0f;

        // Debug overlay resources (lazy-initialized)
        ComPtr<ID3D11Buffer> debug_vb_;
        ComPtr<ID3D11BlendState> debug_blend_state_;
        ComPtr<ID3D11DepthStencilState> debug_no_depth_state_;
    };
}
