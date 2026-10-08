#pragma once

#include <array>
#include <cstddef>
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
        std::array<float, 4> scope_glass;  // distortion, rim distortion, dispersion, vignette strength
        std::array<float, 4> scope_rim;    // rim start, rim end, vignette start, fringe
    };
    static_assert(offsetof(SceneFxBufferData, scope_glass) == 176);
    static_assert(offsetof(SceneFxBufferData, scope_rim) == 192);
    static_assert(sizeof(SceneFxBufferData) == 208);
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
    constexpr unsigned scenefx_flag_scope_glass = 16;

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
    // Scope eyepiece, with radius in half viewport heights; the stock scope rings' clear apertures
    // end at 0.83-0.88. The rim term models the glass curving towards its edge; it ends past the
    // apertures so the image keeps bending harder all the way to the ring. Red and blue are offset
    // from green by a constant fringe plus the bend times the dispersion. r * (1 - bend +/- fringe)
    // must keep increasing across the ring's square (r < 1.42), or the image folds. rim_start must
    // stay below rim_end even with no rim distortion: equal edges make the shader's smoothstep NaN.
    struct ScopeGlassTier
    {
        float distortion;
        float rim_distortion;
        float rim_start;
        float rim_end;
        float dispersion;
        float fringe;
        float vignette;
        float vignette_start;
    };
    constexpr ScopeGlassTier scenefx_scope_glass_light{0.04f, 0.0f, 0.45f, 1.05f, 0.0f, 0.004f, 0.3f, 0.5f};
    constexpr ScopeGlassTier scenefx_scope_glass_heavy{0.02f, 0.15f, 0.45f, 1.05f, 0.1f, 0.0f, 0.3f, 0.5f};

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
        ComPtr<ID3D11SamplerState> linear_sampler_;
        ComPtr<ID3D11BlendState> overlay_blend_state_;
        ComPtr<ID3D11BlendState> distort_blend_state_;
        ComPtr<ID3D11RasterizerState> rasterizer_state_;
        ComPtr<ID3D11DepthStencilState> depth_off_state_;
    };
}
