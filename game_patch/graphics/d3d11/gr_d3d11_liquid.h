#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <d3d11.h>
#include <common/ComPtr.h>
#include "gr_d3d11_transform.h"
#include "../../rf/gr/gr.h"
#include "../../rf/math/matrix.h"
#include "../../rf/math/vector.h"

namespace gr::d3d11
{
    constexpr int max_liquid_volumes = 8;

    // Never below stock's value, and only up to gr::default_wfar, which the engine reloads each
    // frame. Zero/negative/NaN pass through to the engine's own handling.
    constexpr float liquid_far_clip_scale = 4.0f;
    inline float liquid_far_clip(float liquid_visibility)
    {
        if (!(liquid_visibility > 0.0f)) {
            return liquid_visibility;
        }
        return std::max(liquid_visibility, std::min(liquid_visibility * liquid_far_clip_scale, rf::gr::default_wfar));
    }

    // Box arrives pre-expanded and already capped at the surface plane
    struct alignas(16) LiquidVolumeGPUData
    {
        std::array<float, 3> bbox_min; float _pad0;
        std::array<float, 3> bbox_max; float _pad1;
    };
    static_assert(sizeof(LiquidVolumeGPUData) == 32);

    struct alignas(16) LiquidBufferData
    {
        std::array<float, 3> eye_pos;        float mode;
        std::array<float, 3> cam_right;      float proj_sx;
        std::array<float, 3> cam_up;         float proj_sy;
        std::array<float, 3> cam_forward;    float viewport_w;
        float viewport_h; float surface_y; float visibility; float eye_under;
        std::array<float, 3> color;          float viewport_x;
        std::array<float, 3> over_fog_color; float over_fog_far;
        std::array<float, 4> params;
        float far_clip; float num_volumes; float dark_surface_y; float viewport_y;
        LiquidVolumeGPUData volumes[max_liquid_volumes];
    };
    static_assert(sizeof(LiquidBufferData) == 400);
    static_assert(offsetof(LiquidBufferData, volumes) == 144);
    static_assert(sizeof(LiquidBufferData) % 16 == 0);

    struct LiquidState
    {
        int mode = 0;               // 0 = camera room has no liquid, else GRoom::liquid_type (1 water, 2 lava, 3 acid)
        float surface_y = 0.0f;
        rf::Vector3 color{1.0f, 1.0f, 1.0f};
        float alpha = 0.0f;
        float visibility = 1.0f;
        bool eye_under = false;
        rf::Vector3 over_fog_color{0.0f, 0.0f, 0.0f};
        float over_fog_far = 0.0f;  // <= 0 means the level applies no distance fog

        // Consumers want these, not the raw targets above
        rf::Vector3 blended_color{1.0f, 1.0f, 1.0f};
        float blended_alpha = 0.0f;
        float blended_visibility = 1.0f;
        float blended_surface_y = 0.0f;   // depth darkening only
        rf::Vector3 blended_over_fog_color{0.0f, 0.0f, 0.0f};
        float blended_over_fog_far = 0.0f;
    };

    class LiquidFxRenderer
    {
    public:
        explicit LiquidFxRenderer(ID3D11Device* device);

        // Must be given the main scene's camera, not the globals at flip time. Returns the
        // projection to apply: submerged, the far plane is widened to the engine's cull distance.
        Projection update(ID3D11DeviceContext* device_context, const Projection& projection,
                          const rf::Vector3& eye_pos, const rf::Matrix3& eye_orient);

        // b6 must not stay live while the engine renders from another camera into a texture
        void write_disabled(ID3D11DeviceContext* device_context);
        void rewrite(ID3D11DeviceContext* device_context);

        // Colour the underwater fog converges to at long range, for a pixel at eye height
        bool background_color(rf::Vector3& out) const;

        const LiquidState& state() const
        {
            return state_;
        }

        operator ID3D11Buffer*() const
        {
            return buffer_;
        }

    private:
        void upload(ID3D11DeviceContext* device_context, const LiquidBufferData& data);
        void snap_blend();

        ComPtr<ID3D11Buffer> buffer_;
        LiquidState state_;
        LiquidBufferData data_{};
        rf::Vector3 eye_pos_{};   // scene camera the current state was built from
        bool disabled_uploaded_ = true;   // buffer was created zeroed
        int64_t last_update_ms_ = -1;
    };
}
