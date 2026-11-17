#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <d3d11.h>
#include <common/ComPtr.h>
#include <gr_d3d11_engine.h>

namespace gr::d3d11
{
    // Which part of a liquid room's box really is that room. Only rooms whose box overlaps another room's
    // under the surface (not detail, not the same body of liquid) get one; elsewhere the box is exact, since
    // space in it is either the room or solid.
    struct alignas(16) LiquidMaskGPUData
    {
        std::array<float, 3> origin;       float inv_cell;
        std::array<float, 3> dims;         float enabled;
        std::array<float, 3> atlas_offset; float _pad0;
        std::array<float, 3> atlas_scale;  float _pad1;
    };
    static_assert(sizeof(LiquidMaskGPUData) == 64);
    static_assert(offsetof(LiquidMaskGPUData, dims) == 16);
    static_assert(offsetof(LiquidMaskGPUData, atlas_offset) == 32);
    static_assert(offsetof(LiquidMaskGPUData, atlas_scale) == 48);

    class LiquidVolumeMask
    {
    public:
        explicit LiquidVolumeMask(ID3D11Device* device);

        // Builds for a new level's geometry; cheap otherwise
        void update(ID3D11DeviceContext* device_context, bool force_rebuild = false);

        // Disabled (all zero) when the room's box needs no mask
        LiquidMaskGPUData region(const rf::GRoom* room) const;

    private:
        struct Region
        {
            const rf::GRoom* room;
            int uid;
            LiquidMaskGPUData data;
        };

        void build();
        void bind(ID3D11DeviceContext* device_context);

        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11Texture3D> texture_;
        ComPtr<ID3D11ShaderResourceView> srv_;
        ComPtr<ID3D11SamplerState> sampler_;
        std::vector<Region> regions_;
        const rf::GSolid* built_solid_ = nullptr;
    };
}
