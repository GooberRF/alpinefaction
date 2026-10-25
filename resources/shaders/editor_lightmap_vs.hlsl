// The level editor's lightmapped face: pre-transformed like transformed_vs, plus the lightmap UV the standard pixel
// shader samples from its second texture, or from the Alpine atlas chart in z (-1 for none)
struct VsInput
{
    float4 pos : POSITIONT;
    float4 color : COLOR;
    float2 uv0 : TEXCOORD0;
    float3 uv1 : TEXCOORD1;
};

struct VsOutput
{
    float4 pos : SV_POSITION;
    float3 norm : NORMAL;
    float4 color : COLOR;
    float2 uv0 : TEXCOORD0;
    float3 uv1 : TEXCOORD1;
    float4 world_pos_and_depth : TEXCOORD2;
};

VsOutput main(VsInput input)
{
    VsOutput output;
    output.pos = input.pos;
    output.norm = float3(0, 0, 0);
    output.color = input.color;
    output.uv0 = input.uv0;
    output.uv1 = input.uv1;
    output.world_pos_and_depth = float4(0, 0, 0, input.pos.w);
    return output;
}
