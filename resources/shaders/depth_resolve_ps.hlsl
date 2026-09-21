// Sample 0 of the MSAA depth buffer, written into the single-sample copy the liquid surface pass
// reads. ps_4_1 only: an unsized Texture2DMS is illegal in ps_4_0.

struct PsInput
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

Texture2DMS<float> depth_ms : register(t0);

float main(PsInput input) : SV_Depth
{
    return depth_ms.Load(int2(input.pos.xy), 0);
}
