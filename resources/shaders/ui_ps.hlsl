struct VsOutput
{
    float4 pos : SV_POSITION;
    float3 norm : NORMAL;
    float4 color : COLOR;
    float2 uv0 : TEXCOORD0;
    float3 uv1 : TEXCOORD1;
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

#ifdef UI_SDF

#define SDF_SHAPE_ROUNDED_RECT 0
#define SDF_SHAPE_CAPSULE 1

cbuffer UiSdfBuffer : register(b10)
{
    // rounded rect: half_w, half_h, corner_radius, border_thickness (<= 0 means filled)
    // capsule:      half_length, half_width, unused, unused
    float4 sdf_params;
    // x: one of SDF_SHAPE_*
    float4 sdf_type;
};

#else

cbuffer TextureScaleBuffer : register(b2)
{
    float2 tex0_uv_scale;
};

Texture2D tex0;
SamplerState samp0;

#endif

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

#ifdef UI_SDF

float sd_round_box(float2 p, float2 half_size, float radius)
{
    float2 q = abs(p) - half_size + radius;
    return min(max(q.x, q.y), 0.0f) + length(max(q, 0.0f)) - radius;
}

float sd_capsule(float2 p, float half_len, float half_width)
{
    p.x -= clamp(p.x, -half_len, half_len);
    return length(p) - half_width;
}

float4 main(VsOutput input) : SV_TARGET
{
    float2 p = input.uv0;
    int shape = (int)round(sdf_type.x);
    float d;
    if (shape == SDF_SHAPE_ROUNDED_RECT) {
        d = sd_round_box(p, sdf_params.xy, sdf_params.z);
        if (sdf_params.w > 0.0f) {
            // Band lying inside the outline, so a border never grows the shape's footprint.
            d = abs(d + sdf_params.w * 0.5f) - sdf_params.w * 0.5f;
        }
    } else {
        d = sd_capsule(p, sdf_params.x, sdf_params.y);
    }

    float aa = max(fwidth(d), 1e-5f);
    float coverage = saturate(0.5f - d / aa);

    float4 col = input.color;
    col.a *= coverage;
    if (colorblind_mode > 0.5f) {
        col.rgb = saturate(apply_colorblind(col.rgb));
    }
    return col;
}

#else

float4 main(VsOutput input) : SV_TARGET
{
    float2 scaled_uv0 = input.uv0 * tex0_uv_scale;
    float4 col = tex0.Sample(samp0, scaled_uv0) * input.color;
    if (colorblind_mode > 0.5f) {
        col.rgb = saturate(apply_colorblind(col.rgb));
    }
    return col;
}

#endif
