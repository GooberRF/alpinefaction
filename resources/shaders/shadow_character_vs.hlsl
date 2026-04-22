struct VsInput
{
    float3 pos : POSITION;
    float3 norm : NORMAL;
    float2 uv0 : TEXCOORD0;
    float4 color : COLOR0;
    float4 weights : COLOR1;
    float4 indices : COLOR2;
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

cbuffer CharacterBuffer : register(b3)
{
    float4x3 matrices[50];
};

struct VsOutput
{
    float4 pos : SV_POSITION;
};

VsOutput main(VsInput input)
{
    VsOutput output;
    float4 pos_h = float4(input.pos.xyz, 1.0f);
    float3 model_pos =
        mul(pos_h, matrices[input.indices[0] * 255.0f]) * input.weights[0] +
        mul(pos_h, matrices[input.indices[1] * 255.0f]) * input.weights[1] +
        mul(pos_h, matrices[input.indices[2] * 255.0f]) * input.weights[2] +
        mul(pos_h, matrices[input.indices[3] * 255.0f]) * input.weights[3];
    // Reimplement RF bug (dividing bone weight by 256) to fix some animations (especially in cutscenes)
    model_pos *= 255.0f / 256.0f;
    float3 world_pos = mul(float4(model_pos, 1), world_mat);
    float3 view_pos = mul(float4(world_pos, 1), view_mat);
    output.pos = mul(float4(view_pos, 1), proj_mat);
    return output;
}
