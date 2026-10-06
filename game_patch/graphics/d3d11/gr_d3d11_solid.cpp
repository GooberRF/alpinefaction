#define NO_D3D8
#undef NDEBUG

#include <windows.h>
#include <algorithm>
#include <vector>
#include <unordered_map>
#include <map>
#include <memory>
#include <common/ComPtr.h>
#include <xlog/xlog.h>
#include "../../rf/geometry.h"
#include "../../rf/gr/gr.h"
#include "../../rf/gr/gr_light.h"
#include "../../rf/level.h"
#include "../../rf/mover.h"
#include <common/utils/list-utils.h>
#include "../../os/console.h"
#include "../../misc/misc.h"
#include "../../misc/alpine_options.h"
#include "../../misc/alpine_terrain.h"
#include "../../misc/alpine_terrain_decorations.h"
#include "../../os/os.h"
#include "../af_lightmap.h"
#include "gr_d3d11.h"
#include "gr_d3d11_af_lightmap.h"
#include "gr_d3d11_solid.h"
#include "gr_d3d11_shader.h"
#include "gr_d3d11_context.h"
#include "gr_d3d11_dynamic_geometry.h"
#include "gr_d3d11_terrain.h"

namespace gr::d3d11
{
    class RoomRenderCache;
    class GRenderCache;

    static auto& decals_enabled = addr_as_ref<bool>(0x005A4458);
    static auto& gr_decal_tiling_u_mode = addr_as_ref<rf::gr::Mode>(0x0180831C);
    static auto& gr_decal_tiling_v_mode = addr_as_ref<rf::gr::Mode>(0x0181833C);
    static auto& gr_decal_mode = addr_as_ref<rf::gr::Mode>(0x01808318);
    static auto& gr_solid_mode = addr_as_ref<rf::gr::Mode>(0x01808328);
    static auto& gr_solid_alpha_mode = addr_as_ref<rf::gr::Mode>(0x0180832C);

    static rf::gr::Mode sky_room_opaque_mode{
        rf::gr::TEXTURE_SOURCE_WRAP,
        rf::gr::COLOR_SOURCE_TEXTURE,
        rf::gr::ALPHA_SOURCE_TEXTURE,
        rf::gr::ALPHA_BLEND_NONE,
        rf::gr::ZBUFFER_TYPE_FULL,
        rf::gr::FOG_NOT_ALLOWED,
    };

    static rf::gr::Mode sky_room_alpha_mode{
        rf::gr::TEXTURE_SOURCE_WRAP,
        rf::gr::COLOR_SOURCE_TEXTURE,
        rf::gr::ALPHA_SOURCE_TEXTURE,
        rf::gr::ALPHA_BLEND_ALPHA,
        rf::gr::ZBUFFER_TYPE_READ,
        rf::gr::FOG_NOT_ALLOWED,
    };

    static rf::gr::Mode alpha_detail_fullbright_mode{
        rf::gr::TEXTURE_SOURCE_WRAP,
        rf::gr::COLOR_SOURCE_TEXTURE,
        rf::gr::ALPHA_SOURCE_TEXTURE,
        rf::gr::ALPHA_BLEND_ALPHA,
        rf::gr::ZBUFFER_TYPE_FULL_ALPHA_TEST,
        rf::gr::FOG_ALLOWED,
    };

    static inline bool should_render_face(rf::GFace* face)
    {
        return !face->attributes.is_portal() &&
            !face->attributes.is_invisible() &&
            !face->attributes.is_show_sky();
    }

    static inline rf::gr::Mode determine_decal_mode(rf::GDecal* decal)
    {
        if (decal->flags & rf::DF_SELF_ILLUMINATED) {
            return rf::gr_decal_self_illuminated_mode;
        }
        if (decal->flags & rf::DF_TILING_U) {
            return gr_decal_tiling_u_mode;
        }
        if (decal->flags & rf::DF_TILING_V) {
            return gr_decal_tiling_v_mode;
        }
        return gr_decal_mode;
    }

    static inline rf::gr::Mode determine_face_mode(FaceRenderType render_type, bool has_lightmap, bool is_sky)
    {
        if (is_sky) {
            if (render_type == FaceRenderType::opaque) {
                return sky_room_opaque_mode;
            }
            return sky_room_alpha_mode;
        }
        if (render_type == FaceRenderType::opaque) {
            return gr_solid_mode;
        }
        if (render_type == FaceRenderType::alpha && has_lightmap) {
            return gr_solid_alpha_mode;
        }
        return alpha_detail_fullbright_mode;
    }

    template<int N>
    inline void link_faces_to_texture_movers(rf::VList<rf::GFace, N>& face_list, rf::GSolid* solid)
    {
        // Note: this is normally done by geo_cache_prepare
        for (rf::GFace& face : face_list) {
            face.attributes.texture_mover = nullptr;
        }
        for (rf::GTextureMover* texture_mover : solid->texture_movers) {
            for (rf::GFace* face : texture_mover->faces) {
                face->attributes.texture_mover = texture_mover;
            }
        }
    }

    class SolidGeometryBuffers
    {
    public:
        SolidGeometryBuffers(const std::vector<GpuVertex>& vb_data, const std::vector<uint32_t>& ib_data,
            ID3D11Device* device);

        void bind_buffers(RenderContext& render_context)
        {
            render_context.set_vertex_buffer(vertex_buffer_, sizeof(GpuVertex));
            render_context.set_index_buffer(index_buffer_, DXGI_FORMAT_R32_UINT);
        }

    private:
        ComPtr<ID3D11Buffer> vertex_buffer_;
        ComPtr<ID3D11Buffer> index_buffer_;
    };

    SolidGeometryBuffers::SolidGeometryBuffers(const std::vector<GpuVertex>& vb_data,
        const std::vector<uint32_t>& ib_data, ID3D11Device* device)
    {
        if (vb_data.empty() || ib_data.empty()) {
            return;
        }

        CD3D11_BUFFER_DESC vb_desc{
            sizeof(vb_data[0]) * vb_data.size(),
            D3D11_BIND_VERTEX_BUFFER,
            D3D11_USAGE_IMMUTABLE,
        };
        D3D11_SUBRESOURCE_DATA vb_subres_data{vb_data.data(), 0, 0};
        DF_GR_D3D11_CHECK_HR(
            device->CreateBuffer(&vb_desc, &vb_subres_data, &vertex_buffer_)
        );

        CD3D11_BUFFER_DESC ib_desc{
            sizeof(ib_data[0]) * ib_data.size(),
            D3D11_BIND_INDEX_BUFFER,
            D3D11_USAGE_IMMUTABLE,
        };
        D3D11_SUBRESOURCE_DATA ib_subres_data{ib_data.data(), 0, 0};
        DF_GR_D3D11_CHECK_HR(
            device->CreateBuffer(&ib_desc, &ib_subres_data, &index_buffer_)
        );
    }

    // One terrain's faces in a chunk room: top and underside together, told apart per vertex, and
    // crater faces batched by their own texture.
    struct TerrainBatch
    {
        int start_index;
        int num_indices;
        int base_vertex;
        int terrain;
        int crater_texture; // no_crater_texture: top and underside
    };

    struct SolidBatch
    {
        SolidBatch(int start_index, int num_indices, int base_vertex, std::array<int, 2> textures, rf::gr::Mode mode) :
            start_index{start_index}, num_indices{num_indices}, base_vertex{base_vertex}, textures{textures}, mode{mode}
        {}

        int start_index;
        int num_indices;
        int base_vertex = 0;
        std::array<int, 2> textures;
        rf::gr::Mode mode;
    };

    class SolidBatches
    {
    public:
        std::vector<SolidBatch>& get_batches(FaceRenderType render_type)
        {
            if (render_type == FaceRenderType::alpha) {
                return alpha_batches_;
            }
            else if (render_type == FaceRenderType::liquid) {
                return liquid_batches_;
            }
            else {
                return opaque_batches_;
            }
        }

        std::vector<TerrainBatch>& get_terrain_batches()
        {
            return terrain_batches_;
        }

    private:
        std::vector<SolidBatch> opaque_batches_;
        std::vector<TerrainBatch> terrain_batches_;
        std::vector<SolidBatch> alpha_batches_;
        std::vector<SolidBatch> liquid_batches_;
    };

    class GRenderCache
    {
    public:
        GRenderCache(SolidBatches batches, SolidGeometryBuffers geometry_buffers) :
            batches_{batches}, geometry_buffers_{geometry_buffers}
        {}

        void render(FaceRenderType what, RenderContext& context);

        bool has_batches(FaceRenderType what)
        {
            return !batches_.get_batches(what).empty();
        }

        bool has_terrain_batches()
        {
            return !batches_.get_terrain_batches().empty();
        }

        // bind(batch) readies the terrain shader's inputs for a batch; false skips its draw.
        template<typename F>
        void render_terrain(RenderContext& render_context, F&& bind)
        {
            geometry_buffers_.bind_buffers(render_context);
            for (const TerrainBatch& b : batches_.get_terrain_batches()) {
                if (bind(b)) {
                    render_context.draw_indexed(b.num_indices, b.start_index, b.base_vertex);
                }
            }
        }

    private:
        SolidBatches batches_;
        SolidGeometryBuffers geometry_buffers_;
    };

    void GRenderCache::render(FaceRenderType what, RenderContext& render_context)
    {
        auto& batches = batches_.get_batches(what);

        if (batches.empty()) {
            return;
        }

        geometry_buffers_.bind_buffers(render_context);
        // World geometry UVs are authored for pow2 texture dimensions on old GPUs,
        // so suppress UV scaling for this entire pass
        render_context.set_suppress_texture_uv_scale(true);
        // Exempt glass, grating, and other see-through textures from picmip.
        RenderContext::ScopedPicmipActive picmip_scope{render_context,
            render_context.picmip_active() && what != FaceRenderType::alpha};
        // A level with no stock lightmaps section leaves every stock page handle pointing at the
        // engine's synthesised page, whose texture it never writes. -1 resolves to the neutral
        // grey the shader doubles to 1, so what has no alpine chart there renders fullbright.
        const int synth_page_bm = af_lightmap_synthesized_page_bm();
        for (SolidBatch& b : batches) {
            bool lightmap_only = rf::gr::show_lightmaps && what != FaceRenderType::alpha;
            render_context.set_mode(b.mode, {255, 255, 255, 255}, lightmap_only);
            const int lightmap_bm = synth_page_bm != -1 && b.textures[1] == synth_page_bm ? -1 : b.textures[1];
            render_context.set_textures(b.textures[0], lightmap_bm);
            render_context.draw_indexed(b.num_indices, b.start_index, b.base_vertex);
        }
        render_context.set_suppress_texture_uv_scale(false);
    }

    class GRenderCacheBuilder
    {
    private:
        // render_type, texture_1, texture_2
        using FaceBatchKey = std::tuple<FaceRenderType, int, int>;
        // render_type, texture_1, texture_2, mode
        using DecalPolyBatchKey = std::tuple<FaceRenderType, int, int, rf::gr::Mode>;

        int num_verts_ = 0;
        int num_inds_ = 0;
        std::map<FaceBatchKey, std::vector<rf::GFace*>> batched_faces_;
        std::map<DecalPolyBatchKey, std::vector<rf::DecalPoly*>> batched_decal_polys_;
        // (terrain index, crater texture or no_crater_texture) -> its faces in this room with their kind
        std::map<std::pair<int, int>, std::vector<std::pair<rf::GFace*, alpine_terrain::FaceKind>>> terrain_faces_;
        // Set only for caches that may draw terrain batches
        TerrainRenderer* terrain_renderer_ = nullptr;
        const AlpineTerrainRoomRef* terrain_room_ = nullptr;
        alpine_terrain::GridView terrain_grid_{};
        bool is_sky_ = false;
        bool is_sky_fix_ = is_sky_fix_level(rf::level.filename);
        rf::GSolid* solid_ = nullptr;
        // The surface index of each face add_face() found an alpine chart for, so build() cannot reach a
        // different answer if is_sky_ flips between them.
        std::unordered_map<rf::GFace*, int> af_surfaces_;

    public:
        void enable_terrain(TerrainRenderer& terrain_renderer)
        {
            terrain_renderer_ = &terrain_renderer;
        }

        void add_solid(rf::GSolid* solid);
        void add_room(rf::GRoom* room, rf::GSolid* solid);
        void add_face(rf::GFace* face, rf::GSolid* solid);
        GRenderCache build(ID3D11Device* device);

        int get_num_verts() const
        {
            return num_verts_;
        }

        int get_num_inds() const
        {
            return num_inds_;
        }

        int get_num_batches() const
        {
            return batched_faces_.size() + batched_decal_polys_.size() + terrain_faces_.size();
        }

        friend class GRenderCache;
    };

    static rf::Vector3 calculate_face_vertex_normal(rf::GFaceVertex* fvert, rf::GFace* face)
    {
        rf::Vector3 normal = face->plane.normal;
        bool normalize = false;
        for (rf::GFace* adj_face : fvert->vertex->adjacent_faces) {
            if (adj_face != face && (adj_face->attributes.group_id & face->attributes.group_id) != 0) {
                normal += adj_face->plane.normal;
                normalize = true;
            }
        }
        if (normalize) {
            normal.normalize();
        }
        return normal;
    }

    void GRenderCacheBuilder::add_solid(rf::GSolid* solid)
    {
        solid_ = solid;
        link_faces_to_texture_movers(solid->face_list, solid);
        for (rf::GFace& face : solid->face_list) {
            add_face(&face, solid);
        }
    }

    void GRenderCacheBuilder::add_room(rf::GRoom* room, rf::GSolid* solid)
    {
        solid_ = solid;
        if (room->is_sky) {
            is_sky_ = true;
        }
        terrain_room_ = nullptr;
        if (terrain_renderer_ && !is_sky_ && room->is_detail) {
            const AlpineTerrainRoomRef* ref = alpine_terrain_find_room(room);
            if (ref && terrain_renderer_->prepare(ref->terrain)) {
                terrain_room_ = ref;
                terrain_grid_ = alpine_terrain_grid(alpine_terrain_get_all()[ref->terrain]);
            }
        }
        link_faces_to_texture_movers(room->face_list, solid);
        for (rf::GFace& face : room->face_list) {
            add_face(&face, solid);
        }
        // Only iterate detail_rooms for non-detail rooms. Detail rooms should never
        // have sub-detail rooms in Red Faction. After RF2-style geomod, the boolean
        // engine may corrupt the detail_rooms VArray on detail rooms, causing infinite
        // recursion if we iterate it unconditionally.
        if (!room->is_detail) {
            for (rf::GRoom* detail_room : room->detail_rooms) {
                if (detail_room->face_list.empty()) {
                    continue; // skip destroyed breakable detail rooms
                }
                if (alpine_terrain_is_separate_chunk(room, detail_room)) {
                    continue;
                }
                add_room(detail_room, solid);
            }
        }
    }

    static inline FaceRenderType determine_face_render_type(rf::GFace* face)
    {
        if (face->attributes.is_liquid()) {
            return FaceRenderType::liquid;
        }
        if (face->attributes.is_see_thru()) {
            return FaceRenderType::alpha;
        }
        return FaceRenderType::opaque;
    }

    void GRenderCacheBuilder::add_face(rf::GFace* face, rf::GSolid* solid)
    {
        // Drop "Show Sky" flag from skybox faces in levels where it causes issues
        if (is_sky_ && face->attributes.is_show_sky() && is_sky_fix_) {
            face->attributes.flags &= ~rf::FACE_SHOW_SKY;
        }
        if (!should_render_face(face)) {
            return;
        }
        FaceRenderType render_type = determine_face_render_type(face);
        int face_tex = face->attributes.bitmap_id;
        int lightmap_tex = -1;
        int af_chart = -1;
        if (!is_sky_ && render_type != FaceRenderType::liquid && face->attributes.surface_index >= 0) {
            if (face->attributes.surface_index >= solid->surfaces.size()) {
                xlog::warn("add_face: surface_index {} out of bounds (size {}), skipping face",
                    face->attributes.surface_index, solid->surfaces.size());
                return;
            }
            rf::GSurface* surface = solid->surfaces[face->attributes.surface_index];
            if (!surface || !surface->lightmap) {
                xlog::warn("add_face: null surface or lightmap at surface_index {}, skipping face",
                    face->attributes.surface_index);
                return;
            }
            lightmap_tex = surface->lightmap->bm_handle;
            AfLightmapFace af_face;
            if (af_lightmap_face_setup(solid, face->attributes.surface_index, af_face)) {
                af_chart = af_face.chart;
                af_surfaces_[face] = face->attributes.surface_index;
            }
        }
        // Charted faces all sample the same atlas, so the stock page they were derived from does
        // not have to split them into batches. The key stays negative-but-not--1 so the alpha
        // render mode a face gets is still the one it would have had.
        FaceBatchKey key = std::make_tuple(render_type, face_tex,
                                           af_chart >= 0 ? af_lightmap_batch_key : lightmap_tex);
        // Terrain shading covers the chunk's faces with no surface: the ones Build Geometry emitted and
        // the crater faces RF2 carving adds. Anything else in a chunk room keeps the ordinary batches.
        std::vector<std::pair<rf::GFace*, alpine_terrain::FaceKind>>* terrain_list = nullptr;
        if (terrain_room_ && render_type == FaceRenderType::opaque && face->attributes.surface_index < 0) {
            const alpine_terrain::FaceKind kind = alpine_terrain_face_kind(terrain_grid_, *face);
            const int crater_texture =
                kind == alpine_terrain::FaceKind::crater ? std::max(face_tex, -1) : no_crater_texture;
            terrain_list = &terrain_faces_[{terrain_room_->terrain, crater_texture}];
            terrain_list->emplace_back(face, kind);
        }
        else {
            batched_faces_[key].push_back(face);
        }
        auto remove_face = [&]() {
            if (terrain_list) {
                terrain_list->pop_back();
            }
            else {
                batched_faces_[key].pop_back();
            }
        };
        auto fvert = face->edge_loop;
        int num_fverts = 0;
        while (fvert) {
            ++num_fverts;
            if (num_fverts > rf::max_face_vertices) {
                xlog::error("add_face: edge_loop exceeds {} vertices, likely corrupted", rf::max_face_vertices);
                remove_face();
                return;
            }
            fvert = fvert->next;
            if (fvert == face->edge_loop) {
                break;
            }
        }
        if (num_fverts < 3) {
            // Degenerate face, remove from batch
            remove_face();
            return;
        }
        rf::DecalPoly* dp = face->decal_list;
        int num_dp = 0;
        while (dp) {
            if (dp->my_decal->flags & rf::DF_LEVEL_DECAL) {
                rf::gr::Mode mode = determine_decal_mode(dp->my_decal);
                std::array<int, 2> textures = normalize_texture_handles_for_mode(mode, {dp->my_decal->bitmap_id, lightmap_tex});
                DecalPolyBatchKey dp_key = std::make_tuple(render_type, textures[0], textures[1], mode);
                batched_decal_polys_[dp_key].push_back(dp);
                ++num_dp;
            }
            dp = dp->next_for_face;
        }
        num_verts_ += (1 + num_dp) * num_fverts;
        num_inds_ += (1 + num_dp) * (num_fverts - 2) * 3;
    }

    static void report_long_edge_loop()
    {
        xlog::error("build: edge_loop exceeds {} vertices", rf::max_face_vertices);
    }

    // Appends `face` as a triangle fan: fill(vertex, fvert, index) sets each vertex. A loop longer than
    // max_verts calls on_overflow and keeps the fan emitted so far.
    template<typename Overflow, typename Fill>
    static void emit_face_fan(rf::GFace* face, std::vector<GpuVertex>& vb_data, std::vector<uint32_t>& ib_data,
        std::size_t base_vertex, int max_verts, Overflow&& on_overflow, Fill&& fill)
    {
        auto face_start_index = static_cast<uint32_t>(vb_data.size() - base_vertex);
        int fvert_index = 0;
        auto fvert = face->edge_loop;
        while (fvert) {
            if (fvert_index >= max_verts) {
                on_overflow();
                break;
            }
            fill(vb_data.emplace_back(), fvert, fvert_index);
            if (fvert_index >= 2) {
                ib_data.emplace_back(face_start_index);
                ib_data.emplace_back(face_start_index + fvert_index - 1);
                ib_data.emplace_back(face_start_index + fvert_index);
            }
            ++fvert_index;

            fvert = fvert->next;
            if (fvert == face->edge_loop) {
                break;
            }
        }
    }

    GRenderCache GRenderCacheBuilder::build(ID3D11Device* device)
    {
        SolidBatches batches;
        std::vector<GpuVertex> vb_data;
        std::vector<uint32_t> ib_data;
        vb_data.reserve(num_verts_);
        ib_data.reserve(num_inds_);

        for (auto& e : batched_faces_) {
            const GRenderCacheBuilder::FaceBatchKey& key = e.first;
            auto& faces = e.second;
            auto [render_type, texture_1, texture_2] = key;
            std::size_t start_index = ib_data.size();
            std::size_t base_vertex = vb_data.size();

            for (rf::GFace* face : faces) {
                if (!face->edge_loop) continue;
                rf::GTextureMover* texture_mover = face->attributes.texture_mover;
                float u_pan_speed = texture_mover ? texture_mover->u_pan_speed : 0.0f;
                float v_pan_speed = texture_mover ? texture_mover->v_pan_speed : 0.0f;
                AfLightmapFace af_face;
                auto af_it = af_surfaces_.find(face);
                bool has_af = af_it != af_surfaces_.end()
                    && af_lightmap_face_setup(solid_, af_it->second, af_face);
                emit_face_fan(face, vb_data, ib_data, base_vertex, rf::max_face_vertices, report_long_edge_loop,
                    [&](GpuVertex& gpu_vert, rf::GFaceVertex* fvert, int) {
                        gpu_vert.x = fvert->vertex->pos.x;
                        gpu_vert.y = fvert->vertex->pos.y;
                        gpu_vert.z = fvert->vertex->pos.z;
                        rf::Vector3 normal = calculate_face_vertex_normal(fvert, face);
                        gpu_vert.norm = {normal.x, normal.y, normal.z};
                        gpu_vert.diffuse = 0xFFFFFFFF;
                        gpu_vert.u0 = fvert->texture_u;
                        gpu_vert.v0 = fvert->texture_v;
                        if (has_af) {
                            // Re-projected from the surface's own affine: the per-vertex UVs the RFL
                            // stores are clamped into the stock fragment and would shear a refined
                            // chart.
                            af_lightmap_face_texel(af_face, fvert->vertex->pos, gpu_vert.u1, gpu_vert.v1);
                            gpu_vert.lm_chart = static_cast<float>(af_face.chart);
                        }
                        else {
                            gpu_vert.u1 = fvert->lightmap_u;
                            gpu_vert.v1 = fvert->lightmap_v;
                            gpu_vert.lm_chart = -1.0f;
                        }
                        gpu_vert.u0_pan_speed = u_pan_speed;
                        gpu_vert.v0_pan_speed = v_pan_speed;
                    });
            }
            std::size_t num_indices = ib_data.size() - start_index;
            if (num_indices > 0) {
                std::array<int, 2> textures = {texture_1, texture_2};
                rf::gr::Mode mode = determine_face_mode(render_type, texture_2 != -1, is_sky_);
                batches.get_batches(render_type).emplace_back(
                    start_index, num_indices, base_vertex, textures, mode
                );
            }
        }
        for (auto& e : batched_decal_polys_) {
            const GRenderCacheBuilder::DecalPolyBatchKey& key = e.first;
            auto& dps = e.second;
            auto [render_type, texture_1, texture_2, mode] = key;
            std::size_t start_index = ib_data.size();
            std::size_t base_vertex = vb_data.size();

            for (rf::DecalPoly* dp : dps) {
                rf::GDecal* decal = dp->my_decal;
                rf::ubyte alpha = rfl_version_minimum(304) ? decal->alpha : 255;
                int diffuse = pack_color(rf::Color{255, 255, 255, alpha});
                auto face = dp->face;
                if (!face->edge_loop) continue;
                emit_face_fan(face, vb_data, ib_data, base_vertex, static_cast<int>(std::size(dp->uvs)),
                    [dp] {
                        xlog::error("build decal: face has more vertices than decal uvs capacity ({})", std::size(dp->uvs));
                    },
                    [&](GpuVertex& gpu_vert, rf::GFaceVertex* fvert, int fvert_index) {
                        gpu_vert.x = fvert->vertex->pos.x;
                        gpu_vert.y = fvert->vertex->pos.y;
                        gpu_vert.z = fvert->vertex->pos.z;
                        rf::Vector3 normal = calculate_face_vertex_normal(fvert, face);
                        gpu_vert.norm = {normal.x, normal.y, normal.z};
                        gpu_vert.diffuse = diffuse;
                        gpu_vert.u0 = dp->uvs[fvert_index].x;
                        gpu_vert.v0 = dp->uvs[fvert_index].y;
                        gpu_vert.u0_pan_speed = 0.0f;
                        gpu_vert.v0_pan_speed = 0.0f;
                        gpu_vert.u1 = fvert->lightmap_u;
                        gpu_vert.v1 = fvert->lightmap_v;
                        gpu_vert.lm_chart = -1.0f;
                    });
            }
            std::size_t num_indices = ib_data.size() - start_index;
            if (num_indices > 0) {
                std::array<int, 2> textures = {texture_1, texture_2};
                batches.get_batches(render_type).emplace_back(
                    start_index, num_indices, base_vertex, textures, mode
                );
            }
        }
        for (auto& [key, faces] : terrain_faces_) {
            const auto [terrain, crater_texture] = key;
            std::size_t start_index = ib_data.size();
            std::size_t base_vertex = vb_data.size();

            for (auto [face, kind] : faces) {
                if (!face->edge_loop) continue;
                emit_face_fan(face, vb_data, ib_data, base_vertex, rf::max_face_vertices, report_long_edge_loop,
                    [&](GpuVertex& gpu_vert, rf::GFaceVertex* fvert, int) {
                        gpu_vert.x = fvert->vertex->pos.x;
                        gpu_vert.y = fvert->vertex->pos.y;
                        gpu_vert.z = fvert->vertex->pos.z;
                        // Crater faces take the flat face normal like the CPU sampler: carving leaves group_id
                        // -1, so smoothing would blend in adjacent top and wall faces.
                        rf::Vector3 normal = kind == alpine_terrain::FaceKind::crater
                            ? face->plane.normal
                            : calculate_face_vertex_normal(fvert, face);
                        gpu_vert.norm = {normal.x, normal.y, normal.z};
                        gpu_vert.diffuse = 0xFFFFFFFF;
                        gpu_vert.u0 = fvert->texture_u;
                        gpu_vert.v0 = fvert->texture_v;
                        gpu_vert.u0_pan_speed = 0.0f;
                        gpu_vert.v0_pan_speed = 0.0f;
                        gpu_vert.u1 = 0.0f;
                        gpu_vert.v1 = 0.0f;
                        // The terrain shader's face kind: 0 top, 1 underside, 2 crater
                        static_assert(static_cast<int>(alpine_terrain::FaceKind::top) == 0
                            && static_cast<int>(alpine_terrain::FaceKind::underside) == 1
                            && static_cast<int>(alpine_terrain::FaceKind::crater) == 2);
                        gpu_vert.lm_chart = static_cast<float>(kind);
                    });
            }
            std::size_t num_indices = ib_data.size() - start_index;
            if (num_indices > 0) {
                batches.get_terrain_batches().push_back({static_cast<int>(start_index),
                    static_cast<int>(num_indices), static_cast<int>(base_vertex), terrain, crater_texture});
            }
        }

        SolidGeometryBuffers geometry_buffers{vb_data, ib_data, device};
        return GRenderCache{batches, geometry_buffers};
    }

    class RoomRenderCache
    {
    public:
        RoomRenderCache(rf::GSolid* solid, rf::GRoom* room, ID3D11Device* device);
        ~RoomRenderCache() {}
        void render(FaceRenderType render_type, ID3D11Device* device, RenderContext& context);
        void mark_after_boolean();

        rf::GRoom* room() const
        {
            return room_;
        }

    private:
        char padding_[0x20];
        int state_ = 0; // modified by the game engine during geomod operation
        rf::GRoom* room_;
        rf::GSolid* solid_;
        std::optional<GRenderCache> cache_;

        void update(ID3D11Device* device);
        bool invalid() const;
    };

    inline bool RoomRenderCache::invalid() const
    {
        static_assert(offsetof(RoomRenderCache, state_) == offsetof(rf::GCache, state), "Bad state_ offset");
        if (state_ != 0) {
            xlog::trace("room {} state {}", room_->room_index, state_);
        }
        return state_ == 2;
    }

    RoomRenderCache::RoomRenderCache(rf::GSolid* solid, rf::GRoom* room, ID3D11Device* device) :
        room_(room), solid_(solid)
    {
        update(device);
    }


    void RoomRenderCache::update(ID3D11Device* device)
    {
        xlog::debug("RoomRenderCache::update room {} start (faces: {})",
            room_->room_index, room_->face_list.size());
        GRenderCacheBuilder builder;
        builder.add_room(room_, solid_);

        if (builder.get_num_batches() == 0) {
            state_ = 0;
            xlog::debug("Skipping empty room {}", room_->room_index);
            return;
        }

        xlog::debug("Building render cache for room {} - verts {} inds {} batches {}", room_->room_index,
            builder.get_num_verts(), builder.get_num_inds(), builder.get_num_batches());

        cache_ = std::optional{builder.build(device)};
        xlog::debug("RoomRenderCache::update room {} complete", room_->room_index);

        state_ = 0;
    }

    void RoomRenderCache::render(FaceRenderType render_type, ID3D11Device* device, RenderContext& context)
    {
        if (invalid()) {
            xlog::debug("Room {} render cache invalidated! state={}", room_->room_index, state_);
            cache_.reset();
            update(device);
        }

        if (cache_) {
            cache_.value().render(render_type, context);
        }
    }

    void RoomRenderCache::mark_after_boolean()
    {
        if (state_ == 1) {
            state_ = 2;
        }
    }

    SolidRenderer::SolidRenderer(ComPtr<ID3D11Device> device, ShaderManager& shader_manager,
        [[maybe_unused]] StateManager& state_manager, DynamicGeometryRenderer& dyn_geo_renderer,
        RenderContext& render_context, AfLightmapRenderer& af_lightmap_renderer) :
        device_{std::move(device)}, context_{render_context.device_context()}, terrain_renderer_{device_},
        dyn_geo_renderer_{dyn_geo_renderer}, render_context_(render_context),
        af_lightmap_renderer_{af_lightmap_renderer}
    {
        vertex_shader_ = shader_manager.get_vertex_shader(VertexShaderId::standard);
        pixel_shader_ = shader_manager.get_pixel_shader(PixelShaderId::standard);
        pixel_shader_no_gas_ = shader_manager.get_pixel_shader(PixelShaderId::standard_no_gas);
        terrain_pixel_shader_ = shader_manager.get_pixel_shader(PixelShaderId::terrain);
        terrain_pixel_shader_no_gas_ = shader_manager.get_pixel_shader(PixelShaderId::terrain_no_gas);
    }

    SolidRenderer::~SolidRenderer()
    {}

    static rf::gr::Mode dynamic_decal_mode{
        rf::gr::TEXTURE_SOURCE_CLAMP,
        rf::gr::COLOR_SOURCE_VERTEX,
        rf::gr::ALPHA_SOURCE_VERTEX_TIMES_TEXTURE,
        rf::gr::ALPHA_BLEND_ALPHA,
        rf::gr::ZBUFFER_TYPE_READ,
        rf::gr::FOG_ALLOWED,
    };

    static void render_face_dynamic_decals(rf::GFace* face)
    {
        constexpr int max_decal_poly_vertices = 25;
        static rf::Vector3 verts[max_decal_poly_vertices];
        static rf::Vector2 uvs[max_decal_poly_vertices];

        auto dp = face->decal_list;
        while (dp) {
            rf::GDecal* decal = dp->my_decal;
            if (!(decal->flags & rf::DF_LEVEL_DECAL)) {
                int nv = std::min(dp->nv, max_decal_poly_vertices);
                for (int i = 0; i < nv; ++i) {
                    verts[i] = dp->verts[i].pos;
                    uvs[i] = dp->verts[i].uv;
                }
                rf::Color color{255, 255, 255, decal->alpha};
                // TODO: lightmap_uv
                rf::gr::world_poly(decal->bitmap_id, nv, verts, uvs, dynamic_decal_mode, color);
            }
            dp = dp->next_for_face;
        }
    }

    // Calls visit(room) for the room of each face a dynamic decal is clipped to, until visit returns true.
    template<typename F>
    static bool visit_dynamic_decal_rooms(F&& visit)
    {
        rf::GDecal* const decal_head = rf::g_decal_list;
        for (rf::GDecal* decal = decal_head; decal;) {
            if (!(decal->flags & rf::DF_LEVEL_DECAL)) {
                rf::DecalPoly* const dp_head = decal->poly_list;
                for (rf::DecalPoly* dp = dp_head; dp;) {
                    if (dp->face && visit(dp->face->which_room)) {
                        return true;
                    }
                    dp = dp->next;
                    if (dp == dp_head) {
                        break;
                    }
                }
            }
            decal = decal->next;
            if (decal == decal_head) {
                break;
            }
        }
        return false;
    }

    void SolidRenderer::render_dynamic_decals(rf::GRoom** rooms, int num_rooms)
    {
        before_render_decals();

        dynamic_decal_rooms_.clear();
        visit_dynamic_decal_rooms([this](rf::GRoom* room) {
            dynamic_decal_rooms_.push_back(room);
            return false;
        });
        std::sort(dynamic_decal_rooms_.begin(), dynamic_decal_rooms_.end());
        auto has_dynamic_decals = [this](rf::GRoom* room) {
            return std::binary_search(dynamic_decal_rooms_.begin(), dynamic_decal_rooms_.end(), room);
        };

        for (int i = 0; i < num_rooms; ++i) {
            rf::GRoom* room = rooms[i];
            if (has_dynamic_decals(room)) {
                for (rf::GFace& face: room->face_list) {
                    if (should_render_face(&face) && !face.attributes.is_see_thru()) {
                        render_face_dynamic_decals(&face);
                    }
                }
            }
            for (rf::GRoom* detail_room : room->detail_rooms) {
                const bool draw = alpine_terrain_is_separate_chunk(room, detail_room)
                    ? terrain_chunk_drawn(detail_room) && claim_terrain_chunk(terrain_decals_drawn_, detail_room)
                    : detail_room->room_to_render_with == room;
                if (draw && has_dynamic_decals(detail_room)) {
                    for (rf::GFace& face: detail_room->face_list) {
                        if (should_render_face(&face) && !face.attributes.is_see_thru()) {
                            render_face_dynamic_decals(&face);
                        }
                    }
                }
            }
        }
        after_render_decals();
    }

    void SolidRenderer::render_alpha_detail_dynamic_decals(rf::GRoom* detail_room)
    {
        before_render_decals();
        if (visit_dynamic_decal_rooms([detail_room](rf::GRoom* room) { return room == detail_room; })) {
            for (rf::GFace& face: detail_room->face_list) {
                if (should_render_face(&face) && face.attributes.is_see_thru()) {
                    render_face_dynamic_decals(&face);
                }
            }
        }
        after_render_decals();
    }

    void SolidRenderer::render_movable_solid_dynamic_decals(rf::GSolid* solid, const rf::Vector3& pos,
        const rf::Matrix3& orient, bool opaque_faces, bool alpha_faces)
    {
        rf::gr::start_instance(pos, orient);
        before_render_decals();
        for (rf::GFace& face: solid->face_list) {
            bool wanted = determine_face_render_type(&face) == FaceRenderType::alpha ? alpha_faces : opaque_faces;
            if (wanted && should_render_face(&face)) {
                render_face_dynamic_decals(&face);
            }
        }
        after_render_decals();
        rf::gr::stop_instance();
    }

    void SolidRenderer::before_render_decals()
    {
        constexpr int decal_zbias = 1000;
        render_context_.set_zbias(decal_zbias);
        dyn_geo_renderer_.set_cull_mode(D3D11_CULL_BACK);
    }

    void SolidRenderer::after_render_decals()
    {
        dyn_geo_renderer_.flush();
        dyn_geo_renderer_.set_cull_mode(D3D11_CULL_NONE);
        render_context_.set_zbias(0);
    }

    void SolidRenderer::render_room_faces(rf::GSolid* solid, rf::GRoom* room, FaceRenderType render_type)
    {
        render_context_.set_draw_room_uid(room->is_detail ? -1 : room->uid);
        auto cache = get_or_create_normal_room_cache(solid, room);
        cache->render(render_type, device_, render_context_);
    }

    RoomRenderCache* SolidRenderer::get_or_create_normal_room_cache(rf::GSolid* solid, rf::GRoom* room)
    {
        auto cache = reinterpret_cast<RoomRenderCache*>(room->geo_cache);
        if (!cache) {
            xlog::debug("Creating render cache for room {}", room->room_index);
            room_cache_.push_back(std::make_unique<RoomRenderCache>(solid, room, device_));
            cache = room_cache_.back().get();
            room->geo_cache = reinterpret_cast<rf::GCache*>(cache);
            geo_cache_rooms_.push_back(room);
        }
        return cache;
    }

    void SolidRenderer::render_detail(rf::GSolid* solid, rf::GRoom* room, bool alpha)
    {
        // A detail brush sits inside whatever normal room contains it, so its own uid would not
        // match the water room's; fall back to the bbox test.
        render_context_.set_draw_room_uid(-1);
        GRenderCache* cache = get_or_create_detail_room_cache(solid, room);
        if (!cache) return;
        // Terrain first: the room's level decals are in its opaque batches and have to land on top.
        if (!alpha && cache->has_terrain_batches()) {
            render_terrain(*cache);
        }
        FaceRenderType render_type = alpha ? FaceRenderType::alpha : FaceRenderType::opaque;
        cache->render(render_type, render_context_);
    }

    void SolidRenderer::render_terrain(GRenderCache& cache)
    {
        render_context_.set_mode(determine_face_mode(FaceRenderType::opaque, false, false),
                                 {255, 255, 255, 255}, rf::gr::show_lightmaps);
        const bool gas = render_context_.has_gas_regions();
        render_context_.set_pixel_shader(gas ? terrain_pixel_shader_ : terrain_pixel_shader_no_gas_);
        cache.render_terrain(render_context_, [this](const TerrainBatch& b) {
            if (b.terrain != bound_terrain_) {
                if (!terrain_renderer_.bind(context_, render_context_, b.terrain)) {
                    return false;
                }
                bound_terrain_ = b.terrain;
            }
            if (b.crater_texture != no_crater_texture && b.crater_texture != bound_crater_texture_) {
                terrain_renderer_.bind_crater(context_, render_context_, b.crater_texture);
                bound_crater_texture_ = b.crater_texture;
            }
            return true;
        });
        render_context_.set_pixel_shader(gas ? pixel_shader_ : pixel_shader_no_gas_);
    }

    // True the first time this pass; `room` is a resolved terrain chunk, so its index is valid.
    bool SolidRenderer::claim_terrain_chunk(std::vector<int>& stamps, const rf::GRoom* room)
    {
        const auto idx = static_cast<std::size_t>(room->room_index);
        if (idx >= stamps.size()) {
            stamps.resize(idx + 1, 0);
        }
        if (stamps[idx] == terrain_pass_) {
            return false;
        }
        stamps[idx] = terrain_pass_;
        return true;
    }

    // Decorations reach past their chunk's faces, so they are culled by their own box, once per pass.
    void SolidRenderer::collect_decoration_chunk(const rf::GRoom* room)
    {
        const AlpineTerrainRoomRef* ref = alpine_terrain_find_room(room);
        const DecorationChunk* chunk = ref ? alpine_terrain_decorations_chunk(*ref) : nullptr;
        if (!chunk) {
            return;
        }
        if (!rf::gr::cull_bounding_box(chunk->lo_vec(), chunk->hi_vec()) &&
            claim_terrain_chunk(terrain_decorations_seen_, room)) {
            decoration_chunks_.push_back(*ref);
        }
    }

    // Whether render_solid drew terrain chunk `room` this pass.
    bool SolidRenderer::terrain_chunk_drawn(const rf::GRoom* room) const
    {
        const auto idx = static_cast<std::size_t>(room->room_index);
        return idx < terrain_drawn_.size() && terrain_drawn_[idx] == terrain_pass_;
    }

    // Sentinel value stored in room->geo_cache to mark detail rooms that have been checked
    // but have no renderable batches. Avoids rebuilding the cache builder every frame.
    // clear_cache() resets all geo_cache to nullptr, which properly clears this sentinel.
    static const auto k_empty_detail_sentinel = reinterpret_cast<rf::GCache*>(uintptr_t(1));

    GRenderCache* SolidRenderer::get_or_create_detail_room_cache(rf::GSolid* solid, rf::GRoom* room)
    {
        if (room->geo_cache == k_empty_detail_sentinel) {
            return nullptr;
        }
        auto cache = reinterpret_cast<GRenderCache*>(room->geo_cache);
        if (!cache) {
            xlog::debug("Creating render cache for detail room {} (faces: {})",
                room->room_index, room->face_list.size());
            GRenderCacheBuilder builder;
            if (terrain_pixel_shader_ && terrain_pixel_shader_no_gas_) {
                builder.enable_terrain(terrain_renderer_);
            }
            builder.add_room(room, solid);
            xlog::debug("Detail room {} builder: verts={} inds={} batches={}",
                room->room_index, builder.get_num_verts(), builder.get_num_inds(), builder.get_num_batches());
            if (builder.get_num_batches() > 0) {
                detail_render_cache_.push_back(std::make_unique<GRenderCache>(builder.build(device_)));
                cache = detail_render_cache_.back().get();
                room->geo_cache = reinterpret_cast<rf::GCache*>(cache);
            }
            else {
                room->geo_cache = k_empty_detail_sentinel;
            }
            xlog::debug("Detail room {} cache creation complete", room->room_index);
        }
        return cache;
    }

    void SolidRenderer::clear_cache()
    {
        xlog::debug("Room render cache clear (rooms={}, detail={}, movers={})",
            room_cache_.size(), detail_render_cache_.size(), mover_render_cache_.size());

        if (rf::level.geometry) {
            for (rf::GRoom* room: rf::level.geometry->all_rooms) {
                room->geo_cache = nullptr;
            }
        }

        room_cache_.clear();
        detail_render_cache_.clear();
        mover_render_cache_.clear();
        geo_cache_rooms_.clear();
        rf::geo_cache_num_rooms = 0;
        xlog::debug("Room render cache clear complete");
    }

    void SolidRenderer::release_detail_room_cache(rf::GRoom* room)
    {
        rf::GCache* const cache = room->geo_cache;
        room->geo_cache = nullptr;
        if (!cache || cache == k_empty_detail_sentinel) {
            return;
        }
        auto it = std::find_if(detail_render_cache_.begin(), detail_render_cache_.end(), [cache](const auto& c) {
            return reinterpret_cast<rf::GCache*>(c.get()) == cache;
        });
        if (it != detail_render_cache_.end()) {
            detail_render_cache_.erase(it);
        }
    }

    void SolidRenderer::reset_cache_after_boolean()
    {
        xlog::debug("reset_cache_after_boolean ({} rooms)", geo_cache_rooms_.size());
        for (rf::GRoom* room : geo_cache_rooms_) {
            auto cache = reinterpret_cast<RoomRenderCache*>(room->geo_cache);
            if (cache) {
                cache->mark_after_boolean();
            }
        }
    }

    void SolidRenderer::render_sky_room(rf::GRoom *room, rf::Vector3& out_sky_transform_pos, rf::Matrix3& out_sky_transform_orient)
    {
        // Sky room; picmip applies.
        RenderContext::ScopedPicmipActive picmip_scope{render_context_, true};

        xlog::trace("Rendering sky room {} cache {}", room->room_index, room->geo_cache);
        render_context_.set_sky_room(true);
        render_context_.update_lights(true);

        // Compute sky room transform: maps world coords to camera-relative coords
        if (const rf::Matrix3* skybox_orient = rf::sky_room_orient) {
            out_sky_transform_orient = *skybox_orient;
            out_sky_transform_orient.inverse();
            const rf::Vector3 rotated_center = out_sky_transform_orient.transform_vector(rf::sky_room_center);
            out_sky_transform_pos = rf::sky_room_offset + rf::sky_room_center - rotated_center;
        }
        else {
            out_sky_transform_pos = rf::sky_room_offset;
            out_sky_transform_orient = rf::identity_matrix;
        }

        before_render(out_sky_transform_pos, out_sky_transform_orient);
        render_room_faces(rf::level.geometry, room, FaceRenderType::opaque);
        render_room_faces(rf::level.geometry, room, FaceRenderType::alpha);

        // Render mover brushes that are in the sky room being projected
        for (auto& mb : DoublyLinkedList{rf::mover_brush_list}) {
            if (!mb.geometry || mb.room != room) {
                continue;
            }
            if (mb.obj_flags & rf::OF_HIDDEN) {
                continue;
            }
            // Transform mover position and orientation by the sky room transform
            rf::Vector3 transformed_pos = out_sky_transform_orient.transform_vector(mb.pos) + out_sky_transform_pos;
            rf::Matrix3 transformed_orient = out_sky_transform_orient;
            transformed_orient.mul(mb.orient);

            GRenderCache* cache = get_or_create_movable_solid_cache(mb.geometry);
            before_render(transformed_pos, transformed_orient);
            cache->render(FaceRenderType::opaque, render_context_);
            cache->render(FaceRenderType::alpha, render_context_);
        }

        render_context_.update_lights();
    }

    void SolidRenderer::render_movable_solid(rf::GSolid* solid, const rf::Vector3& pos, const rf::Matrix3& orient,
        bool include_alpha)
    {
        // Mover brush geometry; picmip applies.
        RenderContext::ScopedPicmipActive picmip_scope{render_context_, true};

        xlog::trace("Rendering movable solid {}", solid);
        render_context_.set_draw_room_uid(-1);
        // Upload gathered lights so the pixel shader can apply point lighting to movers
        render_context_.update_lights();
        GRenderCache* cache = get_or_create_movable_solid_cache(solid);
        before_render(pos, orient);
        cache->render(FaceRenderType::opaque, render_context_);
        if (include_alpha) {
            cache->render(FaceRenderType::alpha, render_context_);
        }
        if (decals_enabled) {
            // Decals on mover brushes; picmip doesn't apply.
            RenderContext::ScopedPicmipActive decal_scope{render_context_, false};
            render_movable_solid_dynamic_decals(solid, pos, orient, true, include_alpha);
        }
    }

    bool SolidRenderer::movable_solid_has_alpha(rf::GSolid* solid)
    {
        return get_or_create_movable_solid_cache(solid)->has_batches(FaceRenderType::alpha);
    }

    void SolidRenderer::render_movable_solid_alpha(rf::GSolid* solid, const rf::Vector3& pos, const rf::Matrix3& orient)
    {
        // Mover brush geometry; picmip applies.
        RenderContext::ScopedPicmipActive picmip_scope{render_context_, true};

        xlog::trace("Rendering movable solid alpha {}", solid);
        render_context_.set_draw_room_uid(-1);
        render_context_.update_lights();
        GRenderCache* cache = get_or_create_movable_solid_cache(solid);
        before_render(pos, orient);
        cache->render(FaceRenderType::alpha, render_context_);
        if (decals_enabled) {
            // Decals on mover brushes; picmip doesn't apply.
            RenderContext::ScopedPicmipActive decal_scope{render_context_, false};
            render_movable_solid_dynamic_decals(solid, pos, orient, false, true);
        }
    }

    GRenderCache* SolidRenderer::get_or_create_movable_solid_cache(rf::GSolid* solid)
    {
        auto it = mover_render_cache_.find(solid);
        if (it == mover_render_cache_.end()) {
            xlog::debug("Creating render cache for a mover {}", static_cast<void*>(solid));
            GRenderCacheBuilder cache_builder;
            cache_builder.add_solid(solid);
            GRenderCache cache = cache_builder.build(device_);
            auto p = mover_render_cache_.emplace(std::make_pair(solid, std::make_unique<GRenderCache>(cache)));
            it = p.first;
        }
        return it->second.get();
    }

    void SolidRenderer::render_alpha_detail(rf::GRoom *room, rf::GSolid *solid)
    {
        // Alpha-detail CSG geometry; picmip applies.
        RenderContext::ScopedPicmipActive picmip_scope{render_context_, true};

        xlog::trace("Rendering alpha detail room {}", room->room_index);
        if (room->face_list.empty()) {
            // Happens when glass is killed
            return;
        }
        // Clear stale point lights that may remain from mesh rendering (which runs
        // between render_solid and render_alpha_detail). Without this, alpha surfaces
        // pick up mesh lights and get overbright/blinking artifacts.
        render_context_.update_lights();
        before_render(rf::zero_vector, rf::identity_matrix);
        render_detail(solid, room, true);
        if (decals_enabled) {
            // Decals on alpha detail geometry; picmip doesn't apply.
            RenderContext::ScopedPicmipActive decal_scope{render_context_, false};
            render_alpha_detail_dynamic_decals(room);
        }
    }

    void SolidRenderer::render_room_liquid_surface(rf::GSolid* solid, rf::GRoom* room)
    {
        // Liquid surfaces; picmip applies.
        RenderContext::ScopedPicmipActive picmip_scope{render_context_, true};

        // determine_face_mode gives every liquid batch alpha_detail_fullbright_mode, so the pass is
        // always alpha blended with fog allowed - what the shader's distance opacity assumes.
        render_context_.set_liquid_surface(true);
        before_render(rf::zero_vector, rf::identity_matrix);
        render_room_faces(solid, room, FaceRenderType::liquid);
        render_context_.set_liquid_surface(false);
    }

    void SolidRenderer::render_solid(rf::GSolid* solid, rf::GRoom** rooms, int num_rooms)
    {
        // CSG level geometry; picmip applies.
        RenderContext::ScopedPicmipActive picmip_scope{render_context_, true};

        xlog::trace("Rendering level solid");
        // Terrain bindings are re-established once per pass
        bound_terrain_ = -1;
        bound_crater_texture_ = no_crater_texture;
        ++terrain_pass_;
        decoration_chunks_.clear();
        const bool decorations = alpine_terrain_decorations_active();
        render_context_.set_sky_room(false);
        render_context_.set_draw_room_uid(-1);
        rf::gr::light_filter_set_solid(solid, 1, 0);
        render_context_.update_lights();

        before_render(rf::zero_vector, rf::identity_matrix);

        for (int i = 0; i < num_rooms; ++i) {
            auto room = rooms[i];

            render_room_faces(solid, room, FaceRenderType::opaque);

            // Note: calling set_currently_rendered_room could improve culling here but it breaks some levels
            // if a detail brush is contained in multiple normal rooms
            for (rf::GRoom* detail_room : room->detail_rooms) {
                const bool separate_chunk = alpine_terrain_is_separate_chunk(room, detail_room);
                if (decorations && separate_chunk) {
                    collect_decoration_chunk(detail_room);
                }
                if (detail_room->face_list.empty()) {
                    // Happens when a breakable detail brush is destroyed
                    continue;
                }
                // room_to_render_with is fixed by the frame's first portal pass (a monitor, say), so
                // terrain chunks go by this pass's own rooms instead.
                const bool draw = separate_chunk
                    ? !rf::gr::cull_bounding_box(detail_room->bbox_min, detail_room->bbox_max) &&
                          claim_terrain_chunk(terrain_drawn_, detail_room)
                    : detail_room->room_to_render_with == room &&
                          !rf::gr::cull_bounding_box(detail_room->bbox_min, detail_room->bbox_max);
                if (draw) {
                    render_detail(solid, detail_room, false);
                }
            }
        }
        if (bound_terrain_ >= 0) {
            terrain_renderer_.unbind(context_);
            bound_terrain_ = -1;
            bound_crater_texture_ = no_crater_texture;
        }

        if (decals_enabled) {
            // Decals on level geometry; picmip doesn't apply.
            RenderContext::ScopedPicmipActive decal_scope{render_context_, false};
            render_dynamic_decals(rooms, num_rooms);
        }

        rf::gr::light_filter_reset();
        render_context_.update_lights();
    }

    void SolidRenderer::before_render(const rf::Vector3& pos, const rf::Matrix3& orient)
    {
        render_context_.set_vertex_shader(vertex_shader_);
        render_context_.set_pixel_shader(render_context_.has_gas_regions() ? pixel_shader_ : pixel_shader_no_gas_);
        render_context_.set_model_transform(pos, orient);
        render_context_.set_cull_mode(D3D11_CULL_BACK);
        render_context_.set_primitive_topology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        af_lightmap_renderer_.bind();
    }

    void SolidRenderer::page_in_solid(rf::GSolid* solid)
    {
        if (g_alpine_game_config.precache_rooms) {
            for (rf::GRoom* room : solid->cached_normal_room_list) {
                get_or_create_normal_room_cache(solid, room);
            }
        }
        for (rf::GRoom* room: solid->cached_detail_room_list) {
            get_or_create_detail_room_cache(solid, room);
        }
    }

}
