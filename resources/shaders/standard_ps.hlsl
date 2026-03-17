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
    float use_dynamic_lighting;
    float self_illumination;
    float light_scale;
};

struct PointLight {
    float3 pos;
    float radius;
    float3 color;
    float light_type;       // 0=omni, 1=spot, 2=tube
    float3 spot_dir;        // spotlight direction (0,0,0 for omni)
    float spot_fov1_dot;    // -cos(fov1/2): inner cone (negated)
    float spot_fov2_dot;    // -cos(fov2/2): outer cone (negated)
    float spot_atten;       // spotlight distance attenuation modifier
    float spot_sq_falloff;  // 1.0 = squared cone falloff, 0.0 = linear
    float atten_algo;       // distance attenuation: 0=linear, 1=squared, 2=cosine, 3=sqrt
    float3 pos2;            // tube light second endpoint
    float _pad1;
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

float4 main(VsOutput input) : SV_TARGET
{
    float4 tex0_color = disable_textures > 0.5f ? float4(1.0, 1.0, 1.0, 1.0) : tex0.Sample(samp0, input.uv0);
    float4 target = input.color * tex0_color * current_color;

    clip(target.a - alpha_test);

    float3 light_color = tex1.Sample(samp1, input.uv1).rgb;
    if (disable_textures < 0.5f) {
        if (use_dynamic_lighting > 0.5f) {
            // Dynamic-lit meshes (V3D items, characters): no lightmap.
            // Start from level ambient; light_scale applied to total after accumulation
            // to match stock vmesh_update_lighting_data which scales (ambient + lights) together.
            light_color = ambient_light;
        } else {
            // Static meshes: use baked lightmap
            light_color *= 2;
        }
        for (int i = 0; i < num_point_lights; ++i) {
            float3 pixel_pos = input.world_pos_and_depth.xyz;
            float ltype = point_lights[i].light_type;
            float dist;
            float3 light_dir;
            float ndotl_factor;

            if (ltype > 1.5f) {
                // Tube light: closest point on line segment, no N·L
                // (matches RED.exe baking which uses distance-only attenuation for tubes)
                float3 seg_start = point_lights[i].pos;
                float3 seg_end = point_lights[i].pos2;
                float3 seg_dir = seg_end - seg_start;
                float seg_len = length(seg_dir);
                if (seg_len > 0.001f) {
                    float3 seg_unit = seg_dir / seg_len;
                    float3 delta = pixel_pos - seg_start;
                    float t_proj = dot(seg_unit, delta);
                    float3 closest_pt;
                    if (t_proj <= 0.0f) {
                        closest_pt = seg_start;
                    } else if (t_proj >= seg_len) {
                        closest_pt = seg_end;
                    } else {
                        closest_pt = seg_start + seg_unit * t_proj;
                    }
                    float3 to_closest = pixel_pos - closest_pt;
                    dist = length(to_closest);
                } else {
                    // Degenerate tube (zero length) — treat as point light at pos
                    dist = length(pixel_pos - seg_start);
                }
                light_dir = float3(0.0f, 0.0f, 0.0f); // unused for tube
                ndotl_factor = 1.0f; // tube lights have no N·L in editor baking
            } else {
                // Omni or Spot: direction from pixel to light position
                float3 light_vec = point_lights[i].pos - pixel_pos;
                light_dir = normalize(light_vec);
                dist = length(light_vec);
                ndotl_factor = saturate(dot(input.norm, light_dir));
            }

            // Spotlight cone falloff (only for type 1, matches RED.exe baking)
            float spot_factor = 1.0f;
            if (ltype > 0.5f && ltype < 1.5f) {
                float cos_angle = dot(light_dir, point_lights[i].spot_dir);
                if (cos_angle >= point_lights[i].spot_fov2_dot) {
                    spot_factor = 0.0f;
                } else if (point_lights[i].spot_fov1_dot <= cos_angle) {
                    float cone_range = point_lights[i].spot_fov2_dot - point_lights[i].spot_fov1_dot;
                    spot_factor = 1.0f - (cos_angle - point_lights[i].spot_fov1_dot)
                                       / max(cone_range, 0.0001f);
                    if (point_lights[i].spot_sq_falloff > 0.5f) {
                        spot_factor = spot_factor * spot_factor;
                    }
                }
                dist = (1.0f - point_lights[i].spot_atten) * dist;
            }

            // Distance attenuation (same 4 algorithms for all light types,
            // verified against RED.exe disassembly at 0x00488CC0)
            float t = saturate(dist / point_lights[i].radius);
            float r = 1.0f - t;
            float atten;
            float algo = point_lights[i].atten_algo;
            if (algo < 0.5f) {
                atten = r;                      // 0: linear
            } else if (algo < 1.5f) {
                atten = r * r;                  // 1: squared
            } else if (algo < 2.5f) {
                atten = cos(t * 1.5707963f);    // 2: cosine
            } else {
                atten = sqrt(r);                // 3: sqrt
            }
            atten = max(atten, 0.0f);
            float intensity = atten * ndotl_factor * spot_factor;
            if (use_dynamic_lighting > 0.5f) {
                light_color += point_lights[i].color * intensity;
            } else {
                light_color += point_lights[i].color * intensity * 1.5f;
            }
        }
        if (use_dynamic_lighting > 0.5f) {
            // Apply light_scale (default 2.0, per-level configurable) to the total
            // (ambient + direct lights), matching stock vmesh_update_lighting_data
            // which applies the modifier to the entire lighting result.
            light_color *= light_scale;

            // Soft-knee luminance compression: prevents overbright while preserving
            // color hue. Per-channel compression would shift hues (e.g. warm lights
            // with red > green get red compressed more, producing a green tint).
            // Instead, compress based on luminance and scale all channels equally.
            float lum = dot(light_color, float3(0.2126f, 0.7152f, 0.0722f));
            if (lum > 0.0f) {
                float shoulder = 1.0f;
                float range = 0.5f;
                float excess = max(lum - shoulder, 0.0f);
                float compressed_lum = min(lum, shoulder) + excess * range / (excess + range);
                light_color *= compressed_lum / lum;
            }
        }
    }

    // Self-illumination sets a minimum brightness floor (matches stock engine behavior).
    if (self_illumination > 0.0f) {
        light_color = max(light_color, self_illumination);
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
