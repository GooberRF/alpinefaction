#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include <xlog/xlog.h>
#include "brush_preview.h"
#include "gizmo.h"
#include "level.h"
#include "mfc_types.h"
#include "texture_mode.h"
#include "vtypes.h"

namespace
{

// A cancelled Build has numbered the brush faces again for level geometry it never made, until one completes
bool g_face_ids_unbuilt = false;

// A 3D view drawing the level textured; Texture mode keeps the level geometry, which its clicks pick
bool preview_shows(const CDedLevel& level)
{
    return gizmo_textured_preview() && level.edit_mode != DedEditMode::Texture && level.solid && !g_face_ids_unbuilt &&
           gr_perspective && level_render_mode != LEVEL_RENDER_BRUSHES_ONLY && !view_see_through && !view_room_colors;
}

bool transforming(const CDedLevel& level)
{
    return gizmo_dragging() || level.transform_in_progress;
}

// Every brush's node, placement, shape and hiding, so a brush added, moved or hidden without an undo step is
// noticed; edits inside a shape push an undo step, and a Build ends with brush_preview_build_ended
uint32_t brushes_fingerprint(CDedLevel& level)
{
    uint32_t hash = 2166136261u;
    auto mix = [&](const void* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            hash = (hash ^ static_cast<const uint8_t*>(data)[i]) * 16777619u;
        }
    };
    level.for_each_brush([&](BrushNode& brush) {
        const BrushNode* node = &brush;
        const auto* geometry = static_cast<const GSolid*>(brush.geometry);
        const int faces = geometry ? geometry->face_list_count : -1;
        const bool hidden = brush.state == BRUSH_STATE_HIDDEN;
        mix(&node, sizeof(node));
        mix(&brush.pos, sizeof(brush.pos));
        mix(&brush.orient, sizeof(brush.orient));
        mix(&geometry, sizeof(geometry));
        mix(&faces, sizeof(faces));
        mix(&brush.is_portal, sizeof(brush.is_portal));
        mix(&hidden, sizeof(hidden));
    });
    return hash;
}

// What the preview shows, worked out again only when what it was worked out from changes
struct Preview
{
    bool valid = false;
    const UndoEntry* undo_top = nullptr;
    int undo_size = 0;
    const UndoEntry* redo_top = nullptr;
    int redo_size = 0;
    bool transforming = false;
    uint32_t fingerprint = 0;
    std::vector<const BrushNode*> brushes; // drawn textured, sorted
    std::vector<int> hidden_face_ids;      // sorted
};
Preview g_preview;

// The level geometry face ids left out, sorted; brought up to date at each 3D view paint
std::vector<int> g_hidden_face_ids;

// Changed since the last Build: a face never built, or one the level geometry built from it no longer lies on
bool brush_changed_since_build(BrushNode& brush, const std::unordered_map<int, std::vector<GFace*>>& built)
{
    auto* geometry = static_cast<GSolid*>(brush.geometry);
    return geometry && geometry->for_each_face([&](GFace& face) {
        if (face.face_id < 0) {
            return true;
        }
        const auto it = built.find(face.face_id);
        return it != built.end() && !level_faces_lie_on_brush_face(brush, face, it->second);
    });
}

// Brushes changed since the last Build, and the selected ones while Brush or Group mode transforms them, but not hidden
// ones, portals, nor moving group brushes, which stock draws textured already. Their face ids are left out of the
// level geometry, less those another brush has too.
void refresh_preview(CDedLevel& level)
{
    // a Build numbers the brush faces again on its first tick: the preview stays as it was until the Build ends
    if (level.build_running) {
        return;
    }
    Preview key;
    key.undo_top = undo_stack_top(level.undo_stack);
    key.undo_size = level.undo_stack.size;
    key.redo_top = undo_stack_top(level.redo_stack);
    key.redo_size = level.redo_stack.size;
    key.transforming = transforming(level);
    // a transform moves its brushes each frame, and its start and end change the key
    key.fingerprint = key.transforming ? 0 : brushes_fingerprint(level);
    if (g_preview.valid && key.undo_top == g_preview.undo_top && key.undo_size == g_preview.undo_size &&
        key.redo_top == g_preview.redo_top && key.redo_size == g_preview.redo_size &&
        key.transforming == g_preview.transforming && key.fingerprint == g_preview.fingerprint) {
        return;
    }
    const auto built = level.solid->faces_by_id();
    const bool moving_selection =
        key.transforming && (level.edit_mode == DedEditMode::Brush || level.edit_mode == DedEditMode::Group);
    std::vector<GSolid*> others;
    level.for_each_brush([&](BrushNode& brush) {
        auto* geometry = static_cast<GSolid*>(brush.geometry);
        if (!geometry) {
            return;
        }
        const bool previewed = brush.state != BRUSH_STATE_HIDDEN && !brush.is_portal &&
                               ((moving_selection && brush.state == BRUSH_STATE_SELECTED) ||
                                brush_changed_since_build(brush, built)) &&
                               !level.brush_in_moving_group(&brush);
        if (!previewed) {
            others.push_back(geometry);
            return;
        }
        key.brushes.push_back(&brush);
        geometry->for_each_face([&](GFace& face) {
            if (face.face_id >= 0) {
                key.hidden_face_ids.push_back(face.face_id);
            }
            return false;
        });
    });
    std::sort(key.brushes.begin(), key.brushes.end());
    std::vector<int>& ids = key.hidden_face_ids;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    if (!ids.empty()) {
        std::vector<bool> shared(ids.size());
        for (GSolid* geometry : others) {
            geometry->for_each_face([&](GFace& face) {
                const auto it = std::lower_bound(ids.begin(), ids.end(), face.face_id);
                if (it != ids.end() && *it == face.face_id) {
                    shared[it - ids.begin()] = true;
                }
                return false;
            });
        }
        size_t kept = 0;
        for (size_t i = 0; i < ids.size(); ++i) {
            if (!shared[i]) {
                ids[kept++] = ids[i];
            }
        }
        ids.resize(kept);
    }
    key.valid = true;
    g_preview = std::move(key);
}

bool preview_current(const CDedLevel& level)
{
    return preview_shows(level) && g_preview.valid;
}

bool previewed(const CDedLevel& level, const BrushNode& brush)
{
    return preview_current(level) && std::binary_search(g_preview.brushes.begin(), g_preview.brushes.end(), &brush);
}

bool hides_face(const GFace* face)
{
    return face && !g_hidden_face_ids.empty() &&
           std::binary_search(g_hidden_face_ids.begin(), g_hidden_face_ids.end(), face->face_id);
}

// FUN_0041e4c0's draw of a selected brush, in red lines: the preview draws it textured first
CallHook<void __fastcall(GSolid*, int, char, int, char, const Vector3*, const Matrix3*, int, int, int)>
    selected_brush_draw_hook{
    0x0041EA8F,
    [](GSolid* solid, int edx, char outline, int face_outlines, char vertex_marks, const Vector3* pos,
       const Matrix3* orient, int r, int g, int b) FASTCALL_LAMBDA {
        // pos and orient are the brush's own
        const auto* brush =
            reinterpret_cast<const BrushNode*>(reinterpret_cast<const char*>(pos) - offsetof(BrushNode, pos));
        const CDedLevel* level = CDedLevel::Get();
        if (level && previewed(*level, *brush)) {
            selected_brush_draw_hook.call_target(solid, edx, 0, 0, 0, pos, orient, 255, 255, 255);
        }
        selected_brush_draw_hook.call_target(solid, edx, outline, face_outlines, vertex_marks, pos, orient, r, g, b);
    },
};

// FUN_0041e4c0's other brushes, past the view's culling (ESI the brush, EBP the level). Stock leaves them out of the
// 3D view in Brush and Object mode, so the preview draws them here.
CodeInjection unselected_brush_draw_injection{
    0x0041EB52,
    [](auto& regs) {
        auto* brush = reinterpret_cast<BrushNode*>(static_cast<uintptr_t>(regs.esi));
        const auto* level = reinterpret_cast<const CDedLevel*>(static_cast<uintptr_t>(regs.ebp));
        if (previewed(*level, *brush)) {
            auto* geometry = static_cast<GSolid*>(brush->geometry);
            geometry->draw(false, false, false, &brush->pos, &brush->orient, 255, 255, 255);
        }
    },
};

// FUN_0049b550 gathers a room's faces into its render cache (ESI the face); a left out face is skipped as an
// invisible one is
CodeInjection room_cache_face_injection{
    0x0049B670,
    [](auto& regs) {
        if (hides_face(reinterpret_cast<const GFace*>(static_cast<uintptr_t>(regs.esi)))) {
            regs.eip = 0x0049B6EC;
        }
    },
};
// and its detail rooms' faces
CodeInjection room_cache_detail_face_injection{
    0x0049B799,
    [](auto& regs) {
        if (hides_face(reinterpret_cast<const GFace*>(static_cast<uintptr_t>(regs.esi)))) {
            regs.eip = 0x0049B820;
        }
    },
};
// A room's see-through faces, drawn after the caches (ESI the face)
CodeInjection room_alpha_face_injection{
    0x00424809,
    [](auto& regs) {
        if (hides_face(reinterpret_cast<const GFace*>(static_cast<uintptr_t>(regs.esi)))) {
            regs.eip = 0x0042482C;
        }
    },
};

} // namespace

void brush_preview_view_paint(CDedLevel& level)
{
    try {
        if (preview_shows(level)) {
            refresh_preview(level);
        }
        static const std::vector<int> none;
        const std::vector<int>& ids = preview_current(level) ? g_preview.hidden_face_ids : none;
        if (ids != g_hidden_face_ids) {
            g_hidden_face_ids = ids;
            geo_cache_flush_all();
        }
    }
    catch (const std::exception& e) {
        xlog::error("[Preview] {}", e.what());
        g_preview = {};
        if (!g_hidden_face_ids.empty()) {
            g_hidden_face_ids.clear();
            geo_cache_flush_all();
        }
    }
}

void brush_preview_build_started(CDedLevel& level)
{
    // brought up to date before the Build numbers the faces again, whatever was edited since it last showed
    try {
        if (gizmo_textured_preview() && level.solid) {
            refresh_preview(level);
        }
        else {
            g_preview = {};
        }
    }
    catch (const std::exception& e) {
        xlog::error("[Preview] {}", e.what());
        g_preview = {};
    }
}

void brush_preview_level_reset()
{
    g_preview = {};
    g_face_ids_unbuilt = false;
}

void brush_preview_build_ended(bool completed)
{
    g_preview = {};
    g_face_ids_unbuilt = !completed;
}

bool brush_preview_hides(const GFace& face)
{
    return hides_face(&face);
}

void ApplyBrushPreviewPatches()
{
    selected_brush_draw_hook.install();
    unselected_brush_draw_injection.install();
    room_cache_face_injection.install();
    room_cache_detail_face_injection.install();
    room_alpha_face_injection.install();
}
