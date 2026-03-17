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

cbuffer TextureScaleBuffer : register(b2)
{
    float2 tex0_uv_scale;
};

cbuffer ShadowBuffer : register(b3)
{
    float4x4 shadow_vp_mat;
    float shadow_strength;
    float shadow_fade_start;
    float shadow_fade_end;
    float shadow_enabled;
    float3 shadow_light_dir;
    float shadow_normal_offset;
    float shadow_texel_size;
    float shadow_depth_range;
    float shadow_projection_fade_start;
    float shadow_projection_fade_end;
    float shadow_pcf_taps;
    float shadow_debug;
    float shadow_soft_edges;
    float shadow_pad;
};

Texture2D tex0;
Texture2D tex1;
Texture2D shadow_map : register(t2);
SamplerState samp0;
SamplerState samp1;
SamplerComparisonState shadow_sampler : register(s2);
SamplerState shadow_depth_sampler : register(s3);

// Poisson disk offsets for multi-tap PCF (up to 15 extra taps beyond center = 16 max)
static const float2 pcf_offsets[15] = {
    float2(-0.326f, -0.406f),
    float2(-0.840f, -0.074f),
    float2(-0.696f,  0.457f),
    float2(-0.203f,  0.621f),
    float2( 0.962f, -0.195f),
    float2( 0.473f, -0.480f),
    float2( 0.519f,  0.767f),
    float2( 0.185f,  0.893f),
    float2(-0.507f, -0.792f),
    float2( 0.336f, -0.882f),
    float2(-0.946f,  0.250f),
    float2( 0.792f,  0.384f),
    float2(-0.138f, -0.960f),
    float2( 0.891f, -0.546f),
    float2(-0.428f,  0.882f),
};

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
    float2 scaled_uv0 = input.uv0 * tex0_uv_scale;
    float4 tex0_color = disable_textures > 0.5f ? float4(1.0, 1.0, 1.0, 1.0) : tex0.Sample(samp0, scaled_uv0);
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
        float3 pixel_pos = input.world_pos_and_depth.xyz;
        for (int i = 0; i < num_point_lights; ++i) {
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

    if (shadow_enabled > 0.5f) {
        float3 world_pos = input.world_pos_and_depth.xyz;
        float3 normal = normalize(input.norm);

        // NdotL: how much the surface faces the light (light_dir points FROM light)
        float NdotL = dot(normal, -shadow_light_dir);

        // Smooth NdotL fade instead of hard cutoff
        float ndotl_fade = saturate(NdotL * 5.0f);

        if (ndotl_fade > 0.0f) {
            // Normal offset bias scaled by angle to reduce self-shadowing
            float bias_scale = saturate(1.0f - NdotL);
            float3 biased_pos = world_pos + normal * shadow_normal_offset * (1.0f + bias_scale);

            float4 shadow_pos = mul(float4(biased_pos, 1.0f), shadow_vp_mat);
            float3 shadow_ndc = shadow_pos.xyz / shadow_pos.w;
            float2 shadow_uv = shadow_ndc.xy * 0.5f + 0.5f;
            shadow_uv.y = 1.0f - shadow_uv.y;

            float shadow_value = 1.0f;
            float debug_proj_fade = 1.0f;
            if (shadow_uv.x >= 0.0f && shadow_uv.x <= 1.0f && shadow_uv.y >= 0.0f && shadow_uv.y <= 1.0f) {
                float spread = shadow_texel_size * 2.5f;
                int extra_taps = (int)shadow_pcf_taps - 1;

                // Receiver-side comparison bias
                float z_compensation = shadow_normal_offset * (1.0f + bias_scale) * NdotL / shadow_depth_range;
                float compare_depth = shadow_ndc.z + z_compensation;

                // Per-pixel rotation angle to break up PCF banding on small shadows
                float pcf_angle = frac(sin(dot(input.pos.xy * 0.5f, float2(12.9898f, 78.233f))) * 43758.5453f) * 6.28318530718f;
                float pcf_cos = cos(pcf_angle);
                float pcf_sin = sin(pcf_angle);

                float proj_fade_range = shadow_projection_fade_end - shadow_projection_fade_start;

                // Center tap
                float center_depth = shadow_map.SampleLevel(shadow_depth_sampler, shadow_uv, 0).r;
                float center_pd = saturate(shadow_ndc.z - center_depth) * shadow_depth_range;
                float center_pf = 1.0f - saturate((center_pd - shadow_projection_fade_start) / proj_fade_range);
                debug_proj_fade = center_pf;
                float center_cmp = shadow_map.SampleCmpLevelZero(shadow_sampler, shadow_uv, compare_depth);
                float shadow_sum = lerp(1.0f, lerp(shadow_strength, 1.0f, center_cmp), center_pf);

                // Early-out: skip extra taps if center is fully lit (no shadow nearby)
                // Disabled when soft_edges is on (quality 5) for softer shadow boundaries
                if (center_cmp >= 1.0f && extra_taps > 0 && shadow_soft_edges < 0.5f) {
                    shadow_value = 1.0f;
                } else {
                    // Extra taps (PCF with Poisson disk + per-tap projection fade)
                    for (int t = 0; t < extra_taps && t < 15; ++t) {
                        float2 ofs = float2(pcf_offsets[t].x * pcf_cos - pcf_offsets[t].y * pcf_sin,
                                            pcf_offsets[t].x * pcf_sin + pcf_offsets[t].y * pcf_cos);
                        float2 tap_uv = shadow_uv + ofs * spread;
                        float tap_depth = shadow_map.SampleLevel(shadow_depth_sampler, tap_uv, 0).r;
                        float tap_pd = saturate(shadow_ndc.z - tap_depth) * shadow_depth_range;
                        float tap_pf = 1.0f - saturate((tap_pd - shadow_projection_fade_start) / proj_fade_range);
                        float tap_cmp = shadow_map.SampleCmpLevelZero(shadow_sampler, tap_uv, compare_depth);
                        shadow_sum += lerp(1.0f, lerp(shadow_strength, 1.0f, tap_cmp), tap_pf);
                    }
                    shadow_value = shadow_sum / shadow_pcf_taps;
                }
            }

            float cam_dist = input.world_pos_and_depth.w;
            float fade = 1.0f - saturate((cam_dist - shadow_fade_start) / (shadow_fade_end - shadow_fade_start));

            if (shadow_debug > 0.5f) {
                // Debug: Red = shadow darkening, Green = projection fade suppression (center tap)
                float darken = (1.0f - shadow_value) * fade * ndotl_fade;
                float proj_suppress = (1.0f - debug_proj_fade) * fade * ndotl_fade;
                target.rgb *= 0.3f;
                target.rgb += float3(darken * 1.5f, proj_suppress * 1.0f, 0.0f);
            } else {
                float final_shadow = lerp(1.0f, shadow_value, fade * ndotl_fade);
                target.rgb *= final_shadow;
            }
        }
    }

    target.rgb = saturate(target.rgb);

    float fog = saturate(input.world_pos_and_depth.w / fog_far);
    target.rgb = fog * fog_color + (1 - fog) * target.rgb;

    if (colorblind_mode > 0.5f) {
        target.rgb = saturate(apply_colorblind(target.rgb));
    }

    return target;
}
