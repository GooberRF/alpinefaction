#pragma once

#include <span>
#include <type_traits>
#include <vector>

// Clips a face's edge loop by a decal's planes in heap buffers. Returns the clipped polygon, valid until the next
// call, or an empty span when the face is corrupt or clipped away.
template<typename Face, typename Decal, typename ClipFn>
auto decal_clip_face_verts(const Face& face, const Decal& decal, int max_face_vertices, ClipFn clip_poly_to_plane,
                           int& num_face_verts)
{
    using Vector3 = std::remove_cvref_t<decltype(face.edge_loop->vertex->pos)>;
    static std::vector<Vector3> verts;
    static std::vector<Vector3> clipped;
    verts.clear();
    for (auto* fvert = face.edge_loop; fvert && static_cast<int>(verts.size()) <= max_face_vertices;) {
        verts.push_back(fvert->vertex->pos);
        fvert = fvert->next;
        if (fvert == face.edge_loop) {
            break;
        }
    }
    num_face_verts = static_cast<int>(verts.size());
    // A longer edge loop than any face has is corrupt
    if (num_face_verts < 3 || num_face_verts > max_face_vertices) {
        return std::span<const Vector3>{};
    }
    int num_verts = num_face_verts;
    for (const auto& plane : decal.decal_poly_planes) {
        clipped.resize(2 * num_verts);
        num_verts = clip_poly_to_plane(num_verts, verts.data(), clipped.data(), &plane);
        if (num_verts < 3) {
            return std::span<const Vector3>{};
        }
        verts.swap(clipped);
    }
    return std::span<const Vector3>{verts.data(), static_cast<size_t>(num_verts)};
}
