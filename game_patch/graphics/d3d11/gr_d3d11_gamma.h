#pragma once

#include <d3d11.h>
#include <common/ComPtr.h>

namespace gr::d3d11
{
    class ShaderManager;

    class GammaPass
    {
    public:
        GammaPass(ComPtr<ID3D11Device> device, ShaderManager& shader_manager);
        void render(ID3D11DeviceContext* context, ID3D11ShaderResourceView* scene_srv,
                    ID3D11RenderTargetView* back_buffer_rtv, float gamma);

    private:
        ComPtr<ID3D11Device> device_;
        ComPtr<ID3D11VertexShader> vertex_shader_;
        ComPtr<ID3D11PixelShader> pixel_shader_;
        ComPtr<ID3D11Buffer> gamma_cbuffer_;
        ComPtr<ID3D11SamplerState> point_sampler_;
    };
}
