#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <patch_common/MemUtils.h>
#include <xlog/xlog.h>
#include <common/utils/string-utils.h>
#include "lightmap_mesh_occluders.h"
#include "level.h"
#include "mesh.h"
#include "vtypes.h"

namespace
{

struct LocalTri
{
    Vector3 v0, v1, v2;
    bool alpha;
};

struct MeshGeom
{
    bool ok = false;
    std::vector<LocalTri> tris;
};

std::map<std::string, MeshGeom> g_geom_cache;

int g_objects = 0;
int g_tris = 0;
int g_skipped = 0;
int g_alpha_tris = 0;
std::vector<std::pair<int, std::string>> g_all_alpha_objects;

bool bitmap_has_alpha(int handle)
{
    return handle != -1 && bm_has_alpha(handle) != 0;
}

// The double-sided face flag 0x20 is deliberately ignored: a mesh occluder blocks either way.
void collect_lod0(const EditorVifLodMesh* lod, MeshGeom& out)
{
    if (!lod || lod->num_levels <= 0) {
        return;
    }
    const EditorVifMesh* vm = lod->meshes[0];
    if (!vm || !vm->chunks) {
        return;
    }
    for (int c = 0; c < vm->num_chunks; c++) {
        const EditorVifChunk& chunk = vm->chunks[c];
        const Vector3* vecs = chunk.vecs;
        const EditorVifFace* faces = chunk.faces;
        const int num_vecs = chunk.num_vecs;
        const int num_faces = chunk.num_faces;
        if (!vecs || !faces || num_vecs <= 0 || num_faces <= 0) {
            continue;
        }
        const bool alpha = chunk.texture_idx >= 0 && chunk.texture_idx < vm->num_texture_handles &&
                           chunk.texture_idx < 7 &&
                           bitmap_has_alpha(vm->tex_handles[chunk.texture_idx]);
        for (int f = 0; f < num_faces; f++) {
            const EditorVifFace& face = faces[f];
            if (face.vindex1 >= num_vecs || face.vindex2 >= num_vecs || face.vindex3 >= num_vecs) {
                continue;
            }
            out.tris.push_back({vecs[face.vindex1], vecs[face.vindex2], vecs[face.vindex3], alpha});
        }
    }
}

bool collect_v3m(EditorVMesh* vmesh, MeshGeom& out)
{
    const auto* v3d = static_cast<const EditorV3d*>(vmesh->instance);
    if (!v3d) {
        return false;
    }
    if (v3d->num_meshes <= 0 || !v3d->meshes) {
        return false;
    }
    for (int i = 0; i < v3d->num_meshes; i++) {
        collect_lod0(v3d->meshes[i].lod_mesh, out);
    }
    return true;
}

// Only one character mesh is ever drawn and FUN_004c03f0 defaults to entry 0.
bool collect_v3c(EditorVMesh* vmesh, MeshGeom& out)
{
    const auto* character = static_cast<const EditorCharacter*>(vmesh->mesh);
    if (!character || character->num_character_meshes <= 0) {
        return false;
    }
    const EditorV3dMesh* v3d_mesh = character->character_meshes[0].mesh;
    if (!v3d_mesh) {
        return false;
    }
    collect_lod0(v3d_mesh->lod_mesh, out);
    return true;
}

// .vfx frame 0 mesh sections

constexpr std::uint32_t vfx_signature = 0x58465356;   // "VSFX"
constexpr std::uint32_t vfx_section_mesh = 0x4f584653; // "sfxo"
constexpr std::uint32_t vfx_section_material = 0x4c54414d;

constexpr unsigned vfx_mesh_facing = 0x1;
constexpr unsigned vfx_mesh_morph = 0x4;
constexpr unsigned vfx_mesh_dump_uvs = 0x100;
constexpr unsigned vfx_mesh_facing_rod = 0x800;

class VfxReader
{
public:
    VfxReader(const std::uint8_t* data, std::size_t begin, std::size_t end)
        : data_(data), pos_(begin), end_(end)
    {}

    bool bad() const { return bad_; }
    std::size_t pos() const { return pos_; }
    std::size_t remaining() const { return bad_ ? 0 : end_ - pos_; }

    // 64 bit: a count x stride product taken from the file overflows a 32 bit size_t long before it
    // exceeds the section, and a wrapped product would pass this test and then be read past the end
    bool take(std::uint64_t n)
    {
        if (bad_ || n > static_cast<std::uint64_t>(end_ - pos_)) {
            bad_ = true;
            return false;
        }
        pos_ += static_cast<std::size_t>(n);
        return true;
    }

    // Whether a count x stride worth of bytes is still in the section, without consuming them or
    // marking the reader bad; used to bound a count before it sizes a container.
    bool fits(std::uint64_t n) const { return !bad_ && n <= static_cast<std::uint64_t>(end_ - pos_); }

    std::int32_t s4()
    {
        const std::size_t at = pos_;
        if (!take(4)) {
            return 0;
        }
        std::int32_t v;
        std::memcpy(&v, data_ + at, 4);
        return v;
    }

    std::uint32_t u4() { return static_cast<std::uint32_t>(s4()); }

    float f4()
    {
        const std::size_t at = pos_;
        if (!take(4)) {
            return 0.0f;
        }
        float v;
        std::memcpy(&v, data_ + at, 4);
        return v;
    }

    std::uint8_t u1()
    {
        const std::size_t at = pos_;
        return take(1) ? data_[at] : 0;
    }

    Vector3 vec3()
    {
        Vector3 v{};
        v.x = f4();
        v.y = f4();
        v.z = f4();
        return v;
    }

    // x, y, z, w
    void quat(float* out)
    {
        for (int i = 0; i < 4; i++) {
            out[i] = f4();
        }
    }

    std::string strz()
    {
        std::string s;
        while (!bad_) {
            const char c = static_cast<char>(u1());
            if (bad_ || c == '\0') {
                break;
            }
            if (s.size() < 256) {
                s.push_back(c);
            }
        }
        return s;
    }

    const std::uint8_t* at(std::size_t off) const { return data_ + off; }

private:
    const std::uint8_t* data_;
    std::size_t pos_;
    std::size_t end_;
    bool bad_ = false;
};

struct VfxStage
{
    Vector3 scale{1.0f, 1.0f, 1.0f};
    float rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    Vector3 translation{};
};

void quat_to_matrix(const float* q, float* m)
{
    const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (len < 1.0e-12f) {
        const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        std::memcpy(m, identity, sizeof(identity));
        return;
    }
    const float x = q[0] / len, y = q[1] / len, z = q[2] / len, w = q[3] / len;
    m[0] = 1.0f - 2.0f * (y * y + z * z);
    m[1] = 2.0f * (x * y - z * w);
    m[2] = 2.0f * (x * z + y * w);
    m[3] = 2.0f * (x * y + z * w);
    m[4] = 1.0f - 2.0f * (x * x + z * z);
    m[5] = 2.0f * (y * z - x * w);
    m[6] = 2.0f * (x * z - y * w);
    m[7] = 2.0f * (y * z + x * w);
    m[8] = 1.0f - 2.0f * (x * x + y * y);
}

Vector3 apply_stage(const VfxStage& s, const Vector3& v)
{
    const float x = v.x * s.scale.x;
    const float y = v.y * s.scale.y;
    const float z = v.z * s.scale.z;
    return {s.rot[0] * x + s.rot[1] * y + s.rot[2] * z + s.translation.x,
            s.rot[3] * x + s.rot[4] * y + s.rot[5] * z + s.translation.y,
            s.rot[6] * x + s.rot[7] * y + s.rot[8] * z + s.translation.z};
}

bool vfx_read_header(VfxReader& r, std::uint32_t& version)
{
    if (r.u4() != vfx_signature) {
        return false;
    }
    version = r.u4();
    if ((version < 0x30000 || version > 0x3ffff) && version < 0x40005) {
        return false;
    }
    int n = version >= 0x30008 ? 1 : 0;
    n += 7;
    if (version >= 0x3000f) n++;
    if (version >= 0x40000) n++;
    if (version >= 0x40002) n++;
    if (version >= 0x40003) n++;
    if (version >= 0x40005) n++;
    r.take(static_cast<std::size_t>(n) * 4);
    if (version < 0x3000a) {
        r.take(4);
    }
    n = 5;
    if (version >= 0x3000d) n++;
    if (version >= 0x30009) n += 5;
    n += 5;
    if (version >= 0x3000f) n++;
    r.take(static_cast<std::size_t>(n) * 4);
    return !r.bad();
}

void vfx_skip_material_texture(VfxReader& r, std::uint32_t version)
{
    r.strz();
    if (version >= 0x30012) {
        r.take(12);
    }
}

// A pre-0x40000 file inlines its materials in the mesh section; they carry no geometry but their
// length has to be consumed exactly to reach the frames behind them.
void vfx_skip_mesh_material_old(VfxReader& r, std::uint32_t version, int num_frames)
{
    const std::int32_t type = r.s4();
    const bool textured = type == 0 || type == 1;
    if (version >= 0x30003 && textured) {
        r.take(1);
    }
    if (textured) {
        vfx_skip_material_texture(r, version);
    }
    if (type == 1) {
        vfx_skip_material_texture(r, version);
    }
    if (textured && version < 0x30012) {
        r.take(8);
    }
    if (textured && version >= 0x30007) {
        r.take(12);
    }
    if (textured) {
        r.strz();
    }
    if (type == 1 && num_frames > 0) {
        r.take(static_cast<std::uint64_t>(num_frames) * 4);
    }
    if (type == 2) {
        r.take(12);
    }
    if (version >= 0x30011) {
        r.take(4);
    }
}

std::string vfx_read_material(VfxReader& r, std::uint32_t version)
{
    const std::int32_t type = r.s4();
    if (version >= 0x40003) {
        r.take(4);
    }
    if (type == 0 || type == 1 || version >= 0x40006) {
        r.take(1);
    }
    if (type != 0 && type != 1) {
        return {};
    }
    std::string name = r.strz();
    return r.bad() ? std::string{} : name;
}

struct VfxMesh
{
    std::vector<Vector3> positions;
    std::vector<int> faces;          // 3 indices per face
    std::vector<int> face_material;  // index into materials
    std::vector<int> materials;      // index into the file's material sections
    std::vector<VfxStage> stages;
    bool geometry = false;
};

bool vfx_read_mesh(VfxReader& r, std::uint32_t version, VfxMesh& m)
{
    r.strz();  // name
    r.strz();  // parent_name; the engine never composes parent transforms, so neither do we
    r.take(1); // save_parent
    const std::int32_t num_vertices = r.s4();
    // every count is bounded against the bytes the section still holds before it multiplies or
    // sizes anything: a .vfx is untrusted input and these are plain file int32s
    if (r.bad() || num_vertices < 0 || !r.fits(static_cast<std::uint64_t>(num_vertices) * 6)) {
        return false;
    }
    if (version < 0x3000a) {
        r.take(static_cast<std::uint64_t>(num_vertices) * 12);
    }
    const std::int32_t num_faces = r.s4();
    const std::size_t face_size = 12 + (version < 0x3000d ? 24u : 0u) + 36 + 12 + 12 + 4 + 4 + 4 + 12;
    if (r.bad() || num_faces < 0 || !r.fits(static_cast<std::uint64_t>(num_faces) * face_size)) {
        return false;
    }
    const std::size_t faces_at = r.pos();
    if (!r.take(static_cast<std::uint64_t>(num_faces) * face_size)) {
        return false;
    }
    m.faces.reserve(static_cast<std::size_t>(num_faces) * 3);
    m.face_material.reserve(static_cast<std::size_t>(num_faces));
    for (std::int32_t f = 0; f < num_faces; f++) {
        const std::uint8_t* base = r.at(faces_at + static_cast<std::size_t>(f) * face_size);
        std::int32_t idx[3];
        std::memcpy(idx, base, sizeof(idx));
        m.faces.push_back(idx[0]);
        m.faces.push_back(idx[1]);
        m.faces.push_back(idx[2]);
        std::int32_t mat;
        std::memcpy(&mat, base + face_size - 20, 4);
        m.face_material.push_back(mat);
    }
    if (version >= 0x30009) {
        r.take(4); // frames_per_second
    }
    std::int32_t num_frames = 0;
    std::int32_t start_frame = 0, end_frame = 0;
    if (version >= 0x40004) {
        r.take(8);
        num_frames = r.s4();
    }
    else {
        start_frame = r.s4();
        end_frame = r.s4();
        // both are plain file int32s, so the span is taken in 64 bit rather than overflowing
        const std::int64_t span = static_cast<std::int64_t>(end_frame) -
                                  static_cast<std::int64_t>(start_frame) +
                                  (version >= 0x3000c ? 1 : 0);
        if (span < 0 || span > 0x7fffffff) {
            return false;
        }
        num_frames = static_cast<std::int32_t>(span);
    }
    const std::int32_t num_materials = r.s4();
    // the old form reads at least the 4 byte type per material, the new form exactly 4
    if (r.bad() || num_materials < 0 || !r.fits(static_cast<std::uint64_t>(num_materials) * 4)) {
        return false;
    }
    if (version >= 0x40000) {
        m.materials.resize(static_cast<std::size_t>(num_materials));
        for (std::int32_t i = 0; i < num_materials; i++) {
            m.materials[i] = r.s4();
        }
    }
    else {
        for (std::int32_t i = 0; i < num_materials; i++) {
            vfx_skip_mesh_material_old(r, version, num_frames);
        }
    }
    r.take(16); // bounding sphere
    if (version < 0x30002) {
        r.take(4);
    }
    const unsigned flags = r.u4();
    const bool facing = (flags & vfx_mesh_facing) != 0;
    const bool facing_rod = (flags & vfx_mesh_facing_rod) != 0;
    const bool morph = (flags & vfx_mesh_morph) != 0;
    const bool dump_uvs = (flags & vfx_mesh_dump_uvs) != 0;
    if (facing && version == 0x3000a) {
        r.take(8);
    }
    const std::int32_t num_face_vertices = r.s4();
    // 16 bytes plus a 4 byte adjacency count per entry, at a minimum
    if (r.bad() || num_face_vertices < 0 ||
        !r.fits(static_cast<std::uint64_t>(num_face_vertices) * 20)) {
        return false;
    }
    for (std::int32_t i = 0; i < num_face_vertices; i++) {
        r.take(16);
        const std::int32_t adjacent = r.s4();
        if (r.bad() || adjacent < 0) {
            return false;
        }
        r.take(static_cast<std::uint64_t>(adjacent) * 4);
    }
    bool is_keyframed = false;
    if (version >= 0x30009) {
        is_keyframed = r.u1() != 0;
    }
    if (r.bad() || num_frames < 0) {
        return false;
    }

    VfxStage frame0{};
    bool has_frame0_trs = false;
    for (std::int32_t frame = 0; frame < num_frames; frame++) {
        const std::size_t frame_at = r.pos();
        const bool has_geometry = morph || frame == 0;
        if (has_geometry) {
            const Vector3 center = r.vec3();
            const Vector3 mult = r.vec3();
            const std::size_t at = r.pos();
            if (!r.take(static_cast<std::uint64_t>(num_vertices) * 6)) {
                return false;
            }
            if (frame == 0) {
                if (!std::isfinite(center.x) || !std::isfinite(center.y) ||
                    !std::isfinite(center.z) || !std::isfinite(mult.x) || !std::isfinite(mult.y) ||
                    !std::isfinite(mult.z)) {
                    return false;
                }
                m.positions.resize(static_cast<std::size_t>(num_vertices));
                for (std::int32_t v = 0; v < num_vertices; v++) {
                    std::int16_t raw[3];
                    std::memcpy(raw, r.at(at + static_cast<std::size_t>(v) * 6), 6);
                    m.positions[v] = {raw[0] * mult.x + center.x, raw[1] * mult.y + center.y,
                                      raw[2] * mult.z + center.z};
                }
            }
        }
        if (has_geometry && (facing || facing_rod) && version >= 0x3000b) {
            r.take(8);
        }
        if (has_geometry && facing_rod && frame == 0 && version >= 0x40001) {
            r.take(12);
        }
        if ((dump_uvs || frame == 0) && version >= 0x3000d) {
            r.take(static_cast<std::uint64_t>(num_faces) * 24);
        }
        if (!morph && (!is_keyframed || (version < 0x3000e && frame == 0))) {
            const Vector3 t = r.vec3();
            float q[4];
            r.quat(q);
            const Vector3 s = r.vec3();
            if (frame == 0) {
                frame0.translation = t;
                frame0.scale = s;
                quat_to_matrix(q, frame0.rot);
                has_frame0_trs = true;
            }
        }
        if (version < 0x30009) {
            r.take(1);
        }
        if (version < 0x40005) {
            r.take(4);
        }
        if (r.bad()) {
            return false;
        }
        // What a frame reads depends only on whether it is frame 0, so once one consumes nothing
        // every later one consumes nothing too and the count is only a loop bound.
        if (r.pos() == frame_at) {
            break;
        }
    }

    if (is_keyframed) {
        VfxStage pivot{};
        if (version >= 0x3000a) {
            pivot.translation = r.vec3();
            float q[4];
            r.quat(q);
            quat_to_matrix(q, pivot.rot);
            pivot.scale = r.vec3();
        }
        VfxStage key = has_frame0_trs ? frame0 : VfxStage{};
        const std::int32_t num_translation = r.s4();
        // 4 byte time, a vec3 and 24 bytes of tangents per key
        if (r.bad() || num_translation < 0 ||
            !r.fits(static_cast<std::uint64_t>(num_translation) * 40)) {
            return false;
        }
        for (std::int32_t i = 0; i < num_translation; i++) {
            r.take(4);
            const Vector3 v = r.vec3();
            if (i == 0) {
                key.translation = v;
            }
            r.take(24);
        }
        const std::int32_t num_rotation = r.s4();
        // 4 byte time, a quaternion and 20 bytes of tangents per key
        if (r.bad() || num_rotation < 0 ||
            !r.fits(static_cast<std::uint64_t>(num_rotation) * 40)) {
            return false;
        }
        for (std::int32_t i = 0; i < num_rotation; i++) {
            r.take(4);
            float q[4];
            r.quat(q);
            if (i == 0) {
                quat_to_matrix(q, key.rot);
            }
            r.take(20);
        }
        const std::int32_t num_scale = r.s4();
        if (r.bad() || num_scale < 0 || !r.fits(static_cast<std::uint64_t>(num_scale) * 40)) {
            return false;
        }
        for (std::int32_t i = 0; i < num_scale; i++) {
            r.take(4);
            const Vector3 v = r.vec3();
            if (i == 0) {
                key.scale = v;
            }
            r.take(24);
        }
        // FUN_004fb230 runs the pivot transform first and the keyframe transform second, per
        // vertex; the two do not collapse into one because the scales are per axis.
        m.stages.push_back(pivot);
        m.stages.push_back(key);
    }
    else if (has_frame0_trs) {
        m.stages.push_back(frame0);
    }

    m.geometry = !facing && !facing_rod;
    return !r.bad();
}

bool collect_vfx(const char* filename, MeshGeom& out, int uid)
{
    std::vector<std::uint8_t> buffer;
    {
        rf::File file;
        if (file.open_mode(filename) != 0) {
            xlog::warn("[MeshOccluders] object {}: cannot open '{}'", uid, filename);
            return false;
        }
        const int size = file.get_size();
        if (size <= 0) {
            file.close();
            xlog::warn("[MeshOccluders] object {}: '{}' is empty", uid, filename);
            return false;
        }
        // rf::File has no destructor, so a throw here would strand an engine file slot
        try {
            buffer.resize(static_cast<std::size_t>(size));
        }
        catch (...) {
            file.close();
            throw;
        }
        const int got = file.read(buffer.data(), buffer.size());
        file.close();
        if (got != size) {
            xlog::warn("[MeshOccluders] object {}: short read on '{}'", uid, filename);
            return false;
        }
    }

    VfxReader head{buffer.data(), 0, buffer.size()};
    std::uint32_t version = 0;
    if (!vfx_read_header(head, version)) {
        xlog::warn("[MeshOccluders] object {}: '{}' is not a readable .vfx", uid, filename);
        return false;
    }

    std::vector<std::string> materials;
    std::vector<VfxMesh> meshes;
    std::size_t pos = head.pos();
    while (pos + 8 <= buffer.size()) {
        std::uint32_t type;
        std::int32_t len;
        std::memcpy(&type, buffer.data() + pos, 4);
        std::memcpy(&len, buffer.data() + pos + 4, 4);
        pos += 8;
        // the stored length counts its own field, so the body is four bytes shorter
        if (len < 4 || static_cast<std::size_t>(len - 4) > buffer.size() - pos) {
            xlog::warn("[MeshOccluders] object {}: '{}' has a bad section length, stopping there",
                       uid, filename);
            break;
        }
        const std::size_t body_end = pos + static_cast<std::size_t>(len - 4);
        VfxReader r{buffer.data(), pos, body_end};
        if (type == vfx_section_mesh) {
            VfxMesh m;
            if (vfx_read_mesh(r, version, m)) {
                meshes.push_back(std::move(m));
            }
            else {
                xlog::warn("[MeshOccluders] object {}: '{}' has an unreadable mesh section", uid,
                           filename);
            }
        }
        else if (type == vfx_section_material) {
            materials.push_back(vfx_read_material(r, version));
        }
        pos = body_end;
    }

    if (meshes.empty()) {
        xlog::debug("[MeshOccluders] object {}: '{}' has no mesh sections", uid, filename);
        return true;
    }

    std::map<std::string, bool> alpha_by_texture;
    for (const VfxMesh& m : meshes) {
        if (!m.geometry) {
            continue;
        }
        const std::size_t face_count = m.faces.size() / 3;
        for (std::size_t f = 0; f < face_count; f++) {
            const int* idx = &m.faces[f * 3];
            if (idx[0] < 0 || idx[1] < 0 || idx[2] < 0 ||
                static_cast<std::size_t>(idx[0]) >= m.positions.size() ||
                static_cast<std::size_t>(idx[1]) >= m.positions.size() ||
                static_cast<std::size_t>(idx[2]) >= m.positions.size()) {
                continue;
            }
            Vector3 v[3];
            for (int k = 0; k < 3; k++) {
                v[k] = m.positions[static_cast<std::size_t>(idx[k])];
                for (const VfxStage& s : m.stages) {
                    v[k] = apply_stage(s, v[k]);
                }
            }
            bool alpha = false;
            const int local = m.face_material[f];
            if (local >= 0 && static_cast<std::size_t>(local) < m.materials.size()) {
                const int global = m.materials[static_cast<std::size_t>(local)];
                if (global >= 0 && static_cast<std::size_t>(global) < materials.size() &&
                    !materials[static_cast<std::size_t>(global)].empty()) {
                    const std::string& tex = materials[static_cast<std::size_t>(global)];
                    auto it = alpha_by_texture.find(tex);
                    if (it == alpha_by_texture.end()) {
                        it = alpha_by_texture
                                 .emplace(tex, bitmap_has_alpha(bm_load(tex.c_str(), -1, 1)))
                                 .first;
                    }
                    alpha = it->second;
                }
            }
            out.tris.push_back({v[0], v[1], v[2], alpha});
        }
    }
    return true;
}

const MeshGeom& mesh_geometry(DedMesh* mesh)
{
    std::string key = string_to_lower(mesh->mesh_filename.c_str());
    auto it = g_geom_cache.find(key);
    if (it != g_geom_cache.end()) {
        return it->second;
    }
    MeshGeom geom;
    const auto ext = get_ext_from_filename(mesh->mesh_filename.c_str());
    if (string_iequals(ext, "vfx")) {
        geom.ok = collect_vfx(mesh->mesh_filename.c_str(), geom, mesh->uid);
    }
    else {
        if (!mesh->vmesh && !mesh->vmesh_load_failed) {
            mesh_load_vmesh(mesh);
        }
        auto* vmesh = static_cast<EditorVMesh*>(mesh->vmesh);
        if (!vmesh) {
            xlog::warn("[MeshOccluders] object {}: '{}' could not be loaded", mesh->uid,
                       mesh->mesh_filename.c_str());
        }
        else if (vmesh->type == VMESH_TYPE_STATIC) {
            geom.ok = collect_v3m(vmesh, geom);
        }
        else if (vmesh->type == VMESH_TYPE_CHARACTER) {
            geom.ok = collect_v3c(vmesh, geom);
        }
        else {
            xlog::warn("[MeshOccluders] object {}: '{}' loaded as an unexpected mesh type {}",
                       mesh->uid, mesh->mesh_filename.c_str(), static_cast<int>(vmesh->type));
        }
        if (vmesh && !geom.ok) {
            xlog::warn("[MeshOccluders] object {}: no LOD 0 geometry in '{}'", mesh->uid,
                       mesh->mesh_filename.c_str());
        }
    }
    return g_geom_cache.emplace(std::move(key), std::move(geom)).first->second;
}

} // namespace

bool lightmap_collect_mesh_occluders(std::vector<MeshOccluderTri>& out)
{
    g_objects = 0;
    g_tris = 0;
    g_skipped = 0;
    g_alpha_tris = 0;
    g_all_alpha_objects.clear();

    auto* level = CDedLevel::Get();
    if (!level || !level->GetAlpineLevelProperties().meshes_occlude) {
        return false;
    }
    for (DedMesh* mesh : level->GetAlpineLevelProperties().mesh_objects) {
        if (!mesh || mesh->mesh_filename.empty()) {
            g_skipped++;
            continue;
        }
        if (mesh->no_shadow_cast) {
            g_skipped++;
            xlog::debug("[MeshOccluders] object {} '{}' skipped (no shadow cast)", mesh->uid,
                        mesh->mesh_filename.c_str());
            continue;
        }
        const MeshGeom& geom = mesh_geometry(mesh);
        if (geom.tris.empty()) {
            g_skipped++;
            xlog::debug("[MeshOccluders] object {} '{}' contributed nothing", mesh->uid,
                        mesh->mesh_filename.c_str());
            continue;
        }
        // same composition the editor renders these objects with (mesh.cpp bounds transform)
        const Matrix3& o = mesh->orient;
        const Vector3& p = mesh->pos;
        auto to_world = [&](const Vector3& v) {
            return Vector3{p.x + o.rvec.x * v.x + o.uvec.x * v.y + o.fvec.x * v.z,
                           p.y + o.rvec.y * v.x + o.uvec.y * v.y + o.fvec.y * v.z,
                           p.z + o.rvec.z * v.x + o.uvec.z * v.y + o.fvec.z * v.z};
        };
        Vector3 lo{1e30f, 1e30f, 1e30f};
        Vector3 hi{-1e30f, -1e30f, -1e30f};
        int alpha_tris = 0;
        for (const LocalTri& t : geom.tris) {
            const Vector3 v[3] = {to_world(t.v0), to_world(t.v1), to_world(t.v2)};
            for (const Vector3& w : v) {
                lo = {std::min(lo.x, w.x), std::min(lo.y, w.y), std::min(lo.z, w.z)};
                hi = {std::max(hi.x, w.x), std::max(hi.y, w.y), std::max(hi.z, w.z)};
            }
            alpha_tris += t.alpha ? 1 : 0;
            out.push_back({v[0], v[1], v[2], mesh->uid, t.alpha});
        }
        g_objects++;
        g_tris += static_cast<int>(geom.tris.size());
        g_alpha_tris += alpha_tris;
        if (alpha_tris == static_cast<int>(geom.tris.size())) {
            g_all_alpha_objects.emplace_back(mesh->uid, mesh->mesh_filename.c_str());
        }
        xlog::debug("[MeshOccluders] object {} '{}' contributed {} triangles ({} alpha) in "
                    "({:.2f} {:.2f} {:.2f})-({:.2f} {:.2f} {:.2f})",
                    mesh->uid, mesh->mesh_filename.c_str(), geom.tris.size(), alpha_tris, lo.x,
                    lo.y, lo.z, hi.x, hi.y, hi.z);
    }
    return true;
}

void lightmap_mesh_occluders_release()
{
    g_geom_cache.clear();
}

void lightmap_mesh_occluder_report()
{
    auto* level = CDedLevel::Get();
    if (!level || !level->GetAlpineLevelProperties().meshes_occlude) {
        return;
    }
    xlog::info("[MeshOccluders] {} mesh objects contributed {} triangles ({} skipped)", g_objects,
               g_tris, g_skipped);
    // A triangle whose texture carries an alpha channel is in the class "Alpha-textured faces
    // block light" governs, so with that property off the object is in the tree yet blocks
    // nothing - loud, because it is otherwise a silent no-op like an unresolved brush flag.
    if (g_alpha_tris == 0 || level->GetAlpineLevelProperties().alpha_faces_occlude) {
        return;
    }
    xlog::warn("[MeshOccluders] {} of {} triangles are alpha-textured and cast no shadow - enable "
               "'Alpha-textured faces block light' if they should",
               g_alpha_tris, g_tris);
    for (const auto& e : g_all_alpha_objects) {
        xlog::warn("[MeshOccluders] object {} '{}' is entirely alpha-textured and casts no shadow "
                   "at all", e.first, e.second);
    }
}
