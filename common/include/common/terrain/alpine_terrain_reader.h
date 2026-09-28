#pragma once

// Alpine Terrain — the record parse of the 0x0AFBAE0B chunk, shared by the game and RED so both accept
// exactly the same records. Unlike alpine_terrain.h this allocates and lets std::bad_alloc propagate:
// each DLL instantiates its own copy, and a Record never crosses a module boundary.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>

#include "alpine_terrain.h"

namespace alpine_terrain
{

struct RecordLayer
{
    std::string texture;
    float uv_scale = 0.0f;
    bool triplanar = false;
};

struct RecordOverlay
{
    std::string texture;
    float uv_scale = 0.0f;
    bool triplanar = false;
    bool break_tiling = false;
};

struct RecordDecoration
{
    std::string mesh; // empty = none picked
    float density = 0.0f;
    float scale_min = 0.0f;
    float scale_max = 0.0f;
    float max_slope = 0.0f;
    float draw_distance = 0.0f;
    float vertical_offset = 0.0f;
    std::uint8_t link_layer = decoration_link_none;
    bool align_to_slope = false;
    bool random_yaw = false;
    bool casts_shadows = false;
};

// One validated record; array sizes match `header` exactly.
struct Record
{
    std::int32_t uid = 0;
    Header header{};
    std::string script_name;
    std::string underside_texture;
    std::string crater_texture; // empty = level geomod texture
    std::vector<RecordLayer> layers;
    std::vector<RecordOverlay> overlays;
    std::vector<RecordDecoration> decorations;
    std::vector<ChunkMapping> build_mapping; // empty = absent, or stale and dropped
    std::vector<std::uint16_t> heights;
    std::vector<std::uint8_t> weights;
    std::vector<std::uint8_t> holes;
    std::vector<std::uint8_t> diag;
    // flag_geoable only: the chunk geo mask over header_chunk_count chunks, every chunk set when the
    // record carries none.
    std::vector<std::uint8_t> geo_chunks;
    // flag_overlays only: overlay_map_bytes, channels past the overlay count cleared.
    std::vector<std::uint8_t> overlay_coverage;
    // flag_decorations only: one decoration_plane_bytes plane per decoration, in list order.
    std::vector<std::uint8_t> decoration_coverage;
};

// Reader: bool read_bytes(void*, std::size_t) and bool read_string(std::string&) (u16 length prefix)
// that never read past the chunk, and std::size_t `remaining`, the chunk bytes still unread.
// Parses one record into `out` and adds its decompressed size to `total_raw`, the level's running
// total against max_level_raw_bytes. Returns nullptr on success, else why the chunk is unusable.
template<typename Reader>
const char* read_record(Reader& r, Record& out, std::uint64_t& total_raw)
{
    auto get = [&r](auto& v) { return r.read_bytes(&v, sizeof(v)); };
    Header& h = out.header;
    std::uint16_t nx = 0, nz = 0;
    std::uint8_t chunk_cells = 0, mul = 0, density = 0, flags = 0, layer_count = 0;

    if (!get(out.uid) || !get(h.origin)) return "truncated";
    if (!r.read_string(out.script_name)) return "truncated";
    if (out.script_name.size() > max_script_name_len) return "script name too long";
    if (!get(h.cell_size) || !get(nx) || !get(nz)) return "truncated";
    if (!get(h.height_min) || !get(h.height_range)) return "truncated";
    if (!get(chunk_cells) || !get(mul) || !get(density) || !get(flags)) return "truncated";
    if (!get(h.thickness) || !get(h.skirt_depth)) return "truncated";
    if (!r.read_string(out.underside_texture) || !r.read_string(out.crater_texture)) return "truncated";
    if (!get(layer_count)) return "truncated";
    h.nx = nx;
    h.nz = nz;
    h.chunk_cells = chunk_cells;
    h.weight_res_mul = mul;
    h.lightmap_density = density;
    h.flags = flags;
    h.layer_count = layer_count;
    h.overlay_count = 0;
    h.decoration_count = 0;
    if (const char* err = validate_header(h)) return err;
    if (!texture_name_valid(out.underside_texture.c_str(), out.underside_texture.size()) ||
        !texture_name_valid(out.crater_texture.c_str(), out.crater_texture.size())) {
        return "texture name too long";
    }

    out.layers.resize(layer_count);
    for (RecordLayer& layer : out.layers) {
        std::uint8_t layer_flags = 0;
        if (!r.read_string(layer.texture) || !get(layer.uv_scale) || !get(layer_flags)) return "truncated";
        if (!texture_name_valid(layer.texture.c_str(), layer.texture.size())) return "texture name too long";
        if (const char* err = validate_layer(layer.uv_scale, layer_flags)) return err;
        layer.triplanar = (layer_flags & layer_flag_triplanar) != 0;
    }

    if (h.flags & flag_overlays) {
        std::uint8_t overlay_count = 0;
        if (!get(overlay_count)) return "truncated";
        h.overlay_count = overlay_count;
    }
    if (const char* err = validate_overlay_count(h)) return err;
    out.overlays.resize(h.overlay_count);
    for (RecordOverlay& overlay : out.overlays) {
        std::uint8_t overlay_flags = 0;
        if (!r.read_string(overlay.texture) || !get(overlay.uv_scale) || !get(overlay_flags)) return "truncated";
        if (!texture_name_valid(overlay.texture.c_str(), overlay.texture.size())) return "texture name too long";
        if (const char* err = validate_overlay(overlay.uv_scale, overlay_flags)) return err;
        overlay.triplanar = (overlay_flags & overlay_flag_triplanar) != 0;
        overlay.break_tiling = (overlay_flags & overlay_flag_break_tiling) != 0;
    }

    if (h.flags & flag_decorations) {
        std::uint8_t decoration_count = 0;
        if (!get(decoration_count)) return "truncated";
        h.decoration_count = decoration_count;
    }
    if (const char* err = validate_decoration_count(h)) return err;
    out.decorations.resize(h.decoration_count);
    for (RecordDecoration& d : out.decorations) {
        std::uint8_t link = 0, flags_byte = 0;
        if (!r.read_string(d.mesh) || !get(d.density) || !get(d.scale_min) || !get(d.scale_max) || !get(d.max_slope) ||
            !get(d.draw_distance) || !get(d.vertical_offset) || !get(link) || !get(flags_byte)) {
            return "truncated";
        }
        if (!decoration_mesh_valid(d.mesh.c_str(), d.mesh.size())) return "decoration mesh name invalid";
        if (const char* err = validate_decoration(d.density, d.scale_min, d.scale_max, d.max_slope, d.draw_distance,
                                                  d.vertical_offset, link, flags_byte, h.layer_count)) {
            return err;
        }
        d.link_layer = link;
        d.align_to_slope = (flags_byte & decoration_flag_align_to_slope) != 0;
        d.random_yaw = (flags_byte & decoration_flag_random_yaw) != 0;
        d.casts_shadows = (flags_byte & decoration_flag_casts_shadows) != 0;
    }

    std::uint32_t mapping_count = 0;
    if (!get(mapping_count)) return "truncated";
    if (!mapping_count_acceptable(mapping_count)) return "build mapping count out of range";
    out.build_mapping.resize(mapping_count);
    for (ChunkMapping& m : out.build_mapping) {
        if (!get(m.room_uid) || !get(m.vertex_count) || !get(m.pos_hash)) return "truncated";
    }
    if (!mapping_count_valid(h, mapping_count)) out.build_mapping.clear();

    std::uint32_t raw_size = 0, comp_size = 0;
    if (!get(raw_size) || !get(comp_size)) return "truncated";
    const std::size_t expected = header_raw_size(h);
    if (raw_size != expected) return "payload size mismatch";
    if (comp_size == 0 || comp_size > r.remaining || comp_size > compressBound(static_cast<uLong>(expected))) {
        return "compressed size out of range";
    }
    total_raw += expected;
    if (total_raw > max_level_raw_bytes) return "terrain data budget exceeded";

    std::vector<std::uint8_t> comp(comp_size);
    if (!r.read_bytes(comp.data(), comp_size)) return "truncated";
    std::vector<std::uint8_t> raw(expected);
    uLongf out_len = static_cast<uLongf>(expected);
    if (uncompress(raw.data(), &out_len, comp.data(), comp_size) != Z_OK || out_len != expected) {
        return "payload does not decompress";
    }

    out.heights.resize(vertex_count(nx, nz));
    std::memcpy(out.heights.data(), raw.data() + blob_heights_offset(), blob_heights_bytes(nx, nz));
    const std::uint8_t* weights = raw.data() + blob_weights_offset(nx, nz);
    out.weights.assign(weights, weights + blob_weights_bytes(nx, nz, mul));
    const std::size_t mask = bitmask_bytes(cells(nx), cells(nz));
    const std::uint8_t* holes = raw.data() + blob_holes_offset(nx, nz, mul);
    out.holes.assign(holes, holes + mask);
    const std::uint8_t* diag = raw.data() + blob_diag_offset(nx, nz, mul);
    out.diag.assign(diag, diag + mask);
    if (h.flags & flag_geoable) {
        const std::uint32_t chunks = header_chunk_count(h);
        out.geo_chunks.assign(chunk_mask_bytes(chunks), 0);
        if (h.flags & flag_chunk_geo_mask) {
            const std::uint8_t* geo = raw.data() + blob_geo_mask_offset(nx, nz, mul);
            std::memcpy(out.geo_chunks.data(), geo, out.geo_chunks.size());
            clear_chunk_mask_padding(out.geo_chunks.data(), chunks);
        }
        else {
            fill_chunk_mask(out.geo_chunks.data(), chunks);
        }
    }
    const std::uint8_t* cov = raw.data() + blob_overlay_offset(nx, nz, mul, h.chunk_cells, h.flags);
    out.overlay_coverage.assign(cov, cov + blob_overlay_bytes(nx, nz, mul, h.flags));
    clear_unused_overlay_channels(out.overlay_coverage.data(), out.overlay_coverage.size(), h.overlay_count);
    const std::uint8_t* planes = raw.data() + blob_decoration_offset(nx, nz, mul, h.chunk_cells, h.flags);
    out.decoration_coverage.assign(planes, planes + blob_decoration_bytes(nx, nz, mul, h.flags, h.decoration_count));
    return nullptr;
}

} // namespace alpine_terrain
