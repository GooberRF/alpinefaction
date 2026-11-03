cbuffer SceneFxBuffer : register(b0)
{
    float2 rt_size;      float time;         float flags;
    float4 tint;
    float4 vignette;
    float4 damage_edges;                     // top, left, bottom, right
    float4 damage;                           // rgb + radial strength
    float  distort_amp;  float distort_freq; float distort_speed; float _pad0;
    float3 eye_pos;      float surface_y;
    float3 cam_right;    float proj_sx;
    float3 cam_up;       float proj_sy;
    float3 cam_fwd;      float near_dist;
    float4 viewport_rect;                    // min.xy, max.xy in render-target pixels
    float4 scope_glass;                      // distortion, rim distortion, dispersion, vignette strength
    float4 scope_rim;                        // rim start, rim end, vignette start, fringe
};

Texture2D scene_texture : register(t0);
SamplerState scene_sampler : register(s0);

static const uint FLAG_DISTORT = 1u;
static const uint FLAG_LIQUID_TINT = 2u;
static const uint FLAG_LIQUID_VIGNETTE = 4u;
static const uint FLAG_DAMAGE = 8u;
static const uint FLAG_SCOPE_GLASS = 16u;

static const float waterline_band = 0.02f;

// Straight-alpha "over". The overlay path has no scene sample to lerp against, so its
// layers accumulate into a color + alpha the blend state composites onto the frame.
void blend_over(inout float3 acc_rgb, inout float acc_a, float3 src_rgb, float src_a)
{
    float out_a = src_a + acc_a * (1.0f - src_a);
    acc_rgb = out_a > 1e-5f
        ? (src_rgb * src_a + acc_rgb * acc_a * (1.0f - src_a)) / out_a
        : acc_rgb;
    acc_a = out_a;
}

float3 sample_scope_channel(float2 center, float2 offset, float scale)
{
    float2 sample_px = clamp(center + offset * scale, viewport_rect.xy, viewport_rect.zw);
    return scene_texture.Sample(scene_sampler, sample_px / rt_size).rgb;
}

// Scope eyepiece: pincushion distortion grows from zero on the optical axis and steepens where
// the glass curves towards its rim, red and blue split from green, and the image darkens towards
// the field stop. One mapping covers the whole screen, so there is no seam for the stock scope
// ring to hide. Red reads outwards only where the constant fringe exceeds the bend, near the
// centre, so no sample lands past the edge of the frame.
float3 scope_glass_color(float2 pos)
{
    float2 center = (viewport_rect.xy + viewport_rect.zw) * 0.5f;
    float half_height = max((viewport_rect.w - viewport_rect.y) * 0.5f, 1.0f);
    float2 offset = pos - center;
    // Half viewport heights, so the lens stays round at any aspect ratio
    float r = length(offset) / half_height;
    float rim = smoothstep(scope_rim.x, scope_rim.y, r);
    float bend = scope_glass.x * r * r + scope_glass.y * rim * rim;
    float fringe = scope_rim.w + scope_glass.z * bend;
    float3 col;
    col.r = sample_scope_channel(center, offset, 1.0f - bend + fringe).r;
    col.g = sample_scope_channel(center, offset, 1.0f - bend).g;
    col.b = sample_scope_channel(center, offset, 1.0f - bend - fringe).b;
    float falloff = smoothstep(scope_rim.z, 1.0f, r);
    return col * (1.0f - scope_glass.w * falloff * falloff);
}

float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET
{
    uint fl = (uint)flags;

    if ((fl & FLAG_SCOPE_GLASS) != 0u) {
        return float4(scope_glass_color(pos.xy), 1.0f);
    }

    // Near-plane point of this pixel's view ray, same reconstruction the standard PS uses.
    // The split it produces matches the fog split, so the waterline is continuous.
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float3 ray = cam_fwd + cam_right * (ndc.x / proj_sx) + cam_up * (ndc.y / proj_sy);
    float3 near_point = eye_pos + ray * near_dist;
    float under = smoothstep(surface_y + waterline_band, surface_y - waterline_band, near_point.y);

    // Distance from screen center, normalised so the corners are 1
    float2 centered = (uv - 0.5f) * 2.0f;
    float radial = smoothstep(0.35f, 1.0f, length(centered) * 0.70710678f);

    float damage_mask = 0.0f;
    if ((fl & FLAG_DAMAGE) != 0u) {
        // Every band reaches 0 at mid-screen, and the side bands are scaled by the viewport
        // aspect so they are the same width in pixels as the top and bottom ones.
        float2 vp = viewport_rect.zw - viewport_rect.xy;
        float band_x = min(0.5f * max(vp.y, 1.0f) / max(vp.x, 1.0f), 0.5f);
        damage_mask = saturate(
            damage_edges.x * smoothstep(0.5f, 0.0f, uv.y) +
            damage_edges.y * smoothstep(band_x, 0.0f, uv.x) +
            damage_edges.z * smoothstep(0.5f, 1.0f, uv.y) +
            damage_edges.w * smoothstep(1.0f - band_x, 1.0f, uv.x) +
            damage.a * radial) * 0.55f;
    }

    if ((fl & FLAG_DISTORT) != 0u) {
        float2 offset = under * distort_amp * float2(
            sin(uv.y * distort_freq + time * distort_speed),
            sin(uv.x * distort_freq * 0.8f + time * distort_speed * 1.1f));
        float2 vp_size = viewport_rect.zw - viewport_rect.xy;
        float2 sample_px = clamp(pos.xy + offset * vp_size, viewport_rect.xy, viewport_rect.zw);
        float3 col = scene_texture.Sample(scene_sampler, sample_px / rt_size).rgb;
        if ((fl & FLAG_LIQUID_TINT) != 0u) {
            col = lerp(col, tint.rgb, tint.a * under);
        }
        if ((fl & FLAG_LIQUID_VIGNETTE) != 0u) {
            col = lerp(col, vignette.rgb, vignette.a * under * radial);
        }
        col = lerp(col, damage.rgb, damage_mask);
        return float4(col, 1.0f);
    }

    float3 acc_rgb = float3(0.0f, 0.0f, 0.0f);
    float acc_a = 0.0f;
    if ((fl & FLAG_LIQUID_TINT) != 0u) {
        blend_over(acc_rgb, acc_a, tint.rgb, tint.a * under);
    }
    if ((fl & FLAG_LIQUID_VIGNETTE) != 0u) {
        blend_over(acc_rgb, acc_a, vignette.rgb, vignette.a * under * radial);
    }
    blend_over(acc_rgb, acc_a, damage.rgb, damage_mask);
    return float4(acc_rgb, acc_a);
}
