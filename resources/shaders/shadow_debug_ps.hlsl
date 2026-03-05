struct VsOutput
{
    float4 pos : SV_POSITION;
    float3 norm : NORMAL;
    float4 color : COLOR;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float4 world_pos_and_depth : TEXCOORD2;
};

Texture2D tex0;
SamplerState samp0;

float4 main(VsOutput input) : SV_TARGET
{
    float depth = tex0.SampleLevel(samp0, input.uv0, 0).r;
    return float4(depth, depth, depth, input.color.a);
}
