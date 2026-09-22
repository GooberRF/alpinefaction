#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>
#include <vector>
#include <patch_common/MemUtils.h>
#include <xlog/xlog.h>
#include "../../rf/vfx.h"
#include "../../rf/vmesh.h"
#include "../../rf/gr/gr.h"
#include "../gr.h"
#include "gr_d3d11.h"
#include "gr_d3d11_context.h"
#include "gr_d3d11_mesh.h"
#include "gr_d3d11_vfx.h"

namespace gr::d3d11
{
    // Base + specular + chrome copies of a chunk must fit one ring allocation (no overflow guard in RingBuffer)
    constexpr int vfx_ring_verts = 32768;
    constexpr int vfx_ring_indices = 65536;

    // dbg_vfxcull (debug builds): 1 none, 2 front, 3 back (D3D11_CULL_MODE values)
    D3D11_CULL_MODE g_vfx_cull_mode = D3D11_CULL_BACK;

    // Engine helpers the stock gr_d3d_render_vfx (0x00553EE0) uses per material / per vertex
    static auto& gr_light_rotate_all = addr_as_ref<void()>(0x004D9FD0);
    static auto& gr_light_apply = addr_as_ref<void(rf::ubyte* r, rf::ubyte* g, rf::ubyte* b, char use_ambient,
        const rf::Vector3* pos, const rf::Vector3* normal, float scale)>(0x004DAFF0);
    static auto& gr_light_apply_specular = addr_as_ref<bool(const rf::Vector3* pos, const rf::Vector3* normal,
        rf::ubyte* r, rf::ubyte* g, rf::ubyte* b, float level, float power, char allow_restricted)>(0x004DB1B0);
    static auto& gr_apply_chrome_mapping =
        addr_as_ref<void(float* u, float* v, const rf::Vector3* pos, const rf::Vector3* normal)>(0x004DB760);
    static auto& bm_has_alpha = addr_as_ref<bool(int bm_handle)>(0x00510710);
    static auto& sky_room_pass_active = addr_as_ref<bool>(0x0088FD1C);

    // Material animation curves, __thiscall on the material (frame is in 15 fps units)
    static int texmap_get_bitmap(const rf::TexMap* tm, float frame)
    {
        return AddrCaller{0x0054A630}.this_call<int>(tm, frame);
    }
    static float material_get_opacity(const rf::MeshMaterial* m, float frame)
    {
        return AddrCaller{0x0054AA80}.this_call<float>(m, frame);
    }
    static float material_get_self_illum(const rf::MeshMaterial* m, float frame)
    {
        return AddrCaller{0x0054A9E0}.this_call<float>(m, frame);
    }
    static float material_get_crossfade(const rf::MeshMaterial* m, float t)
    {
        return AddrCaller{0x0054A930}.this_call<float>(m, t);
    }

    static const rf::MeshMaterial* slot_material(const rf::VfxSfxoChunk* chunk, int slot)
    {
        return &chunk->geo->materials[chunk->material_indices[slot]];
    }

    // Stock mode ladder for a textured/untextured base pass
    static rf::gr::Mode vfx_base_mode(const rf::VfxSfxoChunk* chunk, const rf::MeshMaterial* m, int bm)
    {
        if (bm < 0) {
            return addr_as_ref<rf::gr::Mode>(0x017C7BB8);
        }
        if (chunk->render_flags & 8) {
            return addr_as_ref<rf::gr::Mode>(0x01775B10);
        }
        if (m->use_additive_blending) {
            return addr_as_ref<rf::gr::Mode>(0x017756B8);
        }
        if (bm_has_alpha(bm)) {
            return {rf::gr::TEXTURE_SOURCE_WRAP, rf::gr::COLOR_SOURCE_VERTEX_TIMES_TEXTURE,
                rf::gr::ALPHA_SOURCE_VERTEX_TIMES_TEXTURE, rf::gr::ALPHA_BLEND_ALPHA,
                rf::gr::ZBUFFER_TYPE_FULL_ALPHA_TEST, rf::gr::FOG_ALLOWED};
        }
        return addr_as_ref<rf::gr::Mode>(0x01775AEC);
    }

    // Stock mode ladder for the two crossfade (material_type 1) passes
    static rf::gr::Mode vfx_crossfade_mode(const rf::MeshMaterial* m, int bm)
    {
        if (m->use_additive_blending) {
            return addr_as_ref<rf::gr::Mode>(0x017756B8);
        }
        if (bm_has_alpha(bm)) {
            return addr_as_ref<rf::gr::Mode>(0x017C7C64);
        }
        return addr_as_ref<rf::gr::Mode>(0x01775AEC);
    }

    static rf::ubyte to_byte(float v)
    {
        return static_cast<rf::ubyte>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
    }

    bool vfx_gpu_eligible(const rf::VfxSfxoRenderObj* obj, float* radius_out)
    {
        const rf::VfxSfxoChunk* chunk = obj->chunk;
        if (!chunk || !chunk->geo || !chunk->geo->materials || chunk->num_faces <= 0 || !chunk->faces ||
            !chunk->material_indices || !chunk->vertex_records || !obj->vertex_positions || !obj->face_uvs) {
            return false;
        }
        // Sky room: stock composes the sky transform around the eye
        if (sky_room_pass_active) {
            return false;
        }
        // Worst case: no corner sharing, plus specular and chrome copies
        if (chunk->num_faces * 3 * 3 > vfx_ring_verts || chunk->num_faces * 3 > vfx_ring_indices) {
            return false;
        }
        float r2 = 0.0f;
        for (int i = 0; i < chunk->num_vertices; ++i) {
            r2 = std::max(r2, obj->vertex_positions[i].len_sq());
        }
        *radius_out = std::sqrt(r2);
        return true;
    }

    VfxMeshRenderer::VfxMeshRenderer(ComPtr<ID3D11Device> device, ShaderManager& shader_manager, RenderContext& render_context) :
        render_context_{render_context},
        vertex_ring_buffer_{vfx_ring_verts, D3D11_BIND_VERTEX_BUFFER, device, render_context.device_context()},
        index_ring_buffer_{vfx_ring_indices, D3D11_BIND_INDEX_BUFFER, device, render_context.device_context()}
    {
        vertex_shader_ = shader_manager.get_vertex_shader(VertexShaderId::standard);
        pixel_shader_ = shader_manager.get_pixel_shader(PixelShaderId::standard);
        pixel_shader_no_gas_ = shader_manager.get_pixel_shader(PixelShaderId::standard_no_gas);
    }

    const VfxMeshRenderer::Topology& VfxMeshRenderer::get_topology(const rf::VfxSfxoChunk* chunk, const rf::VfxSfxoRenderObj* obj)
    {
        Topology& t = topology_cache_[chunk];
        // ponytail: keyed by chunk address; a freed+reused address is caught by these checks, entries are
        // never evicted (a handful of chunks per level)
        if (t.faces_ptr == chunk->faces && t.records_ptr == chunk->vertex_records && t.num_faces == chunk->num_faces &&
            t.num_vertices == chunk->num_vertices && t.num_records == chunk->num_vertex_records) {
            return t;
        }
        t = {};
        t.faces_ptr = chunk->faces;
        t.records_ptr = chunk->vertex_records;
        t.num_faces = chunk->num_faces;
        t.num_vertices = chunk->num_vertices;
        t.num_records = chunk->num_vertex_records;
        const int num_slots = chunk->num_materials;
        // Per-key UVs (render_flags & 0x100) change every frame: no corner merging for those chunks
        const bool static_uvs = (chunk->render_flags & 0x100) == 0;
        auto vertex_ok = [&](int i) { return i >= 0 && i < t.num_vertices; };
        auto record_index = [&](const rf::VfxVertexRecord* rec) -> int {
            if (!rec) {
                return -1;
            }
            const auto idx = rec - chunk->vertex_records;
            return idx >= 0 && idx < t.num_records ? static_cast<int>(idx) : -1;
        };

        // key: vertex, record, slot, uv bits (or the corner itself when uvs animate)
        std::map<std::tuple<int, int, int, unsigned, unsigned>, int> unique_lookup;
        for (int f = 0; f < t.num_faces; ++f) {
            const rf::VfxSubObject& face = chunk->faces[f];
            if (face.material_slot < 0 || face.material_slot >= num_slots || !vertex_ok(face.vertex_indices[0]) ||
                !vertex_ok(face.vertex_indices[1]) || !vertex_ok(face.vertex_indices[2])) {
                continue;
            }
            const int cf_index = static_cast<int>(t.faces.size());
            Topology::Face cf{};
            cf.slot = face.material_slot;
            for (int j = 0; j < 3; ++j) {
                cf.vertex[j] = face.vertex_indices[j];
                const int rec = record_index(face.corner_records[j]);
                cf.record[j] = rec;
                unsigned ubits, vbits;
                if (static_uvs) {
                    std::memcpy(&ubits, &obj->face_uvs[f].u[j], sizeof(ubits));
                    std::memcpy(&vbits, &obj->face_uvs[f].v[j], sizeof(vbits));
                }
                else {
                    ubits = static_cast<unsigned>(f);
                    vbits = static_cast<unsigned>(j);
                }
                auto [it, inserted] = unique_lookup.try_emplace({cf.vertex[j], rec, cf.slot, ubits, vbits},
                    static_cast<int>(t.unique.size()));
                if (inserted) {
                    t.unique.push_back({cf.vertex[j], rec, cf_index, f, j, cf.slot});
                }
                cf.corner[j] = it->second;
            }
            t.faces.push_back(cf);
        }

        t.record_vertex.resize(t.num_records);
        for (int r = 0; r < t.num_records; ++r) {
            const int vi = chunk->vertex_records[r].vertex_index;
            t.record_vertex[r] = vertex_ok(vi) ? vi : -1;
        }
        return t;
    }

    void VfxMeshRenderer::render(rf::VfxSfxoRenderObj* obj, float frame)
    {
        rf::VfxSfxoChunk* chunk = obj->chunk;
        const Topology& topo = get_topology(chunk, obj);
        const rf::Vector3* pos = obj->vertex_positions;
        const int num_slots = chunk->num_materials;
        const int num_faces = static_cast<int>(topo.faces.size());
        const int num_unique = static_cast<int>(topo.unique.size());
        const bool fullbright = (chunk->render_flags & 0x10) != 0;
        // Vertex-lit levels/settings: CPU colours via the engine's own gr_light_apply, like stock and like
        // the v3d path (which keeps stock's CPU vertex colours there). Otherwise the pixel shader lights.
        const bool vertex_lit = level_uses_vertex_lighting() && !fullbright;
        if (!num_faces) {
            return;
        }

        // Face normals from the animated positions (stock: set_face_normal every frame), then the
        // smoothed per-record normals stock averages from the adjacent faces
        // (a record's adjacent-face list is exactly the faces whose corners reference it)
        face_normals_.resize(num_faces);
        record_normals_.assign(topo.num_records, rf::Vector3{0.0f, 0.0f, 0.0f});
        for (int f = 0; f < num_faces; ++f) {
            const Topology::Face& face = topo.faces[f];
            const rf::Vector3& p0 = pos[face.vertex[0]];
            rf::Vector3 n = (pos[face.vertex[1]] - p0).cross(pos[face.vertex[2]] - p0);
            const float len_sq = n.len_sq();
            if (len_sq > 1e-12f) {
                n *= 1.0f / std::sqrt(len_sq);
            }
            face_normals_[f] = n;
            for (int r : face.record) {
                if (r >= 0) {
                    record_normals_[r] += n;
                }
            }
        }
        for (rf::Vector3& n : record_normals_) {
            const float len_sq = n.len_sq();
            // a zero normal means "screen-space vertex" to the pixel shader
            n = len_sq > 1e-12f ? n * (1.0f / std::sqrt(len_sq)) : rf::Vector3{0.0f, 1.0f, 0.0f};
        }

        // Bucket faces by material slot; stock draws slots in descending order (sort key bias slot * 61)
        struct Slot
        {
            const rf::MeshMaterial* material = nullptr;
            int count = 0;  // indices
            int offset = 0; // into this chunk's index range
            int cursor = 0;
            bool specular = false;
            bool chrome = false;
            bool spec_any = false;
            float opacity = 1.0f;
            float self_illum = 0.0f;
            rf::ubyte illum_floor = 0;
            rf::Color tint{255, 255, 255, 255};
        };
        std::vector<Slot> slots(num_slots);
        for (const Topology::Face& face : topo.faces) {
            slots[face.slot].count += 3;
        }
        int total_indices = 0;
        bool any_specular = false;
        bool any_chrome = false;
        for (int s = num_slots - 1; s >= 0; --s) {
            Slot& sl = slots[s];
            if (!sl.count) {
                continue;
            }
            sl.material = slot_material(chunk, s);
            sl.opacity = material_get_opacity(sl.material, frame);
            sl.self_illum = fullbright ? 1.0f : material_get_self_illum(sl.material, frame);
            sl.illum_floor = to_byte(sl.self_illum);
            if (sl.material->material_type == 2) {
                sl.tint = {sl.material->diffuse_color.red, sl.material->diffuse_color.green, sl.material->diffuse_color.blue, 255};
            }
            sl.specular = sl.material->specular_level > 0.0f;
            sl.chrome = sl.material->refl_tex_handle > 0;
            any_specular |= sl.specular;
            any_chrome |= sl.chrome;
            sl.offset = total_indices;
            total_indices += sl.count;
        }

        // Vertex lighting / specular / chrome reuse the engine's per-vertex functions, which expect the
        // instance transform and lights rotated into it (exactly what stock sets up before its face loop)
        const bool need_instance = vertex_lit || any_specular || any_chrome;
        if (need_instance) {
            rf::gr::start_instance(obj->render_pos, obj->render_orient);
            gr_light_rotate_all();
        }
        if (vertex_lit) {
            record_lit_.assign(topo.num_records, rf::Color{255, 255, 255, 255});
            for (int r = 0; r < topo.num_records; ++r) {
                const int vi = topo.record_vertex[r];
                if (vi >= 0) {
                    rf::Color& c = record_lit_[r];
                    gr_light_apply(&c.red, &c.green, &c.blue, 1, &pos[vi], &record_normals_[r], 2.0f);
                }
            }
        }

        // Vertex regions: base, then optional specular and chrome copies (same indices, base_vertex offset).
        // Built in cached memory, then one memcpy into the write-combined mapped buffer.
        const int spec_region = any_specular ? num_unique : -1;
        const int chrome_region = any_chrome ? num_unique * (any_specular ? 2 : 1) : -1;
        const int total_verts = num_unique * (1 + (any_specular ? 1 : 0) + (any_chrome ? 1 : 0));
        vertex_scratch_.resize(total_verts);
        GpuVertex* verts = vertex_scratch_.data();
        for (int i = 0; i < num_unique; ++i) {
            const Topology::Unique& u = topo.unique[i];
            const Slot& sl = slots[u.slot];
            const rf::Vector3& n = u.record >= 0 ? record_normals_[u.record] : face_normals_[u.face];
            const rf::VfxFaceUv& uv = obj->face_uvs[u.src_face];
            rf::Color diffuse = sl.tint;
            if (vertex_lit && u.record >= 0) {
                // Stock: lit colour floored at self-illumination, then scaled by the type-2 tint
                const rf::Color& lit = record_lit_[u.record];
                diffuse.red = static_cast<rf::ubyte>(std::max(lit.red, sl.illum_floor) * sl.tint.red / 255);
                diffuse.green = static_cast<rf::ubyte>(std::max(lit.green, sl.illum_floor) * sl.tint.green / 255);
                diffuse.blue = static_cast<rf::ubyte>(std::max(lit.blue, sl.illum_floor) * sl.tint.blue / 255);
            }
            GpuVertex& gv = verts[i];
            gv.x = pos[u.vertex].x;
            gv.y = pos[u.vertex].y;
            gv.z = pos[u.vertex].z;
            gv.norm = {n.x, n.y, n.z};
            gv.diffuse = pack_color(diffuse);
            gv.u0 = uv.u[u.uv_corner];
            gv.v0 = uv.v[u.uv_corner];
            gv.u0_pan_speed = 0.0f;
            gv.v0_pan_speed = 0.0f;
            gv.u1 = 0.0f;
            gv.v1 = 0.0f;
            if (spec_region >= 0) {
                GpuVertex& sv = verts[spec_region + i];
                sv = gv;
                rf::Color spec{0, 0, 0, 0};
                if (sl.specular && gr_light_apply_specular(&pos[u.vertex], &n, &spec.red, &spec.green, &spec.blue,
                        sl.material->specular_level, sl.material->glossiness, 1)) {
                    slots[u.slot].spec_any = true;
                }
                spec.alpha = static_cast<rf::ubyte>((spec.red + spec.green + spec.blue) / 3);
                sv.diffuse = pack_color(spec);
            }
            if (chrome_region >= 0) {
                GpuVertex& cv = verts[chrome_region + i];
                cv = gv;
                cv.diffuse = pack_color({255, 255, 255, 255});
                if (sl.chrome) {
                    gr_apply_chrome_mapping(&cv.u0, &cv.v0, &pos[u.vertex], &n);
                }
            }
        }
        index_scratch_.resize(total_indices);
        rf::ushort* inds = index_scratch_.data();
        for (const Topology::Face& face : topo.faces) {
            Slot& sl = slots[face.slot];
            rf::ushort* out = inds + sl.offset + sl.cursor;
            out[0] = static_cast<rf::ushort>(face.corner[0]);
            out[1] = static_cast<rf::ushort>(face.corner[1]);
            out[2] = static_cast<rf::ushort>(face.corner[2]);
            sl.cursor += 3;
        }
        std::memcpy(vertex_ring_buffer_.alloc(total_verts), verts, total_verts * sizeof(GpuVertex));
        std::memcpy(index_ring_buffer_.alloc(total_indices), inds, total_indices * sizeof(rf::ushort));
        const int vb_start = vertex_ring_buffer_.submit().first;
        const int ib_start = index_ring_buffer_.submit().first;

        if (need_instance) {
            rf::gr::stop_instance();
        }

        render_context_.set_model_transform(obj->render_pos, obj->render_orient);
        render_context_.set_vertex_buffer(vertex_ring_buffer_.get_buffer(), sizeof(GpuVertex));
        render_context_.set_index_buffer(index_ring_buffer_.get_buffer());
        render_context_.set_vertex_shader(vertex_shader_);
        render_context_.set_pixel_shader(render_context_.has_gas_regions() ? pixel_shader_ : pixel_shader_no_gas_);
        render_context_.set_primitive_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        // Stock culls faces whose normal points away from the camera (single-sided)
        render_context_.set_cull_mode(g_vfx_cull_mode);

        const bool gpu_lit = !vertex_lit && !fullbright;
        if (gpu_lit) {
            render_context_.update_lights(false, nullptr, gr_sun_get_mesh_scale(nullptr));
        }
        else {
            render_context_.update_lights(true); // vertex colours are the whole result
        }

        // Base passes, slots descending. ponytail: no intra-slot back-to-front face sort (z-test +
        // alpha test cover foliage); add a slot-level depth sort if alpha-blended vfx visibly mis-order.
        for (int s = num_slots - 1; s >= 0; --s) {
            const Slot& sl = slots[s];
            if (!sl.count) {
                continue;
            }
            const rf::MeshMaterial* m = sl.material;
            auto draw_pass = [&](int bm, rf::gr::Mode mode, float weight) {
                const rf::ubyte w = to_byte(weight);
                const rf::Color color{w, w, w, to_byte(weight * sl.opacity)};
                render_context_.set_textures(bm, -1);
                // Stock painter-sorts every face back to front. Without that, an alpha-blended soft edge
                // drawn first writes depth and the leaf behind it is rejected, leaving a bright halo of
                // background around each leaf. Split soft-alpha materials into an opaque cutout pass
                // (alpha >= 0.5, z write) and a blended edge pass (z read only) instead.
                if (mode.get_alpha_blend() == rf::gr::ALPHA_BLEND_ALPHA &&
                    mode.get_zbuffer_type() == rf::gr::ZBUFFER_TYPE_FULL_ALPHA_TEST) {
                    rf::gr::Mode cutout = mode;
                    cutout.set_alpha_blend(rf::gr::ALPHA_BLEND_NONE);
                    const float saved_threshold = g_alpha_test_threshold;
                    g_alpha_test_threshold = 0.5f;
                    render_context_.set_mode(cutout, color, false, gpu_lit, gpu_lit ? sl.self_illum : 0.0f, gpu_lit, false);
                    render_context_.draw_indexed(sl.count, ib_start + sl.offset, vb_start);
                    g_alpha_test_threshold = saved_threshold;
                    mode.set_zbuffer_type(rf::gr::ZBUFFER_TYPE_READ);
                }
                render_context_.set_mode(mode, color, false, gpu_lit, gpu_lit ? sl.self_illum : 0.0f, gpu_lit, false);
                render_context_.draw_indexed(sl.count, ib_start + sl.offset, vb_start);
            };
            if (m->material_type == 1) {
                const float fade = material_get_crossfade(m, frame - chunk->start_time);
                const int bm0 = texmap_get_bitmap(&m->texture_maps[0], frame);
                const int bm1 = texmap_get_bitmap(&m->texture_maps[1], frame);
                draw_pass(bm0, vfx_crossfade_mode(m, bm0), 1.0f - fade);
                draw_pass(bm1, vfx_crossfade_mode(m, bm1), fade);
            }
            else {
                const int bm = m->material_type == 2 ? -1 : texmap_get_bitmap(&m->texture_maps[0], frame);
                draw_pass(bm, vfx_base_mode(chunk, m, bm), 1.0f);
            }
        }

        // Specular / chrome passes: unlit (vertex colour is the whole result), z-read only
        if (any_specular || any_chrome) {
            if (gpu_lit) {
                render_context_.update_lights(true);
            }
            for (int s = num_slots - 1; s >= 0; --s) {
                const Slot& sl = slots[s];
                if (!sl.count) {
                    continue;
                }
                if (sl.specular && sl.spec_any) {
                    render_context_.set_mode({rf::gr::TEXTURE_SOURCE_NONE, rf::gr::COLOR_SOURCE_VERTEX,
                        rf::gr::ALPHA_SOURCE_VERTEX, rf::gr::ALPHA_BLEND_ALPHA_ADDITIVE, rf::gr::ZBUFFER_TYPE_READ,
                        rf::gr::FOG_ALLOWED}, {255, 255, 255, to_byte(sl.opacity)}, false, false, 0.0f, false, false);
                    render_context_.set_textures(-1, -1);
                    render_context_.draw_indexed(sl.count, ib_start + sl.offset, vb_start + spec_region);
                }
                if (sl.chrome) {
                    render_context_.set_mode({rf::gr::TEXTURE_SOURCE_WRAP, rf::gr::COLOR_SOURCE_TEXTURE,
                        rf::gr::ALPHA_SOURCE_VERTEX, rf::gr::ALPHA_BLEND_ALPHA, rf::gr::ZBUFFER_TYPE_READ,
                        rf::gr::FOG_ALLOWED}, {255, 255, 255, to_byte(sl.opacity * sl.material->reflection_amount)},
                        false, false, 0.0f, false, false);
                    render_context_.set_textures(sl.material->refl_tex_handle, -1);
                    render_context_.draw_indexed(sl.count, ib_start + sl.offset, vb_start + chrome_region);
                }
            }
        }
    }
}
