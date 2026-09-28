#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <zlib.h>
#include <xxhash.h>

#include <common/utils/list-utils.h>
#include <common/lightmap/alpine_lightmap.h>
#include <common/lightmap/alpine_lightmap_decode.h>
#include <common/lightmap/alpine_lightmap_reader.h>
#include <xlog/xlog.h>

#include "af_lightmap.h"
#include "d3d11/gr_d3d11_hooks.h"
#include "../rf/file/file.h"
#include "../rf/geometry.h"
#include "../rf/level.h"
#include "../rf/math/vector.h"
#include "../rf/mover.h"
#include "../rf/multi.h"
#include "../misc/alpine_settings.h"
#include "../misc/alpine_terrain.h"
#include "../misc/level.h"
#include "../multi/multi.h"

namespace alm = alpine_lightmap;

namespace
{
    // XXH32 of `bytes` of surface records, or of the empty message for none.
    std::uint32_t surface_records_hash(const std::uint8_t* records, std::size_t bytes)
    {
        const std::uint8_t empty = 0;
        return XXH32(bytes > 0 ? static_cast<const void*>(records) : static_cast<const void*>(&empty), bytes, 0);
    }

    // The bake's own page budget is 1024 slices, which is 67 MB of BC7, so a longer section is a
    // malformed length rather than one this build could ever consume.
    constexpr std::size_t af_max_section_bytes = 320u * 1024u * 1024u;

    struct AfFingerprint
    {
        bool valid = false;
        std::uint32_t num_surfaces = 0;
        std::uint32_t hash = 0;
    };

    AfFingerprint g_fp;
    bool g_stock_section_seen = false;
    int g_synth_page_bm = -1;
    alm::ReadResult g_section;
    rf::GSolid* g_solid = nullptr;
    // Page dimension the bake's uv_scale/uv_add normalize against, as the section header records it; it
    // stands in for the engine's synthesised page on a level that ships no stock lightmaps section.
    std::uint32_t g_bake_page_size = 0;

    // Per terrain chart record of g_section: its decoded RGB8 texels reduced by g_terrain_reduction,
    // for the CPU light sampler whatever the renderer. Empty unless the record matched a loaded terrain.
    std::vector<std::vector<std::uint8_t>> g_terrain_rgb;
    std::vector<std::uint32_t> g_terrain_reduction;
    // Per alpine_terrain_get_all() index: the matching chart record, or -1.
    std::vector<int> g_terrain_record;

    // Every mover solid the movers section (0x2000) loaded, with the fingerprint of its surface records.
    struct MoverCapture
    {
        int uid;
        rf::GSolid* solid;
        std::uint32_t num_surfaces;
        std::uint32_t hash;
    };
    std::vector<MoverCapture> g_mover_captures;
    // The matched mover record of each mover solid; a solid missing here has no chart.
    std::unordered_map<rf::GSolid*, std::uint32_t> g_mover_record;

    // Always paired with release_af_lightmap_atlas, which stops the charts being handed out.
    void af_drop_section()
    {
        g_section = alm::ReadResult{};
        g_solid = nullptr;
        g_bake_page_size = 0;
        g_terrain_rgb.clear();
        g_terrain_reduction.clear();
        g_terrain_record.clear();
        g_mover_record.clear();
    }

    // The chart record matched to terrain `index`, or null.
    const alm::TerrainChart* af_terrain_record(int index, int& record)
    {
        record = -1;
        if (!g_section.ok || index < 0 || static_cast<std::size_t>(index) >= g_terrain_record.size()) {
            return nullptr;
        }
        record = g_terrain_record[index];
        if (record < 0 || static_cast<std::size_t>(record) >= g_section.terrain.size()) {
            return nullptr;
        }
        return &g_section.terrain[record];
    }

    // Matches the chart records to the terrains, whose chunk precedes the geometry section and so
    // this one. An unmatched record is marked unusable before anything is decoded for it. A fullbright
    // terrain takes none.
    void af_match_terrains()
    {
        const auto& terrains = alpine_terrain_get_all();
        g_terrain_record.assign(terrains.size(), -1);
        std::vector<std::uint8_t> used(g_section.terrain.size(), 0);
        for (std::size_t k = 0; k < terrains.size(); k++) {
            const AlpineTerrain& t = terrains[k];
            if (t.fullbright()) {
                continue;
            }
            int record = -1;
            for (std::size_t i = 0; i < g_section.terrain.size(); i++) {
                if (g_section.terrain_ok[i] && g_section.terrain[i].terrain_uid == t.uid) {
                    record = static_cast<int>(i);
                }
            }
            if (record < 0) {
                continue;
            }
            const alm::TerrainChart& chart = g_section.terrain[record];
            switch (alm::terrain_chart_fit(chart, alpine_terrain_grid(t), t.decoration_lighting_hash)) {
            case alm::TerrainChartFit::match:
                break;
            case alm::TerrainChartFit::other_grid:
                xlog::warn("[AlpineLightmaps] terrain {} has baked lighting for another grid size, re-bake the level",
                           t.uid);
                continue;
            case alm::TerrainChartFit::stale:
                xlog::warn("[AlpineLightmaps] terrain {} changed since its lighting was baked, re-bake the level",
                           t.uid);
                continue;
            }
            g_terrain_record[k] = record;
            used[record] = 1;
        }
        for (std::size_t i = 0; i < used.size(); i++) {
            if (!used[i]) {
                g_section.terrain_ok[i] = 0;
            }
        }
    }

    // Whether surface `s` lies within a page x page lightmap page.
    bool surface_fits_page(const rf::GSurface* s, std::uint32_t page)
    {
        const auto p = static_cast<std::int64_t>(page);
        return s->xstart >= 0 && s->ystart >= 0 && s->width >= 0 && s->height >= 0
            && static_cast<std::int64_t>(s->xstart) + s->width <= p
            && static_cast<std::int64_t>(s->ystart) + s->height <= p;
    }

    // Whether surface `s`, charted by `c`, is the fragment the chart was baked for and fits the bake_page
    // square page its uv_scale/uv_add normalize against.
    bool af_mover_surface_fits(const rf::GSurface* s, const alm::MoverSurfaceChart& c, std::uint32_t bake_page)
    {
        return s && s->lightmap && s->width == c.w && s->height == c.h && surface_fits_page(s, bake_page);
    }

    // Whether every stock page the surfaces of `solid` point at is edge x edge.
    bool af_stock_pages_are(const rf::GSolid* solid, std::uint32_t edge)
    {
        for (int i = 0; solid && i < solid->surfaces.size(); i++) {
            const rf::GSurface* s = solid->surfaces[i];
            if (s && s->lightmap
                && (static_cast<std::uint32_t>(s->lightmap->w) != edge
                    || static_cast<std::uint32_t>(s->lightmap->h) != edge)) {
                return false;
            }
        }
        return true;
    }

    // Matches the mover records to the mover solids the movers section loaded, by uid, surface count and
    // fingerprint. An unmatched record is marked unusable before the GPU index is built.
    void af_match_movers(std::uint32_t bake_page)
    {
        g_mover_record.clear();
        std::vector<std::uint8_t> used(g_section.movers.size(), 0);
        if (is_d3d11() && !g_section.movers.empty()) {
            // A mover whose creation failed freed its solid, so only the latest capture of a live solid counts.
            std::unordered_set<rf::GSolid*> live;
            for (rf::MoverBrush& mb : DoublyLinkedList{rf::mover_brush_list}) {
                if (mb.geometry) {
                    live.insert(mb.geometry);
                }
            }
            std::vector<const MoverCapture*> current;
            std::unordered_set<rf::GSolid*> seen;
            for (auto it = g_mover_captures.rbegin(); it != g_mover_captures.rend(); ++it) {
                if (live.count(it->solid) && seen.insert(it->solid).second) {
                    current.push_back(&*it);
                }
            }
            std::unordered_map<int, int> uid_count;
            for (const MoverCapture* c : current) {
                uid_count[c->uid]++;
            }
            for (const MoverCapture* c : current) {
                if (uid_count[c->uid] != 1) {
                    continue;
                }
                std::uint32_t record = 0;
                bool found = false;
                for (std::uint32_t i = 0; i < g_section.movers.size() && !found; i++) {
                    if (g_section.mover_ok[i] && g_section.movers[i].mover_uid == c->uid) {
                        record = i;
                        found = true;
                    }
                }
                if (!found) {
                    continue;
                }
                const alm::MoverChart& m = g_section.movers[record];
                bool match = m.num_surfaces == c->num_surfaces && m.surface_hash == c->hash
                          && static_cast<std::uint32_t>(c->solid->surfaces.size()) == c->num_surfaces;
                for (std::uint32_t s = 0; match && s < m.num_surfaces; s++) {
                    const std::uint32_t k = g_section.mover_first_surface[record] + s;
                    match = g_section.mover_geoms[k].empty()
                         || af_mover_surface_fits(c->solid->surfaces[s], g_section.mover_charts[k], bake_page);
                }
                if (!match) {
                    xlog::warn("[AlpineLightmaps] mover {} changed since its lighting was baked, re-bake the level",
                               c->uid);
                    continue;
                }
                g_mover_record[c->solid] = record;
                used[record] = 1;
            }
        }
        for (std::size_t i = 0; i < used.size(); i++) {
            if (!used[i]) {
                g_section.mover_ok[i] = 0;
            }
        }
    }

    // Whether the section declares any terrain chart, peeked from its table directory without reading
    // the body. Leaves the file where it was.
    bool af_peek_terrain_charts(rf::File& file, std::size_t chunk_len)
    {
        const int start = file.tell();
        const auto read = [&file](void* dst, std::size_t len) {
            return file.read(dst, static_cast<int>(len)) == static_cast<int>(len) && !file.error();
        };
        bool found = false;
        alm::SectionHeader head{};
        std::uint32_t num_tables = 0;
        if (chunk_len >= sizeof(head) && read(&head, sizeof(head)) && !alm::header_invalid_reason(head)) {
            std::uint64_t pos = sizeof(head) + static_cast<std::uint64_t>(head.num_charts) * sizeof(alm::Chart);
            if (pos + sizeof(num_tables) <= chunk_len) {
                file.seek(static_cast<int>(start + pos), rf::File::seek_set);
                if (read(&num_tables, sizeof(num_tables)) && num_tables <= alm::max_tables) {
                    pos += sizeof(num_tables);
                    for (std::uint32_t k = 0; k < num_tables && !found; k++) {
                        alm::TableHeader th{};
                        if (pos + sizeof(th) > chunk_len || !read(&th, sizeof(th))) {
                            break;
                        }
                        pos += sizeof(th);
                        if (th.byte_len > chunk_len - pos) {
                            break;
                        }
                        std::uint32_t num_terrain = 0;
                        found = th.tag == static_cast<std::uint32_t>(alm::TableTag::terrain)
                             && th.version == alm::terrain_table_version
                             && th.byte_len >= sizeof(num_terrain) && read(&num_terrain, sizeof(num_terrain))
                             && num_terrain != 0;
                        pos += th.byte_len;
                        file.seek(static_cast<int>(start + pos), rf::File::seek_set);
                    }
                }
            }
        }
        file.seek(start, rf::File::seek_set);
        return found;
    }

    void af_drop_surface_charts()
    {
        g_section.surfaces_ok = false;
        g_section.geoms.clear();
        g_section.bases.clear();
    }

    // Allocations here are sized by the file, so a hostile or truncated level must not throw
    // across the engine call this runs inside.
    void af_load_chunk_inner(rf::File& file, std::size_t chunk_len, std::size_t& remaining)
    {
        af_drop_section();
        gr::d3d11::release_af_lightmap_atlas();

        // No renderer means no consumer: a dedicated server or headless client never decompresses a
        // page. The legacy renderers only take the terrain charts, for the CPU light sampler.
        if (rf::is_dedicated_server || is_headless_mode()) {
            return;
        }
        rf::GSolid* solid = rf::level.geometry;
        const std::int64_t bytes_left = static_cast<std::int64_t>(file.size()) - file.tell();
        if (!solid || !alm::section_len_plausible(chunk_len, bytes_left) || chunk_len > af_max_section_bytes) {
            xlog::warn("[AlpineLightmaps] ignoring a {} byte section", chunk_len);
            return;
        }
        if (!is_d3d11() && (alpine_terrain_get_all().empty() || !af_peek_terrain_charts(file, chunk_len))) {
            return;
        }

        std::vector<std::uint8_t> body(chunk_len);
        const int got = file.read(body.data(), static_cast<int>(chunk_len));
        if (got != static_cast<int>(chunk_len) || file.error()) {
            xlog::warn("[AlpineLightmaps] ignoring the section, only {} of {} bytes were read", got,
                       chunk_len);
            if (got > 0) {
                remaining -= static_cast<std::size_t>(got);
            }
            return;
        }
        remaining = 0;

        const int num_surfaces = solid->surfaces.size();
        std::vector<alm::SurfaceDims> dims(num_surfaces > 0 ? num_surfaces : 0);
        for (int i = 0; i < num_surfaces; i++) {
            rf::GSurface* s = solid->surfaces[i];
            dims[i].w = s && s->width > 0 ? static_cast<std::uint32_t>(s->width) : 0;
            dims[i].h = s && s->height > 0 ? static_cast<std::uint32_t>(s->height) : 0;
        }

        g_section = alm::read_section(body.data(), body.size(), dims.data(),
                                      static_cast<std::uint32_t>(num_surfaces), g_fp.hash, g_fp.valid);
        if (!g_section.ok) {
            xlog::warn("[AlpineLightmaps] ignoring the section: {}", g_section.reason);
            af_drop_section();
            return;
        }
        af_match_terrains();
        // Surface and mover uv_scale/uv_add normalize against the stock page size the bake recorded. With no
        // stock section every page a face holds is the engine's synthesised page, with one it must be that size.
        const std::uint32_t bake_page = alm::stock_page_edge(g_section.head);
        bool stock_pages_differ = false;
        if (g_stock_section_seen && is_d3d11()) {
            stock_pages_differ = !af_stock_pages_are(solid, bake_page);
            for (rf::MoverBrush& mb : DoublyLinkedList{rf::mover_brush_list}) {
                stock_pages_differ = stock_pages_differ || !af_stock_pages_are(mb.geometry, bake_page);
            }
        }
        if (*g_section.mover_reason) {
            xlog::warn("[AlpineLightmaps] ignoring the mover charts: {}", g_section.mover_reason);
        }
        if (stock_pages_differ) {
            xlog::warn("[AlpineLightmaps] ignoring the surface and mover charts: they were baked against {}x{} stock "
                       "lightmap pages and the level's are not",
                       bake_page, bake_page);
            std::fill(g_section.mover_ok.begin(), g_section.mover_ok.end(), std::uint8_t{0});
        }
        af_match_movers(bake_page);
        const auto drop_surfaces = [](const char* why) {
            xlog::warn("[AlpineLightmaps] ignoring the surface charts: {}", why);
            af_drop_surface_charts();
        };
        if (!g_section.surfaces_ok) {
            drop_surfaces(g_section.surface_reason);
        }
        else if (!is_d3d11() || stock_pages_differ) {
            af_drop_surface_charts();
        }

        if (g_section.surfaces_ok && !g_stock_section_seen) {
            const std::uint32_t p = bake_page;
            for (int i = 0; i < num_surfaces; i++) {
                rf::GSurface* s = solid->surfaces[i];
                if (!s || g_section.geoms[i].empty()) {
                    continue;
                }
                if (!surface_fits_page(s, p)) {
                    xlog::warn("[AlpineLightmaps] surface {} does not fit the {}x{} page the section records", i, p, p);
                    drop_surfaces("a surface does not fit its page");
                    break;
                }
            }
        }
        if (g_section.surfaces_ok || !g_mover_record.empty()) {
            g_bake_page_size = bake_page;
        }
        const bool any_terrain = std::any_of(g_section.terrain_ok.begin(), g_section.terrain_ok.end(),
                                             [](std::uint8_t v) { return v != 0; });
        if (!g_section.surfaces_ok && !any_terrain && g_mover_record.empty()) {
            af_drop_section();
            return;
        }

        std::vector<std::uint8_t> blocks;
        int zr = Z_OK;
        if (!alm::decompress_layer(body.data(), body.size(), g_section.layer, blocks, &zr)) {
            xlog::warn("[AlpineLightmaps] ignoring the section: zlib returned {}", zr);
            af_drop_section();
            return;
        }

        body.clear();
        body.shrink_to_fit();

        // Before the GPU index is built, so a chart that does not decode is left out of it as well.
        g_terrain_rgb.assign(g_section.terrain.size(), {});
        g_terrain_reduction.assign(g_section.terrain.size(), 1);
        const auto& terrains = alpine_terrain_get_all();
        for (std::size_t k = 0; k < g_terrain_record.size(); k++) {
            const int record = g_terrain_record[k];
            if (record < 0 || !g_section.terrain_ok[record] || !g_terrain_rgb[record].empty()) {
                continue;
            }
            const auto i = static_cast<std::uint32_t>(record);
            std::uint32_t r = 1;
            if (!alm::decode_reduced_terrain_chart(g_section, blocks.data(), blocks.size(), i,
                                                   alpine_terrain::cells(terrains[k].header.nx), alm::unpack_bc7_rgba,
                                                   g_terrain_rgb[i], r)) {
                g_section.terrain_ok[i] = 0;
                g_terrain_rgb[i].clear();
                continue;
            }
            g_terrain_reduction[i] = r;
        }
        for (int& record : g_terrain_record) {
            if (record >= 0 && !g_section.terrain_ok[record]) {
                record = -1;
            }
        }

        g_solid = solid;
        if (is_d3d11() && !gr::d3d11::upload_af_lightmap_atlas(g_section, blocks)) {
            xlog::warn("[AlpineLightmaps] the atlas could not be uploaded, the level renders with its stock "
                       "lightmaps");
            af_drop_surface_charts();
            g_mover_record.clear();
        }
        xlog::info("[AlpineLightmaps] {} pages, {} surface charts{}, {} terrain charts, {} mover charts ({} matched), "
                   "{} tiles, density {} px/m",
                   g_section.head.num_pages, g_section.head.num_charts, g_section.surfaces_ok ? "" : " (unused)",
                   g_section.terrain.size(), g_section.movers.size(), g_mover_record.size(), g_section.head.num_tiles,
                   g_section.head.base_density);
    }
}

int af_lightmap_synthesized_page_bm()
{
    return g_synth_page_bm;
}

AfLightmapConstants af_lightmap_constants()
{
    if (!gr::d3d11::af_lightmap_atlas_live()) {
        return AfLightmapConstants{0.0f, 0.0f, 0.0f, 0.0f};
    }
    return AfLightmapConstants{
        1.0f,
        static_cast<float>(g_section.page_size),
        static_cast<float>(g_section.tile_step),
        static_cast<float>(g_section.gutter),
    };
}

bool af_lightmap_face_setup(rf::GSolid* solid, int surface_index, AfLightmapFace& out)
{
    out = AfLightmapFace{};
    if (!gr::d3d11::af_lightmap_atlas_live() || !solid || surface_index < 0
        || surface_index >= solid->surfaces.size()) {
        return false;
    }
    const auto index = static_cast<std::uint32_t>(surface_index);
    std::uint32_t chart = 0;
    alm::Chart k{};
    if (solid == g_solid) {
        if (!g_section.surfaces_ok || index >= g_section.head.num_charts || g_section.geoms[index].empty()) {
            return false;
        }
        chart = index;
        k = g_section.charts[index];
    }
    else {
        const auto it = g_mover_record.find(solid);
        if (it == g_mover_record.end() || it->second >= g_section.movers.size() || !g_section.mover_ok[it->second]
            || index >= g_section.movers[it->second].num_surfaces) {
            return false;
        }
        const std::uint32_t flat = g_section.mover_first_surface[it->second] + index;
        if (g_section.mover_geoms[flat].empty()) {
            return false;
        }
        chart = alm::gpu_mover_chart(g_section, it->second, index);
        k = alm::Chart{g_section.mover_charts[flat].k_u, g_section.mover_charts[flat].k_v};
    }
    rf::GSurface* surface = solid->surfaces[surface_index];
    if (!surface || !surface->lightmap) {
        return false;
    }
    if (surface->u_coefficient < 0 || surface->u_coefficient > 2 || surface->v_coefficient < 0
        || surface->v_coefficient > 2 || surface->lightmap->w <= 0 || surface->lightmap->h <= 0
        || surface->xstart < 0 || surface->ystart < 0) {
        return false;
    }
    // uv_scale/uv_add normalize against the page RED packed the fragment into, which is not
    // the page the surface points at when the level ships no stock lightmaps section.
    const std::uint32_t lm_w = g_bake_page_size ? g_bake_page_size
                                                : static_cast<std::uint32_t>(surface->lightmap->w);
    const std::uint32_t lm_h = g_bake_page_size ? g_bake_page_size
                                                : static_cast<std::uint32_t>(surface->lightmap->h);
    out.chart = static_cast<int>(chart);
    out.axis_u = surface->u_coefficient;
    out.axis_v = surface->v_coefficient;
    out.scale_u = surface->uv_scale.x;
    out.scale_v = surface->uv_scale.y;
    out.add_u = surface->uv_add.x;
    out.add_v = surface->uv_add.y;
    out.lm_w = lm_w;
    out.lm_h = lm_h;
    out.surf_x = static_cast<std::uint32_t>(surface->xstart);
    out.surf_y = static_cast<std::uint32_t>(surface->ystart);
    out.k_u = k.k_u;
    out.k_v = k.k_v;
    return true;
}

void af_lightmap_face_texel(const AfLightmapFace& face, const rf::Vector3& pos, float& out_u, float& out_v)
{
    const alm::ChartTexel t =
        alm::surface_chart_texel(&pos.x, face.axis_u, face.axis_v, face.scale_u, face.scale_v, face.add_u,
                                 face.add_v, face.lm_w, face.lm_h, face.surf_x, face.surf_y, face.k_u, face.k_v);
    out_u = t.u;
    out_v = t.v;
}

bool af_lightmap_terrain_chart(int terrain_index, AfTerrainChart& out)
{
    int record = -1;
    const alm::TerrainChart* t = af_terrain_record(terrain_index, record);
    if (!gr::d3d11::af_lightmap_atlas_live() || !t) {
        return false;
    }
    out.chart = static_cast<int>(alm::gpu_terrain_chart(g_section, static_cast<std::uint32_t>(record)));
    out.origin_x = t->origin_x;
    out.origin_z = t->origin_z;
    out.texel_size = t->texel_size;
    return true;
}

// ─── level load ───────────────────────────────────────────────────────────────

void af_lightmap_note_stock_lightmaps()
{
    g_stock_section_seen = true;
}

void af_lightmap_level_reset()
{
    gr::d3d11::release_af_lightmap_atlas();
    af_drop_section();
    g_fp = AfFingerprint{};
    g_stock_section_seen = false;
    g_synth_page_bm = -1;
    g_mover_captures.clear();
}

void af_lightmap_capture_mover(int uid, rf::GSolid* solid, const rf::VFile& reader)
{
    if (rf::is_dedicated_server || is_headless_mode() || !is_d3d11() || !solid) {
        return;
    }
    const std::uint8_t* const buf = reader.buf;
    const int pos = reader.pos;
    const int n = solid->surfaces.size();
    const std::int64_t need = static_cast<std::int64_t>(n) * alm::surface_record_size;
    if (reader.is_memory != 1 || !buf || pos < 0 || pos > reader.size || reader.error != 0 || reader.version < 0xB5 ||
        n < 0 || need > pos) {
        return;
    }
    // the solid's surface records are the last bytes its loader consumed
    const std::uint32_t hash = surface_records_hash(buf + (pos - need), static_cast<std::size_t>(need));
    try {
        g_mover_captures.push_back({uid, solid, static_cast<std::uint32_t>(n), hash});
    }
    catch (...) {
    }
}

void af_lightmap_resolve_terrains()
{
    // matched when the section loaded, so this only reports
    const auto& terrains = alpine_terrain_get_all();
    int matched = 0;
    int lit = 0;
    for (std::size_t k = 0; k < terrains.size(); k++) {
        if (terrains[k].fullbright()) {
            continue;
        }
        lit++;
        int record = -1;
        matched += af_terrain_record(static_cast<int>(k), record) ? 1 : 0;
    }
    if (!g_section.terrain.empty() || lit > 0) {
        xlog::info("[AlpineLightmaps] {} of {} lit terrain(s) have baked lighting", matched, lit);
    }
}

bool af_lightmap_terrain_sample(int terrain_index, float world_x, float world_z, float (&texel)[3])
{
    // what the GPU draws: without the atlas, the placeholder
    if (is_d3d11() && !gr::d3d11::af_lightmap_atlas_live()) {
        return false;
    }
    int record = -1;
    const alm::TerrainChart* t = af_terrain_record(terrain_index, record);
    if (!t || static_cast<std::size_t>(record) >= g_terrain_rgb.size() || g_terrain_rgb[record].empty()) {
        return false;
    }
    alm::terrain_reduced_chart_sample(g_terrain_rgb[record].data(), *t, g_terrain_reduction[record], world_x,
                                      world_z, texel);
    return true;
}

void af_lightmap_init_synthesized_page()
{
    g_synth_page_bm = -1;
    rf::GSolid* solid = rf::level.geometry;
    if (g_stock_section_seen || !solid) {
        return;
    }
    // with no stock pages every surface's lightmap index clamps to the one synthesised page
    for (int i = 0; i < solid->surfaces.size(); i++) {
        rf::GSurface* s = solid->surfaces[i];
        rf::GLightmap* lm = s ? s->lightmap : nullptr;
        if (!lm) {
            continue;
        }
        if (lm->buf && lm->w > 0 && lm->h > 0) {
            std::memset(lm->buf, 0xff, static_cast<std::size_t>(lm->w) * lm->h * 3);
        }
        g_synth_page_bm = lm->bm_handle;
        return;
    }
}

void af_lightmap_capture_surface_fingerprint(rf::File& file)
{
    g_fp = AfFingerprint{};
    rf::GSolid* solid = rf::level.geometry;
    if (!solid) {
        return;
    }
    const int count = solid->surfaces.size();
    if (count < 0) {
        return;
    }
    const int end = file.tell();
    const std::int64_t need = static_cast<std::int64_t>(count) * alm::surface_record_size;
    if (end < 0 || need > end) {
        return;
    }

    // The surface records are the last bytes of the geometry section, which is what makes the
    // range the bake hashed addressable without re-parsing the section.
    std::vector<std::uint8_t> buf;
    try {
        buf.resize(static_cast<std::size_t>(need));
    }
    catch (...) {
        return;
    }
    if (need > 0) {
        file.seek(static_cast<int>(end - need), rf::File::seek_set);
        const int got = file.read(buf.data(), static_cast<int>(need));
        file.seek(end, rf::File::seek_set);
        if (got != static_cast<int>(need) || file.error()) {
            xlog::warn("[AlpineLightmaps] could not re-read the surface records for the fingerprint");
            return;
        }
    }
    g_fp.valid = true;
    g_fp.num_surfaces = static_cast<std::uint32_t>(count);
    g_fp.hash = surface_records_hash(buf.data(), static_cast<std::size_t>(need));
}

void af_lightmap_load_chunk(rf::File& file, std::size_t chunk_len)
{
    std::size_t remaining = chunk_len;
    rf::File::ChunkGuard chunk_guard{file, remaining};
    try {
        af_load_chunk_inner(file, chunk_len, remaining);
    }
    catch (...) {
        xlog::warn("[AlpineLightmaps] ignoring the section: out of memory");
        af_drop_section();
        gr::d3d11::release_af_lightmap_atlas();
    }
}
