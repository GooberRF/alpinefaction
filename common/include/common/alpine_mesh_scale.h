#pragma once

// Alpine Mesh draw scale limits and .vfx draw helpers, shared by the game runtime and the editor.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace alpine_mesh_scale
{

inline constexpr float min_scale = 0.01f;
inline constexpr float max_scale = 16.0f;

inline float sanitize(float scale)
{
    if (!std::isfinite(scale) || scale <= 0.0f) {
        return 1.0f;
    }
    return std::clamp(scale, min_scale, max_scale);
}

template<typename Vector>
void scale_vector(Vector& v, float scale)
{
    v.x *= scale;
    v.y *= scale;
    v.z *= scale;
}

template<typename Matrix>
Matrix scale_orient(const Matrix& orient, float scale)
{
    Matrix out = orient;
    scale_vector(out.rvec, scale);
    scale_vector(out.uvec, scale);
    scale_vector(out.fvec, scale);
    return out;
}

// The uniform scale an orient carries, else 1: for an orthonormal or degenerate orient, and for one stretched along
// only some axes (the widescreen cockpit), which is drawn as given.
template<typename Matrix>
float orient_scale(const Matrix& orient)
{
    auto len_sq = [](const auto& v) { return v.x * v.x + v.y * v.y + v.z * v.z; };
    const float r_sq = len_sq(orient.rvec);
    if (!(r_sq > 1e-12f) || std::abs(r_sq - 1.0f) <= 1e-4f) {
        return 1.0f;
    }
    const float tolerance = r_sq * 1e-3f;
    if (std::abs(len_sq(orient.uvec) - r_sq) > tolerance || std::abs(len_sq(orient.fvec) - r_sq) > tolerance) {
        return 1.0f;
    }
    return std::sqrt(r_sq);
}

// The engine's instance transform misplaces a .vfx part drawn with a scaled orient. While alive, this multiplies the
// object-space data a part's draw reads (vertices, pivot, sprite and beam sizes, glow radius, ribbon width) by
// `scale` so the part can be drawn with the orthonormal orient. The destructor restores the exact saved values.
// The game and the editor share the part and chunk layout. Part draws never nest, so the saved vertices share one
// buffer.
template<typename Part>
class ScopedVfxPartScale
{
public:
    ScopedVfxPartScale(Part& part, float scale) :
        part_{part},
        pivot_{part.pivot},
        width_{part.width},
        height_{part.height},
        radius_{part.chunk->radius},
        ribbon_width_{part.chunk->ribbon_width}
    {
        const int num_vertices = part.chunk->num_vertices;
        if (part.vertex_positions && num_vertices > 0) {
            num_saved_ = static_cast<std::size_t>(num_vertices);
            saved_vertices().assign(part.vertex_positions, part.vertex_positions + num_vertices);
            for (int i = 0; i < num_vertices; ++i) {
                scale_vector(part.vertex_positions[i], scale);
            }
        }
        scale_vector(part.pivot, scale);
        part.width *= scale;
        part.height *= scale;
        part.chunk->radius *= scale;
        part.chunk->ribbon_width *= scale;
    }

    ~ScopedVfxPartScale()
    {
        const auto& saved = saved_vertices();
        for (std::size_t i = 0; i < num_saved_; ++i) {
            part_.vertex_positions[i] = saved[i];
        }
        part_.pivot = pivot_;
        part_.width = width_;
        part_.height = height_;
        part_.chunk->radius = radius_;
        part_.chunk->ribbon_width = ribbon_width_;
    }

    ScopedVfxPartScale(const ScopedVfxPartScale&) = delete;
    ScopedVfxPartScale& operator=(const ScopedVfxPartScale&) = delete;

private:
    using Point = std::remove_pointer_t<decltype(Part::vertex_positions)>;

    static std::vector<Point>& saved_vertices()
    {
        static std::vector<Point> buffer;
        return buffer;
    }

    Part& part_;
    decltype(Part::pivot) pivot_;
    float width_;
    float height_;
    float radius_;
    float ribbon_width_;
    std::size_t num_saved_ = 0;
};

} // namespace alpine_mesh_scale
