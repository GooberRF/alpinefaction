#include <cstring>
#include <cmath>
#include <vector>
#include <set>
#include <algorithm>
#include <windows.h>
#include <patch_common/MemUtils.h>
#include <xlog/xlog.h>
#include "geometry.h"
#include "level.h"
#include "mfc_types.h"
#include "resources.h"


// Recompute a GSolid's bounding box from its face bounding boxes.
static void recompute_solid_bbox(GSolid* solid)
{
    solid->bbox_min = {1e30f, 1e30f, 1e30f};
    solid->bbox_max = {-1e30f, -1e30f, -1e30f};
    GFace* f = solid->face_list_head;
    while (f) {
        solid->bbox_min.x = std::min(solid->bbox_min.x, f->bounding_box_min.x);
        solid->bbox_min.y = std::min(solid->bbox_min.y, f->bounding_box_min.y);
        solid->bbox_min.z = std::min(solid->bbox_min.z, f->bounding_box_min.z);
        solid->bbox_max.x = std::max(solid->bbox_max.x, f->bounding_box_max.x);
        solid->bbox_max.y = std::max(solid->bbox_max.y, f->bounding_box_max.y);
        solid->bbox_max.z = std::max(solid->bbox_max.z, f->bounding_box_max.z);
        f = f->next_solid;
    }
}

// Recompute a GFace's bounding box from its edge loop vertices.
static void recompute_face_bbox(GFace* face)
{
    GFaceVertex* fv = face->edge_loop;
    if (!fv) return;
    face->bounding_box_min = fv->vertex->pos;
    face->bounding_box_max = fv->vertex->pos;
    GFaceVertex* cur = fv->next;
    while (cur != fv) {
        auto& p = cur->vertex->pos;
        face->bounding_box_min.x = std::min(face->bounding_box_min.x, p.x);
        face->bounding_box_min.y = std::min(face->bounding_box_min.y, p.y);
        face->bounding_box_min.z = std::min(face->bounding_box_min.z, p.z);
        face->bounding_box_max.x = std::max(face->bounding_box_max.x, p.x);
        face->bounding_box_max.y = std::max(face->bounding_box_max.y, p.y);
        face->bounding_box_max.z = std::max(face->bounding_box_max.z, p.z);
        cur = cur->next;
    }
}

// ============================================================================
// Brush mode: Mirror
// ============================================================================

static int g_mirror_axis = 0; // 0=X, 1=Y, 2=Z

static INT_PTR CALLBACK MirrorDlgProc(HWND hdlg, UINT msg, WPARAM wParam, LPARAM)
{
    switch (msg) {
        case WM_INITDIALOG:
            CheckRadioButton(hdlg, IDC_MIRROR_X, IDC_MIRROR_Z,
                             IDC_MIRROR_X + g_mirror_axis);
            return TRUE;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                if (IsDlgButtonChecked(hdlg, IDC_MIRROR_X) == BST_CHECKED) g_mirror_axis = 0;
                else if (IsDlgButtonChecked(hdlg, IDC_MIRROR_Y) == BST_CHECKED) g_mirror_axis = 1;
                else g_mirror_axis = 2;
                EndDialog(hdlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                EndDialog(hdlg, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

// Mirror a single brush's geometry across the world origin on the given axis.
static void mirror_brush(BrushNode* brush, int axis, std::set<GVertex*>& mirrored_verts)
{
    auto* solid = static_cast<GSolid*>(brush->geometry);

    GFace* face = solid->face_list_head;
    while (face) {
        // Mirror vertices referenced by this face
        GFaceVertex* fv = face->edge_loop;
        if (fv) {
            GFaceVertex* start = fv;
            do {
                if (!mirrored_verts.count(fv->vertex)) {
                    mirrored_verts.insert(fv->vertex);
                    reinterpret_cast<float*>(&fv->vertex->pos)[axis] = -reinterpret_cast<float*>(&fv->vertex->pos)[axis];
                }
                fv = fv->next;
            } while (fv != start);
        }

        // Reverse the face winding order (reflection flips handedness)
        fv = face->edge_loop;
        if (fv) {
            GFaceVertex* start = fv;
            do {
                GFaceVertex* tmp = fv->next;
                fv->next = fv->prev;
                fv->prev = tmp;
                fv = tmp;
            } while (fv != start);
        }

        // Negate normal axis component and recompute plane dist
        reinterpret_cast<float*>(&face->plane.normal)[axis] = -reinterpret_cast<float*>(&face->plane.normal)[axis];
        fv = face->edge_loop;
        if (fv) {
            auto& n = face->plane.normal;
            auto& p = fv->vertex->pos;
            face->plane.dist = -(n.x * p.x + n.y * p.y + n.z * p.z);
        }

        recompute_face_bbox(face);

        face = face->next_solid;
    }

    recompute_solid_bbox(solid);

    // Mirror brush position and orientation across world origin
    reinterpret_cast<float*>(&brush->pos)[axis] = -reinterpret_cast<float*>(&brush->pos)[axis];

    // R' = F * R * F: negate elements where exactly one of (row, col) equals the axis
    float* orient = reinterpret_cast<float*>(&brush->orient);
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            if ((i == axis) != (j == axis)) {
                orient[i * 3 + j] = -orient[i * 3 + j];
            }
        }
    }
}

// Mirror a DedObject's position and orientation across the world origin on the given axis.
static void mirror_object(DedObject* obj, int axis)
{
    reinterpret_cast<float*>(&obj->pos)[axis] = -reinterpret_cast<float*>(&obj->pos)[axis];

    // R' = R * F: negate column `axis` to reflect each basis vector across the mirror plane
    float* orient = reinterpret_cast<float*>(&obj->orient);
    for (int i = 0; i < 3; i++) {
        orient[i * 3 + axis] = -orient[i * 3 + axis];
    }

    // For lights, sync the updated position/orient to the level_light object
    // so lightmap calculation uses the new values
    if (obj->type == DedObjectType::DED_LIGHT) {
        DedLight_UpdateLevelLight(obj);
    }
}

// Collect selected brushes from the level's brush list.
static std::vector<BrushNode*> collect_selected_brushes(CDedLevel* level)
{
    std::vector<BrushNode*> selected;
    BrushNode* head = level->brush_list;
    if (!head) return selected;
    BrushNode* b = head;
    do {
        if (b->state == BRUSH_STATE_SELECTED && b->geometry) {
            selected.push_back(b);
        }
        b = b->next;
    } while (b != head);
    return selected;
}

void handle_brush_mirror()
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto selected = collect_selected_brushes(level);
    if (selected.empty()) {
        show_error_message("You must select at least one brush to mirror.");
        return;
    }

    // Show mirror axis dialog
    HWND main_wnd = GetMainFrameHandle();
    INT_PTR result = DialogBoxParam(g_module, MAKEINTRESOURCE(IDD_BRUSH_MIRROR),
                                    main_wnd, MirrorDlgProc, 0);
    if (result != IDOK) return;

    int axis = g_mirror_axis; // 0=X, 1=Y, 2=Z

    level->create_undo_snapshot();

    // Re-collect after undo snapshot (undo clones brushes)
    selected = collect_selected_brushes(level);

    std::set<GVertex*> mirrored_verts;
    for (BrushNode* brush : selected) {
        mirror_brush(brush, axis, mirrored_verts);
    }

    level->mark_geometry_dirty();
    redraw_all_viewports();

    const char* axis_names[] = {"X", "Y", "Z"};
    LogDlg_Append(GetLogDlg(), "Mirrored %d brush(es) across %s axis.",
                  static_cast<int>(selected.size()), axis_names[axis]);
}

void handle_group_mirror()
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto selected_brushes = collect_selected_brushes(level);
    auto& selected_objects = level->selection;

    if (selected_brushes.empty() && selected_objects.size == 0) {
        show_error_message("You must select at least one brush or object to mirror.");
        return;
    }

    // Show mirror axis dialog
    HWND main_wnd = GetMainFrameHandle();
    INT_PTR result = DialogBoxParam(g_module, MAKEINTRESOURCE(IDD_BRUSH_MIRROR),
                                    main_wnd, MirrorDlgProc, 0);
    if (result != IDOK) return;

    int axis = g_mirror_axis; // 0=X, 1=Y, 2=Z

    level->create_undo_snapshot();

    // Re-collect after undo snapshot (undo clones brushes)
    selected_brushes = collect_selected_brushes(level);

    // Mirror brushes
    std::set<GVertex*> mirrored_verts;
    for (BrushNode* brush : selected_brushes) {
        mirror_brush(brush, axis, mirrored_verts);
    }

    // Mirror objects
    int num_objects = 0;
    auto& sel = level->selection;
    for (int i = 0; i < sel.size; i++) {
        mirror_object(sel.data_ptr[i], axis);
        num_objects++;
    }

    level->mark_geometry_dirty();
    redraw_all_viewports();

    const char* axis_names[] = {"X", "Y", "Z"};
    LogDlg_Append(GetLogDlg(), "Mirrored %d brush(es) and %d object(s) across %s axis.",
                  static_cast<int>(selected_brushes.size()), num_objects, axis_names[axis]);
}

// ============================================================================
// Face mode: Delete
// ============================================================================

void handle_face_delete()
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    if (!level->has_face_selection()) {
        show_error_message("You must select a face to delete.");
        return;
    }

    level->create_undo_snapshot();

    BrushNode* head = level->brush_list;
    BrushNode* brush = head;
    if (!brush) return;

    int total_deleted = 0;
    int total_verts_removed = 0;

    do {
        auto* solid = static_cast<GSolid*>(brush->geometry);
        if (solid) {
            auto& sel = solid->face_selection;

            if (sel.size == 0) {
                brush = brush->next;
                continue;
            }

            // Collect vertices referenced by selected faces before deletion
            std::vector<GVertex*> face_verts;
            for (int i = 0; i < sel.size; i++) {
                GFace* face = sel.data_ptr[i];
                GFaceVertex* fv = face->edge_loop;
                if (fv) {
                    GFaceVertex* start = fv;
                    do {
                        face_verts.push_back(fv->vertex);
                        fv = fv->next;
                    } while (fv != start);
                }
            }

            // Delete selected faces
            for (int i = sel.size - 1; i >= 0; i--) {
                GFace* face = sel.data_ptr[i];
                solid->remove_face(face);
                GFace::destroy(face);
                total_deleted++;
            }
            sel.size = 0;

            // Build set of vertices still referenced by remaining faces
            std::set<GVertex*> referenced_verts;
            GFace* f = solid->face_list_head;
            while (f) {
                GFaceVertex* fv = f->edge_loop;
                if (fv) {
                    GFaceVertex* start = fv;
                    do {
                        referenced_verts.insert(fv->vertex);
                        fv = fv->next;
                    } while (fv != start);
                }
                f = f->next_solid;
            }

            // Remove orphaned vertices (no longer referenced by any remaining face)
            std::sort(face_verts.begin(), face_verts.end());
            face_verts.erase(std::unique(face_verts.begin(), face_verts.end()), face_verts.end());

            for (GVertex* vertex : face_verts) {
                if (referenced_verts.find(vertex) == referenced_verts.end()) {
                    solid->remove_vertex(vertex);
                    total_verts_removed++;
                }
            }

            recompute_solid_bbox(solid);
        }
        brush = brush->next;
    } while (brush != head);

    level->mark_geometry_dirty();
    redraw_all_viewports();
    LogDlg_Append(GetLogDlg(), "Deleted %d face(s), removed %d orphaned vertex(es).",
                  total_deleted, total_verts_removed);
}

// ============================================================================
// Face mode: Delete Ext. (delete faces + all their vertices)
// ============================================================================

void handle_face_delete_ext()
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    if (!level->has_face_selection()) {
        show_error_message("You must select a face to delete.");
        return;
    }

    level->create_undo_snapshot();

    BrushNode* head = level->brush_list;
    BrushNode* brush = head;
    if (!brush) return;

    int total_deleted = 0;
    int total_verts_removed = 0;

    do {
        auto* solid = static_cast<GSolid*>(brush->geometry);
        if (solid) {
            auto& sel = solid->face_selection;

            if (sel.size == 0) {
                brush = brush->next;
                continue;
            }

            // Collect all GVertex* referenced by selected faces
            std::set<GVertex*> doomed_verts;
            for (int i = 0; i < sel.size; i++) {
                GFace* face = sel.data_ptr[i];
                GFaceVertex* fv = face->edge_loop;
                if (fv) {
                    GFaceVertex* start = fv;
                    do {
                        doomed_verts.insert(fv->vertex);
                        fv = fv->next;
                    } while (fv != start);
                }
            }

            // Delete selected faces
            for (int i = sel.size - 1; i >= 0; i--) {
                GFace* face = sel.data_ptr[i];
                solid->remove_face(face);
                GFace::destroy(face);
                total_deleted++;
            }
            sel.size = 0;

            // Also delete any other faces that reference doomed vertices
            GFace* face = solid->face_list_head;
            std::vector<GFace*> extra_faces;
            while (face) {
                GFace* next = face->next_solid;
                GFaceVertex* fv = face->edge_loop;
                if (fv) {
                    GFaceVertex* start = fv;
                    bool has_doomed = false;
                    do {
                        if (doomed_verts.count(fv->vertex)) { has_doomed = true; break; }
                        fv = fv->next;
                    } while (fv != start);
                    if (has_doomed) extra_faces.push_back(face);
                }
                face = next;
            }
            for (GFace* f : extra_faces) {
                solid->remove_face(f);
                GFace::destroy(f);
                total_deleted++;
            }

            // Remove doomed vertices from the vertex list
            for (GVertex* v : doomed_verts) {
                solid->remove_vertex(v);
                total_verts_removed++;
            }

            recompute_solid_bbox(solid);
        }
        brush = brush->next;
    } while (brush != head);

    level->mark_geometry_dirty();
    redraw_all_viewports();
    LogDlg_Append(GetLogDlg(), "Deleted %d face(s) and removed %d vertex(es).",
                  total_deleted, total_verts_removed);
}

// ============================================================================
// Face split helpers
// ============================================================================

struct SplitVert {
    Vector3 pos;
    float u, v, lm_u, lm_v;
    GVertex* gvertex; // existing GVertex* or nullptr for new (will be allocated)
};

// Allocate a GVertex (0x2C bytes) and set its position
static GVertex* alloc_gvertex(const Vector3& pos)
{
    auto* gv = static_cast<GVertex*>(editor_alloc(0x2C));
    if (!gv) return nullptr;
    std::memset(gv, 0, 0x2C);
    gv->pos = pos;
    return gv;
}

// Create a new GFace from a polygon of SplitVerts, copying attributes from original
static GFace* create_split_face(GSolid* solid, GFace* original, std::vector<SplitVert>& verts)
{
    if (verts.size() < 3) return nullptr;

    GFace* face = GFace::alloc();
    if (!face) return nullptr;
    std::memset(face, 0, 0x60);

    // Copy plane and attributes from original
    face->plane = original->plane;
    face->bounding_box_min = verts[0].pos;
    face->bounding_box_max = verts[0].pos;
    face->flags = original->flags;
    face->group_id = original->group_id;
    face->bitmap_id = original->bitmap_id;
    face->portal_id = original->portal_id;
    face->surface_index = original->surface_index;
    face->face_id = GFace::generate_uid();
    face->smoothing_groups = original->smoothing_groups;

    // Build edge_loop as circular doubly-linked list
    GFaceVertex* first = nullptr;
    GFaceVertex* prev = nullptr;
    bool alloc_failed = false;

    for (auto& sv : verts) {
        GFaceVertex* fv = GFaceVertex::alloc();
        if (!fv) { alloc_failed = true; break; }
        std::memset(fv, 0, sizeof(GFaceVertex));

        // Allocate a new GVertex for interpolated points (gvertex == nullptr)
        if (!sv.gvertex) {
            sv.gvertex = alloc_gvertex(sv.pos);
            if (!sv.gvertex) { GFaceVertex::free(fv); alloc_failed = true; break; }
        }
        fv->vertex = sv.gvertex;
        fv->u = sv.u;
        fv->v = sv.v;
        fv->lm_u = sv.lm_u;
        fv->lm_v = sv.lm_v;

        if (!first) first = fv;
        if (prev) {
            prev->next = fv;
            fv->prev = prev;
        }
        prev = fv;

        // Update bounding box
        face->bounding_box_min.x = std::min(face->bounding_box_min.x, sv.pos.x);
        face->bounding_box_min.y = std::min(face->bounding_box_min.y, sv.pos.y);
        face->bounding_box_min.z = std::min(face->bounding_box_min.z, sv.pos.z);
        face->bounding_box_max.x = std::max(face->bounding_box_max.x, sv.pos.x);
        face->bounding_box_max.y = std::max(face->bounding_box_max.y, sv.pos.y);
        face->bounding_box_max.z = std::max(face->bounding_box_max.z, sv.pos.z);
    }

    // On alloc failure, free any face vertices built so far and the face itself
    if (alloc_failed) {
        GFaceVertex* fv = first;
        while (fv) {
            GFaceVertex* nxt = fv->next;
            GFaceVertex::free(fv);
            fv = nxt;
        }
        GFace::destroy(face);
        return nullptr;
    }

    // Close circular list
    prev->next = first;
    first->prev = prev;
    face->edge_loop = first;

    // Register vertices in solid and prepend face to solid's face list
    for (auto& sv : verts) {
        solid->vertices.add_if_not_exists_raw(sv.gvertex);
    }
    face->next_solid = solid->face_list_head;
    solid->face_list_head = face;
    solid->face_list_count++;

    return face;
}

// Split a single face into (num_splits+1) faces along a local face axis.
static int split_face(GSolid* solid, GFace* face, int num_splits, bool along_x)
{
    // Collect edge_loop vertices
    std::vector<SplitVert> verts;
    GFaceVertex* start = face->edge_loop;
    if (!start) return 0;
    GFaceVertex* curr = start;
    do {
        verts.push_back({curr->vertex->pos, curr->u, curr->v, curr->lm_u, curr->lm_v, curr->vertex});
        curr = curr->next;
    } while (curr != start);

    int n = static_cast<int>(verts.size());
    if (n < 3) return 0;

    Vector3 normal = face->plane.normal;
    Vector3 split_axis;

    if (!along_x) {
        float dot = normal.y;
        split_axis = {-dot * normal.x, 1.0f - dot * normal.y, -dot * normal.z};
        float sa_len_y = std::sqrt(split_axis.x * split_axis.x + split_axis.y * split_axis.y +
                                   split_axis.z * split_axis.z);
        if (sa_len_y < 1e-6f) {
            // Normal is near-parallel to Y; fall back to projecting X onto the face plane
            dot = normal.x;
            split_axis = {1.0f - dot * normal.x, -dot * normal.y, -dot * normal.z};
        }
    } else {
        split_axis = {normal.z, 0.0f, -normal.x};
        float sa_len = std::sqrt(split_axis.x * split_axis.x + split_axis.z * split_axis.z);
        if (sa_len < 1e-6f) {
            float dot = normal.x;
            split_axis = {1.0f - dot * normal.x, -dot * normal.y, -dot * normal.z};
        }
    }

    float sa_len = std::sqrt(split_axis.x * split_axis.x + split_axis.y * split_axis.y +
                             split_axis.z * split_axis.z);
    if (sa_len < 1e-9f) return 0;
    split_axis.x /= sa_len; split_axis.y /= sa_len; split_axis.z /= sa_len;

    std::vector<float> proj(n);
    float min_val = 1e30f, max_val = -1e30f;
    for (int i = 0; i < n; i++) {
        proj[i] = verts[i].pos.x * split_axis.x +
                  verts[i].pos.y * split_axis.y +
                  verts[i].pos.z * split_axis.z;
        min_val = std::min(min_val, proj[i]);
        max_val = std::max(max_val, proj[i]);
    }
    if (max_val - min_val < 1e-6f) return 0;

    int faces_created = 0;
    std::vector<SplitVert> remaining = verts;
    std::vector<float> rem_proj = proj;

    for (int cut = 1; cut <= num_splits; cut++) {
        float t = static_cast<float>(cut) / static_cast<float>(num_splits + 1);
        float cut_val = min_val + t * (max_val - min_val);

        int rn = static_cast<int>(remaining.size());
        if (rn < 3) break;

        std::vector<bool> is_left(rn);
        for (int i = 0; i < rn; i++) {
            is_left[i] = rem_proj[i] < cut_val;
        }

        int cross1 = -1, cross2 = -1, num_crossings = 0;
        for (int i = 0; i < rn; i++) {
            int j = (i + 1) % rn;
            if (is_left[i] != is_left[j]) {
                if (num_crossings == 0) cross1 = i;
                else if (num_crossings == 1) cross2 = i;
                num_crossings++;
            }
        }
        if (num_crossings != 2) break;

        auto interp = [&](int i, int j) -> std::pair<SplitVert, float> {
            float frac = (cut_val - rem_proj[i]) / (rem_proj[j] - rem_proj[i]);
            SplitVert sv;
            sv.pos.x = remaining[i].pos.x + frac * (remaining[j].pos.x - remaining[i].pos.x);
            sv.pos.y = remaining[i].pos.y + frac * (remaining[j].pos.y - remaining[i].pos.y);
            sv.pos.z = remaining[i].pos.z + frac * (remaining[j].pos.z - remaining[i].pos.z);
            sv.u = remaining[i].u + frac * (remaining[j].u - remaining[i].u);
            sv.v = remaining[i].v + frac * (remaining[j].v - remaining[i].v);
            sv.lm_u = remaining[i].lm_u + frac * (remaining[j].lm_u - remaining[i].lm_u);
            sv.lm_v = remaining[i].lm_v + frac * (remaining[j].lm_v - remaining[i].lm_v);
            sv.gvertex = nullptr;
            return {sv, cut_val};
        };

        auto [sv1, p1] = interp(cross1, (cross1 + 1) % rn);
        auto [sv2, p2] = interp(cross2, (cross2 + 1) % rn);

        sv1.gvertex = alloc_gvertex(sv1.pos);
        sv2.gvertex = alloc_gvertex(sv2.pos);

        std::vector<SplitVert> poly_a, poly_b;
        std::vector<float> proj_a, proj_b;

        poly_a.push_back(sv1); proj_a.push_back(p1);
        for (int i = (cross1 + 1) % rn; ; i = (i + 1) % rn) {
            poly_a.push_back(remaining[i]); proj_a.push_back(rem_proj[i]);
            if (i == cross2) break;
        }
        poly_a.push_back(sv2); proj_a.push_back(p2);

        poly_b.push_back(sv2); proj_b.push_back(p2);
        for (int i = (cross2 + 1) % rn; ; i = (i + 1) % rn) {
            poly_b.push_back(remaining[i]); proj_b.push_back(rem_proj[i]);
            if (i == cross1) break;
        }
        poly_b.push_back(sv1); proj_b.push_back(p1);

        bool poly_a_is_left = rem_proj[(cross1 + 1) % rn] < cut_val;

        auto& left_poly = poly_a_is_left ? poly_a : poly_b;
        auto& right_poly = poly_a_is_left ? poly_b : poly_a;
        auto& right_proj = poly_a_is_left ? proj_b : proj_a;

        if (left_poly.size() >= 3) {
            GFace* new_face = create_split_face(solid, face, left_poly);
            if (new_face) faces_created++;
        }

        remaining = right_poly;
        rem_proj = right_proj;
    }

    // Emit the final remaining polygon
    if (remaining.size() >= 3 && faces_created > 0) {
        GFace* new_face = create_split_face(solid, face, remaining);
        if (new_face) faces_created++;
    }

    return faces_created;
}

// ============================================================================
// Face mode: Split
// ============================================================================

static int g_split_count = 1;
static bool g_split_along_u = true;

static INT_PTR CALLBACK SplitDlgProc(HWND hdlg, UINT msg, WPARAM wParam, LPARAM)
{
    switch (msg) {
        case WM_INITDIALOG:
            SetDlgItemInt(hdlg, IDC_SPLIT_AMOUNT, g_split_count, FALSE);
            CheckRadioButton(hdlg, IDC_SPLIT_ALONG_U, IDC_SPLIT_ALONG_V,
                             g_split_along_u ? IDC_SPLIT_ALONG_U : IDC_SPLIT_ALONG_V);
            return TRUE;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                g_split_count = GetDlgItemInt(hdlg, IDC_SPLIT_AMOUNT, nullptr, FALSE);
                if (g_split_count < 1) g_split_count = 1;
                g_split_along_u = (IsDlgButtonChecked(hdlg, IDC_SPLIT_ALONG_U) == BST_CHECKED);
                EndDialog(hdlg, IDOK);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL) {
                EndDialog(hdlg, IDCANCEL);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

void handle_face_split()
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    if (!level->has_face_selection()) {
        show_error_message("You must select a face to split.");
        return;
    }

    // Show split dialog
    HWND main_wnd = GetMainFrameHandle();
    INT_PTR result = DialogBoxParam(g_module, MAKEINTRESOURCE(IDD_FACE_SPLIT),
                                    main_wnd, SplitDlgProc, 0);
    if (result != IDOK) return;

    int num_splits = g_split_count;
    bool along_x = g_split_along_u;

    level->create_undo_snapshot();

    BrushNode* head = level->brush_list;
    BrushNode* brush = head;
    if (!brush) return;

    int total_created = 0;

    do {
        auto* solid = static_cast<GSolid*>(brush->geometry);
        if (solid) {
            auto& sel = solid->face_selection;

            bool modified = false;
            for (int i = sel.size - 1; i >= 0; i--) {
                GFace* face = sel.data_ptr[i];
                int created = split_face(solid, face, num_splits, along_x);
                if (created > 0) {
                    total_created += created;
                    solid->remove_face(face);
                    GFace::destroy(face);
                    modified = true;
                }
            }
            sel.size = 0;
            if (modified) recompute_solid_bbox(solid);
        }
        brush = brush->next;
    } while (brush != head);

    level->mark_geometry_dirty();
    redraw_all_viewports();
    LogDlg_Append(GetLogDlg(), "Split %d face(s) into %d (count=%d, direction=%s).",
                  total_created > 0 ? total_created / (num_splits + 1) : 0,
                  total_created, num_splits, along_x ? "X" : "Y");
}

// ============================================================================
// Face mode: Flip Normal
// ============================================================================

void handle_face_flip_normal()
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    if (!level->has_face_selection()) {
        show_error_message("You must select a face to flip.");
        return;
    }

    level->create_undo_snapshot();

    BrushNode* head = level->brush_list;
    BrushNode* brush = head;
    if (!brush) return;

    int total_flipped = 0;

    do {
        auto* solid = static_cast<GSolid*>(brush->geometry);
        if (solid) {
            auto& sel = solid->face_selection;
            for (int i = 0; i < sel.size; i++) {
                GFace* face = sel.data_ptr[i];

                face->plane.normal.x = -face->plane.normal.x;
                face->plane.normal.y = -face->plane.normal.y;
                face->plane.normal.z = -face->plane.normal.z;
                face->plane.dist = -face->plane.dist;

                GFaceVertex* fv = face->edge_loop;
                if (fv) {
                    GFaceVertex* start_fv = fv;
                    do {
                        GFaceVertex* tmp = fv->next;
                        fv->next = fv->prev;
                        fv->prev = tmp;
                        fv = tmp;
                    } while (fv != start_fv);
                }

                total_flipped++;
            }
        }
        brush = brush->next;
    } while (brush != head);

    level->mark_geometry_dirty();
    redraw_all_viewports();
    LogDlg_Append(GetLogDlg(), "Flipped normal on %d face(s).", total_flipped);
}

// ============================================================================
// Vertex mode: Delete
// ============================================================================

void handle_vertex_delete()
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    if (!level->has_vertex_selection()) {
        show_error_message("You must select a vertex to delete.");
        return;
    }

    level->create_undo_snapshot();

    // Re-read brush list after undo snapshot (matches face delete pattern)
    BrushNode* head = level->brush_list;
    BrushNode* brush = head;
    if (!brush) return;

    int total_verts_deleted = 0;
    int total_faces_deleted = 0;

    do {
        auto* solid = static_cast<GSolid*>(brush->geometry);
        if (solid) {
            auto& sel = solid->vertex_selection;

            if (sel.size == 0) {
                brush = brush->next;
                continue;
            }

            // Collect selected vertices into a set
            std::set<GVertex*> doomed_verts;
            for (int i = 0; i < sel.size; i++) {
                doomed_verts.insert(sel.data_ptr[i]);
            }

            // For each face, check if it references a doomed vertex
            GFace* face = solid->face_list_head;
            std::vector<GFace*> faces_to_delete;

            while (face) {
                GFace* next_face = face->next_solid;
                GFaceVertex* fv = face->edge_loop;
                if (!fv) { face = next_face; continue; }

                // Count total vertices and doomed vertices in this face
                int total_in_face = 0;
                int doomed_in_face = 0;
                GFaceVertex* start = fv;
                do {
                    total_in_face++;
                    if (doomed_verts.count(fv->vertex)) doomed_in_face++;
                    fv = fv->next;
                } while (fv != start);

                if (doomed_in_face > 0) {
                    int remaining = total_in_face - doomed_in_face;
                    if (remaining < 3) {
                        // Face will be degenerate — delete it
                        faces_to_delete.push_back(face);
                    } else {
                        // Remove doomed vertices from this face's edge loop
                        fv = face->edge_loop;
                        while (doomed_verts.count(fv->vertex)) {
                            fv = fv->next;
                        }
                        face->edge_loop = fv;

                        GFaceVertex* cur = fv;
                        do {
                            GFaceVertex* nxt = cur->next;
                            if (doomed_verts.count(nxt->vertex) && nxt != fv) {
                                cur->next = nxt->next;
                                nxt->next->prev = cur;
                                GFaceVertex::free(nxt);
                            } else {
                                cur = nxt;
                            }
                        } while (cur->next != fv);

                        recompute_face_bbox(face);
                    }
                }

                face = next_face;
            }

            // Delete degenerate faces
            for (GFace* f : faces_to_delete) {
                solid->remove_face(f);
                GFace::destroy(f);
                total_faces_deleted++;
            }

            // Remove doomed vertices from the brush vertex list
            for (GVertex* v : doomed_verts) {
                solid->remove_vertex(v);
                total_verts_deleted++;
            }

            recompute_solid_bbox(solid);
            sel.size = 0;
        }
        brush = brush->next;
    } while (brush != head);

    level->mark_geometry_dirty();
    redraw_all_viewports();
    LogDlg_Append(GetLogDlg(), "Deleted %d vertex(es), removed %d degenerate face(s).",
                  total_verts_deleted, total_faces_deleted);
}

// ============================================================================
// Vertex mode: Bridge (create face between selected vertices)
// ============================================================================

void handle_vertex_bridge()
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    // Find the brush(es) with selected vertices
    BrushNode* head = level->brush_list;
    BrushNode* brush = head;
    if (!brush) return;

    BrushNode* target_brush = nullptr;
    int brushes_with_selection = 0;

    do {
        auto* solid = static_cast<GSolid*>(brush->geometry);
        if (solid && solid->vertex_selection.size > 0) {
            brushes_with_selection++;
            target_brush = brush;
        }
        brush = brush->next;
    } while (brush != head);

    if (brushes_with_selection == 0) {
        show_error_message("You must select at least 3 vertices to bridge.");
        return;
    }
    if (brushes_with_selection > 1) {
        show_error_message("Cannot bridge: selected vertices are on different brushes.");
        return;
    }

    {
        auto* solid = static_cast<GSolid*>(target_brush->geometry);
        if (!solid || solid->vertex_selection.size < 3) {
            show_error_message("You must select at least 3 vertices to bridge.");
            return;
        }
    }

    level->create_undo_snapshot();

    // Re-find target brush after undo snapshot. The undo function clones the brush,
    // replaces it in the brush list, and stores the original in the undo record.
    target_brush = nullptr;
    head = level->brush_list;
    brush = head;
    if (!brush) return;
    do {
        auto* solid = static_cast<GSolid*>(brush->geometry);
        if (solid && solid->vertex_selection.size > 0) {
            target_brush = brush;
            break;
        }
        brush = brush->next;
    } while (brush != head);
    if (!target_brush) return;

    auto* solid = static_cast<GSolid*>(target_brush->geometry);
    auto& sel = solid->vertex_selection;

    // Collect vertex pointers
    std::vector<GVertex*> verts;
    for (int i = 0; i < sel.size; i++) {
        verts.push_back(sel.data_ptr[i]);
    }

    // Compute centroid
    Vector3 centroid{0, 0, 0};
    for (GVertex* v : verts) {
        centroid.x += v->pos.x;
        centroid.y += v->pos.y;
        centroid.z += v->pos.z;
    }
    float inv_n = 1.0f / static_cast<float>(verts.size());
    centroid.x *= inv_n;
    centroid.y *= inv_n;
    centroid.z *= inv_n;

    // Compute normal from first 3 vertices using cross product
    auto* p0 = &verts[0]->pos;
    auto* p1 = &verts[1]->pos;
    auto* p2 = &verts[2]->pos;
    Vector3 e1{p1->x - p0->x, p1->y - p0->y, p1->z - p0->z};
    Vector3 e2{p2->x - p0->x, p2->y - p0->y, p2->z - p0->z};
    Vector3 normal{
        e1.y * e2.z - e1.z * e2.y,
        e1.z * e2.x - e1.x * e2.z,
        e1.x * e2.y - e1.y * e2.x
    };
    float len = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
    if (len < 1e-9f) {
        show_error_message("Cannot bridge: vertices are collinear.");
        return;
    }
    normal.x /= len;
    normal.y /= len;
    normal.z /= len;

    // Check neighboring faces' normals to ensure consistent orientation.
    // If all faces sharing a vertex with the bridge have the same normal direction,
    // flip our normal to match if needed.
    {
        std::set<GVertex*> vert_set(verts.begin(), verts.end());
        int pos_count = 0, neg_count = 0;
        GFace* face = solid->face_list_head;
        while (face) {
            GFaceVertex* fv = face->edge_loop;
            if (fv) {
                bool shares_vertex = false;
                GFaceVertex* start = fv;
                do {
                    if (vert_set.count(fv->vertex)) { shares_vertex = true; break; }
                    fv = fv->next;
                } while (fv != start);
                if (shares_vertex) {
                    float dot = normal.x * face->plane.normal.x +
                                normal.y * face->plane.normal.y +
                                normal.z * face->plane.normal.z;
                    if (dot > 0.0f) pos_count++;
                    else if (dot < 0.0f) neg_count++;
                }
            }
            face = face->next_solid;
        }
        if (neg_count > 0 && pos_count == 0) {
            normal.x = -normal.x;
            normal.y = -normal.y;
            normal.z = -normal.z;
        }
    }

    // Sort vertices in winding order around the centroid, projected onto the face plane
    Vector3 ref_dir{p0->x - centroid.x, p0->y - centroid.y, p0->z - centroid.z};
    float ref_len = std::sqrt(ref_dir.x * ref_dir.x + ref_dir.y * ref_dir.y + ref_dir.z * ref_dir.z);
    if (ref_len > 1e-9f) {
        ref_dir.x /= ref_len; ref_dir.y /= ref_len; ref_dir.z /= ref_len;
    }
    Vector3 perp{
        normal.y * ref_dir.z - normal.z * ref_dir.y,
        normal.z * ref_dir.x - normal.x * ref_dir.z,
        normal.x * ref_dir.y - normal.y * ref_dir.x
    };

    std::vector<std::pair<float, GVertex*>> angle_verts;
    for (GVertex* v : verts) {
        float dx = v->pos.x - centroid.x;
        float dy = v->pos.y - centroid.y;
        float dz = v->pos.z - centroid.z;
        float u = dx * ref_dir.x + dy * ref_dir.y + dz * ref_dir.z;
        float w = dx * perp.x + dy * perp.y + dz * perp.z;
        float angle = std::atan2(w, u);
        angle_verts.push_back({angle, v});
    }
    std::sort(angle_verts.begin(), angle_verts.end());

    // Create a new GFace
    GFace* new_face = GFace::alloc();
    if (!new_face) return;

    std::memset(new_face, 0, sizeof(GFace));
    new_face->plane.normal = normal;
    new_face->plane.dist = -(normal.x * p0->x + normal.y * p0->y + normal.z * p0->z);

    // Copy texture from first face in the solid
    GFace* ref_face = solid->face_list_head;
    if (ref_face) {
        new_face->bitmap_id = ref_face->bitmap_id;
        new_face->smoothing_groups = ref_face->smoothing_groups;
        new_face->flags = ref_face->flags;
    }

    new_face->face_id = GFace::generate_uid();

    // Build edge loop from sorted vertices
    GFaceVertex* first_fv = nullptr;
    GFaceVertex* prev_fv = nullptr;
    int fv_count = 0;
    bool alloc_failed = false;
    for (auto& [angle, v] : angle_verts) {
        auto* fv = GFaceVertex::alloc();
        if (!fv) { alloc_failed = true; break; }
        std::memset(fv, 0, sizeof(GFaceVertex));
        fv->vertex = v;
        fv->u = v->pos.x * 0.03125f;
        fv->v = v->pos.z * 0.03125f;

        if (!first_fv) {
            first_fv = fv;
        }
        if (prev_fv) {
            prev_fv->next = fv;
            fv->prev = prev_fv;
        }
        prev_fv = fv;
        fv_count++;
    }

    // On failure or degenerate result, free allocated face vertices and the face
    if (alloc_failed || fv_count < 3) {
        GFaceVertex* fv = first_fv;
        while (fv) {
            GFaceVertex* nxt = fv->next;
            GFaceVertex::free(fv);
            fv = nxt;
        }
        GFace::destroy(new_face);
        return;
    }

    // Close the circular list
    prev_fv->next = first_fv;
    first_fv->prev = prev_fv;
    new_face->edge_loop = first_fv;

    // Register vertices in the solid and compute bounding box
    new_face->bounding_box_min = first_fv->vertex->pos;
    new_face->bounding_box_max = first_fv->vertex->pos;
    GFaceVertex* fv = first_fv;
    do {
        solid->vertices.add_if_not_exists_raw(fv->vertex);
        auto& p = fv->vertex->pos;
        new_face->bounding_box_min.x = std::min(new_face->bounding_box_min.x, p.x);
        new_face->bounding_box_min.y = std::min(new_face->bounding_box_min.y, p.y);
        new_face->bounding_box_min.z = std::min(new_face->bounding_box_min.z, p.z);
        new_face->bounding_box_max.x = std::max(new_face->bounding_box_max.x, p.x);
        new_face->bounding_box_max.y = std::max(new_face->bounding_box_max.y, p.y);
        new_face->bounding_box_max.z = std::max(new_face->bounding_box_max.z, p.z);
        fv = fv->next;
    } while (fv != first_fv);

    // Add face to solid's face list (prepend)
    new_face->next_solid = solid->face_list_head;
    solid->face_list_head = new_face;
    solid->face_list_count++;
    recompute_solid_bbox(solid);

    sel.size = 0;
    level->mark_geometry_dirty();
    redraw_all_viewports();
    LogDlg_Append(GetLogDlg(), "Created bridge face with %d vertices.", static_cast<int>(verts.size()));
}
