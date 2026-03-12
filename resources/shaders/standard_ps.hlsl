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
    float _pad0;
    // Side-scroller occlusion
    float3 ss_player_pos;
    float ss_fade_strength;
    float ss_camera_x;
    float ss_radius;
    float ss_is_detail;
    float _ss_pad;
};

struct PointLight {
    float3 pos;
    float radius;
    float3 color;
};

#define MAX_POINT_LIGHTS 32

cbuffer LightsBuffer : register(b1)
{
    float3 ambient_light;
    float num_point_lights;
    PointLight point_lights[MAX_POINT_LIGHTS];
};

Texture2D tex0;
Texture2D tex1;
SamplerState samp0;
SamplerState samp1;

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

// 4x4 Bayer dither matrix (normalized 0..1)
static const float bayer4x4[4][4] = {
    {  0.0/16.0,  8.0/16.0,  2.0/16.0, 10.0/16.0 },
    { 12.0/16.0,  4.0/16.0, 14.0/16.0,  6.0/16.0 },
    {  3.0/16.0, 11.0/16.0,  1.0/16.0,  9.0/16.0 },
    { 15.0/16.0,  7.0/16.0, 13.0/16.0,  5.0/16.0 },
};

float4 main(VsOutput input) : SV_TARGET
{
    float4 tex0_color = disable_textures > 0.5f ? float4(1.0, 1.0, 1.0, 1.0) : tex0.Sample(samp0, input.uv0);
    float4 target = input.color * tex0_color * current_color;

    clip(target.a - alpha_test);

    // Side-scroller occlusion: dithered transparency for detail/mover geometry near player
    if (ss_fade_strength > 0.0f && ss_is_detail > 0.5f) {
        float3 world_pos = input.world_pos_and_depth.xyz;
        // Distance from player axis in YZ plane (cylinder around player)
        float2 delta_yz = world_pos.yz - ss_player_pos.yz;
        float dist_yz = length(delta_yz);
        if (dist_yz < ss_radius) {
            // Linear falloff: full fade at center, zero at edge
            float fade = ss_fade_strength * saturate(1.0f - dist_yz / ss_radius);
            // Dithered discard using Bayer matrix
            uint2 pixel = uint2(input.pos.xy) % 4;
            float dither = bayer4x4[pixel.y][pixel.x];
            clip(dither - fade);
        }
    }

    float3 light_color = tex1.Sample(samp1, input.uv1).rgb;
    if (disable_textures < 0.5f) {
        light_color *= 2;
        for (int i = 0; i < num_point_lights; ++i) {
            float3 light_vec = point_lights[i].pos - input.world_pos_and_depth.xyz;
            float3 light_dir = normalize(light_vec);
            float dist = length(light_vec);
            float atten = saturate(dist / point_lights[i].radius);
            atten = atten * atten * (3.0f - 2.0f * atten);
            float intensity = (1.0f - atten) * saturate(dot(input.norm, light_dir));
            light_color += point_lights[i].color * intensity * 1.5;
        }
    }

    target.rgb *= light_color;
    target.rgb = saturate(target.rgb);

    float fog = saturate(input.world_pos_and_depth.w / fog_far);
    target.rgb = fog * fog_color + (1 - fog) * target.rgb;

    if (colorblind_mode > 0.5f) {
        target.rgb = saturate(apply_colorblind(target.rgb));
    }

    return target;
}
