// Particle quads expanded from one instance record each, with the exact math of the stock CPU paths the
// D3D11 renderer runs them through: gr_d3d_3d_bitmap_angle (billboard) or, with STRETCHED,
// gr_d3d_3d_bitmap_stretched_square (velocity stretched), each followed by Renderer::poly.
// With WEATHER, rain and snow placed from a fixed seed per particle and drawn as the quad
// gr_3d_bitmap_oriented_wh builds.

// Layout owned by gr_d3d11_particle.cpp (ParticleViewBufferData)
cbuffer ParticleViewBuffer : register(b5)
{
    // gr view matrix rows as rotate_vertex applies them, already scaled by matrix_scale
    float3 view_rvec;
    // Scaled view depth beyond which compute_vertex_codes clips, when far_clip_on is set
    float far_clip_z;
    float3 view_uvec;
    float far_clip_on;
    float3 view_fvec;
    float view_pad0;
    float3 view_pos;
    float view_pad1;
    float3 matrix_scale;
    float view_pad2;
    // Projection: scale_x, scale_y, scale_z, translate_z
    float4 proj;
};

struct VsOutput
{
    float4 pos : SV_POSITION;
    float3 norm : NORMAL;
    float4 color : COLOR;
    float2 uv0 : TEXCOORD0;
    float3 uv1 : TEXCOORD1;
    float4 world_pos_and_depth : TEXCOORD2;
};

// rotate_vertex 0x00518BF0
float3 rotate(float3 world_pos)
{
    float3 rel = world_pos - view_pos;
    return float3(dot(view_rvec, rel), dot(view_uvec, rel), dot(view_fvec, rel));
}

// compute_vertex_codes 0x005475D0 on a rotated vertex: x/y planes, near (z <= 0) and far
uint vertex_codes(float3 v)
{
    return (v.x > v.z ? 0x08u : 0u) | (v.y > v.z ? 0x20u : 0u) | (-v.z > v.x ? 0x04u : 0u)
        | (-v.z > v.y ? 0x10u : 0u) | (v.z <= 0.0f ? 0x80u : 0u)
        | (far_clip_on > 0.5f && v.z > far_clip_z ? 0x02u : 0u);
}

// One of a quad's four corners by vertex index. A select, not an index into a local array: vkd3d-shader (the GNU
// build's compiler, under Wine) crashes unrolling and propagating through the indexed form.
float3 pick_corner3(uint corner, float3 c0, float3 c1, float3 c2, float3 c3)
{
    return corner == 0 ? c0 : corner == 1 ? c1 : corner == 2 ? c2 : c3;
}

float2 pick_corner2(uint corner, float2 c0, float2 c1, float2 c2, float2 c3)
{
    return corner == 0 ? c0 : corner == 1 ? c1 : corner == 2 ? c2 : c3;
}

// Renderer::poly drops a quad wholly outside one clip plane
uint quad_and_codes(float3 c0, float3 c1, float3 c2, float3 c3)
{
    return 0xFF & vertex_codes(c0) & vertex_codes(c1) & vertex_codes(c2) & vertex_codes(c3);
}

#ifdef WEATHER

struct VsInput
{
    uint vertex_id : SV_VertexID;
    uint instance_id : SV_InstanceID;
    // xyz: position in the unit box, w: sway phase
    float4 seed_pos : INSTANCE0;
    // x: angle, y: spin, z: drift x, w: drift z
    float4 seed_motion : INSTANCE1;
};

// Layout owned by gr_d3d11_particle.cpp (WeatherBufferData)
cbuffer WeatherBuffer : register(b6)
{
    float3 box_min;
    // Whole vertical laps the box lattice has made
    uint wrap_base;
    float3 box_size;
    uint salt;
    // Box-relative offset the lattice has moved by, in [0, box_size)
    float3 box_shift;
    float sim_time;
    float3 camera_pos;
    // Rain streak half width, or snow sprite radius
    float half_width;
    float3 camera_rvec;
    float streak_half_len;
    float3 camera_uvec;
    float sway_amplitude;
    float3 fall_dir;
    float sway_speed;
    float3 region_center;
    // Sway phase advance, reduced to a common period of sin and cos(0.7)
    float sway_phase;
    float3 region_rvec;
    float region_radius;
    float3 region_uvec;
    float surface_y;
    float3 region_fvec;
    float column_cell;
    float3 region_half_extents;
    uint weather_flags;
    int2 column_cols;
    // A whole number of grids below every column the box covers, so the slot needs no signed modulus
    int2 column_origin;
    float4 weather_color;
};

// Ceiling cache, one slot per world column modulo the grid: x, y the column it holds, z the floor height
Buffer<uint4> weather_columns : register(t0);

static const uint weather_snow = 1;
static const uint weather_sphere = 2;
static const uint weather_liquid_surface = 4;
static const uint weather_ceiling = 8;

// gr_3d_bitmap_oriented_wh corners in poly order: (-w,-h), (+w,-h), (+w,+h), (-w,+h)
static const float2 weather_corner[4] = {float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, 1)};
static const float2 weather_corner_uv[4] = {float2(1, 1), float2(0, 1), float2(0, 0), float2(1, 0)};

uint pcg_hash(uint v)
{
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float hash01(uint h)
{
    return float(h >> 8) * (1.0f / 16777216.0f);
}

VsOutput main(VsInput input)
{
    uint key = pcg_hash(input.instance_id + pcg_hash(salt));

    // Height: the seed carried down by the fall and folded into the box. Every pass through the box is
    // a new lap, and a new lap rerolls the lateral position, as a vertical wrap does on the CPU path.
    float fall = frac(input.seed_pos.y + hash01(key)) * box_size.y + box_shift.y;
    uint lap = fall >= box_size.y ? 1u : 0u;
    float local_y = fall - box_size.y * lap;
    uint lap_key = pcg_hash(key ^ pcg_hash(wrap_base + lap));
    float2 lateral = frac(input.seed_pos.xz + float2(hash01(lap_key), hash01(pcg_hash(lap_key))));
    lateral = lateral * box_size.xz + box_shift.xz;

    bool snow = (weather_flags & weather_snow) != 0;
    if (snow) {
        // Closed form of the CPU path's per frame sway: x' = sin(phase) * a, z' = cos(0.7 * phase) * a
        float phase = input.seed_pos.w + sway_phase;
        float2 sway = sway_speed > 0.0001f
            ? float2(-cos(phase), sin(0.7f * phase) / 0.7f) * (sway_amplitude / sway_speed)
            : float2(sin(input.seed_pos.w), cos(0.7f * input.seed_pos.w)) * (sway_amplitude * sim_time);
        lateral += input.seed_motion.zw * sim_time + sway;
    }
    lateral -= box_size.xz * floor(lateral / box_size.xz);
    float3 pos = box_min + float3(lateral.x, local_y, lateral.y);

    // weather_particle_drawn
    bool hidden = (weather_flags & weather_liquid_surface) != 0 && pos.y < surface_y;
    float3 rel = pos - region_center;
    if (weather_flags & weather_sphere) {
        hidden = hidden || dot(rel, rel) > region_radius * region_radius;
    } else {
        float3 local = abs(float3(dot(rel, region_rvec), dot(rel, region_uvec), dot(rel, region_fvec)));
        hidden = hidden || any(local > region_half_extents);
    }
    // A column not traced yet hides the particle, like the CPU path's lookup without an answer
    if (weather_flags & weather_ceiling) {
        int2 cell = int2(floor(pos.xz / column_cell));
        uint2 slot = uint2(cell - column_origin) % uint2(column_cols);
        uint4 column = weather_columns.Load(slot.x + slot.y * column_cols.x);
        hidden = hidden || any(int2(column.xy) != cell) || pos.y < asfloat(column.z);
    }

    float3 right;
    float3 up;
    float half_height;
    float3 centre;
    if (snow) {
        // The camera plane spun by the flake's own angle
        float s;
        float c;
        sincos(input.seed_motion.x + input.seed_motion.y * sim_time, s, c);
        right = camera_rvec * c + camera_uvec * s;
        up = camera_uvec * c - camera_rvec * s;
        half_height = half_width;
        centre = pos;
    } else {
        // A streak that faces the camera while staying aligned with the fall vector
        right = cross(fall_dir, camera_pos - pos);
        float right_len = length(right);
        hidden = hidden || right_len < 0.0001f;
        right = right_len > 0.0f ? right / right_len : right;
        up = fall_dir;
        half_height = streak_half_len;
        centre = pos - fall_dir * streak_half_len;
    }

    float3 r0 = rotate(centre + right * (weather_corner[0].x * half_width) + up * (weather_corner[0].y * half_height));
    float3 r1 = rotate(centre + right * (weather_corner[1].x * half_width) + up * (weather_corner[1].y * half_height));
    float3 r2 = rotate(centre + right * (weather_corner[2].x * half_width) + up * (weather_corner[2].y * half_height));
    float3 r3 = rotate(centre + right * (weather_corner[3].x * half_width) + up * (weather_corner[3].y * half_height));
    uint and_codes = quad_and_codes(r0, r1, r2, r3);

    // Renderer::poly without constant sw: each corner keeps its own depth
    uint corner = input.vertex_id & 3;
    float3 unscaled = pick_corner3(corner, r0, r1, r2, r3) / matrix_scale;
    float w = unscaled.z;
    VsOutput output;
    // A hidden or rejected quad collapses to one point, which rasterises nothing
    output.pos = (hidden || and_codes != 0) ? float4(0, 0, -1, 1)
        : float4(unscaled.xy * proj.xy, w * proj.z + proj.w, w);
    output.norm = float3(0, 0, 0); // pre-transformed, like transformed_vs
    output.color = weather_color;
    output.uv0 = pick_corner2(corner, weather_corner_uv[0], weather_corner_uv[1], weather_corner_uv[2],
                              weather_corner_uv[3]);
    output.uv1 = float3(0, 0, -1);
    output.world_pos_and_depth = float4(0, 0, 0, w);
    return output;
}

#else

struct VsInput
{
    uint vertex_id : SV_VertexID;
    // xyz: world position, w: radius
    float4 inst_pos : INSTANCE0;
#ifdef STRETCHED
    // xyz: world position the streak trails back to
    float4 inst_tail : INSTANCE1;
#else
    // Rotated half extents: x = fx * cos, y = fy * sin, z = fx * sin, w = fy * cos
    float4 inst_axes : INSTANCE1;
#endif
    float4 inst_color : INSTANCE_COLOR;
};

static const float2 corner_uv[4] = {float2(0, 0), float2(1, 0), float2(1, 1), float2(0, 1)};

#ifdef STRETCHED
// Vector3::normalize 0x004FAB70: a zero vector becomes the x axis
float3 normalize_rf(float3 v)
{
    float len = sqrt(dot(v, v));
    return len <= 0.0f ? float3(1, 0, 0) : v * (1.0f / len);
}
#endif

VsOutput main(VsInput input)
{
    VsOutput output;
    uint corner = input.vertex_id & 3;
    float3 r0;
    float3 r1;
    float3 r2;
    float3 r3;
#ifdef STRETCHED
    // A world space square from pos along the streak, turned to face the view direction
    float3 pos = input.inst_pos.xyz;
    float3 dir = input.inst_tail.xyz - pos;
    float3 back = -view_fvec;
    float3 right = normalize_rf(cross(back, normalize_rf(dir)));
    float3 up = normalize_rf(cross(right, back));
    float half_radius = input.inst_pos.w * 0.5f;
    r0 = rotate(pos + up * (dot(up, dir) + half_radius));
    r1 = rotate(pos + right * half_radius);
    r2 = rotate(pos - up * half_radius);
    r3 = rotate(pos - right * half_radius);
#else
    // Camera facing corners around the rotated centre, offset in unscaled view units
    float3 centre = rotate(input.inst_pos.xyz);
    float dc = input.inst_axes.x;
    float e0 = input.inst_axes.y;
    float e4 = input.inst_axes.z;
    float f2 = input.inst_axes.w;
    r0 = float3(float2(-dc - e0, f2 - e4) * matrix_scale.xy + centre.xy, centre.z);
    r1 = float3(float2(dc - e0, f2 + e4) * matrix_scale.xy + centre.xy, centre.z);
    r2 = float3(float2(dc + e0, e4 - f2) * matrix_scale.xy + centre.xy, centre.z);
    r3 = float3(float2(e0 - dc, -e4 - f2) * matrix_scale.xy + centre.xy, centre.z);
#endif

    uint and_codes = quad_and_codes(r0, r1, r2, r3);
    float3 unscaled = pick_corner3(corner, r0, r1, r2, r3) / matrix_scale;
#ifdef STRETCHED
    // Each corner keeps its own depth (Renderer::poly without constant sw), so x/z * z needs no divide
    float w = unscaled.z;
    float2 clip_xy = unscaled.xy * proj.xy;
#else
    // One depth for the whole quad, pulled towards the camera by the radius (constant sw)
    float depth = centre.z;
    float depth_bias = matrix_scale.z * input.inst_pos.w;
    if (depth_bias < depth) {
        depth -= depth_bias;
    }
    float w = max(depth / matrix_scale.z, 0.1f);
    float2 clip_xy = unscaled.xy * proj.xy / unscaled.z * w;
#endif
    // A rejected quad collapses to one point, which rasterises nothing
    output.pos = and_codes != 0 ? float4(0, 0, -1, 1) : float4(clip_xy, w * proj.z + proj.w, w);
    output.norm = float3(0, 0, 0); // pre-transformed, like transformed_vs
    output.color = input.inst_color;
    output.uv0 = pick_corner2(corner, corner_uv[0], corner_uv[1], corner_uv[2], corner_uv[3]);
    output.uv1 = float3(0, 0, -1);
    output.world_pos_and_depth = float4(0, 0, 0, w);
    return output;
}

#endif
