#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>
#include <vector>

namespace rf
{
    struct VMesh;
}

struct DecorationMesh
{
    rf::VMesh* mesh;
    // About the mesh origin, clamped to the cache's max_radius
    float radius;
};

// Static decoration meshes of one level by name, each loaded once. The engine owns level meshes and frees them on
// unload, so clear() drops them without vmesh_free. Level init only: a mesh load at render time corrupts the
// bitmap manager.
class DecorationMeshCache
{
public:
    explicit DecorationMeshCache(float max_radius) : max_radius_{max_radius} {}

    // The slot of `name`'s mesh, loaded on first use; -1 for a name that does not load, warned once as `log_tag`.
    int resolve(const std::string& name, const char* log_tag);
    // Records `name` as not loading without trying; false when it was already known.
    bool reject(const std::string& name);
    const DecorationMesh& operator[](int slot) const
    {
        return meshes_[static_cast<std::size_t>(slot)];
    }
    void clear();

private:
    float max_radius_;
    std::vector<DecorationMesh> meshes_;
    // Lowercased name -> slot, or -1 for a name known not to load
    std::unordered_map<std::string, int> lookup_;
};
