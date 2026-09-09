#pragma once

#include <array>
#include <cstddef>
#include <d3d11.h>
#include <common/ComPtr.h>
#include "gr_d3d11_transform.h"
#include "../../rf/math/matrix.h"
#include "../../rf/math/vector.h"

namespace gr::d3d11
{
    constexpr int max_liquid_volumes = 8;

    // One liquid room. The box is pre-expanded by the epsilon and its top already capped at the
    // surface plane, so the shader can slab-test it as-is.
    struct alignas(16) LiquidVolumeGPUData
    {
        std::array<float, 3> bbox_min; float surface_y;
        std::array<float, 3> bbox_max; float _pad;
    };
    static_assert(sizeof(LiquidVolumeGPUData) == 32);

    struct alignas(16) LiquidBufferData
    {
        std::array<float, 3> eye_pos;        float mode;
        std::array<float, 3> cam_right;      float proj_sx;
        std::array<float, 3> cam_up;         float proj_sy;
        std::array<float, 3> cam_forward;    float viewport_w;
        float viewport_h; float surface_y; float visibility; float eye_under;
        std::array<float, 3> color;          float alpha;
        std::array<float, 3> over_fog_color; float over_fog_far;
        std::array<float, 4> params;
        float far_clip; float num_volumes; float dark_surface_y; float _pad0;
        LiquidVolumeGPUData volumes[max_liquid_volumes];
    };
    static_assert(sizeof(LiquidBufferData) == 400);
    static_assert(offsetof(LiquidBufferData, volumes) == 144);
    static_assert(sizeof(LiquidBufferData) % 16 == 0);

    struct LiquidState
    {
        int mode = 0;               // 0 = camera room has no liquid, else GRoom::liquid_type (1 water, 2 lava, 3 acid)
        int room_uid = -1;          // camera room, -1 when it holds no liquid
        float surface_y = 0.0f;
        rf::Vector3 bbox_min{};
        rf::Vector3 bbox_max{};
        rf::Vector3 color{1.0f, 1.0f, 1.0f};
        float alpha = 0.0f;
        float visibility = 1.0f;
        bool eye_under = false;
        rf::Vector3 over_fog_color{0.0f, 0.0f, 0.0f};
        float over_fog_far = 0.0f;  // <= 0 means the level applies no distance fog

        // Time-blended copies of everything that can change when the camera crosses into a
        // differently coloured liquid room. The geometric values above snap: adjacent liquid
        // rooms nearly always share a surface height. Consumers want these, not the raw targets.
        rf::Vector3 blended_color{1.0f, 1.0f, 1.0f};
        float blended_alpha = 0.0f;
        float blended_visibility = 1.0f;
        // Only the depth-darkening term uses this. In a flooded cave each room's "surface" is its
        // own ceiling, so the snapped value steps at every boundary.
        float blended_surface_y = 0.0f;
        rf::Vector3 blended_over_fog_color{0.0f, 0.0f, 0.0f};
        float blended_over_fog_far = 0.0f;
    };

    class LiquidFxRenderer
    {
    public:
        explicit LiquidFxRenderer(ID3D11Device* device);

        // Driven from Renderer::setup_3d with the main scene's camera, which is the camera the
        // fog and the post pass are evaluated against. The engine's closing setup_3d rebinds the
        // unoffset camera, so reading the globals at flip time would be off by 0.14 forward.
        void update(ID3D11DeviceContext* device_context, const Projection& projection,
                    const rf::Vector3& eye_pos, const rf::Matrix3& eye_orient);

        // b6 carries the player's camera, so it must not stay live while the engine renders the
        // world from another camera into a texture (monitors, rail/IR scanner).
        void write_disabled(ID3D11DeviceContext* device_context);
        void rewrite(ID3D11DeviceContext* device_context);

        // Colour the underwater fog converges to at long range, for a pixel at eye height.
        // False when the camera room holds no liquid.
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
        int64_t last_update_ms_ = -1;
    };
}
