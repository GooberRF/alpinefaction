#include <cstddef>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>
#include <xlog/xlog.h>
#include <common/lightmap/af_lightmap_surface.h>
#include <common/lightmap/alpine_lightmap.h>
#include <common/lightmap/alpine_lightmap_decode.h>
#include <common/lightmap/alpine_lightmap_reader.h>
#include <common/scope_guard.h>
#include <gr_d3d11_engine.h>
#include "../../alpine_lightmaps.h"
#include "../editor_gr.h"
#include "../../headless_bake.h"
#include "../../overflow_charts.h"
#include "gr_d3d11_editor_hooks.h"
#include "gr_d3d11_editor_lightmaps.h"

namespace alm = alpine_lightmap;

namespace
{
    // The section drawn, read against the level solid it lights
    alm::ReadResult g_section;
    const GSolid* g_solid = nullptr;
    // The page edge the surfaces' uv_scale/uv_add normalize against, as the section records it
    std::uint32_t g_bake_page = 0;
    std::unordered_map<const GFace*, std::uint32_t> g_overflow_record;

    void drop_section()
    {
        g_section = alm::ReadResult{};
        g_solid = nullptr;
        g_bake_page = 0;
        g_overflow_record.clear();
    }

    bool section_live()
    {
        return g_solid && gr::d3d11::af_lightmap_atlas_live();
    }

    bool is_bound(const rf::GSolid* solid)
    {
        return solid && solid == reinterpret_cast<const rf::GSolid*>(g_solid);
    }

    void drop_surface_charts(const char* why)
    {
        xlog::warn("[AlpineLightmaps] the viewport draws the surfaces without their charts: {}", why);
        g_section.surfaces_ok = false;
        g_section.geoms.clear();
        g_section.bases.clear();
    }

    // The section's whole layer: a bake's pages joined, each freed once copied, or the stored layer decompressed
    bool layer_blocks(const std::vector<std::uint8_t>& body, LightmapPages* layer, std::vector<std::uint8_t>& out)
    {
        const std::uint64_t page_len = alm::layer_payload_size(static_cast<alm::Codec>(g_section.layer.codec), 1,
                                                               g_section.page_size);
        if (layer && layer->size() == g_section.head.num_pages
            && page_len * layer->size() == g_section.layer.uncompressed_size) {
            out.reserve(static_cast<std::size_t>(g_section.layer.uncompressed_size));
            for (LightmapPageBuffer& page : *layer) {
                if (page.size() != page_len) {
                    out.clear();
                    return false;
                }
                out.insert(out.end(), page.begin(), page.end());
                LightmapPageBuffer{}.swap(page);
            }
            return true;
        }
        return alm::decompress_layer(body.data(), body.size(), g_section.layer, out);
    }

    // Reads `body` against `solid` with every check the game makes that the editor can, and uploads its atlas
    void show_section(std::vector<std::uint8_t>& body, bool fresh, const GSolid* solid, LightmapPages* layer)
    {
        const auto surfaces = solid_surfaces(solid);
        const auto num_surfaces = static_cast<std::uint32_t>(surfaces.size());
        std::vector<alm::SurfaceDims> dims(num_surfaces);
        for (std::uint32_t i = 0; i < num_surfaces; i++) {
            const GSurface* s = surfaces[i];
            dims[i].w = s && s->width > 0 ? static_cast<std::uint32_t>(s->width) : 0;
            dims[i].h = s && s->height > 0 ? static_cast<std::uint32_t>(s->height) : 0;
        }
        // The game hashes the surface records it loads against the section's fingerprint. The editor has no such
        // bytes: a saved section was stamped with the surfaces saved beside it (alpine_lm_serialize_chunk), and a
        // bake's describes the surfaces it lit, so the stored hash is taken as is and every other check runs.
        alm::SectionHeader head{};
        std::memcpy(&head, body.data(), sizeof(head));
        {
            // The body keeps its header as stored, also when the read runs out of memory
            const ScopeGuard restore_header{[&] {
                std::memcpy(body.data(), &head, sizeof(head));
            }};
            if (fresh) {
                alm::SectionHeader stamped = head;
                stamped.src_num_surfaces = num_surfaces;
                std::memcpy(body.data(), &stamped, sizeof(stamped));
            }
            g_section = alm::read_section(body.data(), body.size(), dims.data(), num_surfaces, head.src_surface_hash,
                                          true);
        }
        if (!g_section.ok) {
            xlog::warn("[AlpineLightmaps] the viewport cannot draw the section: {}", g_section.reason);
            drop_section();
            return;
        }
        g_solid = solid;
        g_bake_page = alm::stock_page_edge(g_section.head);
        const auto* rf_solid = reinterpret_cast<const rf::GSolid*>(solid);
        if (!g_section.surfaces_ok) {
            drop_surface_charts(g_section.surface_reason);
        }
        else if (!alm::stock_pages_are(rf_solid, g_bake_page)) {
            drop_surface_charts("they were baked against other stock lightmap pages");
        }
        for (std::uint32_t i = 0; g_section.surfaces_ok && i < num_surfaces; i++) {
            const rf::GSurface* s = rf_solid->surfaces[i];
            if (s && !g_section.geoms[i].empty() && !alm::surface_fits_page(*s, g_bake_page)) {
                drop_surface_charts("a surface does not fit its page");
            }
        }
        // As the game: only a D3D11-only level's faces are matched to overflow charts
        CDedLevel* level = CDedLevel::Get();
        if (level && level->GetAlpineLevelProperties().d3d11_only_lightmaps) {
            g_overflow_record = overflow_face_records(g_section, solid);
        }
        if (!g_section.surfaces_ok && g_overflow_record.empty()) {
            drop_section();
            return;
        }
        std::vector<std::uint8_t> blocks;
        if (!layer_blocks(body, layer, blocks)) {
            xlog::warn("[AlpineLightmaps] the viewport cannot draw the section: its layer does not decompress");
            drop_section();
            return;
        }
        if (!gr::d3d11::upload_af_lightmap_atlas(g_section, blocks)) {
            xlog::warn("[AlpineLightmaps] the viewport cannot draw the section: the atlas could not be uploaded");
            drop_section();
        }
    }
}

void editor_af_lightmap_show(std::vector<std::uint8_t>& body, bool fresh, const GSolid* solid, LightmapPages* layer)
{
    const bool was_shown = g_solid != nullptr;
    editor_af_lightmap_hide(false);
    if (editor_gr_is_d3d11() && !headless_bake_active() && solid
        && body.size() >= sizeof(alm::SectionHeader)) {
        // The section is sized by the file, so running out of memory only leaves it undrawn
        try {
            show_section(body, fresh, solid, layer);
        }
        catch (const std::bad_alloc&) {
            xlog::warn("[AlpineLightmaps] the viewport cannot draw the section: out of memory");
            gr::d3d11::release_af_lightmap_atlas();
            drop_section();
        }
    }
    // The room caches hold the charts of the section they were built with
    if (g_solid || was_shown) {
        geo_cache_flush_all();
        editor_views_mark_repaint_all();
    }
}

void editor_af_lightmap_hide(bool flush_caches)
{
    const bool shown = g_solid != nullptr;
    gr::d3d11::release_af_lightmap_atlas();
    drop_section();
    if (shown && flush_caches) {
        geo_cache_flush_all();
        editor_views_mark_repaint_all();
    }
}

void editor_af_lightmap_solid_destroyed(const GSolid* solid)
{
    // its caches are already flushed
    if (solid && solid == g_solid) {
        editor_af_lightmap_hide(false);
    }
}

AfLightmapConstants editor_af_lightmap_constants()
{
    if (!section_live()) {
        return AfLightmapConstants{0.0f, 0.0f, 0.0f, 0.0f};
    }
    return AfLightmapConstants{
        1.0f,
        static_cast<float>(g_section.page_size),
        static_cast<float>(g_section.tile_step),
        static_cast<float>(g_section.gutter),
    };
}

bool editor_af_lightmap_face_setup(const rf::GSolid* solid, int surface_index, AfLightmapFace& out)
{
    out = AfLightmapFace{};
    if (!section_live() || !is_bound(solid) || !g_section.surfaces_ok || surface_index < 0
        || surface_index >= solid->surfaces.size()) {
        return false;
    }
    const auto index = static_cast<std::uint32_t>(surface_index);
    if (index >= g_section.head.num_charts || index >= g_section.geoms.size() || g_section.geoms[index].empty()) {
        return false;
    }
    const rf::GSurface* surface = solid->surfaces[surface_index];
    return surface && alm::surface_chart_face(*surface, index, g_section.charts[index], g_bake_page, out);
}

void editor_af_lightmap_face_texel(const AfLightmapFace& face, const rf::Vector3& pos, float& out_u, float& out_v)
{
    const alm::ChartTexel t = alm::face_chart_texel(face, &pos.x);
    out_u = t.u;
    out_v = t.v;
}

bool editor_af_lightmap_overflow_face(const rf::GSolid* solid, const rf::GFace* face, AfOverflowFace& out)
{
    out = AfOverflowFace{};
    if (!section_live() || !is_bound(solid) || !face || g_overflow_record.empty()) {
        return false;
    }
    const auto it = g_overflow_record.find(reinterpret_cast<const GFace*>(face));
    if (it == g_overflow_record.end()) {
        return false;
    }
    const std::uint32_t record = it->second;
    if (record >= g_section.overflow.size() || !g_section.overflow_ok[record]) {
        return false;
    }
    out.chart = static_cast<int>(alm::gpu_overflow_chart(g_section, record));
    out.record = record;
    return true;
}

bool editor_af_lightmap_overflow_texel(const AfOverflowFace& face, const rf::Vector3& pos, float& out_u, float& out_v)
{
    if (face.chart < 0 || face.record >= g_section.overflow.size()) {
        return false;
    }
    const alm::ChartTexel t = alm::overflow_chart_coord(g_section.overflow[face.record], &pos.x);
    out_u = t.u;
    out_v = t.v;
    return true;
}
