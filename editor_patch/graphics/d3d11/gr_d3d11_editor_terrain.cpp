#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <vector>
#include <xlog/xlog.h>
#include <common/gr_d3d11/gr_d3d11_af_lightmap.h>
#include <common/gr_d3d11/gr_d3d11_context.h>
#include <common/gr_d3d11/gr_d3d11_vertex.h>
#include "../../terrain_preview.h"
#include "gr_d3d11_editor_terrain_renderer.h"

namespace at = alpine_terrain;

namespace gr::d3d11
{
    namespace
    {
        struct PendingRect
        {
            bool any = false;
            std::uint32_t x0 = 0, z0 = 0, x1 = 0, z1 = 0;

            void add(std::uint32_t ax0, std::uint32_t az0, std::uint32_t ax1, std::uint32_t az1)
            {
                if (ax0 >= ax1 || az0 >= az1) return;
                if (!any) {
                    x0 = ax0;
                    z0 = az0;
                    x1 = ax1;
                    z1 = az1;
                    any = true;
                    return;
                }
                x0 = std::min(x0, ax0);
                z0 = std::min(z0, az0);
                x1 = std::max(x1, ax1);
                z1 = std::max(z1, az1);
            }
        };

        // What the chunks' vertices are placed from, besides the heights
        struct GeometryInputs
        {
            float origin[3] = {};
            float cell_size = 0.0f;
            float height_min = 0.0f;
            float height_range = 0.0f;
            float thickness = 0.0f;
            float skirt_depth = 0.0f;
            float underside_uv_scale = 0.0f;
            std::uint32_t flags = 0;
            std::uint32_t nx = 0;
            std::uint32_t nz = 0;
            std::uint32_t edge = 0;

            bool operator==(const GeometryInputs&) const = default;
        };

        GeometryInputs geometry_inputs(const at::GridView& v, std::uint32_t edge)
        {
            GeometryInputs g;
            std::memcpy(g.origin, v.origin, sizeof(g.origin));
            g.cell_size = v.cell_size;
            g.height_min = v.height_min;
            g.height_range = v.height_range;
            g.thickness = v.thickness;
            g.skirt_depth = v.skirt_depth;
            g.underside_uv_scale = v.layer_uv_scale[0];
            g.flags = v.flags;
            g.nx = v.nx;
            g.nz = v.nz;
            g.edge = edge;
            return g;
        }

        bool cell_bits_differ(const std::vector<std::uint8_t>& held, const std::uint8_t* now, std::uint32_t cx,
                              std::uint32_t x, std::uint32_t z)
        {
            return at::get_cell_bit(held.data(), cx, x, z) != at::get_cell_bit(now, cx, x, z);
        }
    }

    struct EditorTerrainRenderer::Terrain
    {
        struct Chunk
        {
            ComPtr<ID3D11Buffer> vertex_buffer;
            ComPtr<ID3D11Buffer> index_buffer;
            UINT num_indices = 0;
            rf::Vector3 lo{};
            rf::Vector3 hi{};
            bool dirty = true;
        };

        const void* grid = nullptr;
        GeometryInputs inputs;
        // The holes and diagonals the chunks were built with
        std::vector<std::uint8_t> holes;
        std::vector<std::uint8_t> diag;
        std::vector<Chunk> chunks;
        // Everything is read again from the next draw's source
        bool all = true;
        PendingRect cells;
        PendingRect verts;
    };

    EditorTerrainRenderer::EditorTerrainRenderer(ComPtr<ID3D11Device> device, ShaderManager& shader_manager,
                                                 RenderContext& render_context,
                                                 AfLightmapRenderer& af_lightmap_renderer) :
        device_{device}, render_context_{render_context}, af_lightmap_renderer_{af_lightmap_renderer},
        terrain_renderer_{device}
    {
        vertex_shader_ = shader_manager.get_vertex_shader(VertexShaderId::standard);
        pixel_shader_ = shader_manager.get_pixel_shader(PixelShaderId::terrain);
        pixel_shader_no_gas_ = shader_manager.get_pixel_shader(PixelShaderId::terrain_no_gas);
    }

    EditorTerrainRenderer::~EditorTerrainRenderer() = default;

    // False when the chunks cannot be built
    bool EditorTerrainRenderer::sync(Terrain& t, const EditorTerrainDraw& draw)
    {
        const at::GridView& v = draw.view;
        const void* key = draw.key;
        const TerrainSource& source = draw.source;
        ID3D11DeviceContext* context = render_context_.device_context();
        const std::uint32_t cx = at::cells(v.nx), cz = at::cells(v.nz);
        const std::uint32_t edge = draw.chunk_cells;
        const std::size_t bitmask_bytes = at::bitmask_bytes(cx, cz);

        const GeometryInputs inputs = geometry_inputs(v, edge);
        if (t.grid != draw.grid || t.inputs != inputs || t.holes.size() != bitmask_bytes) {
            t.grid = draw.grid;
            t.inputs = inputs;
            t.all = true;
        }
        if (t.all) {
            terrain_renderer_.update_heights(context, key, source, 0, 0, v.nx, v.nz);
            terrain_renderer_.update_maps(context, key, source, 0, 0, cx, cz);
            t.chunks.assign(at::chunk_count(cx, cz, edge), Terrain::Chunk{});
        }
        else {
            if (t.verts.any) {
                terrain_renderer_.update_heights(context, key, source, t.verts.x0, t.verts.z0, t.verts.x1, t.verts.z1);
                for (std::uint32_t k = 0; k < t.chunks.size(); k++) {
                    // A chunk spans vertices [x0, x1] x [z0, z1].
                    const at::ChunkRect r = at::chunk_rect(cx, cz, edge, k);
                    if (r.x0 < t.verts.x1 && t.verts.x0 <= r.x1 && r.z0 < t.verts.z1 && t.verts.z0 <= r.z1) {
                        t.chunks[k].dirty = true;
                    }
                }
            }
            if (t.cells.any) {
                terrain_renderer_.update_maps(context, key, source, t.cells.x0, t.cells.z0, t.cells.x1, t.cells.z1);
                // Only holes change geometry. A skirt follows the holes next to its chunk, so the box of the changed
                // cells grows by one.
                PendingRect changed;
                const std::uint32_t x1 = std::min(t.cells.x1, cx), z1 = std::min(t.cells.z1, cz);
                for (std::uint32_t z = t.cells.z0; z < z1; z++) {
                    for (std::uint32_t x = t.cells.x0; x < x1; x++) {
                        if (cell_bits_differ(t.holes, v.holes, cx, x, z) || cell_bits_differ(t.diag, v.diag, cx, x, z)) {
                            changed.add(x > 0 ? x - 1 : 0, z > 0 ? z - 1 : 0, x + 2, z + 2);
                        }
                    }
                }
                if (changed.any) {
                    for (std::uint32_t k = 0; k < t.chunks.size(); k++) {
                        const at::ChunkRect r = at::chunk_rect(cx, cz, edge, k);
                        if (r.x0 < changed.x1 && changed.x0 < r.x1 && r.z0 < changed.z1 && changed.z0 < r.z1) {
                            t.chunks[k].dirty = true;
                        }
                    }
                }
            }
        }
        t.all = false;
        t.cells = {};
        t.verts = {};

        bool rebuilt = false;
        for (std::uint32_t k = 0; k < t.chunks.size(); k++) {
            if (!t.chunks[k].dirty) continue;
            if (!build_chunk(v, edge, k, t)) {
                t.all = true;
                return false;
            }
            rebuilt = true;
        }
        if (rebuilt || t.holes.size() != bitmask_bytes) {
            t.holes.assign(v.holes, v.holes + bitmask_bytes);
            t.diag.assign(v.diag, v.diag + bitmask_bytes);
        }
        return true;
    }

    bool EditorTerrainRenderer::build_chunk(const at::GridView& v, std::uint32_t edge, std::uint32_t index, Terrain& t)
    {
        const std::uint32_t cx = at::cells(v.nx), cz = at::cells(v.nz);
        const at::ChunkRect r = at::chunk_rect(cx, cz, edge, index);
        Terrain::Chunk& chunk = t.chunks[index];
        chunk = Terrain::Chunk{};

        // The top grid's vertices are shared: the shader takes the top's normal from the heights. Walls and
        // bottoms get their own, with the face's normal.
        std::vector<GpuVertex> verts;
        std::vector<std::uint32_t> inds;
        const std::uint32_t top_count = (r.x1 - r.x0 + 1) * (r.z1 - r.z0 + 1);
        verts.reserve(top_count);
        for (std::uint32_t z = r.z0; z <= r.z1; z++) {
            for (std::uint32_t x = r.x0; x <= r.x1; x++) {
                float p[3];
                at::grid_position(v, x, z, p);
                GpuVertex& gv = verts.emplace_back();
                gv.x = p[0];
                gv.y = p[1];
                gv.z = p[2];
                gv.norm = {0.0f, 1.0f, 0.0f};
                gv.diffuse = static_cast<int>(0xFFFFFFFF);
                gv.u0 = gv.v0 = gv.u0_pan_speed = gv.v0_pan_speed = gv.u1 = gv.v1 = 0.0f;
                // The terrain shader's face kind: 0 top, 1 underside, 2 crater
                gv.lm_chart = static_cast<float>(static_cast<int>(at::FaceKind::top));
            }
        }
        at::emit_chunk(v, r, [&](const at::EmitFace& f) {
            if (f.count < 3) return;
            std::uint32_t base = 0;
            if (f.material != at::material_underside) {
                // Top faces only use top slots, which are the shared vertices' indices
                for (std::uint32_t k = 1; k + 1 < f.count; k++) {
                    inds.insert(inds.end(), {f.slot[0], f.slot[k], f.slot[k + 1]});
                }
                return;
            }
            float p[4][3];
            for (std::uint32_t k = 0; k < f.count; k++) at::slot_position(v, r, f.slot[k], p[k]);
            const float e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
            const float e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
            float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
            const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (len > 0.0f) {
                for (float& c : n) c /= len;
            }
            base = static_cast<std::uint32_t>(verts.size());
            for (std::uint32_t k = 0; k < f.count; k++) {
                GpuVertex& gv = verts.emplace_back();
                gv.x = p[k][0];
                gv.y = p[k][1];
                gv.z = p[k][2];
                gv.norm = {n[0], n[1], n[2]};
                gv.diffuse = static_cast<int>(0xFFFFFFFF);
                gv.u0 = f.uv[k][0];
                gv.v0 = f.uv[k][1];
                gv.u0_pan_speed = gv.v0_pan_speed = gv.u1 = gv.v1 = 0.0f;
                gv.lm_chart = static_cast<float>(static_cast<int>(at::FaceKind::underside));
            }
            for (std::uint32_t k = 1; k + 1 < f.count; k++) {
                inds.insert(inds.end(), {base, base + k, base + k + 1});
            }
        });
        if (inds.empty()) {
            // Every cell a hole: nothing to draw
            chunk.dirty = false;
            return true;
        }

        chunk.lo = {verts[0].x, verts[0].y, verts[0].z};
        chunk.hi = chunk.lo;
        for (const GpuVertex& gv : verts) {
            chunk.lo = {std::min(chunk.lo.x, gv.x), std::min(chunk.lo.y, gv.y), std::min(chunk.lo.z, gv.z)};
            chunk.hi = {std::max(chunk.hi.x, gv.x), std::max(chunk.hi.y, gv.y), std::max(chunk.hi.z, gv.z)};
        }
        CD3D11_BUFFER_DESC vb_desc{static_cast<UINT>(verts.size() * sizeof(GpuVertex)), D3D11_BIND_VERTEX_BUFFER,
                                   D3D11_USAGE_IMMUTABLE};
        const D3D11_SUBRESOURCE_DATA vb_data{verts.data(), 0, 0};
        CD3D11_BUFFER_DESC ib_desc{static_cast<UINT>(inds.size() * sizeof(std::uint32_t)), D3D11_BIND_INDEX_BUFFER,
                                   D3D11_USAGE_IMMUTABLE};
        const D3D11_SUBRESOURCE_DATA ib_data{inds.data(), 0, 0};
        if (FAILED(device_->CreateBuffer(&vb_desc, &vb_data, &chunk.vertex_buffer))
            || FAILED(device_->CreateBuffer(&ib_desc, &ib_data, &chunk.index_buffer))) {
            chunk = Terrain::Chunk{};
            return false;
        }
        chunk.num_indices = static_cast<UINT>(inds.size());
        chunk.dirty = false;
        return true;
    }

    bool EditorTerrainRenderer::draw(const EditorTerrainDraw& draw, rf::gr::Mode mode, D3D11_CULL_MODE cull_mode)
    {
        const at::GridView& v = draw.view;
        if (v.nx < 2 || v.nz < 2 || !draw.chunk_cells || !v.heights || !v.holes || !v.diag) {
            return false;
        }
        std::unique_ptr<Terrain>& t = terrains_[draw.key];
        if (!t) {
            t = std::make_unique<Terrain>();
        }
        // A source of another shape gets new maps, which prepare fills from it
        if (!terrain_renderer_.prepare(draw.key, draw.source)) {
            return false;
        }
        try {
            if (!sync(*t, draw)) {
                xlog::warn("[Terrain] A terrain's chunks could not be built for the Direct3D 11 view");
                return false;
            }
        }
        catch (const std::bad_alloc&) {
            t->all = true;
            return false;
        }

        ID3D11DeviceContext* context = render_context_.device_context();
        // The lights the level faces get (SolidRenderer::render_solid); the shader reads the level ambient from them
        rf::GSolid* const solid = engine::level_solid();
        if (solid) {
            rf::gr::light_filter_set_solid(solid, 1, 0);
        }
        render_context_.update_lights();
        render_context_.set_vertex_shader(vertex_shader_);
        render_context_.set_pixel_shader(render_context_.has_gas_regions() ? pixel_shader_ : pixel_shader_no_gas_);
        render_context_.set_model_transform(rf::zero_vector, rf::identity_matrix);
        render_context_.set_cull_mode(cull_mode);
        render_context_.set_primitive_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        af_lightmap_renderer_.bind();
        render_context_.set_mode(mode, {255, 255, 255, 255}, draw.lightmaps_only);
        const bool bound = terrain_renderer_.bind(context, render_context_, draw.key, draw.source);
        if (bound) {
            for (const Terrain::Chunk& chunk : t->chunks) {
                if (!chunk.num_indices || rf::gr::cull_bounding_box(chunk.lo, chunk.hi)) continue;
                render_context_.set_vertex_buffer(chunk.vertex_buffer, sizeof(GpuVertex));
                render_context_.set_index_buffer(chunk.index_buffer, DXGI_FORMAT_R32_UINT);
                render_context_.draw_indexed(static_cast<int>(chunk.num_indices), 0, 0);
            }
            terrain_renderer_.unbind(context);
        }
        if (solid) {
            rf::gr::light_filter_reset();
        }
        render_context_.update_lights();
        return bound;
    }

    void EditorTerrainRenderer::cells_changed(const void* key, const TerrainCellRect* cells)
    {
        const auto it = terrains_.find(key);
        if (it == terrains_.end()) return;
        if (!cells) {
            it->second->all = true;
            return;
        }
        it->second->cells.add(cells->x0, cells->z0, cells->x1, cells->z1);
    }

    void EditorTerrainRenderer::heights_changed(const void* key, const TerrainCellRect* verts)
    {
        const auto it = terrains_.find(key);
        if (it == terrains_.end()) return;
        if (!verts) {
            it->second->all = true;
            return;
        }
        it->second->verts.add(verts->x0, verts->z0, verts->x1, verts->z1);
    }

    void EditorTerrainRenderer::rebind_grid(const void* key, const void* old_grid, const void* new_grid)
    {
        const auto it = terrains_.find(key);
        if (it != terrains_.end() && it->second->grid == old_grid) {
            it->second->grid = new_grid;
        }
    }

    void EditorTerrainRenderer::textures_reloaded()
    {
        // The next draw's prepare makes the maps and loads the bitmaps again; the chunks stay
        for (const auto& entry : terrains_) {
            terrain_renderer_.release(entry.first);
        }
    }

    void EditorTerrainRenderer::forget(const void* key)
    {
        terrain_renderer_.release(key);
        terrains_.erase(key);
    }
}
