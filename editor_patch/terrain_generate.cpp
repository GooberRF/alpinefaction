#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <numbers>
#include <vector>
#include <common/terrain/alpine_terrain.h>
#include "terrain_generate.h"
#include "work_pool.h"

namespace at = alpine_terrain;

namespace
{

// Every table and stream derives from the user's seed through its own salt.
class Rng
{
public:
    Rng(uint32_t seed, uint64_t salt) : state_{(static_cast<uint64_t>(seed) << 32) ^ salt} {}

    uint64_t next()
    {
        const uint64_t r = at::splitmix64(state_);
        state_ += 0x9E3779B97F4A7C15ull;
        return r;
    }

    // [0, 1)
    double uniform()
    {
        return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0);
    }

private:
    uint64_t state_;
};

float smoothstep(float a, float b, float v)
{
    if (!(b > a)) return v >= a ? 1.0f : 0.0f;
    const float t = std::clamp((v - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

// 2D gradient noise over a seeded permutation, with 16 gradient directions so the lattice shows less.
class GradientNoise
{
public:
    explicit GradientNoise(uint32_t seed)
    {
        Rng rng{seed, 0x5045524D5441424Cull};
        for (int i = 0; i < 256; i++) perm_[i] = static_cast<uint8_t>(i);
        for (int i = 255; i > 0; i--) std::swap(perm_[i], perm_[rng.next() % static_cast<uint64_t>(i + 1)]);
        std::memcpy(perm_ + 256, perm_, 256);
    }

    // About [-1, 1].
    float operator()(double x, double z) const
    {
        static constexpr float grads[16][2] = {
            {1.0f, 0.0f},  {0.9238795f, 0.3826834f},   {0.7071068f, 0.7071068f},   {0.3826834f, 0.9238795f},
            {0.0f, 1.0f},  {-0.3826834f, 0.9238795f},  {-0.7071068f, 0.7071068f},  {-0.9238795f, 0.3826834f},
            {-1.0f, 0.0f}, {-0.9238795f, -0.3826834f}, {-0.7071068f, -0.7071068f}, {-0.3826834f, -0.9238795f},
            {0.0f, -1.0f}, {0.3826834f, -0.9238795f},  {0.7071068f, -0.7071068f},  {0.9238795f, -0.3826834f},
        };
        const double fx = std::floor(x), fz = std::floor(z);
        const int x0 = static_cast<int>(static_cast<int64_t>(fx) & 255);
        const int z0 = static_cast<int>(static_cast<int64_t>(fz) & 255);
        const auto tx = static_cast<float>(x - fx), tz = static_cast<float>(z - fz);
        auto dot = [&](int hash, float dx, float dz) { return grads[hash & 15][0] * dx + grads[hash & 15][1] * dz; };
        const float n00 = dot(perm_[perm_[x0] + z0], tx, tz);
        const float n10 = dot(perm_[perm_[x0 + 1] + z0], tx - 1.0f, tz);
        const float n01 = dot(perm_[perm_[x0] + z0 + 1], tx, tz - 1.0f);
        const float n11 = dot(perm_[perm_[x0 + 1] + z0 + 1], tx - 1.0f, tz - 1.0f);
        const float u = tx * tx * tx * (tx * (tx * 6.0f - 15.0f) + 10.0f);
        const float v = tz * tz * tz * (tz * (tz * 6.0f - 15.0f) + 10.0f);
        return lerp(lerp(n00, n10, u), lerp(n01, n11, u), v) * std::numbers::sqrt2_v<float>;
    }

private:
    uint8_t perm_[512];
};

// One octave's frequency, amplitude, and a rotation and offset that keep octaves from lining up.
struct Octave
{
    double c, s, freq, ox, oz;
    float amp;
};

std::vector<Octave> make_octaves(int count, double lacunarity, float gain, uint32_t seed, uint64_t salt)
{
    Rng rng{seed, salt};
    std::vector<Octave> octaves;
    double freq = 1.0;
    float amp = 1.0f;
    for (int i = 0; i < count; i++) {
        const double angle = rng.uniform() * 2.0 * std::numbers::pi;
        octaves.push_back({std::cos(angle), std::sin(angle), freq, rng.uniform() * 256.0, rng.uniform() * 256.0, amp});
        freq *= lacunarity;
        amp *= gain;
    }
    return octaves;
}

class Field
{
public:
    explicit Field(const TerrainGenSettings& s) :
        s_{s}, noise_{s.seed}, main_{make_octaves(s.octaves, s.lacunarity, s.roughness, s.seed, 0x4D41494E)},
        ridge_{make_octaves(s.octaves, s.lacunarity, s.roughness, s.seed, 0x52494447)},
        warp_x_{make_octaves(std::min(s.octaves, 4), 2.0, 0.5f, s.seed, 0x57415258)},
        warp_z_{make_octaves(std::min(s.octaves, 4), 2.0, 0.5f, s.seed, 0x5741525A)}
    {}

    // x, z in feature sizes.
    float sample(double x, double z) const
    {
        if (s_.warp > 0.0f) {
            const double wx = fbm(warp_x_, x, z), wz = fbm(warp_z_, x, z);
            x += s_.warp * wx;
            z += s_.warp * wz;
        }
        switch (s_.type) {
        case TerrainNoiseType::ridged:
            return ridged(ridge_, x, z);
        case TerrainNoiseType::billow:
            return billow(x, z);
        case TerrainNoiseType::hybrid: {
            const float f = 0.5f + 0.5f * fbm(main_, x, z);
            return f * 0.6f + smoothstep(0.45f, 0.75f, f) * ridged(ridge_, x, z) * 0.7f;
        }
        default:
            return fbm(main_, x, z);
        }
    }

private:
    float octave(const Octave& o, double x, double z) const
    {
        return noise_((x * o.c - z * o.s) * o.freq + o.ox, (x * o.s + z * o.c) * o.freq + o.oz);
    }

    float fbm(const std::vector<Octave>& octaves, double x, double z) const
    {
        float sum = 0.0f, norm = 0.0f;
        for (const Octave& o : octaves) {
            sum += o.amp * octave(o, x, z);
            norm += o.amp;
        }
        return sum / norm;
    }

    float billow(double x, double z) const
    {
        float sum = 0.0f, norm = 0.0f;
        for (const Octave& o : main_) {
            sum += o.amp * (2.0f * std::abs(octave(o, x, z)) - 1.0f);
            norm += o.amp;
        }
        return sum / norm;
    }

    // Musgrave's ridged multifractal: each octave is weighted by the ridge signal below it, so detail
    // gathers on the ridges and the valleys stay smooth. [0, 1]
    float ridged(const std::vector<Octave>& octaves, double x, double z) const
    {
        float sum = 0.0f, norm = 0.0f, weight = 1.0f;
        for (const Octave& o : octaves) {
            float signal = 1.0f - std::abs(octave(o, x, z));
            signal *= signal * weight;
            weight = std::clamp(signal * 2.0f, 0.0f, 1.0f);
            sum += signal * o.amp;
            norm += o.amp;
        }
        return sum / norm;
    }

    const TerrainGenSettings& s_;
    GradientNoise noise_;
    std::vector<Octave> main_, ridge_, warp_x_, warp_z_;
};

struct Dims
{
    uint32_t nx, nz;

    std::size_t count() const
    {
        return at::vertex_count(nx, nz);
    }

    std::size_t index(uint32_t x, uint32_t z) const
    {
        return static_cast<std::size_t>(z) * nx + x;
    }
};

void for_rows(uint32_t rows, const std::function<void(uint32_t)>& fn)
{
    work_pool_run(static_cast<int>(rows), [&](int row) { fn(static_cast<uint32_t>(row)); });
}

// Rescales to [0, 1]; a flat field becomes all 0.
void normalize01(std::vector<float>& h)
{
    const auto [lo_it, hi_it] = std::minmax_element(h.begin(), h.end());
    const float lo = *lo_it, span = *hi_it - *lo_it;
    if (!(span > 1e-12f) || !std::isfinite(span)) {
        std::fill(h.begin(), h.end(), 0.0f);
        return;
    }
    for (float& v : h) v = (v - lo) / span;
}

void shape(std::vector<float>& h, const Dims& d, const TerrainGenSettings& s)
{
    for_rows(d.nz, [&](uint32_t z) {
        const float v = 2.0f * static_cast<float>(z) / static_cast<float>(d.nz - 1) - 1.0f;
        for (uint32_t x = 0; x < d.nx; x++) {
            float n = std::clamp(h[d.index(x, z)] + s.offset, 0.0f, 1.0f);
            if (s.falloff != TerrainEdgeFalloff::none) {
                // A squircle: straight along the edges, rounded at the corners.
                const float u = 2.0f * static_cast<float>(x) / static_cast<float>(d.nx - 1) - 1.0f;
                const float r = std::pow(u * u * u * u + v * v * v * v, 0.25f);
                const float inside = 1.0f - smoothstep(1.0f - s.falloff_width, 1.0f, r);
                n = s.falloff == TerrainEdgeFalloff::island ? n * inside : lerp(1.0f, n, inside);
            }
            n = std::pow(n, s.exponent);
            if (s.terraces > 0) {
                // Flat treads with a short smooth riser at the top of each step.
                const float t = n * static_cast<float>(s.terraces);
                const float step = std::floor(t);
                n = (step + smoothstep(0.75f, 1.0f, t - step)) / static_cast<float>(s.terraces);
            }
            h[d.index(x, z)] = n;
        }
    });
}

constexpr int neighbour_dx[8] = {1, -1, 0, 0, 1, -1, 1, -1};
constexpr int neighbour_dz[8] = {0, 0, 1, -1, 1, -1, -1, 1};
// Neighbour k's opposite is k ^ 1.

// Moves material off slopes steeper than the talus angle, heights in cell units. Each pass reads only the
// previous one, so rows run in parallel and the result is independent of the thread count.
void thermal_erode(std::vector<float>& h, const Dims& d, int iterations, float talus_deg)
{
    const float tan_talus = std::tan(talus_deg * std::numbers::pi_v<float> / 180.0f);
    float limit[8];
    for (int k = 0; k < 8; k++) limit[k] = tan_talus * (k < 4 ? 1.0f : std::numbers::sqrt2_v<float>);
    std::vector<float> out(d.count() * 8), next(d.count());
    for (int it = 0; it < iterations; it++) {
        for_rows(d.nz, [&](uint32_t z) {
            for (uint32_t x = 0; x < d.nx; x++) {
                const std::size_t i = d.index(x, z);
                float excess[8] = {}, total = 0.0f, largest = 0.0f;
                for (int k = 0; k < 8; k++) {
                    const int nx = static_cast<int>(x) + neighbour_dx[k], nz = static_cast<int>(z) + neighbour_dz[k];
                    if (nx < 0 || nz < 0 || nx >= static_cast<int>(d.nx) || nz >= static_cast<int>(d.nz)) continue;
                    const float e = h[i] - h[d.index(nx, nz)] - limit[k];
                    if (e > 0.0f) {
                        excess[k] = e;
                        total += e;
                        largest = std::max(largest, e);
                    }
                }
                const float move = total > 0.0f ? 0.5f * largest / total : 0.0f;
                for (int k = 0; k < 8; k++) out[i * 8 + k] = excess[k] * move;
            }
        });
        for_rows(d.nz, [&](uint32_t z) {
            for (uint32_t x = 0; x < d.nx; x++) {
                const std::size_t i = d.index(x, z);
                float v = h[i];
                for (int k = 0; k < 8; k++) {
                    v -= out[i * 8 + k];
                    const int nx = static_cast<int>(x) + neighbour_dx[k], nz = static_cast<int>(z) + neighbour_dz[k];
                    if (nx < 0 || nz < 0 || nx >= static_cast<int>(d.nx) || nz >= static_cast<int>(d.nz)) continue;
                    v += out[d.index(nx, nz) * 8 + (k ^ 1)];
                }
                next[i] = v;
            }
        });
        h.swap(next);
    }
}

// Water droplets that pick up sediment running downhill and drop it where they slow (after Hans Beyer's
// droplet model). Heights are scaled as a 256-cell map spanning 0..1, the scale the constants are tuned
// for. Droplets run one after another, so this pass is single threaded.
void hydraulic_erode(std::vector<float>& h, const Dims& d, int droplets, float strength, uint32_t seed)
{
    if (d.nx < 3 || d.nz < 3) return;
    constexpr float inertia = 0.05f, min_capacity = 0.01f, deposit_rate = 0.3f, evaporation = 0.01f;
    constexpr float gravity = 4.0f;
    constexpr int max_steps = 30;
    const float erode_rate = 0.3f * (0.25f + 1.5f * strength);
    const float capacity = 4.0f * (0.5f + strength);

    struct BrushTap
    {
        int dx, dz;
        float w;
    };
    std::vector<BrushTap> brush;
    constexpr int brush_reach = 2;
    constexpr float brush_radius = 3.0f;
    float interior_sum = 0.0f;
    for (int dz = -brush_reach; dz <= brush_reach; dz++) {
        for (int dx = -brush_reach; dx <= brush_reach; dx++) {
            const float r = std::sqrt(static_cast<float>(dx * dx + dz * dz));
            if (r < brush_radius) {
                brush.push_back({dx, dz, 1.0f - r / brush_radius});
                interior_sum += brush.back().w;
            }
        }
    }

    const float max_x = static_cast<float>(d.nx - 1), max_z = static_cast<float>(d.nz - 1);
    auto sample = [&](float px, float pz, float& gx, float& gz) {
        const uint32_t ix = std::min(static_cast<uint32_t>(px), d.nx - 2);
        const uint32_t iz = std::min(static_cast<uint32_t>(pz), d.nz - 2);
        const float fx = px - static_cast<float>(ix), fz = pz - static_cast<float>(iz);
        const float h00 = h[d.index(ix, iz)], h10 = h[d.index(ix + 1, iz)];
        const float h01 = h[d.index(ix, iz + 1)], h11 = h[d.index(ix + 1, iz + 1)];
        gx = (h10 - h00) * (1.0f - fz) + (h11 - h01) * fz;
        gz = (h01 - h00) * (1.0f - fx) + (h11 - h10) * fx;
        return lerp(lerp(h00, h10, fx), lerp(h01, h11, fx), fz);
    };

    Rng rng{seed, 0x44524F50};
    for (int drop = 0; drop < droplets; drop++) {
        float px = static_cast<float>(rng.uniform()) * max_x, pz = static_cast<float>(rng.uniform()) * max_z;
        float dir_x = 0.0f, dir_z = 0.0f, speed = 1.0f, water = 1.0f, sediment = 0.0f;
        for (int step = 0; step < max_steps; step++) {
            float gx, gz;
            const float height = sample(px, pz, gx, gz);
            dir_x = dir_x * inertia - gx * (1.0f - inertia);
            dir_z = dir_z * inertia - gz * (1.0f - inertia);
            float len = std::sqrt(dir_x * dir_x + dir_z * dir_z);
            if (!(len > 1e-6f)) {
                const float angle = static_cast<float>(rng.uniform()) * 2.0f * std::numbers::pi_v<float>;
                dir_x = std::cos(angle);
                dir_z = std::sin(angle);
                len = 1.0f;
            }
            dir_x /= len;
            dir_z /= len;
            const float nx = px + dir_x, nz = pz + dir_z;
            if (!(nx >= 0.0f && nz >= 0.0f && nx <= max_x && nz <= max_z)) break;
            float ngx, ngz;
            const float dh = sample(nx, nz, ngx, ngz) - height;

            const float cap = std::max(-dh * speed * water * capacity, min_capacity);
            const uint32_t ix = std::min(static_cast<uint32_t>(px), d.nx - 2);
            const uint32_t iz = std::min(static_cast<uint32_t>(pz), d.nz - 2);
            if (sediment > cap || dh > 0.0f) {
                const float amount = dh > 0.0f ? std::min(dh, sediment) : (sediment - cap) * deposit_rate;
                sediment -= amount;
                const float fx = px - static_cast<float>(ix), fz = pz - static_cast<float>(iz);
                h[d.index(ix, iz)] += amount * (1.0f - fx) * (1.0f - fz);
                h[d.index(ix + 1, iz)] += amount * fx * (1.0f - fz);
                h[d.index(ix, iz + 1)] += amount * (1.0f - fx) * fz;
                h[d.index(ix + 1, iz + 1)] += amount * fx * fz;
            }
            else {
                const float amount = std::min((cap - sediment) * erode_rate, -dh);
                const int cx = static_cast<int>(std::lround(px)), cz = static_cast<int>(std::lround(pz));
                auto inside = [&](int x, int z) {
                    return x >= 0 && z >= 0 && x < static_cast<int>(d.nx) && z < static_cast<int>(d.nz);
                };
                float wsum = interior_sum;
                if (!inside(cx - brush_reach, cz - brush_reach) || !inside(cx + brush_reach, cz + brush_reach)) {
                    wsum = 0.0f;
                    for (const BrushTap& t : brush) {
                        if (inside(cx + t.dx, cz + t.dz)) {
                            wsum += t.w;
                        }
                    }
                }
                const float per_weight = amount / wsum;
                for (const BrushTap& t : brush) {
                    if (inside(cx + t.dx, cz + t.dz)) {
                        h[d.index(cx + t.dx, cz + t.dz)] -= per_weight * t.w;
                    }
                }
                sediment += amount;
            }
            speed = std::sqrt(std::max(speed * speed - dh * gravity, 0.0f));
            water *= 1.0f - evaporation;
            px = nx;
            pz = nz;
        }
    }
}

// 1-2-1 binomial blur along both axes, edges clamped.
void blur(std::vector<float>& h, const Dims& d, int passes)
{
    std::vector<float> tmp(d.count());
    for (int p = 0; p < passes; p++) {
        for_rows(d.nz, [&](uint32_t z) {
            for (uint32_t x = 0; x < d.nx; x++) {
                const uint32_t l = x > 0 ? x - 1 : 0, r = std::min(x + 1, d.nx - 1);
                tmp[d.index(x, z)] = 0.25f * (h[d.index(l, z)] + 2.0f * h[d.index(x, z)] + h[d.index(r, z)]);
            }
        });
        for_rows(d.nz, [&](uint32_t z) {
            const uint32_t b = z > 0 ? z - 1 : 0, t = std::min(z + 1, d.nz - 1);
            for (uint32_t x = 0; x < d.nx; x++) {
                h[d.index(x, z)] = 0.25f * (tmp[d.index(x, b)] + 2.0f * tmp[d.index(x, z)] + tmp[d.index(x, t)]);
            }
        });
    }
}

float bilinear(const std::vector<float>& v, const Dims& d, float x, float z)
{
    const uint32_t ix = std::min(static_cast<uint32_t>(std::max(x, 0.0f)), d.nx - 2);
    const uint32_t iz = std::min(static_cast<uint32_t>(std::max(z, 0.0f)), d.nz - 2);
    const float fx = std::clamp(x - static_cast<float>(ix), 0.0f, 1.0f);
    const float fz = std::clamp(z - static_cast<float>(iz), 0.0f, 1.0f);
    return lerp(lerp(v[d.index(ix, iz)], v[d.index(ix + 1, iz)], fx),
                lerp(v[d.index(ix, iz + 1)], v[d.index(ix + 1, iz + 1)], fx), fz);
}

// Slope in degrees at every vertex, central differences inside and one-sided on the edges.
std::vector<float> vertex_slopes(const std::vector<float>& h, const Dims& d, float height_range, float cell_size)
{
    std::vector<float> slope(d.count());
    for_rows(d.nz, [&](uint32_t z) {
        const uint32_t b = z > 0 ? z - 1 : 0, t = std::min(z + 1, d.nz - 1);
        for (uint32_t x = 0; x < d.nx; x++) {
            const uint32_t l = x > 0 ? x - 1 : 0, r = std::min(x + 1, d.nx - 1);
            const float gx =
                (h[d.index(r, z)] - h[d.index(l, z)]) * height_range / (static_cast<float>(r - l) * cell_size);
            const float gz =
                (h[d.index(x, t)] - h[d.index(x, b)]) * height_range / (static_cast<float>(t - b) * cell_size);
            slope[d.index(x, z)] = std::atan(std::sqrt(gx * gx + gz * gz)) * (180.0f / std::numbers::pi_v<float>);
        }
    });
    return slope;
}

// Convexity (how far a vertex stands above its neighbours' mean) on a softened copy, scaled so the
// strongest tenth of the ridges reach 1.
std::vector<float> vertex_ridges(const std::vector<float>& h, const Dims& d)
{
    std::vector<float> soft = h;
    blur(soft, d, 1);
    std::vector<float> ridge(d.count());
    for_rows(d.nz, [&](uint32_t z) {
        const uint32_t b = z > 0 ? z - 1 : 0, t = std::min(z + 1, d.nz - 1);
        for (uint32_t x = 0; x < d.nx; x++) {
            const uint32_t l = x > 0 ? x - 1 : 0, r = std::min(x + 1, d.nx - 1);
            const float mean =
                (soft[d.index(l, z)] + soft[d.index(r, z)] + soft[d.index(x, b)] + soft[d.index(x, t)]) * 0.15f
                + (soft[d.index(l, b)] + soft[d.index(r, b)] + soft[d.index(l, t)] + soft[d.index(r, t)]) * 0.1f;
            ridge[d.index(x, z)] = std::max(soft[d.index(x, z)] - mean, 0.0f);
        }
    });
    std::vector<float> positive;
    for (float v : ridge) {
        if (v > 0.0f) {
            positive.push_back(v);
        }
    }
    float scale = 0.0f;
    if (!positive.empty()) {
        const auto k = positive.begin() + static_cast<std::ptrdiff_t>((positive.size() - 1) * 9 / 10);
        std::nth_element(positive.begin(), k, positive.end());
        scale = *k;
    }
    for (float& v : ridge) v = scale > 0.0f ? std::min(v / scale, 1.0f) : 0.0f;
    return ridge;
}

// Weights that sum to 1 as bytes that sum to 255, the remainder to the largest fractions.
void weights_to_bytes(const float (&w)[terrain_gen_layer_count], uint8_t* out)
{
    float sum = 0.0f;
    for (float v : w) sum += std::max(v, 0.0f);
    float scaled[terrain_gen_layer_count];
    int total = 0;
    for (int c = 0; c < terrain_gen_layer_count; c++) {
        scaled[c] = sum > 0.0f ? std::max(w[c], 0.0f) / sum * 255.0f : (c == 0 ? 255.0f : 0.0f);
        out[c] = static_cast<uint8_t>(std::min(std::floor(scaled[c]), 255.0f));
        total += out[c];
        scaled[c] -= out[c];
    }
    while (total < 255) {
        int best = 0;
        for (int c = 1; c < terrain_gen_layer_count; c++) {
            if (scaled[c] > scaled[best]) {
                best = c;
            }
        }
        out[best]++;
        scaled[best] = -1.0f;
        total++;
    }
}

void make_splat(const std::vector<float>& h, const std::vector<float>& slope, const Dims& d,
                const TerrainGenSettings& s, TerrainGenResult& out)
{
    const std::vector<float> ridge = vertex_ridges(h, d);
    const uint32_t mul = out.weight_res_mul;
    const uint32_t dw = at::weight_width(d.nx, mul), dh = at::weight_height(d.nz, mul);
    const std::size_t texels = at::weight_texel_count(d.nx, d.nz, mul);

    // Variation patches cover the requested share of the ground: threshold the noise at that quantile.
    std::vector<float> variation(texels, 0.0f);
    float threshold = 0.0f;
    if (s.variation > 0.0f) {
        TerrainGenSettings vs;
        vs.seed = s.seed ^ 0x56415249u;
        vs.type = TerrainNoiseType::fbm;
        vs.octaves = 3;
        vs.warp = 0.0f;
        const Field field{vs};
        const double scale = static_cast<double>(out.cell_size) / (s.variation_size * mul);
        for_rows(dh, [&](uint32_t j) {
            for (uint32_t i = 0; i < dw; i++) {
                variation[static_cast<std::size_t>(j) * dw + i] = field.sample((i + 0.5) * scale, (j + 0.5) * scale);
            }
        });
        std::vector<float> sorted = variation;
        const auto k = static_cast<std::size_t>(std::floor((1.0f - s.variation) * static_cast<float>(texels - 1)));
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(k), sorted.end());
        threshold = sorted[k];
    }

    const float rock_lo = s.slope_deg - s.slope_blend_deg * 0.5f, rock_hi = s.slope_deg + s.slope_blend_deg * 0.5f;
    const float high_lo = s.high_start - s.high_blend * 0.5f, high_hi = s.high_start + s.high_blend * 0.5f;
    out.splat.assign(texels * 4, 0);
    std::vector<float> row_coverage(static_cast<std::size_t>(dh) * terrain_gen_layer_count, 0.0f);
    for_rows(dh, [&](uint32_t j) {
        const float z = (static_cast<float>(j) + 0.5f) / static_cast<float>(mul);
        uint8_t* row = out.splat.data() + static_cast<std::size_t>(at::image_row(j, dh)) * dw * 4;
        for (uint32_t i = 0; i < dw; i++) {
            const float x = (static_cast<float>(i) + 0.5f) / static_cast<float>(mul);
            const float r = bilinear(ridge, d, x, z) * s.ridge_emphasis;
            const float rock = smoothstep(rock_lo, rock_hi, bilinear(slope, d, x, z) + r * s.slope_deg * 0.5f);
            const float high = smoothstep(high_lo, high_hi, bilinear(h, d, x, z) + r * 0.15f);
            const float flat = (1.0f - rock) * (1.0f - high);
            float patch = 0.0f;
            if (s.variation >= 1.0f) {
                patch = 1.0f;
            }
            else if (s.variation > 0.0f) {
                patch =
                    smoothstep(threshold - 0.04f, threshold + 0.04f, variation[static_cast<std::size_t>(j) * dw + i]);
            }
            float w[terrain_gen_layer_count];
            w[terrain_gen_base] = flat * (1.0f - patch);
            w[terrain_gen_slope] = rock;
            w[terrain_gen_high] = (1.0f - rock) * high;
            w[terrain_gen_variation] = flat * patch;
            weights_to_bytes(w, row + static_cast<std::size_t>(i) * 4);
            for (int c = 0; c < terrain_gen_layer_count; c++) {
                row_coverage[static_cast<std::size_t>(j) * terrain_gen_layer_count + c] += row[i * 4 + c];
            }
        }
    });
    for (int c = 0; c < terrain_gen_layer_count; c++) {
        double sum = 0.0;
        for (uint32_t j = 0; j < dh; j++) {
            sum += row_coverage[static_cast<std::size_t>(j) * terrain_gen_layer_count + c];
        }
        out.coverage[c] = static_cast<float>(sum / (255.0 * static_cast<double>(texels)));
    }
}

} // namespace

TerrainGenSettings terrain_gen_clamp(const TerrainGenSettings& in)
{
    const TerrainGenSettings def;
    TerrainGenSettings s = in;
    auto clamp_f = [](float& v, float lo, float hi, float fallback) { v = at::clamp_finite(v, lo, hi, fallback); };
    if (static_cast<unsigned>(s.type) > static_cast<unsigned>(TerrainNoiseType::hybrid)) {
        s.type = def.type;
    }
    if (static_cast<unsigned>(s.falloff) > static_cast<unsigned>(TerrainEdgeFalloff::basin)) {
        s.falloff = def.falloff;
    }
    clamp_f(s.feature_size, 1.0f, 65536.0f, def.feature_size);
    s.octaves = std::clamp(s.octaves, 1, 10);
    clamp_f(s.roughness, 0.1f, 0.9f, def.roughness);
    clamp_f(s.lacunarity, 1.25f, 4.0f, def.lacunarity);
    clamp_f(s.warp, 0.0f, 2.0f, def.warp);
    clamp_f(s.exponent, 0.2f, 5.0f, def.exponent);
    s.terraces = std::clamp(s.terraces, 0, 64);
    clamp_f(s.falloff_width, 0.02f, 1.0f, def.falloff_width);
    clamp_f(s.offset, -0.5f, 0.5f, def.offset);
    s.thermal_iterations = std::clamp(s.thermal_iterations, 0, 100);
    clamp_f(s.talus_deg, 5.0f, 75.0f, def.talus_deg);
    s.droplets = std::clamp(s.droplets, 0, 100000);
    clamp_f(s.erosion_strength, 0.0f, 1.0f, def.erosion_strength);
    s.smooth_passes = std::clamp(s.smooth_passes, 0, 10);
    clamp_f(s.slope_deg, 0.0f, 89.0f, def.slope_deg);
    clamp_f(s.slope_blend_deg, 0.0f, 45.0f, def.slope_blend_deg);
    clamp_f(s.high_start, 0.0f, 1.0f, def.high_start);
    clamp_f(s.high_blend, 0.0f, 1.0f, def.high_blend);
    clamp_f(s.variation, 0.0f, 1.0f, def.variation);
    clamp_f(s.variation_size, 1.0f, 65536.0f, def.variation_size);
    clamp_f(s.ridge_emphasis, 0.0f, 1.0f, def.ridge_emphasis);
    return s;
}

void terrain_generate(const TerrainGenSettings& settings, const TerrainGenInput& input, TerrainGenResult& out)
{
    const TerrainGenSettings s = terrain_gen_clamp(settings);
    const Dims d{std::clamp(input.nx, at::min_verts, at::max_verts),
                 std::clamp(input.nz, at::min_verts, at::max_verts)};
    out.nx = d.nx;
    out.nz = d.nz;
    out.weight_res_mul = at::is_allowed_weight_res_mul(input.weight_res_mul) ? input.weight_res_mul : 1;
    out.cell_size = at::clamp_finite(input.cell_size, at::min_cell_size, at::max_cell_size, at::default_cell_size);
    out.height_range =
        at::clamp_finite(input.height_range, at::min_height_range, at::max_height_range, at::default_height_range);

    std::vector<float> h(d.count());
    const Field field{s};
    const double scale = static_cast<double>(out.cell_size) / s.feature_size;
    for_rows(d.nz, [&](uint32_t z) {
        for (uint32_t x = 0; x < d.nx; x++) h[d.index(x, z)] = field.sample(x * scale, z * scale);
    });
    normalize01(h);
    shape(h, d, s);

    if (s.thermal_iterations > 0) {
        // In cell units a height difference is also a slope.
        const float to_cells = out.height_range / out.cell_size;
        for (float& v : h) v *= to_cells;
        thermal_erode(h, d, s.thermal_iterations, s.talus_deg);
        for (float& v : h) v /= to_cells;
    }
    if (s.droplets > 0) {
        const float to_droplet = static_cast<float>(std::max(d.nx, d.nz) - 1) / 256.0f;
        for (float& v : h) v *= to_droplet;
        hydraulic_erode(h, d, s.droplets, s.erosion_strength, s.seed);
        for (float& v : h) v /= to_droplet;
    }
    blur(h, d, s.smooth_passes);
    normalize01(h);

    out.heights.resize(d.count());
    for (std::size_t i = 0; i < h.size(); i++) out.heights[i] = at::encode_height01(h[i]);

    const std::vector<float> slope = vertex_slopes(h, d, out.height_range, out.cell_size);
    double slope_sum = 0.0;
    std::size_t steep = 0;
    out.slope_max_deg = 0.0f;
    for (float v : slope) {
        slope_sum += v;
        out.slope_max_deg = std::max(out.slope_max_deg, v);
        if (v > s.slope_deg) {
            steep++;
        }
    }
    out.slope_mean_deg = static_cast<float>(slope_sum / static_cast<double>(slope.size()));
    out.steep_share = static_cast<float>(steep) / static_cast<float>(slope.size());

    std::fill(std::begin(out.coverage), std::end(out.coverage), 0.0f);
    out.splat.clear();
    if (s.splat) {
        make_splat(h, slope, d, s, out);
    }
}

void terrain_generate_preview(const TerrainGenResult& result, TerrainGenPreview mode, int size,
                              std::vector<uint32_t>& pixels)
{
    constexpr uint32_t background = 0x303030;
    pixels.assign(static_cast<std::size_t>(std::max(size, 0)) * std::max(size, 0), background);
    const Dims d{result.nx, result.nz};
    if (size <= 0 || d.nx < at::min_verts || d.nz < at::min_verts || result.heights.size() != d.count()) return;
    if (mode == TerrainGenPreview::splat && result.splat.empty()) {
        mode = TerrainGenPreview::shaded;
    }

    std::vector<float> h(d.count());
    for (std::size_t i = 0; i < h.size(); i++) h[i] = static_cast<float>(result.heights[i]) / 65535.0f;

    // Lambert shading lit from the upper left (-X, +Z) at 45 degrees.
    std::vector<float> shade;
    if (mode != TerrainGenPreview::height) {
        shade.resize(d.count());
        const float lx = -0.5f, ly = std::numbers::sqrt2_v<float> * 0.5f, lz = 0.5f;
        for_rows(d.nz, [&](uint32_t z) {
            const uint32_t b = z > 0 ? z - 1 : 0, t = std::min(z + 1, d.nz - 1);
            for (uint32_t x = 0; x < d.nx; x++) {
                const uint32_t l = x > 0 ? x - 1 : 0, r = std::min(x + 1, d.nx - 1);
                const float gx = (h[d.index(r, z)] - h[d.index(l, z)]) * result.height_range /
                                 (static_cast<float>(r - l) * result.cell_size);
                const float gz = (h[d.index(x, t)] - h[d.index(x, b)]) * result.height_range /
                                 (static_cast<float>(t - b) * result.cell_size);
                const float lit = (-gx * lx + ly - gz * lz) / std::sqrt(gx * gx + 1.0f + gz * gz);
                shade[d.index(x, z)] = 0.25f + 0.75f * std::clamp(lit, 0.0f, 1.0f);
            }
        });
    }

    // Letterbox by the grid's aspect in cells.
    const float cells_x = static_cast<float>(d.nx - 1), cells_z = static_cast<float>(d.nz - 1);
    const float fit = static_cast<float>(size) / std::max(cells_x, cells_z);
    const int draw_w = std::max(1, static_cast<int>(std::lround(cells_x * fit)));
    const int draw_h = std::max(1, static_cast<int>(std::lround(cells_z * fit)));
    const int x0 = (size - draw_w) / 2, y0 = (size - draw_h) / 2;

    static constexpr float layer_rgb[terrain_gen_layer_count][3] = {
        {96, 138, 62},   // base: grass
        {118, 108, 98},  // slope: rock
        {236, 238, 244}, // high: snow
        {150, 118, 72},  // variation: dirt
    };
    const uint32_t mul = result.weight_res_mul;
    const uint32_t dw = at::weight_width(d.nx, mul), dh = at::weight_height(d.nz, mul);

    for_rows(static_cast<uint32_t>(draw_h), [&](uint32_t py) {
        const float gz = cells_z - (static_cast<float>(py) + 0.5f) / static_cast<float>(draw_h) * cells_z;
        uint32_t* row = pixels.data() + static_cast<std::size_t>(y0 + static_cast<int>(py)) * size + x0;
        for (int px = 0; px < draw_w; px++) {
            const float gx = (static_cast<float>(px) + 0.5f) / static_cast<float>(draw_w) * cells_x;
            float rgb[3];
            if (mode == TerrainGenPreview::height) {
                rgb[0] = rgb[1] = rgb[2] = bilinear(h, d, gx, gz) * 255.0f;
            }
            else if (mode == TerrainGenPreview::shaded) {
                rgb[0] = rgb[1] = rgb[2] = bilinear(shade, d, gx, gz) * 255.0f;
            }
            else {
                const uint32_t ti = std::min(static_cast<uint32_t>(gx * static_cast<float>(mul)), dw - 1);
                const uint32_t tj = std::min(static_cast<uint32_t>(gz * static_cast<float>(mul)), dh - 1);
                const uint8_t* w =
                    result.splat.data() + (static_cast<std::size_t>(at::image_row(tj, dh)) * dw + ti) * 4;
                const float lit = bilinear(shade, d, gx, gz);
                for (int c = 0; c < 3; c++) {
                    float v = 0.0f;
                    for (int k = 0; k < terrain_gen_layer_count; k++) v += layer_rgb[k][c] * w[k];
                    rgb[c] = v / 255.0f * lit;
                }
            }
            uint32_t packed = 0;
            for (int c = 0; c < 3; c++) {
                packed = (packed << 8) | static_cast<uint32_t>(std::clamp(std::lround(rgb[c]), 0L, 255L));
            }
            row[px] = packed;
        }
    });
}
