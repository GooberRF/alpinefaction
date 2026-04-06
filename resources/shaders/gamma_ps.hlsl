cbuffer GammaBuffer : register(b0)
{
    float gamma_value;
};

Texture2D scene_texture : register(t0);
SamplerState point_sampler : register(s0);

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    float4 color = scene_texture.Sample(point_sampler, uv);
    color.rgb = pow(color.rgb, 1.0 / gamma_value);
    return color;
}
