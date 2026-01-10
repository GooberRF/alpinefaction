struct VsOutput
{
    float4 pos : SV_POSITION;
    float3 norm : NORMAL;
    float4 color : COLOR;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    float4 world_pos_and_depth : TEXCOORD2;
};

cbuffer RenderModeBuffer : register(b0)
{
    float4 current_color;
    float alpha_test;
    float fog_far;
    float colorblind_mode;
    float disable_textures;
    float3 fog_color;
};

Texture2D tex0;
SamplerState samp0;

float3 apply_colorblind(float3 color)
{
    float3x3 mat;
    if (colorblind_mode < 1.5f) {
        // Protanopia
        mat = float3x3(
            0.567, 0.433, 0.0,
            0.558, 0.442, 0.0,
            0.0,   0.242, 0.758
        );
    } else if (colorblind_mode < 2.5f) {
        // Deuteranopia
        mat = float3x3(
            0.625, 0.375, 0.0,
            0.7,   0.3,   0.0,
            0.0,   0.3,   0.7
        );
    } else {
        // Tritanopia
        mat = float3x3(
            0.95,  0.05,  0.0,
            0.0,   0.433, 0.567,
            0.0,   0.475, 0.525
        );
    }
    return mul(color, mat);
}

float4 main(VsOutput input) : SV_TARGET
{
    float4 col = tex0.Sample(samp0, input.uv0) * input.color;
    if (colorblind_mode > 0.5f) {
        col.rgb = saturate(apply_colorblind(col.rgb));
    }
    return col;
}
