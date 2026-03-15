#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <cstdio>
#include <windows.h>
#include <commdlg.h>
#include <xlog/xlog.h>
#include "level.h"
#include "textures.h"
#include "resources.h"
#include "meshes.h"
#include "mesh.h"

// ─── V3M format constants ───────────────────────────────────────────────────

static constexpr int32_t V3M_SIGNATURE = 0x52463344; // 'RF3D'
static constexpr int32_t V3D_VERSION = 0x40000;
static constexpr int32_t V3D_SECTION_SUBMESH = 0x5355424D; // 'SUBM'
static constexpr int32_t V3D_SECTION_END = 0x00000000;
static constexpr uint32_t V3D_LOD_TRIANGLE_PLANES = 0x20;
static constexpr int V3D_ALIGNMENT = 16;

// v3d_batch_info stores positions_size as uint16_t (num_vertices * 12, aligned to 16).
// 5461 * 12 = 65532, which aligns up to 65536 and overflows uint16_t.
// 5460 * 12 = 65520, already 16-byte aligned, fits in uint16_t.
static constexpr size_t MAX_BATCH_VERTICES = 5460;

// ─── Export options (persisted across dialog invocations) ────────────────────

static bool g_opt_drop_mesh_object = false;
static bool g_opt_reset_origin = false;

// ─── Intermediate representation ────────────────────────────────────────────

struct ExportVertex
{
    Vector3 pos;
    Vector3 normal;
    float u, v;
};

struct ExportTriangle
{
    uint16_t i0, i1, i2;
    Plane plane;
};

struct ExportBatch
{
    std::string texture_name;
    std::vector<ExportVertex> vertices;
    std::vector<ExportTriangle> triangles;
};

// ─── Texture name resolution ────────────────────────────────────────────────

static std::string resolve_texture_name(int bitmap_id)
{
    if (bitmap_id < 0) return "default.tga";
    int index = BitmapEntry::handle_to_index(bitmap_id);
    if (index < 0) return "default.tga";
    BitmapEntry* entry = &BitmapEntry::entries[index];
    if (entry->name[0] == '\0') return "default.tga";
    return std::string(entry->name);
}

// ─── Geometry gathering ─────────────────────────────────────────────────────

// Transform a vector by a rotation matrix (matches rf::Matrix3::transform_vector).
// rvec/uvec/fvec are stored as rows but the transform treats them as columns.
static Vector3 rotate_vector(const Matrix3& m, const Vector3& v)
{
    return Vector3{
        m.rvec.x * v.x + m.uvec.x * v.y + m.fvec.x * v.z,
        m.rvec.y * v.x + m.uvec.y * v.y + m.fvec.y * v.z,
        m.rvec.z * v.x + m.uvec.z * v.y + m.fvec.z * v.z
    };
}

enum class GatherResult { ok, no_faces, vertex_limit };

// origin: subtracted from all vertex positions (world-space point that becomes mesh local origin)
// Vertices and normals are transformed from brush-local space to world space using brush->orient.
static GatherResult gather_brush_geometry(BrushNode* brush, const Vector3& origin,
                                          std::vector<ExportBatch>& out_batches)
{
    out_batches.clear();

    auto* solid = static_cast<GSolid*>(brush->geometry);
    if (!solid) return GatherResult::no_faces;

    const Matrix3& orient = brush->orient;
    std::map<std::string, ExportBatch> batch_map;

    GFace* face = solid->face_list_head;
    while (face) {
        GFaceVertex* fv = face->edge_loop;
        if (!fv) {
            face = face->next_solid;
            continue;
        }

        struct FaceVert {
            Vector3 pos;
            float u, v;
        };
        std::vector<FaceVert> face_verts;

        GFaceVertex* start = fv;
        do {
            // Transform local vertex to world space, then subtract origin
            Vector3 world = rotate_vector(orient, fv->vertex->pos);
            world.x += brush->pos.x;
            world.y += brush->pos.y;
            world.z += brush->pos.z;

            FaceVert fvert;
            fvert.pos.x = world.x - origin.x;
            fvert.pos.y = world.y - origin.y;
            fvert.pos.z = world.z - origin.z;
            fvert.u = fv->u;
            fvert.v = fv->v;
            face_verts.push_back(fvert);
            fv = fv->next;
        } while (fv != start);

        if (face_verts.size() < 3) {
            face = face->next_solid;
            continue;
        }

        std::string tex_name = resolve_texture_name(face->bitmap_id);
        ExportBatch& batch = batch_map[tex_name];
        if (batch.texture_name.empty()) {
            batch.texture_name = tex_name;
        }

        // Rotate face normal from brush-local to world space
        Vector3 normal = rotate_vector(orient, face->plane.normal);

        size_t vert_count = batch.vertices.size() + face_verts.size();
        if (vert_count > MAX_BATCH_VERTICES) {
            return GatherResult::vertex_limit;
        }

        uint16_t base = static_cast<uint16_t>(batch.vertices.size());
        for (auto& fvert : face_verts) {
            ExportVertex ev;
            ev.pos = fvert.pos;
            ev.normal = normal;
            ev.u = fvert.u;
            ev.v = fvert.v;
            batch.vertices.push_back(ev);
        }

        for (size_t i = 2; i < face_verts.size(); i++) {
            ExportTriangle tri;
            tri.i0 = base;
            tri.i1 = static_cast<uint16_t>(base + i - 1);
            tri.i2 = static_cast<uint16_t>(base + i);
            tri.plane.normal = normal;
            // Compute plane distance from the exported (origin-relative) positions
            tri.plane.dist = -(normal.x * batch.vertices[base].pos.x +
                               normal.y * batch.vertices[base].pos.y +
                               normal.z * batch.vertices[base].pos.z);
            batch.triangles.push_back(tri);
        }

        face = face->next_solid;
    }

    for (auto& [name, batch] : batch_map) {
        out_batches.push_back(std::move(batch));
    }
    return out_batches.empty() ? GatherResult::no_faces : GatherResult::ok;
}

// ─── Binary writer helpers ──────────────────────────────────────────────────

class BinaryWriter
{
public:
    std::vector<uint8_t> data;

    void write_i32(int32_t val) { write_raw(&val, 4); }
    void write_u32(uint32_t val) { write_raw(&val, 4); }
    void write_u16(uint16_t val) { write_raw(&val, 2); }
    void write_i16(int16_t val) { write_raw(&val, 2); }
    void write_u8(uint8_t val) { data.push_back(val); }
    void write_f32(float val) { write_raw(&val, 4); }

    void write_vec3(const Vector3& v)
    {
        write_f32(v.x);
        write_f32(v.y);
        write_f32(v.z);
    }

    void write_vec2(float u, float v)
    {
        write_f32(u);
        write_f32(v);
    }

    void write_plane(const Plane& p)
    {
        write_vec3(p.normal);
        write_f32(p.dist);
    }

    void write_string_fixed(const char* str, size_t len)
    {
        size_t slen = std::strlen(str);
        size_t copy_len = (slen < len) ? slen : (len - 1);
        for (size_t i = 0; i < copy_len; i++) data.push_back(static_cast<uint8_t>(str[i]));
        for (size_t i = copy_len; i < len; i++) data.push_back(0);
    }

    void write_string_nt(const char* str)
    {
        size_t len = std::strlen(str);
        for (size_t i = 0; i <= len; i++) data.push_back(static_cast<uint8_t>(str[i]));
    }

    void write_zeros(size_t count)
    {
        for (size_t i = 0; i < count; i++) data.push_back(0);
    }

    void align(size_t alignment, size_t base_offset)
    {
        size_t current = data.size() - base_offset;
        size_t remainder = current % alignment;
        if (remainder != 0) {
            write_zeros(alignment - remainder);
        }
    }

    size_t size() const { return data.size(); }

    void write_raw(const void* ptr, size_t len)
    {
        auto* bytes = static_cast<const uint8_t*>(ptr);
        data.insert(data.end(), bytes, bytes + len);
    }
};

// ─── V3M serializer ─────────────────────────────────────────────────────────

// Data for a single submesh in the V3M file
struct SubmeshData
{
    std::string name;
    std::vector<ExportBatch> batches;
};

static constexpr int V3D_MAX_TEXTURES_PER_LOD = 7;
static constexpr int V3D_MAX_SUBMESHES = 8192;

static void write_submesh_section(BinaryWriter& out, const SubmeshData& submesh)
{
    auto& batches = submesh.batches;

    uint32_t total_verts = 0;
    uint32_t total_tris = 0;
    for (auto& b : batches) {
        total_verts += static_cast<uint32_t>(b.vertices.size());
        total_tris += static_cast<uint32_t>(b.triangles.size());
    }

    // Compute bounding box and sphere
    Vector3 bb_min{1e30f, 1e30f, 1e30f};
    Vector3 bb_max{-1e30f, -1e30f, -1e30f};
    for (auto& b : batches) {
        for (auto& v : b.vertices) {
            bb_min.x = std::min(bb_min.x, v.pos.x);
            bb_min.y = std::min(bb_min.y, v.pos.y);
            bb_min.z = std::min(bb_min.z, v.pos.z);
            bb_max.x = std::max(bb_max.x, v.pos.x);
            bb_max.y = std::max(bb_max.y, v.pos.y);
            bb_max.z = std::max(bb_max.z, v.pos.z);
        }
    }

    Vector3 center{
        (bb_min.x + bb_max.x) * 0.5f,
        (bb_min.y + bb_max.y) * 0.5f,
        (bb_min.z + bb_max.z) * 0.5f
    };

    float radius = 0.0f;
    for (auto& b : batches) {
        for (auto& v : b.vertices) {
            float dx = v.pos.x - center.x;
            float dy = v.pos.y - center.y;
            float dz = v.pos.z - center.z;
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dist > radius) radius = dist;
        }
    }

    uint32_t num_batches = static_cast<uint32_t>(batches.size());
    uint32_t num_materials = num_batches;
    uint32_t num_textures = num_batches;

    // Vertex positions are stored relative to the submesh offset (center).
    // The engine applies: final_pos = object_transform * (offset + local_vertex).
    // AABB is also relative to the offset.
    Vector3 local_bb_min{
        bb_min.x - center.x, bb_min.y - center.y, bb_min.z - center.z
    };
    Vector3 local_bb_max{
        bb_max.x - center.x, bb_max.y - center.y, bb_max.z - center.z
    };

    // ── Build the lod mesh data blob ──

    BinaryWriter mesh_data;
    size_t mesh_data_base = 0;

    // Batch headers
    for (uint32_t i = 0; i < num_batches; i++) {
        mesh_data.write_zeros(0x20);
        mesh_data.write_i32(static_cast<int32_t>(i));
        mesh_data.write_zeros(0x38 - 0x24);
    }
    mesh_data.align(V3D_ALIGNMENT, mesh_data_base);

    // Per-batch geometry data (positions written relative to submesh offset)
    for (uint32_t i = 0; i < num_batches; i++) {
        auto& batch = batches[i];

        for (auto& v : batch.vertices) {
            Vector3 local{v.pos.x - center.x, v.pos.y - center.y, v.pos.z - center.z};
            mesh_data.write_vec3(local);
        }
        mesh_data.align(V3D_ALIGNMENT, mesh_data_base);

        for (auto& v : batch.vertices) mesh_data.write_vec3(v.normal);
        mesh_data.align(V3D_ALIGNMENT, mesh_data_base);

        for (auto& v : batch.vertices) mesh_data.write_vec2(v.u, v.v);
        mesh_data.align(V3D_ALIGNMENT, mesh_data_base);

        for (auto& tri : batch.triangles) {
            mesh_data.write_u16(tri.i0);
            mesh_data.write_u16(tri.i1);
            mesh_data.write_u16(tri.i2);
            mesh_data.write_u16(0);
        }
        mesh_data.align(V3D_ALIGNMENT, mesh_data_base);

        // Adjust triangle plane distances for the offset: d' = d + dot(normal, center)
        for (auto& tri : batch.triangles) {
            Plane adjusted = tri.plane;
            adjusted.dist += tri.plane.normal.x * center.x
                           + tri.plane.normal.y * center.y
                           + tri.plane.normal.z * center.z;
            mesh_data.write_plane(adjusted);
        }
        mesh_data.align(V3D_ALIGNMENT, mesh_data_base);

        // same_pos_vertex_offsets: compare local positions (shifted uniformly, so results match)
        for (size_t vi = 0; vi < batch.vertices.size(); vi++) {
            int16_t soffset = 0;
            for (size_t vj = 0; vj < vi; vj++) {
                if (std::memcmp(&batch.vertices[vi].pos, &batch.vertices[vj].pos, sizeof(Vector3)) == 0) {
                    soffset = static_cast<int16_t>(vi - vj);
                    break;
                }
            }
            mesh_data.write_i16(soffset);
        }
        mesh_data.align(V3D_ALIGNMENT, mesh_data_base);
    }

    uint32_t data_size = static_cast<uint32_t>(mesh_data.size());

    // ── Build batch_info array ──

    struct BatchInfo {
        uint16_t num_vertices;
        uint16_t num_triangles;
        uint16_t positions_size;
        uint16_t indices_size;
        uint16_t same_pos_vertex_offsets_size;
        uint16_t bone_links_size;
        uint16_t tex_coords_size;
        uint32_t render_flags;
    };

    std::vector<BatchInfo> batch_infos;
    for (auto& batch : batches) {
        BatchInfo bi;
        bi.num_vertices = static_cast<uint16_t>(batch.vertices.size());
        bi.num_triangles = static_cast<uint16_t>(batch.triangles.size());

        auto align_up = [](uint32_t size) -> uint16_t {
            uint32_t remainder = size % V3D_ALIGNMENT;
            if (remainder != 0) size += V3D_ALIGNMENT - remainder;
            return static_cast<uint16_t>(size);
        };

        bi.positions_size = align_up(bi.num_vertices * 12);
        bi.tex_coords_size = align_up(bi.num_vertices * 8);
        bi.indices_size = align_up(bi.num_triangles * 8);
        bi.same_pos_vertex_offsets_size = align_up(bi.num_vertices * 2);
        bi.bone_links_size = 0;
        bi.render_flags = 0x518C41;
        batch_infos.push_back(bi);
    }

    // ── Write section ──

    // v3d_section_header
    out.write_i32(V3D_SECTION_SUBMESH);
    out.write_i32(0);

    // v3d_submesh
    out.write_string_fixed(submesh.name.c_str(), 24);
    out.write_string_fixed("None", 24);
    out.write_u32(7);  // version
    out.write_u32(1);  // num_lods
    out.write_f32(0.0f);
    out.write_vec3(center);    // submesh offset in model space
    out.write_f32(radius);
    out.write_vec3(local_bb_min);
    out.write_vec3(local_bb_max);

    // v3d_submesh_lod
    out.write_u32(V3D_LOD_TRIANGLE_PLANES);
    out.write_u32(total_verts);
    out.write_u16(static_cast<uint16_t>(num_batches));
    out.write_u32(data_size);
    out.write_raw(mesh_data.data.data(), mesh_data.data.size());
    out.write_i32(-1);

    for (auto& bi : batch_infos) {
        out.write_u16(bi.num_vertices);
        out.write_u16(bi.num_triangles);
        out.write_u16(bi.positions_size);
        out.write_u16(bi.indices_size);
        out.write_u16(bi.same_pos_vertex_offsets_size);
        out.write_u16(bi.bone_links_size);
        out.write_u16(bi.tex_coords_size);
        out.write_u32(bi.render_flags);
    }

    out.write_u32(0); // num_prop_points
    out.write_u32(num_textures);

    for (uint32_t i = 0; i < num_textures; i++) {
        out.write_u8(static_cast<uint8_t>(i));
        out.write_string_nt(batches[i].texture_name.c_str());
    }

    out.write_u32(num_materials);
    for (auto& batch : batches) {
        out.write_string_fixed(batch.texture_name.c_str(), 32);
        out.write_f32(0.0f); // emissive_factor
        out.write_f32(0.0f); // unknown[0]
        out.write_f32(0.0f); // unknown[1]
        out.write_f32(0.0f); // ref_cof
        out.write_string_fixed("", 32);
        out.write_u32(0x01); // flags
    }

    out.write_u32(1); // num_unknown1
    out.write_string_fixed(submesh.name.c_str(), 24);
    out.write_f32(0.0f);
}

static bool write_v3m(const char* filepath, const std::vector<SubmeshData>& submeshes)
{
    if (submeshes.empty()) return false;

    // Count total materials across all submeshes
    uint32_t total_materials = 0;
    for (auto& sm : submeshes) {
        total_materials += static_cast<uint32_t>(sm.batches.size());
    }

    BinaryWriter out;

    // v3d_file_header
    out.write_i32(V3M_SIGNATURE);
    out.write_i32(V3D_VERSION);
    out.write_i32(static_cast<int32_t>(submeshes.size()));
    out.write_i32(0);  // num_all_vertices (zeroed like ccrunch output)
    out.write_i32(0);  // num_all_triangles
    out.write_i32(0);  // unknown0
    out.write_i32(static_cast<int32_t>(total_materials));
    out.write_i32(0);  // unknown1
    out.write_i32(0);  // unknown2
    out.write_i32(0);  // num_colspheres

    // Write each submesh section
    for (auto& sm : submeshes) {
        write_submesh_section(out, sm);
    }

    // V3D_END
    out.write_i32(V3D_SECTION_END);
    out.write_i32(0);

    FILE* fp = std::fopen(filepath, "wb");
    if (!fp) return false;
    size_t written = std::fwrite(out.data.data(), 1, out.data.size(), fp);
    std::fclose(fp);
    return written == out.data.size();
}

// ─── Options dialog ─────────────────────────────────────────────────────────

extern HMODULE g_module;

static INT_PTR CALLBACK ConvertOptionsDlgProc(HWND hdlg, UINT msg, WPARAM wParam, LPARAM /*lParam*/)
{
    switch (msg) {
        case WM_INITDIALOG:
            CheckDlgButton(hdlg, IDC_CONVERT_REPLACE, g_opt_drop_mesh_object ? BST_CHECKED : BST_UNCHECKED);
            CheckDlgButton(hdlg, IDC_CONVERT_RESET_ORIGIN, g_opt_reset_origin ? BST_CHECKED : BST_UNCHECKED);
            return TRUE;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK) {
                g_opt_drop_mesh_object = IsDlgButtonChecked(hdlg, IDC_CONVERT_REPLACE) == BST_CHECKED;
                g_opt_reset_origin = IsDlgButtonChecked(hdlg, IDC_CONVERT_RESET_ORIGIN) == BST_CHECKED;
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

// ─── Drop mesh object ───────────────────────────────────────────────────────

// Create a DedMesh object at the given position with the given filename.
// Returns true if the mesh was successfully loaded and added to the level.
static bool create_mesh_object_at(CDedLevel* level, const char* mesh_filename,
                                  const Vector3& pos)
{
    // Reload VFS paths so the editor can find the newly exported file
    reload_custom_meshes();

    // Verify the mesh can be loaded before creating the object
    EditorVMesh* loaded_vmesh = vmesh_load_v3m(mesh_filename, 1, -1);
    if (!loaded_vmesh) {
        return false;
    }

    auto* mesh = new DedMesh();
    std::memset(static_cast<DedObject*>(mesh), 0, sizeof(DedObject));
    mesh->vtbl = reinterpret_cast<void*>(ded_object_vtbl_addr);
    mesh->type = DedObjectType::DED_MESH;
    mesh->collision_mode = 2; // All

    mesh->script_name.assign_0("Mesh");
    mesh->mesh_filename.assign_0(mesh_filename);
    mesh->pos = pos;
    mesh->orient.rvec = {1.0f, 0.0f, 0.0f};
    mesh->orient.uvec = {0.0f, 1.0f, 0.0f};
    mesh->orient.fvec = {0.0f, 0.0f, 1.0f};
    mesh->uid = generate_uid();
    mesh->vmesh = loaded_vmesh;
    mesh->vmesh_load_failed = false;

    level->GetAlpineLevelProperties().mesh_objects.push_back(mesh);
    level->master_objects.add(static_cast<DedObject*>(mesh));
    return true;
}

// ─── Handler ────────────────────────────────────────────────────────────────

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

void handle_brush_convert()
{
    auto* level = CDedLevel::Get();
    if (!level) return;

    auto selected = collect_selected_brushes(level);
    if (selected.empty()) {
        show_error_message("You must select at least one brush to convert to mesh.");
        return;
    }

    if (selected.size() > V3D_MAX_SUBMESHES) {
        char msg[256];
        std::snprintf(msg, sizeof(msg),
            "Too many brushes selected (%d). The maximum number of brushes that can be "
            "exported as a single mesh is %d.",
            static_cast<int>(selected.size()), V3D_MAX_SUBMESHES);
        show_error_message(msg);
        return;
    }

    // Show options dialog
    HWND main_wnd = GetMainFrameHandle();
    INT_PTR dlg_result = DialogBoxParam(g_module, MAKEINTRESOURCE(IDD_BRUSH_CONVERT_OPTIONS),
                                        main_wnd, ConvertOptionsDlgProc, 0);
    if (dlg_result != IDOK) return;

    // Compute combined world-space AABB center across all selected brushes
    Vector3 combined_min{1e30f, 1e30f, 1e30f};
    Vector3 combined_max{-1e30f, -1e30f, -1e30f};
    for (auto* brush : selected) {
        auto* solid = static_cast<GSolid*>(brush->geometry);
        if (!solid) continue;
        const Matrix3& orient = brush->orient;
        GFace* face = solid->face_list_head;
        while (face) {
            GFaceVertex* fv = face->edge_loop;
            if (fv) {
                GFaceVertex* start = fv;
                do {
                    Vector3 w = rotate_vector(orient, fv->vertex->pos);
                    w.x += brush->pos.x;
                    w.y += brush->pos.y;
                    w.z += brush->pos.z;
                    combined_min.x = std::min(combined_min.x, w.x);
                    combined_min.y = std::min(combined_min.y, w.y);
                    combined_min.z = std::min(combined_min.z, w.z);
                    combined_max.x = std::max(combined_max.x, w.x);
                    combined_max.y = std::max(combined_max.y, w.y);
                    combined_max.z = std::max(combined_max.z, w.z);
                    fv = fv->next;
                } while (fv != start);
            }
            face = face->next_solid;
        }
    }

    Vector3 world_center{
        (combined_min.x + combined_max.x) * 0.5f,
        (combined_min.y + combined_max.y) * 0.5f,
        (combined_min.z + combined_max.z) * 0.5f
    };

    // gather_brush_geometry transforms vertices to world space (orient * local + pos),
    // then subtracts export_origin.
    //
    // Reset origin checked:   export_origin = world_center → vertices centered at (0,0,0)
    // Reset origin unchecked: export_origin = (0,0,0) → exported = world-space positions
    Vector3 export_origin = g_opt_reset_origin
        ? world_center
        : Vector3{0.0f, 0.0f, 0.0f};

    // Mesh object placement:
    // Reset origin checked:   place at world_center (mesh geometry is centered at origin)
    // Reset origin unchecked: place at world origin (mesh geometry is in world coordinates)
    Vector3 mesh_object_pos = g_opt_reset_origin
        ? world_center
        : Vector3{0.0f, 0.0f, 0.0f};

    // Save file dialog (before gathering geometry so user can cancel cheaply)
    char filename[MAX_PATH] = "brush.v3m";
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = main_wnd;
    ofn.lpstrFilter = "Mesh (*.v3m)\0*.v3m\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = "v3m";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = "Export Brush as V3M Mesh";

    if (!GetSaveFileNameA(&ofn)) return;

    // Derive base name from filename (used for submesh naming)
    const char* file_base = std::strrchr(filename, '\\');
    file_base = file_base ? file_base + 1 : filename;
    const char* dot = std::strrchr(file_base, '.');
    size_t base_name_len = dot ? static_cast<size_t>(dot - file_base) : std::strlen(file_base);
    if (base_name_len > 20) base_name_len = 20; // leave room for suffix

    // Build one submesh per selected brush
    std::vector<SubmeshData> submeshes;

    for (size_t si = 0; si < selected.size(); si++) {
        auto* brush = selected[si];
        auto* solid = static_cast<GSolid*>(brush->geometry);
        if (!solid || !solid->face_list_head) continue;

        std::vector<ExportBatch> batches;
        auto result = gather_brush_geometry(brush, export_origin, batches);
        if (result == GatherResult::vertex_limit) {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                "Brush %d has too many vertices sharing a single texture (limit is %zu per texture batch). "
                "Split the brush or use more distinct textures.",
                static_cast<int>(si + 1), MAX_BATCH_VERTICES);
            show_error_message(msg);
            return;
        }
        if (result == GatherResult::no_faces) continue;

        // V3M format: max 7 textures per LOD (each batch = 1 texture)
        if (batches.size() > V3D_MAX_TEXTURES_PER_LOD) {
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                "Brush %d has %d unique textures, but the V3M format allows a maximum of %d "
                "textures per submesh. Reduce the number of textures on this brush.",
                static_cast<int>(si + 1), static_cast<int>(batches.size()),
                V3D_MAX_TEXTURES_PER_LOD);
            show_error_message(msg);
            return;
        }

        SubmeshData sm;
        // Name submeshes: "basename" for single, "basename_1", "basename_2", etc. for multi
        if (selected.size() == 1) {
            sm.name.assign(file_base, base_name_len);
        }
        else {
            char suffix[8];
            std::snprintf(suffix, sizeof(suffix), "_%d", static_cast<int>(si + 1));
            sm.name.assign(file_base, base_name_len);
            sm.name += suffix;
        }
        if (sm.name.size() > 23) sm.name.resize(23);

        sm.batches = std::move(batches);
        submeshes.push_back(std::move(sm));
    }

    if (submeshes.empty()) {
        show_error_message("No exportable faces found in selected brushes.");
        return;
    }

    if (!write_v3m(filename, submeshes)) {
        show_error_message("Failed to write V3M file.");
        return;
    }

    int total_tris = 0;
    for (auto& sm : submeshes)
        for (auto& b : sm.batches)
            total_tris += static_cast<int>(b.triangles.size());

    LogDlg_Append(GetLogDlg(), "Exported %d brush(es) to %s (%d submeshes, %d triangles).",
                  static_cast<int>(selected.size()), filename,
                  static_cast<int>(submeshes.size()), total_tris);

    // Replace with mesh object if requested
    if (g_opt_drop_mesh_object) {
        const char* mesh_basename = std::strrchr(filename, '\\');
        mesh_basename = mesh_basename ? mesh_basename + 1 : filename;

        if (create_mesh_object_at(level, mesh_basename, mesh_object_pos)) {
            redraw_all_viewports();
            LogDlg_Append(GetLogDlg(), "Created mesh object '%s'.", mesh_basename);
        }
        else {
            show_error_message("Mesh file was exported but could not be loaded by the editor.\n"
                               "Make sure the file is saved in red\\meshes or user_maps\\meshes.");
        }
    }
}
