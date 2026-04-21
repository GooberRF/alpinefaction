#include <algorithm>
#include "gr_d3d11_gamma.h"
#include "gr_d3d11.h"
#include "gr_d3d11_shader.h"

namespace gr::d3d11
{
    struct alignas(16) GammaBufferData
    {
        float gamma;
    };
    static_assert(sizeof(GammaBufferData) % 16 == 0);

    GammaPass::GammaPass(ComPtr<ID3D11Device> device, ShaderManager& shader_manager)
        : device_{std::move(device)}
    {
        vertex_shader_ = shader_manager.load_vertex_shader_only(
            get_vertex_shader_filename(VertexShaderId::gamma));

        pixel_shader_ = shader_manager.get_pixel_shader(PixelShaderId::gamma);

        CD3D11_BUFFER_DESC cb_desc{
            sizeof(GammaBufferData),
            D3D11_BIND_CONSTANT_BUFFER,
            D3D11_USAGE_DYNAMIC,
            D3D11_CPU_ACCESS_WRITE,
        };
        DF_GR_D3D11_CHECK_HR(device_->CreateBuffer(&cb_desc, nullptr, &gamma_cbuffer_));

        CD3D11_SAMPLER_DESC sampler_desc{D3D11_DEFAULT};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        DF_GR_D3D11_CHECK_HR(device_->CreateSamplerState(&sampler_desc, &point_sampler_));
    }

    void GammaPass::render(ID3D11DeviceContext* context, ID3D11ShaderResourceView* scene_srv,
                           ID3D11RenderTargetView* back_buffer_rtv, float gamma)
    {
        // Update gamma constant buffer
        D3D11_MAPPED_SUBRESOURCE mapped;
        DF_GR_D3D11_CHECK_HR(
            context->Map(gamma_cbuffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)
        );
        auto* data = static_cast<GammaBufferData*>(mapped.pData);
        data->gamma = std::max(gamma, 0.01f);
        context->Unmap(gamma_cbuffer_, 0);

        // Set render target to back buffer (no depth testing)
        context->OMSetRenderTargets(1, &back_buffer_rtv, nullptr);

        // Bind shaders
        context->VSSetShader(vertex_shader_, nullptr, 0);
        context->PSSetShader(pixel_shader_, nullptr, 0);

        // Bind scene texture and sampler
        context->PSSetShaderResources(0, 1, &scene_srv);
        ID3D11SamplerState* samplers[] = {point_sampler_};
        context->PSSetSamplers(0, 1, samplers);

        // Bind gamma constant buffer
        ID3D11Buffer* cbuffers[] = {gamma_cbuffer_};
        context->PSSetConstantBuffers(0, 1, cbuffers);

        // No input layout, no vertex/index buffers — VS generates vertices from SV_VertexID
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        // No blending, no depth test
        context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
        context->OMSetDepthStencilState(nullptr, 0);

        // Draw fullscreen triangle
        context->Draw(3, 0);

        // Unbind scene SRV to avoid hazard (it will be used as a render target next frame)
        ID3D11ShaderResourceView* null_srv = nullptr;
        context->PSSetShaderResources(0, 1, &null_srv);
    }
}
