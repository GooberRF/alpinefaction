struct VsInput
{
    float3 pos : POSITION;
    float3 norm : NORMAL;
    float4 color : COLOR;
    float4 uv0 : TEXCOORD0;
    // xy: stock lightmap page UV, or alpine chart texel coords when z >= 0
    // z:  alpine lightmap chart (its af_lm_index chart record), -1 for the stock path
    float3 uv1 : TEXCOORD1;
#ifdef INSTANCED
    // Terrain decoration instance: model to world rows, scale included, and its light
    float4 inst_row0 : INSTANCE0;
    float4 inst_row1 : INSTANCE1;
    float4 inst_row2 : INSTANCE2;
    float4 inst_light : INSTANCE_LIGHT;
#endif
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

cbuffer PerFrameBuffer : register(b2)
{
    float time;
};

#ifdef INSTANCED
// Layout owned by gr_d3d11_decoration.cpp (DecorationBufferData)
cbuffer DecorationBuffer : register(b4)
{
    float3 submesh_center;
    float draw_distance;
    float fade_band;
};
#endif

struct VsOutput
{
    float4 pos : SV_POSITION;
    float3 norm : NORMAL;
    float4 color : COLOR;
    float2 uv0 : TEXCOORD0;
    float3 uv1 : TEXCOORD1;
    float4 world_pos_and_depth : TEXCOORD2;
#ifdef INSTANCED
    float4 inst_light : TEXCOORD3;
#endif
};

VsOutput main(VsInput input)
{
    VsOutput output;
#ifdef INSTANCED
    // The engine draws a submesh at pos + orient * (center + v); past the draw distance the instance shrinks
    // to its origin over the fade band.
    float3 origin = float3(input.inst_row0.w, input.inst_row1.w, input.inst_row2.w);
    float fade = saturate((draw_distance - length(mul(float4(origin, 1), view_mat))) / fade_band);
    float4 local = float4((input.pos + submesh_center) * fade, 1);
    float3 world_pos = float3(dot(input.inst_row0, local), dot(input.inst_row1, local), dot(input.inst_row2, local));
    float3 world_norm = float3(dot(input.inst_row0.xyz, input.norm), dot(input.inst_row1.xyz, input.norm),
                               dot(input.inst_row2.xyz, input.norm));
    output.uv1 = float3(0, 0, -1);
    output.inst_light = input.inst_light;
#else
    float3 world_pos = mul(float4(input.pos.xyz, 1), world_mat);
    float3 world_norm = mul(float4(input.norm, 0.0f), world_mat);
    output.uv1 = input.uv1;
#endif
    float3 view_pos = mul(float4(world_pos, 1), view_mat);
    output.pos = mul(float4(view_pos, 1), proj_mat);
    output.norm = normalize(world_norm);
    output.uv0 = input.uv0.xy + input.uv0.zw * time;
    output.color = input.color;
    output.world_pos_and_depth = float4(world_pos, view_pos.z);
    return output;
}
