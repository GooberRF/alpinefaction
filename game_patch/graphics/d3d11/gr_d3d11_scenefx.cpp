#include <cstring>
#include <utility>
#include "gr_d3d11_scenefx.h"
#include "gr_d3d11.h"
#include "gr_d3d11_shader.h"

namespace gr::d3d11
{
    ScenePostPass::ScenePostPass(ComPtr<ID3D11Device> device, ShaderManager& shader_manager)
        : device_{std::move(device)}
    {
        vertex_shader_ = shader_manager.load_vertex_shader_only(
            get_vertex_shader_filename(VertexShaderId::gamma));

        pixel_shader_ = shader_manager.get_pixel_shader(PixelShaderId::scenefx);

        CD3D11_BUFFER_DESC cb_desc{
            sizeof(SceneFxBufferData),
            D3D11_BIND_CONSTANT_BUFFER,
            D3D11_USAGE_DYNAMIC,
            D3D11_CPU_ACCESS_WRITE,
        };
        DF_GR_D3D11_CHECK_HR(device_->CreateBuffer(&cb_desc, nullptr, &cbuffer_));

        CD3D11_SAMPLER_DESC sampler_desc{D3D11_DEFAULT};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        DF_GR_D3D11_CHECK_HR(device_->CreateSamplerState(&sampler_desc, &point_sampler_));

        CD3D11_BLEND_DESC blend_desc{D3D11_DEFAULT};
        auto& rt_blend = blend_desc.RenderTarget[0];
        rt_blend.BlendEnable = TRUE;
        rt_blend.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        rt_blend.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        rt_blend.BlendOp = D3D11_BLEND_OP_ADD;
        rt_blend.SrcBlendAlpha = D3D11_BLEND_ONE;
        rt_blend.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt_blend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        // Colour only: the scene's alpha channel is not ours to touch
        rt_blend.RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        DF_GR_D3D11_CHECK_HR(device_->CreateBlendState(&blend_desc, &overlay_blend_state_));

        // The distort path replaces the frame, but the scene's alpha channel is still not ours
        CD3D11_BLEND_DESC opaque_desc{D3D11_DEFAULT};
        opaque_desc.RenderTarget[0].RenderTargetWriteMask =
            D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        DF_GR_D3D11_CHECK_HR(device_->CreateBlendState(&opaque_desc, &distort_blend_state_));

        CD3D11_DEPTH_STENCIL_DESC ds_desc{D3D11_DEFAULT};
        ds_desc.DepthEnable = FALSE;
        ds_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        DF_GR_D3D11_CHECK_HR(device_->CreateDepthStencilState(&ds_desc, &depth_off_state_));
    }

    void ScenePostPass::render(ID3D11DeviceContext* context, ID3D11ShaderResourceView* scene_srv,
                               ID3D11RenderTargetView* target_rtv, const SceneFxBufferData& data)
    {
        D3D11_MAPPED_SUBRESOURCE mapped;
        DF_GR_D3D11_CHECK_HR(
            context->Map(cbuffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)
        );
        std::memcpy(mapped.pData, &data, sizeof(data));
        context->Unmap(cbuffer_, 0);

        context->OMSetRenderTargets(1, &target_rtv, nullptr);

        context->VSSetShader(vertex_shader_, nullptr, 0);
        context->PSSetShader(pixel_shader_, nullptr, 0);

        if (scene_srv) {
            context->PSSetShaderResources(0, 1, &scene_srv);
            ID3D11SamplerState* samplers[] = {point_sampler_};
            context->PSSetSamplers(0, 1, samplers);
        }

        ID3D11Buffer* cbuffers[] = {cbuffer_};
        context->PSSetConstantBuffers(0, 1, cbuffers);

        // No input layout, no vertex/index buffers — VS generates vertices from SV_VertexID
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        ID3D11BlendState* blend_state = scene_srv ? distort_blend_state_.get() : overlay_blend_state_.get();
        context->OMSetBlendState(blend_state, nullptr, 0xffffffff);
        context->OMSetDepthStencilState(depth_off_state_, 0);

        context->Draw(3, 0);

        // Unbind the scene copy so the next frame can write it again
        ID3D11ShaderResourceView* null_srv = nullptr;
        context->PSSetShaderResources(0, 1, &null_srv);
    }
}
