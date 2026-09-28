struct VsOutput
{
    float4 pos : SV_POSITION;
    float3 norm : NORMAL;
    float4 color : COLOR;
    float2 uv0 : TEXCOORD0;
    // xy: stock lightmap page UV, or alpine chart texel coords when z >= 0
    // z:  alpine lightmap chart (its af_lm_index chart record), -1 for the stock path
    float3 uv1 : TEXCOORD1;
    float4 world_pos_and_depth : TEXCOORD2;
#ifdef INSTANCE_LIGHT
    // Terrain decoration: rgb its mesh ambient, a its sun scale
    float4 inst_light : TEXCOORD3;
#endif
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
    float dynamic_light_ndotl;
    float pixel_light_overbright;
    float emissive_override;
    float gas_fog_allowed;
    float sky_room;             // sky fragments carry authored, not viewed, world positions
    float draw_room_uid;        // room this draw belongs to, -1 when it has none
    float liquid_surface;       // this draw is the liquid surface pass
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
    float3 sun_travel_dir;  // direction the sunlight travels
    float sun_scale;        // 0 = no sun term (all world/solid passes)
    float3 sun_color;       // premultiplied by sun intensity
    float af_lm_enabled;    // alpine lightmap atlas is live for this level
    float af_lm_page_size;  // P, edge length of every atlas page
    float af_lm_tile_step;  // S, chart texel step between tile origins
    float af_lm_gutter;     // G, stored overlap each side of a tiled chart
    float _af_lm_pad;
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

#ifndef MAX_GAS_REGIONS
#define MAX_GAS_REGIONS 32
#endif

#if MAX_GAS_REGIONS > 0

struct GasRegionData
{
    float3 center;    float density;
    float3 color;     float shape;       // 0=sphere, 1=box
    float3 extents;   float _pad0;
    float3 orient_r0; float _pad1;       // transpose row 0 (for world-to-local)
    float3 orient_r1; float _pad2;       // transpose row 1
    float3 orient_r2; float _pad3;       // transpose row 2
};

cbuffer GasRegionBuffer : register(b4)
{
    float3 gas_eye_pos;
    int num_gas_regions;
    float3 gas_cam_right;   float gas_proj_sx;
    float3 gas_cam_up;      float gas_proj_sy;
    float3 gas_cam_forward; float gas_viewport_w;
    float gas_viewport_h;   float gas_viewport_x; float gas_viewport_y; float _gas_header_pad;
    GasRegionData gas_regions[MAX_GAS_REGIONS];
};

#endif

struct CausticVolume
{
    float3 bbox_min;  float surface_y;
    float3 bbox_max;  float room_uid;
    float3 color;     float _cpad1;
};

#define MAX_CAUSTIC_VOLUMES 16

cbuffer CausticsBuffer : register(b5)
{
    int   num_caustic_volumes;
    float caustic_time;
    float caustic_intensity;
    float caustic_scale;
    float caustic_speed;
    float caustic_floor;
    float caustic_exponent;
    float caustic_depth_fade;
    float caustic_above_water;
    float caustic_drift;
    float caustic_wall_stretch;
    float _caustic_hdr_pad;
    CausticVolume caustic_volumes[MAX_CAUSTIC_VOLUMES];
};

#define MAX_LIQUID_VOLUMES 16

// One liquid room. The box arrives pre-expanded and already capped at the surface plane.
struct LiquidVolume
{
    float3 bbox_min; float _pad0;
    float3 bbox_max; float _pad1;
};

cbuffer LiquidBuffer : register(b6)
{
    float3 liq_eye_pos;        float liq_mode;         // 0 disables the block; nonzero is not inspected
    float3 liq_cam_right;      float liq_proj_sx;
    float3 liq_cam_up;         float liq_proj_sy;
    float3 liq_cam_forward;    float liq_viewport_w;
    float  liq_viewport_h;     float liq_surface_y;
    float  liq_visibility;     float liq_eye_under;
    float3 liq_color;          float liq_viewport_x;
    float3 liq_over_fog_color; float liq_over_fog_far;
    float4 liq_params;         // sigma_k, absorb_hi, absorb_lo, depth_darken
    float  liq_far_clip;       float liq_num_volumes;  // far plane the fade targets, volume count
    float  liq_dark_surface_y; float liq_viewport_y;   // blended surface, depth darkening only
    LiquidVolume liq_volumes[MAX_LIQUID_VOLUMES];
    float  liq_depth_sz;       float liq_depth_tz;     // view z = tz / (device depth - sz)
    float  liq_depth_mode;     float _liq_pad2;        // 0 none, 1 Texture2D
};

Texture2D tex0;
Texture2D tex1;
Texture2D shadow_map : register(t2);
SamplerState samp0;
SamplerState samp1;
SamplerComparisonState shadow_sampler : register(s2);
SamplerState shadow_depth_sampler : register(s3);
Texture2DArray caustic_tex : register(t3);
SamplerState   caustic_samp : register(s4);
// Copy of the scene depth buffer, for the liquid surface pass only.
Texture2D<float> scene_depth : register(t6);

// Alpine lightmaps. af_lm_index holds the chart records (surfaces, terrains, mover surfaces) and the tile records
// after them; a chart's .z is already biased past the chart block, so it is added to ty*nx+tx and
// nothing else. Layout is owned by common/lightmap/alpine_lightmap_reader.h.
Texture2DArray            af_lm_pages : register(t4);
Buffer<uint4>             af_lm_index : register(t5);
SamplerState              af_lm_samp  : register(s5);

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

// One medium over the fragment: dim by its transmittance, then add its in-scatter where the
// draw mode allows fog. Apply the farther medium first so each in-scatter is dimmed by the
// medium in front of it.
float3 apply_medium(float3 c, float3 medium_t, float3 in_scatter, bool add_scatter)
{
    c *= medium_t;
    if (add_scatter) {
        c += in_scatter * (float3(1.0f, 1.0f, 1.0f) - medium_t);
    }
    return c;
}

// SV_POSITION is render-target space, so the viewport origin has to come off first.
float2 screen_to_ndc(float2 screen_pos, float viewport_x, float viewport_y,
                     float viewport_w, float viewport_h)
{
    return float2(((screen_pos.x - viewport_x) / viewport_w) * 2.0f - 1.0f,
                  ((screen_pos.y - viewport_y) / viewport_h) * -2.0f + 1.0f);
}

// World position of a pre-transformed fragment, from its screen position and view depth.
// The camera header (eye/right/up/forward/proj scale/viewport) is passed in because b4 and b6
// each carry their own copy and b4 does not exist in the no-gas permutation.
float3 reconstruct_world_pos(float2 screen_pos, float depth, float3 eye,
                             float3 cam_right, float3 cam_up, float3 cam_forward,
                             float proj_sx, float proj_sy,
                             float viewport_x, float viewport_y, float viewport_w, float viewport_h)
{
    float2 ndc = screen_to_ndc(screen_pos, viewport_x, viewport_y, viewport_w, viewport_h);
    float view_x = ndc.x * depth / proj_sx;
    float view_y = ndc.y * depth / proj_sy;
    return eye + cam_right * view_x + cam_up * view_y + cam_forward * depth;
}

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

// vkd3d-shader (Wine's d3dcompiler, used by the Linux CI build) requires float3 gradients for
// Texture2DArray::SampleGrad; FXC requires float2 and warns on truncation.
#ifdef ARRAY_GRAD_FLOAT3
#define ARRAY_GRAD(g) float3(g, 0.0f)
#else
#define ARRAY_GRAD(g) (g)
#endif

// One triplanar plane: two drifting layers, each blended across two animation frames.
float caustic_sample_plane(float2 p, float2 drift, float s0, float s1, float sf,
                           float2 dpdx, float2 dpdy)
{
    float2 uv_a = p * caustic_scale + drift;
    float2 uv_b = float2(-p.y, p.x) * caustic_scale * 0.61f - drift * 0.8f;
    float2 ga_x = dpdx * caustic_scale;
    float2 ga_y = dpdy * caustic_scale;
    float2 gb_x = float2(-dpdx.y, dpdx.x) * caustic_scale * 0.61f;
    float2 gb_y = float2(-dpdy.y, dpdy.x) * caustic_scale * 0.61f;
    float ca = lerp(caustic_tex.SampleGrad(caustic_samp, float3(uv_a, s0), ARRAY_GRAD(ga_x), ARRAY_GRAD(ga_y)).r,
                    caustic_tex.SampleGrad(caustic_samp, float3(uv_a, s1), ARRAY_GRAD(ga_x), ARRAY_GRAD(ga_y)).r, sf);
    float cb = lerp(caustic_tex.SampleGrad(caustic_samp, float3(uv_b, s0), ARRAY_GRAD(gb_x), ARRAY_GRAD(gb_y)).r,
                    caustic_tex.SampleGrad(caustic_samp, float3(uv_b, s1), ARRAY_GRAD(gb_x), ARRAY_GRAD(gb_y)).r, sf);
    return (ca + cb) * 0.5f;
}

// Mirror of alpine_lightmap::sample_page_coords + page_uv. SampleLevel only: vkd3d rejects the
// implicit-gradient array sample the MSVC compiler would take here.
float3 af_lm_sample(float2 chart_uv, uint chart)
{
    uint4 rec = af_lm_index[chart];
    if (rec.x == 0 || rec.y == 0) {
        return float3(0.5f, 0.5f, 0.5f);
    }
    float step = af_lm_tile_step;
    float2 t = clamp(floor(chart_uv / step), float2(0.0f, 0.0f),
                     float2((float)(rec.x - 1u), (float)(rec.y - 1u)));
    uint4 tile = af_lm_index[rec.z + (uint)t.y * rec.x + (uint)t.x];
    float2 pad = float2((float)(rec.w & 0xffffu), (float)(rec.w >> 16));
    float2 p = float2((float)tile.y, (float)tile.z) + pad + (chart_uv - t * step);
    return af_lm_pages.SampleLevel(af_lm_samp, float3(p / af_lm_page_size, (float)tile.x), 0).rgb;
}

// The scene lights over a base light: the sun where the pass enables it, the dynamic lights, and
// for dynamic-lit meshes the light scale and overbright compression.
float3 add_scene_lights(float3 light_color, float3 pixel_pos, float3 norm)
{
#ifndef INSTANCE_LIGHT
    if (sun_scale > 0.0f) {
        light_color += sun_color * sun_scale * saturate(dot(norm, -sun_travel_dir));
    }
#endif
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
            ndotl_factor = saturate(dot(norm, light_dir));
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
        float intensity;
        if (use_dynamic_lighting > 0.5f) {
            intensity = atten * ndotl_factor * spot_factor;
            light_color += point_lights[i].color * intensity;
        } else {
            // Non-dynamic-lighting path (e.g. lightmapped / pre-lit geometry).
            // Emulates stock D3D8 behavior for dynamic lights on BSP faces,
            // which uses pure distance attenuation (no N·L). r_dynamiclightndotl
            // blends between stock (0.0) and full N·L (1.0).
            float ndotl = lerp(1.0f, ndotl_factor, dynamic_light_ndotl);
            intensity = atten * ndotl * spot_factor;
            light_color += point_lights[i].color * intensity;
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
        if (lum > 1.0f) {
            float range = pixel_light_overbright;
            float excess = lum - 1.0f;
            float compressed_lum = (range > 0.0f) ? 1.0f + excess * range / (excess + range) : 1.0f;
            light_color *= compressed_lum / lum;
        }
    }
    return light_color;
}

// Everything after the light is known: self-illumination, emissive override, caustics, entity
// shadows, liquid and fog, gas and colorblind. `norm` shades; input.norm only tells pre-transformed
// vertices apart.
float4 finish_fragment(VsOutput input, float4 target, float3 tex0_rgb, float3 light_color, float3 norm)
{
    // Self-illumination sets a minimum brightness floor (matches stock engine behavior).
    if (self_illumination > 0.0f) {
        light_color = max(light_color, self_illumination);
    }
    // Emissive override: render at pure texture brightness, ignoring vertex color
    // darkening and lighting. Used for monitor screens that should appear self-lit.
    if (emissive_override > 0.5f) {
        target.rgb = tex0_rgb * current_color.rgb;
        light_color = float3(1.0f, 1.0f, 1.0f);
    }

    if (num_caustic_volumes > 0 && emissive_override < 0.5f && disable_textures < 0.5f
        && sky_room < 0.5f && dot(input.norm, input.norm) > 0.0f) {
        float3 wp = input.world_pos_and_depth.xyz;
        float  mask = 0.0f, depth = 0.0f;
        float3 tint = float3(1, 1, 1);
        for (int ci = 0; ci < num_caustic_volumes; ++ci) {
            CausticVolume v = caustic_volumes[ci];
            // A room AABB routinely overshoots into dry neighbours, so prefer matching the
            // fragment's own room and fall back to the box only when the draw has no room.
            bool in_volume = draw_room_uid >= 0.0f
                ? abs(v.room_uid - draw_room_uid) < 0.5f
                : (all(wp >= v.bbox_min - 0.05f) && all(wp <= v.bbox_max + 0.05f));
            bool inside = in_volume && wp.y < v.surface_y - 0.02f;
            if (inside && mask == 0.0f) { mask = 1.0f; depth = v.surface_y - wp.y; tint = v.color; }
        }
        float  slice = frac(caustic_time * caustic_speed) * 16.0f;
        float  s0 = floor(slice), s1 = fmod(s0 + 1.0f, 16.0f), sf = slice - s0;
        float2 drift = caustic_time * caustic_drift * float2(1.0f, 0.7f);
        // Triplanar: XZ for floors/ceilings, ZY and XY for walls, with the wall
        // planes squeezed vertically so their pattern reads as elongated streaks.
        float3 tw = abs(norm);
        tw = pow(tw, 4.0f);
        tw /= max(tw.x + tw.y + tw.z, 1e-4f);
        float  wall_y = wp.y * caustic_wall_stretch;
        float2 p_xz = wp.xz;
        float2 p_zy = float2(wp.z, wall_y);
        float2 p_xy = float2(wp.x, wall_y);
        // Differentiate the interpolated world position itself rather than the derived plane
        // coordinates: the operand is then quad-valid in every lane even where the branch
        // below is divergent. caustic_wall_stretch is uniform, so this is the same value.
        float3 dwp_x = ddx(input.world_pos_and_depth.xyz);
        float3 dwp_y = ddy(input.world_pos_and_depth.xyz);
        float2 dxz_x = dwp_x.xz, dxz_y = dwp_y.xz;
        float2 dzy_x = float2(dwp_x.z, dwp_x.y * caustic_wall_stretch);
        float2 dzy_y = float2(dwp_y.z, dwp_y.y * caustic_wall_stretch);
        float2 dxy_x = float2(dwp_x.x, dwp_x.y * caustic_wall_stretch);
        float2 dxy_y = float2(dwp_y.x, dwp_y.y * caustic_wall_stretch);
        [branch] if (mask > 0.0f) {
            float  c_xz = caustic_sample_plane(p_xz, drift, s0, s1, sf, dxz_x, dxz_y);
            float  c_zy = caustic_sample_plane(p_zy, drift, s0, s1, sf, dzy_x, dzy_y);
            float  c_xy = caustic_sample_plane(p_xy, drift, s0, s1, sf, dxy_x, dxy_y);
            float  c_mix = c_xz * tw.y + c_zy * tw.x + c_xy * tw.z;
            float  c = saturate((c_mix - caustic_floor) / max(1.0f - caustic_floor, 0.001f));
            c = pow(c, caustic_exponent);
            float atten  = saturate(1.0f - depth / max(caustic_depth_fade, 0.01f));
            float facing = saturate(norm.y) * 0.75f + 0.25f;
            float lit    = saturate(dot(light_color, float3(0.2126f, 0.7152f, 0.0722f)) * 2.0f);
            light_color += tint * (c * caustic_intensity * caustic_above_water * atten * facing * lit);
        }
    }

    target.rgb *= light_color;

    if (shadow_enabled > 0.5f) {
        // Early-out: skip all shadow work for fragments beyond the fade distance
        float cam_dist = input.world_pos_and_depth.w;
        float fade = 1.0f - saturate((cam_dist - shadow_fade_start) / (shadow_fade_end - shadow_fade_start));

        if (fade > 0.0f) {
            float3 world_pos = input.world_pos_and_depth.xyz;
            float3 normal = normalize(norm);

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
    }

    target.rgb = saturate(target.rgb);

    // Detect pre-transformed vertices via dummy normal (transformed_vs outputs norm=(0,0,0)).
    // For those, world position has to be reconstructed from screen position and depth.
    bool is_pretransformed = dot(input.norm, input.norm) == 0.0f;
    bool can_reconstruct = is_pretransformed && input.world_pos_and_depth.w > 0.0f;

    [branch] if (liq_mode > 0.5f && (!is_pretransformed || can_reconstruct)) {
        float3 liq_pixel_pos = can_reconstruct
            ? reconstruct_world_pos(input.pos.xy, input.world_pos_and_depth.w, liq_eye_pos,
                                    liq_cam_right, liq_cam_up, liq_cam_forward,
                                    liq_proj_sx, liq_proj_sy,
                                    liq_viewport_x, liq_viewport_y, liq_viewport_w, liq_viewport_h)
            : input.world_pos_and_depth.xyz;

        // Lengths stay on the engine's view-depth metric so a fully dry fragment is fogged
        // exactly like stock; only the fraction comes from the geometric clip.
        float seg_len = max(input.world_pos_and_depth.w, 0.0f);
        float under_len;
        // Liquid surface pass only: the water column behind the fragment and the view depth where
        // that column ends, on the same metric as seg_len.
        float behind_len = 0.0f;
        float behind_end = 0.0f;

        if (sky_room > 0.5f) {
            // The sky room is drawn at its authored location with the camera translated into it,
            // so liq_pixel_pos means nothing here. Measure the real view ray against the surface.
            float2 ndc = screen_to_ndc(input.pos.xy, liq_viewport_x, liq_viewport_y,
                                       liq_viewport_w, liq_viewport_h);
            float3 dir = normalize(liq_cam_forward
                + liq_cam_right * (ndc.x / liq_proj_sx)
                + liq_cam_up * (ndc.y / liq_proj_sy));
            float under_depth = 0.0f;
            if (liq_eye_under > 0.5f) {
                float t_surf = dir.y > 1e-4f ? (liq_surface_y - liq_eye_pos.y) / dir.y : 1e9f;
                under_depth = t_surf * dot(dir, liq_cam_forward);
            }
            under_len = min(seg_len, max(under_depth, 0.0f));
        }
        else {
            // Clip the eye->pixel segment against every nearby liquid room of the camera room's
            // type, each capped at its own surface plane. Rooms do not overlap, so the segments
            // add. A single box would end the fogged length at the camera room's walls and step
            // at every boundary; without any box the surface plane would extend level-wide.
            float3 seg_vec = liq_pixel_pos - liq_eye_pos;
            float geo_len = length(seg_vec);
            float3 seg_dir = seg_vec / max(geo_len, 1e-6f);
            float3 dir_sign = step(float3(0, 0, 0), seg_dir) * 2.0f - 1.0f;
            float3 safe_dir = dir_sign * max(abs(seg_dir), float3(1e-8f, 1e-8f, 1e-8f));
            float3 inv_dir = 1.0f / safe_dir;

            float under_len_geo = 0.0f;
            int vol_count = min((int)liq_num_volumes, MAX_LIQUID_VOLUMES);
            for (int vi = 0; vi < vol_count; ++vi) {
                float3 t0 = (liq_volumes[vi].bbox_min - liq_eye_pos) * inv_dir;
                float3 t1 = (liq_volumes[vi].bbox_max - liq_eye_pos) * inv_dir;
                float3 tmin_v = min(t0, t1);
                float3 tmax_v = max(t0, t1);
                float t_enter = max(max(tmin_v.x, tmin_v.y), max(tmin_v.z, 0.0f));
                float t_exit = min(min(tmax_v.x, tmax_v.y), min(tmax_v.z, geo_len));
                under_len_geo += max(t_exit - t_enter, 0.0f);
            }
            under_len = seg_len * saturate(under_len_geo / max(geo_len, 1e-6f));

            // A liquid surface seen from above stands in front of water that nothing else draws:
            // past the far clip, past the rooms the engine culled, past the volume list. Walk the
            // same slabs on from the fragment to the far clip so the surface can carry it. The
            // volumes are room boxes and know nothing about what is inside them, so the walk stops
            // at the scene depth as well: a pool floor a few metres down ends the column there
            // instead of letting the box run on and turn the surface opaque over it.
            [branch] if (liquid_surface > 0.5f && liq_eye_under < 0.5f) {
                float scene_dist = 1e9f;
                [branch] if (liq_depth_mode > 0.5f) {
                    float device_depth = scene_depth.Load(int3(int2(input.pos.xy), 0));
                    // Reversed-Z: an untouched pixel reads 0 and unprojects to the far plane
                    scene_dist = liq_depth_tz / max(device_depth - liq_depth_sz, 1e-6f);
                }
                float view_per_geo = seg_len / max(geo_len, 1e-6f);
                float geo_end = min(max(liq_far_clip, 1.0f), scene_dist) / max(view_per_geo, 1e-4f);
                float behind_geo = 0.0f;
                float end_geo = 0.0f;
                for (int bi = 0; bi < vol_count; ++bi) {
                    float3 bt0 = (liq_volumes[bi].bbox_min - liq_eye_pos) * inv_dir;
                    float3 bt1 = (liq_volumes[bi].bbox_max - liq_eye_pos) * inv_dir;
                    float3 btmin_v = min(bt0, bt1);
                    float3 btmax_v = max(bt0, bt1);
                    float bt_enter = max(max(btmin_v.x, btmin_v.y), max(btmin_v.z, geo_len));
                    float bt_exit = min(min(btmax_v.x, btmax_v.y), min(btmax_v.z, geo_end));
                    float bt_len = max(bt_exit - bt_enter, 0.0f);
                    behind_geo += bt_len;
                    end_geo = max(end_geo, bt_len > 0.0f ? bt_exit : 0.0f);
                }
                behind_len = behind_geo * view_per_geo;
                behind_end = end_geo * view_per_geo;
            }
        }
        float over_len = seg_len - under_len;

        // Above-surface part: stock linear fog. While submerged the level fog has been replaced
        // by the liquid fog in b0, so the level values come from b6 instead.
        float3 over_color = liq_eye_under > 0.5f ? liq_over_fog_color : fog_color;
        float over_far = liq_eye_under > 0.5f ? liq_over_fog_far : fog_far;
        float over_fog = over_far < 1e30f ? saturate(over_len / over_far) : 0.0f;

        // Below-surface part: per-channel Beer-Lambert, dominant channels of the liquid color
        // surviving longest, in-scatter darkened with depth below the surface.
        float sigma = liq_params.x / max(liq_visibility, 1.0f);
        float3 sigma_rgb = sigma * lerp(liq_params.yyy, liq_params.zzz, saturate(liq_color));
        float3 transmittance = exp(-under_len * sigma_rgb);

        // Converge to the in-scatter colour before the engine's frustum plane cuts the geometry,
        // so the far clip reads as fog rather than an edge. The background rect behind it is set
        // to the same colour. Above water the far plane behaves as stock.
        if (liq_eye_under > 0.5f) {
            float fade_far = max(liq_far_clip, 1.0f);
            transmittance *= 1.0f - smoothstep(0.7f * fade_far, 0.97f * fade_far, seg_len);
        }
        else if (over_far < 1e30f) {
            // Dry, the background behind the clip is the level fog colour, but a pixel that got
            // here mostly through water accrued almost no over_len and never reaches it. Bring the
            // water-derived part of the pixel to the background before the cut; a dry pixel has
            // none and is left exactly as stock.
            float fade_far = max(liq_far_clip, 1.0f);
            float water_frac = 1.0f - exp(-under_len * sigma);
            over_fog = max(over_fog, water_frac * smoothstep(0.7f * fade_far, 0.97f * fade_far, seg_len));
        }

        float pix_y = sky_room > 0.5f ? liq_eye_pos.y : liq_pixel_pos.y;
        float depth_below = max(liq_dark_surface_y - min(liq_eye_pos.y, pix_y), 0.0f);
        float3 inscatter = liq_color * exp(-depth_below * liq_params.w);

        // Distance opacity for the column the surface carries. It stays out of the way until the
        // column has taken half the light on its own, so shallow and near water keeps its authored
        // alpha, and goes to full where the column is still running at the far clip - the one place
        // the cut edge can show. Authored alpha is the floor: this composites the column behind the
        // surface over it, so opacity only ever climbs. The length term needs the scene depth to be
        // honest about where the column ends, so without it only the far-clip term survives.
        [branch] if (liquid_surface > 0.5f && behind_len > 0.0f) {
            float fade_far = max(liq_far_clip, 1.0f);
            float column = liq_depth_mode > 0.5f ? 1.0f - exp(-behind_len * sigma) : 0.0f;
            float murk = max(saturate((column - 0.5f) * 2.0f),
                             smoothstep(0.7f * fade_far, 0.97f * fade_far, behind_end));
            float src_a = target.a;
            float out_a = src_a + (1.0f - src_a) * murk;
            target.rgb = (target.rgb * src_a + inscatter * (murk * (1.0f - src_a))) / max(out_a, 1e-4f);
            target.a = out_a;
        }

        // Dimming always applies so anything behind liquid reads as occluded; color is only
        // added where the draw mode allows fog, as in the gas block below.
        bool add_scatter = gas_fog_allowed > 0.5f;
        float3 over_t = float3(1.0f, 1.0f, 1.0f) * (1.0f - over_fog);
        if (liq_eye_under > 0.5f) {
            target.rgb = apply_medium(target.rgb, over_t, over_color, add_scatter);
            target.rgb = apply_medium(target.rgb, transmittance, inscatter, add_scatter);
        }
        else {
            target.rgb = apply_medium(target.rgb, transmittance, inscatter, add_scatter);
            target.rgb = apply_medium(target.rgb, over_t, over_color, add_scatter);
        }
    }
    else {
        float fog = saturate(input.world_pos_and_depth.w / fog_far);
        target.rgb = fog * fog_color + (1 - fog) * target.rgb;
    }

    // Gas region volumetric fog — per-region compositing (Beer-Lambert)
    // Transmittance (dimming) is always applied so sprites behind gas appear occluded.
    // Gas color accumulation is only added when fog is allowed, to avoid artifacts with
    // additive blending (e.g. muzzle flash) where the background already contains the gas color.
#if MAX_GAS_REGIONS > 0
    float3 gas_world_pos = input.world_pos_and_depth.xyz;
    int gas_count = min(num_gas_regions, MAX_GAS_REGIONS);
    if (gas_count > 0 && (!is_pretransformed || can_reconstruct)) {
        // Reconstruct world position for pre-transformed vertices (dynamic decals, etc.)
        if (can_reconstruct) {
            gas_world_pos = reconstruct_world_pos(input.pos.xy, input.world_pos_and_depth.w, gas_eye_pos,
                                                  gas_cam_right, gas_cam_up, gas_cam_forward,
                                                  gas_proj_sx, gas_proj_sy,
                                                  gas_viewport_x, gas_viewport_y,
                                                  gas_viewport_w, gas_viewport_h);
        }
        float3 ray_origin = gas_eye_pos;
        float3 to_pixel = gas_world_pos - ray_origin;
        float ray_len = length(to_pixel);
        float3 ray_dir = to_pixel / max(ray_len, 0.0001f);

        // Composite each region independently via Beer-Lambert extinction.
        // For overlapping regions this is physically correct (optical depths add).
        float3 gas_accumulated = float3(0, 0, 0);
        float gas_transmittance = 1.0f;

        for (int gi = 0; gi < gas_count; gi++) {
            float t_enter, t_exit;
            bool hit = false;

            if (gas_regions[gi].shape < 0.5f) {
                // Sphere: analytical ray-sphere intersection
                float3 oc = ray_origin - gas_regions[gi].center;
                float r = gas_regions[gi].extents.x;
                float b = dot(oc, ray_dir);
                float c = dot(oc, oc) - r * r;
                float disc = b * b - c;
                if (disc > 0.0f) {
                    float sq = sqrt(disc);
                    t_enter = max(-b - sq, 0.0f);
                    t_exit = min(-b + sq, ray_len);
                    hit = (t_exit > t_enter);
                }
            } else {
                // OBB: transform ray to local space, then ray-AABB
                float3 delta = ray_origin - gas_regions[gi].center;
                float3 local_origin = float3(
                    dot(delta, gas_regions[gi].orient_r0),
                    dot(delta, gas_regions[gi].orient_r1),
                    dot(delta, gas_regions[gi].orient_r2));
                float3 local_dir = float3(
                    dot(ray_dir, gas_regions[gi].orient_r0),
                    dot(ray_dir, gas_regions[gi].orient_r1),
                    dot(ray_dir, gas_regions[gi].orient_r2));
                float3 dir_sign = (local_dir >= 0.0f) ? 1.0f : -1.0f;
                local_dir = dir_sign * max(abs(local_dir), 1e-8f);
                float3 inv_dir = 1.0f / local_dir;
                float3 t0 = (-gas_regions[gi].extents - local_origin) * inv_dir;
                float3 t1 = ( gas_regions[gi].extents - local_origin) * inv_dir;
                float3 tmin_v = min(t0, t1);
                float3 tmax_v = max(t0, t1);
                t_enter = max(max(tmin_v.x, tmin_v.y), max(tmin_v.z, 0.0f));
                t_exit = min(min(tmax_v.x, tmax_v.y), min(tmax_v.z, ray_len));
                hit = (t_exit > t_enter);
            }

            if (hit) {
                float seg_len = t_exit - t_enter;
                float seg_t = exp(-gas_regions[gi].density * seg_len);
                gas_accumulated += gas_transmittance * gas_regions[gi].color * (1.0f - seg_t);
                gas_transmittance *= seg_t;
            }
        }

        target.rgb = target.rgb * gas_transmittance;
        if (!can_reconstruct || gas_fog_allowed > 0.5f) {
            target.rgb += gas_accumulated;
        }
    }
#endif

    if (colorblind_mode > 0.5f) {
        target.rgb = saturate(apply_colorblind(target.rgb));
    }

    return target;
}

#ifndef TERRAIN

float4 main(VsOutput input) : SV_TARGET
{
    float2 scaled_uv0 = input.uv0 * tex0_uv_scale;
    float4 tex0_color = disable_textures > 0.5f ? float4(1.0, 1.0, 1.0, 1.0) : tex0.Sample(samp0, scaled_uv0);
    float4 target = input.color * tex0_color * current_color;

    clip(target.a - alpha_test);

    float3 light_color;
#ifdef INSTANCE_LIGHT
    // Per instance, what the mesh path uploads per mesh as ambient_light and sun_scale
    light_color = input.inst_light.rgb + sun_color * input.inst_light.a * saturate(dot(input.norm, -sun_travel_dir));
    if (disable_textures < 0.5f) {
#else
    // The alpine branch is taken before the *2 modulate below, so show_lightmaps still emits the
    // raw atlas texel exactly as it emits the raw stock texel.
    [branch] if (af_lm_enabled > 0.5f && input.uv1.z >= 0.0f) {
        light_color = af_lm_sample(input.uv1.xy, (uint)(input.uv1.z + 0.5f));
    } else {
        light_color = tex1.Sample(samp1, input.uv1.xy).rgb;
    }
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
#endif
        light_color = add_scene_lights(light_color, input.world_pos_and_depth.xyz, input.norm);
    }
    return finish_fragment(input, target, tex0_color.rgb, light_color, input.norm);
}

#else

// Terrain chunk faces (-DTERRAIN): layers blended by the weight maps with overlays over them,
// heightmap normals, and the same light chain as every other solid face. Registers and the cbuffer
// layout are owned by gr_d3d11_terrain.cpp (TerrainBufferData).
cbuffer TerrainBuffer : register(b7)
{
    float3 ter_origin;          float ter_cell_size;
    float2 ter_extent;          float ter_height_min;       float ter_height_range;
    float2 ter_grid_size;       float ter_layer_count;      float ter_underside_uv_scale;
    float3 ter_sun_travel_dir;  float ter_debug;
    float3 ter_sun_color;       float ter_lm_chart;         // af_lm_index chart record, < 0: none
    float4 ter_layer_uv_scale[2];
    float4 ter_layer_triplanar[2];
    float2 ter_lm_origin;       float ter_lm_texel_size;    float ter_overlay_count;
    float4 ter_overlay_uv_scale;
    float4 ter_overlay_triplanar;
    float4 ter_overlay_break_tiling;
    float4 ter_overlay_enabled;
    float4 ter_overlay_premultiplied;   // 1: the texture holds premultiplied colour
    float  ter_fullbright;              // 1: lit as a stock fullbright face
};

Texture2D        ter_weights0   : register(t7);
Texture2D        ter_weights1   : register(t8);
Texture2D<float> ter_height     : register(t9);
Texture2D        ter_layer0     : register(t10);
Texture2D        ter_layer1     : register(t11);
Texture2D        ter_layer2     : register(t12);
Texture2D        ter_layer3     : register(t13);
Texture2D        ter_layer4     : register(t14);
Texture2D        ter_layer5     : register(t15);
Texture2D        ter_layer6     : register(t16);
Texture2D        ter_layer7     : register(t17);
Texture2D        ter_underside  : register(t18);
Texture2D        ter_crater     : register(t19);
Texture2D        ter_overlay0   : register(t20);
Texture2D        ter_overlay1   : register(t21);
Texture2D        ter_overlay2   : register(t22);
Texture2D        ter_overlay3   : register(t23);
Texture2D        ter_overlay_coverage : register(t24);
SamplerState     ter_layer_samp : register(s6);
SamplerState     ter_map_samp   : register(s7);

// Texture space of one layer: u = x / uv_scale, v = z / uv_scale as the emitted faces' layer_uv,
// taken relative to the origin so large coordinates keep their precision; the origin's own
// fraction keeps the repeat where the faces have it.
struct TerCoords
{
    float3 p;
    float3 gx;
    float3 gy;
};

TerCoords ter_coords(float3 wp, float3 dwx, float3 dwy, float uv_scale)
{
    float inv = 1.0f / uv_scale;
    TerCoords tc;
    tc.p = (wp - ter_origin) * inv + frac(ter_origin * inv);
    tc.gx = dwx * inv;
    tc.gy = dwy * inv;
    return tc;
}

float3 ter_triplanar_weights(float3 n)
{
    float3 w = pow(abs(n), 4.0f);
    return w / max(w.x + w.y + w.z, 1e-4f);
}

// One texture in a layer's space: planar XZ, and the two wall planes stood upright as wall_uv lays
// them. Explicit gradients, so a layer without weight can be skipped per pixel. Macros because
// texture parameters are not portable to every HLSL compiler the build uses.
#define TER_PLANAR(tex, tc) tex.SampleGrad(ter_layer_samp, tc.p.xz, tc.gx.xz, tc.gy.xz).rgb
#define TER_SIDES(tex, tc, tw)                                                                     \
    (tex.SampleGrad(ter_layer_samp, float2(tc.p.z, -tc.p.y), float2(tc.gx.z, -tc.gx.y),            \
                    float2(tc.gy.z, -tc.gy.y)).rgb * tw.x                                          \
   + tex.SampleGrad(ter_layer_samp, float2(tc.p.x, -tc.p.y), float2(tc.gx.x, -tc.gx.y),            \
                    float2(tc.gy.x, -tc.gy.y)).rgb * tw.z)

// Adds `w` of one layer to `c`, triplanar when the layer asks for it.
#define TER_ADD(tex, w, uv_scale, triplanar)                                                       \
    [branch] if ((w) > 0.0f) {                                                                     \
        TerCoords tc = ter_coords(wp, dwx, dwy, (uv_scale));                                       \
        float3 s = TER_PLANAR(tex, tc);                                                            \
        if ((triplanar) > 0.5f) {                                                                  \
            s = s * tw.y + TER_SIDES(tex, tc, tw);                                                 \
        }                                                                                          \
        c += (w) * s;                                                                              \
    }

float3 ter_albedo(float3 wp, float3 dwx, float3 dwy, float3 tw)
{
    float2 map_uv = (wp.xz - ter_origin.xz) / ter_extent;
    // Only the terrain's own layers count; with no weight left, layer 0 is the base.
    float4 w0 = ter_weights0.SampleLevel(ter_map_samp, map_uv, 0)
              * step(float4(0.5f, 1.5f, 2.5f, 3.5f), ter_layer_count.xxxx);
    float4 w1 = ter_weights1.SampleLevel(ter_map_samp, map_uv, 0)
              * step(float4(4.5f, 5.5f, 6.5f, 7.5f), ter_layer_count.xxxx);
    float sum = dot(w0, float4(1.0f, 1.0f, 1.0f, 1.0f)) + dot(w1, float4(1.0f, 1.0f, 1.0f, 1.0f));
    if (sum < 1e-4f) {
        w0 = float4(1.0f, 0.0f, 0.0f, 0.0f);
        w1 = float4(0.0f, 0.0f, 0.0f, 0.0f);
        sum = 1.0f;
    }
    w0 /= sum;
    w1 /= sum;

    float3 c = float3(0.0f, 0.0f, 0.0f);
    TER_ADD(ter_layer0, w0.x, ter_layer_uv_scale[0].x, ter_layer_triplanar[0].x)
    TER_ADD(ter_layer1, w0.y, ter_layer_uv_scale[0].y, ter_layer_triplanar[0].y)
    TER_ADD(ter_layer2, w0.z, ter_layer_uv_scale[0].z, ter_layer_triplanar[0].z)
    TER_ADD(ter_layer3, w0.w, ter_layer_uv_scale[0].w, ter_layer_triplanar[0].w)
    TER_ADD(ter_layer4, w1.x, ter_layer_uv_scale[1].x, ter_layer_triplanar[1].x)
    TER_ADD(ter_layer5, w1.y, ter_layer_uv_scale[1].y, ter_layer_triplanar[1].y)
    TER_ADD(ter_layer6, w1.z, ter_layer_uv_scale[1].z, ter_layer_triplanar[1].z)
    TER_ADD(ter_layer7, w1.w, ter_layer_uv_scale[1].w, ter_layer_triplanar[1].w)
    return c;
}

// Always triplanar, at its own scale (layer 0's, as the faces were mapped)
float3 ter_underside_albedo(float3 wp, float3 dwx, float3 dwy, float3 tw)
{
    TerCoords tc = ter_coords(wp, dwx, dwy, ter_underside_uv_scale);
    return TER_PLANAR(ter_underside, tc) * tw.y + TER_SIDES(ter_underside, tc, tw);
}

// Overlays, each over the result so far in order: albedo = lerp(albedo, rgb, a * coverage), worked
// premultiplied. The textures are premultiplied per texel with their own mips (gr_d3d11_terrain.cpp),
// so filtering never mixes in the colour of transparent texels; a texture without that copy is
// premultiplied after filtering, which darkens its alpha edges and its distant mips.
float4 ter_premul(float4 c)
{
    return float4(c.rgb * c.a, c.a);
}

uint ter_hash(uint h)
{
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return h;
}

// Break-up tiling (hex tiling, Mikkelsen 2022): hexagons one texture repeat apart, centred on the
// vertices of a triangle lattice, each showing the texture turned and shifted by a hash of its vertex.
// A point blends the three hexagons of its lattice triangle by barycentric weights, sharpened; a weight
// is 0 on the edge opposite its vertex, so the blend is continuous across every triangle. A hexagon's
// UVs are an affine map of the input's, so its gradients are the input's rotated and mips stay right.
struct TerHex
{
    float2 base;  // lattice cell, skewed
    float upper;  // 1 in the cell's upper triangle
    float3 w;     // the triangle's vertex weights
};

TerHex ter_hex(float2 st)
{
    float2 skew = float2(st.x - 0.57735027f * st.y, 1.15470054f * st.y);
    TerHex h;
    h.base = floor(skew);
    float2 f = skew - h.base;
    float z = 1.0f - f.x - f.y;
    h.upper = z < 0.0f ? 1.0f : 0.0f;
    float sgn = 2.0f * h.upper - 1.0f;
    float3 w = float3(-z * sgn, h.upper - f.y * sgn, h.upper - f.x * sgn);
    // w^8: each hexagon shows its own texture unblended over most of its area
    w *= w;
    w *= w;
    w *= w;
    h.w = w / (w.x + w.y + w.z);
    return h;
}

// Moves `st` and its gradients into the hexagon of vertex `i` (0-2) of the triangle; returns its weight.
float ter_hex_cell(TerHex h, int i, inout float2 st, inout float2 gx, inout float2 gy)
{
    float u = h.upper;
    float2 vertex = h.base + (i == 0 ? float2(u, u) : i == 1 ? float2(u, 1.0f - u) : float2(1.0f - u, u));
    int2 v = int2(vertex);
    uint k = ter_hash((uint)v.x ^ ter_hash((uint)v.y + 0x9e3779b9u));
    uint k1 = ter_hash(k + 0x632be5abu);
    uint k2 = ter_hash(k1 + 0x85157af5u);
    float a = float(k) * (6.28318531f / 4294967296.0f);
    float s = sin(a), c = cos(a);
    float2x2 rot = float2x2(c, -s, s, c);
    // The vertex in texture space: the inverse of ter_hex's skew
    float2 centre = float2(vertex.x + 0.5f * vertex.y, 0.8660254f * vertex.y);
    st = mul(st - centre, rot) + float2(float(k1), float(k2)) * (1.0f / 4294967296.0f);
    gx = mul(gx, rot);
    gy = mul(gy, rot);
    return i == 0 ? h.w.x : i == 1 ? h.w.y : h.w.z;
}

// Projection `k` of a layer's texture space as TER_PLANAR (0) and TER_SIDES (1, 2) lay them; returns
// its weight.
float ter_projection(TerCoords tc, float3 tw, int k, bool triplanar, out float2 st, out float2 gx, out float2 gy)
{
    st = k == 0 ? tc.p.xz : float2(k == 1 ? tc.p.z : tc.p.x, -tc.p.y);
    gx = k == 0 ? tc.gx.xz : float2(k == 1 ? tc.gx.z : tc.gx.x, -tc.gx.y);
    gy = k == 0 ? tc.gy.xz : float2(k == 1 ? tc.gy.z : tc.gy.x, -tc.gy.y);
    return k == 0 ? (triplanar ? tw.y : 1.0f) : k == 1 ? tw.x : tw.z;
}

// Triplanar projections under this weight are skipped (below an 8-bit step).
static const float ter_projection_min = 1.0f / 512.0f;

// Lays one overlay over `albedo` by coverage `cov`: per projection, one tap or three hex-tiled ones.
#define TER_OVERLAY(tex, cov, uv_scale, triplanar, break_tiling, premultiplied)                    \
    [branch] if ((cov) > 0.0f) {                                                                   \
        TerCoords tc = ter_coords(wp, dwx, dwy, (uv_scale));                                       \
        bool tri_ = (triplanar) > 0.5f;                                                            \
        bool pre_ = (premultiplied) > 0.5f;                                                        \
        int cells_ = (break_tiling) > 0.5f ? 3 : 1;                                                \
        float4 s = float4(0.0f, 0.0f, 0.0f, 0.0f);                                                 \
        for (int k_ = 0; k_ < (tri_ ? 3 : 1); k_++) {                                              \
            float2 st_, gx_, gy_;                                                                  \
            float wk_ = ter_projection(tc, tw, k_, tri_, st_, gx_, gy_);                           \
            [branch] if (wk_ > ter_projection_min) {                                               \
                TerHex hx_ = ter_hex(st_);                                                         \
                for (int i_ = 0; i_ < cells_; i_++) {                                              \
                    float2 uv_ = st_, cgx_ = gx_, cgy_ = gy_;                                      \
                    float wi_ = 1.0f;                                                              \
                    if (cells_ > 1) {                                                              \
                        wi_ = ter_hex_cell(hx_, i_, uv_, cgx_, cgy_);                              \
                    }                                                                              \
                    float4 c_ = tex.SampleGrad(ter_layer_samp, uv_, cgx_, cgy_);                   \
                    s += (pre_ ? c_ : ter_premul(c_)) * (wk_ * wi_);                               \
                }                                                                                  \
            }                                                                                      \
        }                                                                                          \
        albedo = albedo * (1.0f - s.a * (cov)) + s.rgb * (cov);                                    \
    }

float3 ter_overlays(float3 albedo, float3 wp, float3 dwx, float3 dwy, float3 tw)
{
    [branch] if (ter_overlay_count > 0.5f) {
        float2 map_uv = (wp.xz - ter_origin.xz) / ter_extent;
        float4 cov = ter_overlay_coverage.SampleLevel(ter_map_samp, map_uv, 0) * ter_overlay_enabled;
        TER_OVERLAY(ter_overlay0, cov.x, ter_overlay_uv_scale.x, ter_overlay_triplanar.x,
                    ter_overlay_break_tiling.x, ter_overlay_premultiplied.x)
        TER_OVERLAY(ter_overlay1, cov.y, ter_overlay_uv_scale.y, ter_overlay_triplanar.y,
                    ter_overlay_break_tiling.y, ter_overlay_premultiplied.y)
        TER_OVERLAY(ter_overlay2, cov.z, ter_overlay_uv_scale.z, ter_overlay_triplanar.z,
                    ter_overlay_break_tiling.z, ter_overlay_premultiplied.z)
        TER_OVERLAY(ter_overlay3, cov.w, ter_overlay_uv_scale.w, ter_overlay_triplanar.w,
                    ter_overlay_break_tiling.w, ter_overlay_premultiplied.w)
    }
    return albedo;
}

// Central differences of the heightmap in world units. Vertex (x, z) is texel (x, z), sampled at
// its centre, so between vertices the slope is the bilinear one and never steps at a face edge.
// The samples stay on the grid, one-sided at the border.
float3 ter_normal(float3 wp)
{
    float2 texel = 1.0f / ter_grid_size;
    float2 t = (wp.xz - ter_origin.xz) / ter_cell_size + 0.5f;
    float2 lo = max(t - 1.0f, 0.5f);
    float2 hi = min(t + 1.0f, ter_grid_size - 0.5f);
    float hl = ter_height.SampleLevel(ter_map_samp, float2(lo.x, t.y) * texel, 0);
    float hr = ter_height.SampleLevel(ter_map_samp, float2(hi.x, t.y) * texel, 0);
    float hd = ter_height.SampleLevel(ter_map_samp, float2(t.x, lo.y) * texel, 0);
    float hu = ter_height.SampleLevel(ter_map_samp, float2(t.x, hi.y) * texel, 0);
    float2 k = ter_height_range / (max(hi - lo, 1e-3f) * ter_cell_size);
    return normalize(float3((hl - hr) * k.x, 1.0f, (hd - hu) * k.y));
}

// The original surface's height at wp.xz, from the same clamped bilinear reads (alpine_terrain.h
// surface_y_bilinear).
float ter_surface_y(float3 wp)
{
    float2 t = (wp.xz - ter_origin.xz) / ter_cell_size + 0.5f;
    return ter_origin.y + ter_height_min + ter_height.SampleLevel(ter_map_samp, t / ter_grid_size, 0) * ter_height_range;
}

// A crater fragment's dimming by its depth under the original surface (alpine_terrain.h
// crater_light_factor: linear to 0.35 at 6 units).
float ter_crater_factor(float3 wp)
{
    float depth = ter_surface_y(wp) - wp.y;
    return 1.0f - (1.0f - 0.35f) * saturate(depth / 6.0f);
}

// A terrain fragment's base light: the baked chart at its XZ, doubled like a lightmap texel; with no
// chart or on the underside, the level ambient plus the sun's N.L.
float3 ter_base_light(float3 wp, float3 n, bool underside)
{
    float3 light;
    [branch] if (ter_lm_chart >= 0.0f && af_lm_enabled > 0.5f && !underside) {
        light = af_lm_sample((wp.xz - ter_lm_origin) / ter_lm_texel_size, (uint)(ter_lm_chart + 0.5f));
        if (disable_textures < 0.5f) {
            light *= 2.0f;
        }
    } else {
        light = ambient_light + ter_sun_color * saturate(dot(n, -ter_sun_travel_dir));
    }
    return light;
}

float4 main(VsOutput input) : SV_TARGET
{
    float3 wp = input.world_pos_and_depth.xyz;
    float3 dwx = ddx(wp);
    float3 dwy = ddy(wp);
    float2 duvx = ddx(input.uv0);
    float2 duvy = ddy(input.uv0);
    // uv1.z is the face kind: 0 top, 1 underside (skirts, walls and bottoms), 2 crater
    bool crater = input.uv1.z > 1.5f;
    bool underside = !crater && input.uv1.z > 0.5f;
    float3 n = (underside || crater) ? normalize(input.norm) : ter_normal(wp);
    float3 tw = ter_triplanar_weights(n);

    float3 albedo = float3(1.0f, 1.0f, 1.0f);
    if (disable_textures < 0.5f) {
        [branch] if (crater) {
            // The texture and planar UVs the carve gave the face
            albedo = ter_crater.SampleGrad(ter_layer_samp, input.uv0, duvx, duvy).rgb;
        } else if (underside) {
            albedo = ter_underside_albedo(wp, dwx, dwy, tw);
        } else {
            albedo = ter_overlays(ter_albedo(wp, dwx, dwy, tw), wp, dwx, dwy, tw);
        }
    }
    if (ter_debug > 0.5f) {
        float3 tint = crater ? float3(0.2f, 0.2f, 1.0f) : underside ? float3(1.0f, 0.2f, 0.2f) : float3(0.2f, 1.0f, 0.2f);
        albedo = lerp(albedo, tint, 0.5f);
    }
    float4 target = float4(albedo, 1.0f) * input.color * current_color;

    // Craters take the top's light at their XZ, dimmed by depth. A fullbright terrain has a stock
    // fullbright face's light: no lightmap, the neutral texel doubled to 1 (raw when lightmaps are shown).
    float3 light_color;
    [branch] if (ter_fullbright > 0.5f) {
        light_color = disable_textures < 0.5f ? 1.0f : 0.5f;
    } else {
        light_color = ter_base_light(wp, n, underside);
        if (crater) {
            light_color *= ter_crater_factor(wp);
        }
    }
    if (disable_textures < 0.5f) {
        light_color = add_scene_lights(light_color, wp, n);
    }
    return finish_fragment(input, target, albedo, light_color, n);
}

#endif
