#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include <windows.h>
#include <xlog/xlog.h>
#include "brush_import.h"
#include "level.h"
#include "mesh.h"
#include "mfc_types.h"
#include "resources.h"
#include "vtypes.h"

extern HMODULE g_module;

namespace
{

// Above this the CSG stage starts choking on a single brush, so the prompt says so.
constexpr int tri_warn_threshold = 5000;

struct SourceTri
{
    int v[3];          // indices into SourceGeom::positions
    float uv[3][2];
    int bitmap_id;
    bool double_sided;
};

struct SourceGeom
{
    std::vector<Vector3> positions;
    std::vector<SourceTri> tris;
    int degenerate = 0;
    int double_sided = 0;
    bool missing_texture = false;
    bool flattened_normals = false;
};

struct ConversionStats
{
    int meshes = 0;
    int brushes = 0;
    int faces = 0;
    int reversed_copies = 0;
    int double_sided = 0;
    int degenerate = 0;
    int vertices = 0;
    bool missing_texture = false;
    bool flattened_normals = false;
};

// ─── Vertex welding ─────────────────────────────────────────────────────────

// Welding is by the exact IEEE bit pattern of each component, with -0.0 folded onto +0.0. That is
// the same "same position" relation the v3m format uses for its own same_pos_vertex_offsets table
// (mesh_export.cpp compares positions with memcmp), so a mesh this editor exported from a brush
// welds back to exactly the corner set the brush had. An epsilon weld would instead depend on
// visit order and could collapse two genuinely distinct corners of a dense mesh.
struct WeldKey
{
    std::uint32_t x, y, z;

    bool operator<(const WeldKey& o) const
    {
        if (x != o.x) return x < o.x;
        if (y != o.y) return y < o.y;
        return z < o.z;
    }
};

std::uint32_t float_bits(float f)
{
    if (f == 0.0f) f = 0.0f; // -0.0 and +0.0 are the same point
    std::uint32_t bits;
    std::memcpy(&bits, &f, sizeof(bits));
    return bits;
}

// ─── Source geometry ────────────────────────────────────────────────────────

const EditorV3d* mesh_static_v3d(DedMesh* mesh)
{
    if (!mesh) {
        return nullptr;
    }
    if (!mesh->vmesh && !mesh->vmesh_load_failed) {
        mesh_load_vmesh(mesh);
    }
    auto* vmesh = static_cast<EditorVMesh*>(mesh->vmesh);
    if (!vmesh || vmesh->type != VMESH_TYPE_STATIC) {
        return nullptr;
    }
    auto* v3d = static_cast<const EditorV3d*>(vmesh->instance);
    if (!v3d || v3d->num_meshes <= 0 || !v3d->meshes) {
        return nullptr;
    }
    return v3d;
}

// The chunk's loaded texture handle, falling back to the material name its texture slot points at.
// -1 leaves the face untextured, which renders white rather than failing the conversion.
int resolve_chunk_bitmap(const EditorV3dMesh& sub, const EditorVifMesh& vm,
                         const EditorVifChunk& chunk)
{
    const int idx = chunk.texture_idx;
    if (idx < 0 || idx >= 7 || idx >= vm.num_texture_handles) {
        return -1;
    }
    if (vm.tex_handles[idx] != -1) {
        return vm.tex_handles[idx];
    }
    const int material = vm.tex_ids[idx];
    if (sub.materials && material >= 0 && material < sub.num_materials) {
        const char* name = sub.materials[material].texture_maps[0].name;
        if (name[0] != '\0') {
            return bm_load(name, -1, 1);
        }
    }
    return -1;
}

bool finite_vec(const Vector3& v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool gather_geometry(DedMesh* mesh, SourceGeom& out)
{
    const EditorV3d* v3d = mesh_static_v3d(mesh);
    if (!v3d) {
        return false;
    }

    std::map<WeldKey, int> weld;
    auto weld_vertex = [&](const Vector3& p) {
        const WeldKey key{float_bits(p.x), float_bits(p.y), float_bits(p.z)};
        auto it = weld.find(key);
        if (it != weld.end()) {
            return it->second;
        }
        const int index = static_cast<int>(out.positions.size());
        out.positions.push_back(p);
        weld.emplace(key, index);
        return index;
    };

    for (int i = 0; i < v3d->num_meshes; i++) {
        const EditorV3dMesh& sub = v3d->meshes[i];
        vmesh_for_each_lod0_chunk(sub.lod_mesh, [&](const EditorVifMesh& vm,
                                                    const EditorVifChunk& chunk, auto&& vertex) {
            const int bitmap_id = resolve_chunk_bitmap(sub, vm, chunk);
            if (bitmap_id < 0) {
                out.missing_texture = true;
            }
            if (chunk.norms) {
                out.flattened_normals = true;
            }
            const auto* uvs = static_cast<const float*>(chunk.uvs);

            for (int f = 0; f < chunk.num_faces; f++) {
                const EditorVifFace& face = chunk.faces[f];
                if (!vmesh_lod0_face_valid(chunk, face)) {
                    out.degenerate++;
                    continue;
                }
                const std::uint16_t idx[3] = {face.vindex1, face.vindex2, face.vindex3};
                Vector3 p[3];
                bool finite = true;
                for (int k = 0; k < 3; k++) {
                    p[k] = vertex(idx[k]);
                    finite = finite && finite_vec(p[k]);
                }
                if (!finite) {
                    out.degenerate++;
                    continue;
                }

                // Cross product of the stored winding. Its magnitude is twice the triangle area,
                // so this rejects both zero-area and repeated-vertex triangles; after it the three
                // positions are necessarily distinct, hence so are their weld indices.
                const Vector3 e1{p[1].x - p[0].x, p[1].y - p[0].y, p[1].z - p[0].z};
                const Vector3 e2{p[2].x - p[0].x, p[2].y - p[0].y, p[2].z - p[0].z};
                const Vector3 n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z,
                                e1.x * e2.y - e1.y * e2.x};
                if (!(n.x * n.x + n.y * n.y + n.z * n.z > 1.0e-20f)) {
                    out.degenerate++;
                    continue;
                }

                // A GFace's outward side is whichever way Newell's method takes the edge loop, and
                // for a triangle that is this same cross product. The chunk's own triangle plane is
                // the authority on which side the mesh calls out, so a winding that disagrees with
                // it is reversed before it becomes an edge loop.
                bool flip = false;
                if (chunk.face_planes) {
                    const Vector3& pn = chunk.face_planes[f].normal;
                    flip = n.x * pn.x + n.y * pn.y + n.z * pn.z < 0.0f;
                }

                SourceTri tri{};
                tri.bitmap_id = bitmap_id;
                tri.double_sided = (face.flags & VIF_FACE_DOUBLE_SIDED) != 0;
                if (tri.double_sided) {
                    out.double_sided++;
                }
                for (int k = 0; k < 3; k++) {
                    const int c = flip ? 2 - k : k;
                    tri.v[k] = weld_vertex(p[c]);
                    if (uvs) {
                        tri.uv[k][0] = uvs[idx[c] * 2];
                        tri.uv[k][1] = uvs[idx[c] * 2 + 1];
                    }
                }
                out.tris.push_back(tri);
            }
        });
    }

    return !out.tris.empty();
}

// ─── Brush construction ─────────────────────────────────────────────────────

// Builds the brush's GSolid the way RED's own from-scratch solid builder (FUN_004a5020) does:
// vertices, then faces, then the solid bbox/sphere, then the single room that owns every face.
// Nothing here can fail except allocation, and on any such failure the whole solid is destroyed as
// one unit — it owns every vertex, face and room reached by that point.
GSolid* build_solid(const SourceGeom& geom, const MeshToBrushOptions& opts, ConversionStats& stats)
{
    GSolid* solid = GSolid::create();
    if (!solid) {
        return nullptr;
    }

    std::vector<GVertex*> gverts;
    gverts.reserve(geom.positions.size());
    for (const Vector3& p : geom.positions) {
        GVertex* gv = solid->add_vertex(&p);
        if (!gv) {
            GSolid::destroy(solid);
            return nullptr;
        }
        gverts.push_back(gv);
    }

    GFaceAttributes attrs;
    attrs.init();

    for (const SourceTri& tri : geom.tris) {
        const int passes = (tri.double_sided && opts.duplicate_double_sided) ? 2 : 1;
        for (int pass = 0; pass < passes; pass++) {
            GFace* face = solid->create_face(&attrs);
            if (!face) {
                GSolid::destroy(solid);
                return nullptr;
            }
            face->bitmap_id = tri.bitmap_id;
            for (int k = 0; k < 3; k++) {
                const int c = pass == 0 ? k : 2 - k;
                // Lightmap coordinates are regenerated by Build Geometry, so they start at zero
                // exactly as they do for a brush RED itself creates.
                if (!face->add_vertex(gverts[tri.v[c]], tri.uv[c][0], tri.uv[c][1], 0.0f, 0.0f)) {
                    GSolid::destroy(solid);
                    return nullptr;
                }
            }
            if (pass == 1) {
                stats.reversed_copies++;
            }
        }
    }

    solid->compute_bbox_sphere();

    GRoom* room = GRoom::alloc();
    if (!room) {
        GSolid::destroy(solid);
        return nullptr;
    }
    room->init(solid, &solid->bbox_min, &solid->bbox_max);

    int room_faces = 0;
    for (GFace* face = solid->face_list_head; face;) {
        GFace* next = face->next_solid;
        if (!face->compute_plane_and_bbox()) {
            // Same disposal the .rfl solid reader uses for a face it cannot plane
            solid->remove_face(face);
            GFace::destroy(face);
            stats.degenerate++;
        }
        else {
            room->add_face(face);
            room_faces++;
        }
        face = next;
    }
    room->rebuild_bbox();

    if (room_faces == 0) {
        GSolid::destroy(solid);
        return nullptr;
    }

    stats.faces += room_faces;
    stats.vertices += static_cast<int>(gverts.size());
    return solid;
}

BrushNode* mesh_to_brush(CDedLevel* level, DedMesh* mesh, const MeshToBrushOptions& opts,
                         ConversionStats& stats)
{
    SourceGeom geom;
    if (!gather_geometry(mesh, geom)) {
        xlog::warn("[ToBrush] object {} '{}' has no usable LOD 0 geometry", mesh->uid,
                   mesh->mesh_filename.c_str());
        return nullptr;
    }

    stats.degenerate += geom.degenerate;
    stats.double_sided += geom.double_sided;
    stats.missing_texture = stats.missing_texture || geom.missing_texture;
    stats.flattened_normals = stats.flattened_normals || geom.flattened_normals;

    GSolid* solid = build_solid(geom, opts, stats);
    if (!solid) {
        xlog::warn("[ToBrush] object {} '{}' could not be built into a solid", mesh->uid,
                   mesh->mesh_filename.c_str());
        return nullptr;
    }

    BrushNode* brush = BrushNode::create();
    if (!brush) {
        GSolid::destroy(solid);
        return nullptr;
    }
    brush->geometry = solid;
    // Vertices stay in mesh-local space, so the brush inherits the object's transform unchanged
    brush->pos = mesh->pos;
    brush->orient = mesh->orient;
    brush->brush_type = BRUSH_TYPE_SOLID;
    brush->is_detail = 1;
    // uid -1, life -1, state NORMAL and the portal/scrolling flags come from the constructor.
    //
    // No undo record (see CDedLevel::insert_brush): the record would only cover the brush, and
    // deleting the source Mesh object is not undoable, so undoing would take the brush away and
    // leave the mesh gone. To Mesh Object makes the same trade.
    level->insert_brush(brush, false);

    stats.brushes++;
    return brush;
}

// ─── Options dialog ─────────────────────────────────────────────────────────

MeshToBrushOptions g_opts;
int g_dlg_mesh_count = 0;
int g_dlg_tri_count = 0;
int g_dlg_max_mesh_tri_count = 0;

INT_PTR CALLBACK ToBrushOptionsDlgProc(HWND hdlg, UINT msg, WPARAM wParam, LPARAM /*lParam*/)
{
    switch (msg) {
        case WM_INITDIALOG: {
            CheckDlgButton(hdlg, IDC_TOBRUSH_KEEP_MESH,
                           g_opts.keep_mesh ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hdlg, IDC_TOBRUSH_DOUBLE_SIDED,
                           g_opts.duplicate_double_sided ? BST_CHECKED : BST_UNCHECKED);
            char summary[128];
            std::snprintf(summary, sizeof(summary), "%d mesh object%s, %d triangle%s.",
                          g_dlg_mesh_count, g_dlg_mesh_count == 1 ? "" : "s", g_dlg_tri_count,
                          g_dlg_tri_count == 1 ? "" : "s");
            SetDlgItemTextA(hdlg, IDC_TOBRUSH_SUMMARY, summary);
            // The limit is per brush, so the largest single mesh decides, not the selection's total
            if (g_dlg_max_mesh_tri_count > tri_warn_threshold) {
                SetDlgItemTextA(hdlg, IDC_TOBRUSH_WARNING,
                                "Brushes this large may fail Build Geometry.");
            }
            else {
                ShowWindow(GetDlgItem(hdlg, IDC_TOBRUSH_WARNING), SW_HIDE);
            }
            return TRUE;
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                g_opts.keep_mesh = IsDlgButtonChecked(hdlg, IDC_TOBRUSH_KEEP_MESH) == BST_CHECKED;
                g_opts.duplicate_double_sided =
                    IsDlgButtonChecked(hdlg, IDC_TOBRUSH_DOUBLE_SIDED) == BST_CHECKED;
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

} // namespace

int mesh_to_brush_triangle_count(DedMesh* mesh)
{
    const EditorV3d* v3d = mesh_static_v3d(mesh);
    if (!v3d) {
        return 0;
    }
    int count = 0;
    for (int i = 0; i < v3d->num_meshes; i++) {
        vmesh_for_each_lod0_chunk(
            v3d->meshes[i].lod_mesh,
            [&](const EditorVifMesh&, const EditorVifChunk& chunk, auto&&) {
                count += chunk.num_faces;
            });
    }
    return count;
}

bool mesh_to_brush_options_dialog(HWND parent, int mesh_count, int triangle_count,
                                  int max_mesh_triangle_count, MeshToBrushOptions& opts)
{
    g_dlg_mesh_count = mesh_count;
    g_dlg_tri_count = triangle_count;
    g_dlg_max_mesh_tri_count = max_mesh_triangle_count;
    const INT_PTR result = DialogBoxParam(g_module, MAKEINTRESOURCE(IDD_MESH_TO_BRUSH_OPTIONS),
                                          parent, ToBrushOptionsDlgProc, 0);
    if (result != IDOK) {
        return false;
    }
    opts = g_opts;
    return true;
}

std::vector<BrushNode*> meshes_to_brushes(CDedLevel* level, const std::vector<DedMesh*>& meshes,
                                          const MeshToBrushOptions& opts,
                                          std::vector<DedMesh*>& converted)
{
    std::vector<BrushNode*> brushes;
    converted.clear();
    if (!level) {
        return brushes;
    }

    ConversionStats stats;
    for (DedMesh* mesh : meshes) {
        if (!mesh) {
            continue;
        }
        stats.meshes++;
        if (BrushNode* brush = mesh_to_brush(level, mesh, opts, stats)) {
            brushes.push_back(brush);
            converted.push_back(mesh);
        }
    }

    xlog::info("[ToBrush] converted {} of {} mesh object(s) into {} face(s) over {} welded "
               "vertices",
               stats.brushes, stats.meshes, stats.faces, stats.vertices);
    if (stats.flattened_normals) {
        xlog::info("[ToBrush] per-vertex normals and smoothing are not kept: brush faces are flat "
                   "and their smoothing groups start empty");
    }
    if (stats.brushes > 0) {
        xlog::info("[ToBrush] material properties other than the diffuse texture are not kept; "
                   "every face starts with RED's default face flags");
    }
    if (stats.missing_texture) {
        xlog::warn("[ToBrush] some chunks had no resolvable texture; those faces are untextured");
    }
    if (stats.double_sided > 0) {
        if (opts.duplicate_double_sided) {
            xlog::info("[ToBrush] {} double-sided face(s) were duplicated into {} reversed copies",
                       stats.double_sided, stats.reversed_copies);
        }
        else {
            xlog::info("[ToBrush] {} double-sided face(s) became single-sided", stats.double_sided);
        }
    }
    if (stats.degenerate > 0) {
        xlog::warn("[ToBrush] {} degenerate triangle(s) were dropped", stats.degenerate);
    }

    return brushes;
}
