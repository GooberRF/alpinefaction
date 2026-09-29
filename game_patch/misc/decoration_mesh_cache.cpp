#include <algorithm>
#include <cmath>
#include <common/utils/string-utils.h>
#include <xlog/xlog.h>
#include "decoration_mesh_cache.h"
#include "../rf/math/vector.h"
#include "../rf/vmesh.h"

int DecorationMeshCache::resolve(const std::string& name, const char* log_tag)
{
    const std::string key = string_to_lower(name);
    auto it = lookup_.find(key);
    if (it != lookup_.end()) {
        return it->second;
    }
    rf::VMesh* mesh = rf::vmesh_load(name.c_str(), rf::MESH_TYPE_STATIC, -1);
    if (!mesh) {
        xlog::warn("[{}] Failed to load decoration mesh '{}'", log_tag, name);
        lookup_.emplace(key, -1);
        return -1;
    }
    rf::Vector3 bbox_min{}, bbox_max{};
    rf::vmesh_get_bbox(mesh, &bbox_min, &bbox_max);
    // About the origin, the point placed, not the bbox centre the engine would use
    const rf::Vector3 extent{std::max(std::fabs(bbox_min.x), std::fabs(bbox_max.x)),
                             std::max(std::fabs(bbox_min.y), std::fabs(bbox_max.y)),
                             std::max(std::fabs(bbox_min.z), std::fabs(bbox_max.z))};
    float radius = extent.len();
    if (!std::isfinite(radius) || radius < 0.0f) {
        radius = 0.0f;
    }
    const int slot = static_cast<int>(meshes_.size());
    meshes_.push_back({mesh, std::min(radius, max_radius_)});
    lookup_.emplace(key, slot);
    return slot;
}

bool DecorationMeshCache::reject(const std::string& name)
{
    return lookup_.emplace(string_to_lower(name), -1).second;
}

void DecorationMeshCache::clear()
{
    meshes_.clear();
    lookup_.clear();
}
