#pragma once

#include <array>
#include <d3d11.h>
#include <common/ComPtr.h>

namespace gr::d3d11
{
    class ShaderManager;

    struct alignas(16) SceneFxBufferData
    {
        std::array<float, 2> rt_size;      float time;         float flags;
        std::array<float, 4> tint;
        std::array<float, 4> vignette;
        std::array<float, 4> damage_edges;
        std::array<float, 4> damage;
        float distort_amp; float distort_freq; float distort_speed; float _pad0;
        std::array<float, 3> eye_pos;      float surface_y;
        std::array<float, 3> cam_right;    float proj_sx;
        std::array<float, 3> cam_up;       float proj_sy;
        std::array<float, 3> cam_fwd;      float near_dist;
        std::array<float, 4> viewport_rect;
    };
    static_assert(sizeof(SceneFxBufferData) == 176);
    static_assert(sizeof(SceneFxBufferData) % 16 == 0);

    // Screen-edge damage feedback, decayed per frame by the renderer.
    struct DamageVignetteState
    {
        std::array<float, 4> edges{};   // top, left, bottom, right
        float radial = 0.0f;
        int radial_frame = -1; // frame the radial hit was armed, so a directional mask can replace it

        bool active() const
        {
            return radial > 0.0f || edges[0] > 0.0f || edges[1] > 0.0f || edges[2] > 0.0f || edges[3] > 0.0f;
        }
    };

    constexpr unsigned scenefx_flag_distort = 1;
    constexpr unsigned scenefx_flag_liquid_tint = 2;
    constexpr unsigned scenefx_flag_liquid_vignette = 4;
    constexpr unsigned scenefx_flag_damage = 8;

    constexpr float scenefx_distort_amp = 0.002f;
    constexpr float scenefx_distort_freq = 14.0f;
    constexpr float scenefx_distort_speed = 1.6f;
    constexpr float scenefx_vignette_darken = 0.45f;
    constexpr float scenefx_vignette_strength = 0.7f;
    // Stock screen-flash decay rate, rescaled from 0-255 to 0-1
    constexpr float scenefx_damage_decay_per_sec = 170.0f / 255.0f;
    // Must match the shader's waterline_band and gr_d3d_setup_3d_injection's near plane
    constexpr float scenefx_waterline_band = 0.02f;
    constexpr float scenefx_near_dist = 0.1f;

    class ScenePostPass
    {
    public:
        ScenePostPass(ComPtr<ID3D11Device> device, ShaderManager& shader_manager);

        // scene_srv is null in overlay mode: the pass then alpha-blends its layers onto
        // whatever is already in target_rtv instead of sampling and replacing it.
        void render(ID3D11DeviceContext* context, ID3D11ShaderResourceView* scene_srv,
                    ID3D11RenderTargetView* target_rtv, const SceneFxBufferData& data);

    private:
        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11VertexShader> vertex_shader_;
        ComPtr<ID3D11PixelShader> pixel_shader_;
        ComPtr<ID3D11Buffer> cbuffer_;
        ComPtr<ID3D11SamplerState> point_sampler_;
        ComPtr<ID3D11BlendState> overlay_blend_state_;
        ComPtr<ID3D11BlendState> distort_blend_state_;
        ComPtr<ID3D11RasterizerState> rasterizer_state_;
        ComPtr<ID3D11DepthStencilState> depth_off_state_;
    };
}
