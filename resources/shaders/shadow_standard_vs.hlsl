struct VsInput
{
    float3 pos : POSITION;
    float3 norm : NORMAL;
    float4 color : COLOR;
    float4 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
};

cbuffer ModelTransformBuffer : register(b0)
{
    float4x3 world_mat;
};

cbuffer ViewProjTransformBuffer : register(b1)
{
    float4x3 view_mat;
    float4x4 proj_mat;
};

struct VsOutput
{
    float4 pos : SV_POSITION;
};

VsOutput main(VsInput input)
{
    VsOutput output;
    float3 world_pos = mul(float4(input.pos.xyz, 1), world_mat);
    float3 view_pos = mul(float4(world_pos, 1), view_mat);
    output.pos = mul(float4(view_pos, 1), proj_mat);
    return output;
}
