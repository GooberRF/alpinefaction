#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <xlog/xlog.h>
#include "gizmo.h"
#include "level.h"
#include "texture_lock.h"

namespace
{

using BrushLocks = std::vector<std::pair<BrushNode*, std::vector<FaceTextureLock>>>;

void apply_locks(const BrushLocks& locked)
{
    try {
        for (const auto& [brush, locks] : locked) {
            texture_lock_apply(*brush, locks, nullptr);
        }
    }
    catch (const std::exception& e) {
        xlog::error("[Texture lock] {}", e.what());
    }
}

// The faces of the brushes a face or vertex mode transform moves, captured before its finalize writes the vertices
FunHook<void __fastcall(CDedLevel*)> transform_vertices_finalize_hook{
    0x00427460,
    [](CDedLevel* level) FASTCALL_LAMBDA {
        BrushLocks locked;
        if (gizmo_texture_lock()) {
            try {
                for (int i = 0; i < level->transform_groups.size; ++i) {
                    const TransformGroup* group = level->transform_groups.data_ptr[i];
                    if (group && group->brush && std::none_of(locked.begin(), locked.end(), [&](const auto& l) {
                            return l.first == group->brush;
                        })) {
                        locked.emplace_back(group->brush, texture_lock_capture(*group->brush));
                    }
                }
            }
            catch (const std::exception& e) {
                xlog::error("[Texture lock] {}", e.what());
                locked.clear();
            }
        }
        transform_vertices_finalize_hook.call_target(level);
        apply_locks(locked);
    },
};

// Stretch (FUN_0043b2d0) between its undo snapshot, whose clones it then reshapes, and its re-planing of the faces
BrushLocks g_stretch_locks;

CallHook<void __fastcall(CDedLevel*)> stretch_snapshot_hook{
    0x0043B335,
    [](CDedLevel* level) FASTCALL_LAMBDA {
        stretch_snapshot_hook.call_target(level);
        g_stretch_locks.clear();
        if (!gizmo_texture_lock()) return;
        try {
            level->for_each_brush([&](BrushNode& brush) {
                const auto* solid = static_cast<const GSolid*>(brush.geometry);
                if (solid && (brush.state == BRUSH_STATE_SELECTED || solid->face_selection.size > 0 ||
                              solid->vertex_selection.size > 0)) {
                    g_stretch_locks.emplace_back(&brush, texture_lock_capture(brush));
                }
            });
        }
        catch (const std::exception& e) {
            xlog::error("[Texture lock] {}", e.what());
            g_stretch_locks.clear();
        }
    },
};

CallHook<void __fastcall(CDedLevel*)> stretch_replane_hook{
    0x0043B4EF,
    [](CDedLevel* level) FASTCALL_LAMBDA {
        apply_locks(g_stretch_locks);
        g_stretch_locks.clear();
        stretch_replane_hook.call_target(level);
    },
};

} // namespace

std::vector<FaceTextureLock> texture_lock_capture(const BrushNode& brush)
{
    std::vector<FaceTextureLock> locks;
    auto* solid = static_cast<GSolid*>(brush.geometry);
    if (!solid) return locks;
    solid->for_each_face([&](GFace& face) {
        FaceTextureLock lock{};
        std::vector<Vector3> points;
        std::vector<UvCorner> uvs;
        bool whole = true;
        for_each_face_corner(face, [&](GFaceVertex& fv) {
            whole = whole && fv.vertex;
            if (fv.vertex) {
                lock.corners.push_back({&fv, brush.to_world(fv.vertex->pos), fv.u, fv.v});
                points.push_back(lock.corners.back().world);
                uvs.push_back({points.back(), fv.u, fv.v});
            }
        });
        if (!whole || points.size() < 3) return false;
        lock.normal = normalized(newell_normal(points));
        const auto map = uv_map_through(uvs);
        if (length(lock.normal) == 0.0f || !map) return false;
        lock.centroid = centroid(points);
        lock.map = *map;
        const bool flat = std::all_of(lock.corners.begin(), lock.corners.end(), [&](const FaceTextureLock::Corner& c) {
            return std::abs(lock.map.u(c.world) - c.u) <= 1e-3f && std::abs(lock.map.v(c.world) - c.v) <= 1e-3f &&
                   std::abs(dot(c.world - lock.centroid, lock.normal)) <= 1e-3f;
        });
        if (flat) {
            locks.push_back(std::move(lock));
        }
        return false;
    });
    return locks;
}

void texture_lock_apply(BrushNode& brush, const std::vector<FaceTextureLock>& locks, const Vector3* fixed)
{
    constexpr float eps = 1e-4f;
    for (const FaceTextureLock& lock : locks) {
        std::vector<Vector3> points;
        for (const auto& c : lock.corners) {
            points.push_back(brush.to_world(c.face_vertex->vertex->pos));
        }
        bool rigid = true;
        for (size_t i = 0; i < points.size() && rigid; ++i) {
            for (size_t j = i + 1; j < points.size(); ++j) {
                const float before = length(lock.corners[i].world - lock.corners[j].world);
                const float after = length(points[i] - points[j]);
                if (std::abs(before - after) > eps * std::max(1.0f, before)) {
                    rigid = false;
                    break;
                }
            }
        }
        // as captured, also for a live drag coming back to its press shape
        auto keep_press_uvs = [&] {
            for (const auto& c : lock.corners) {
                c.face_vertex->u = c.u;
                c.face_vertex->v = c.v;
            }
        };
        if (rigid) {
            keep_press_uvs();
            continue;
        }
        const Vector3 normal = normalized(newell_normal(points));
        if (length(normal) == 0.0f || dot(normal, lock.normal) < 0.0f) {
            keep_press_uvs();
            continue;
        }
        const Vector3 new_centroid = centroid(points);

        // A world point held still: the corners left in place, else the point the change was made about
        Vector3 anchor = fixed ? *fixed : lock.centroid;
        Vector3 still{0.0f, 0.0f, 0.0f};
        int still_count = 0;
        for (size_t i = 0; i < points.size(); ++i) {
            if (length(points[i] - lock.corners[i].world) <= eps) {
                still = still + points[i];
                ++still_count;
            }
        }
        if (still_count > 0) {
            anchor = still * (1.0f / static_cast<float>(still_count));
        }
        // Its foot on the old plane keeps its UV, at its foot on the new one, the gradients turned with the plane
        const Vector3 old_foot = anchor - lock.normal * dot(anchor - lock.centroid, lock.normal);
        const Vector3 new_foot = anchor - normal * dot(anchor - new_centroid, normal);
        const float foot_u = lock.map.u(old_foot);
        const float foot_v = lock.map.v(old_foot);
        const Vector3 grad_u = rotate_between(lock.normal, normal, lock.map.grad_u);
        const Vector3 grad_v = rotate_between(lock.normal, normal, lock.map.grad_v);
        for (size_t i = 0; i < points.size(); ++i) {
            GFaceVertex& fv = *lock.corners[i].face_vertex;
            fv.u = foot_u + dot(grad_u, points[i] - new_foot);
            fv.v = foot_v + dot(grad_v, points[i] - new_foot);
        }
    }
}

void ApplyTextureLockPatches()
{
    transform_vertices_finalize_hook.install();
    stretch_snapshot_hook.install();
    stretch_replane_hook.install();
}
