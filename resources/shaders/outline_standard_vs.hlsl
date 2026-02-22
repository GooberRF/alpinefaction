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

cbuffer OutlineParams : register(b4)
{
    float2 screen_resolution;
    float outline_thickness;
    float padding;
};

struct VsOutput
{
    float4 pos : SV_POSITION;
};

VsOutput main(VsInput input)
{
    VsOutput output;

    // Transform to world and view space
    float3 world_pos = mul(float4(input.pos.xyz, 1), world_mat);
    float3 world_norm = normalize(mul(float4(input.norm, 0), world_mat));
    float3 view_pos = mul(float4(world_pos, 1), view_mat);
    output.pos = mul(float4(view_pos, 1), proj_mat);

    // Screen-space normal extrusion for pixel-accurate outline thickness
    float3 view_norm = mul(float4(world_norm, 0), view_mat);
    float2 proj_norm = mul(float4(view_norm, 0), proj_mat).xy;
    float len = length(proj_norm);
    if (len > 0.0001f) {
        float2 screen_offset = proj_norm / len;
        output.pos.xy += screen_offset * outline_thickness * (2.0f / screen_resolution) * output.pos.w;
    }

    return output;
}
