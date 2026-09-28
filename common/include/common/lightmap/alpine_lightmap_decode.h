#pragma once

// Alpine Lightmaps — the layer payload decoders both readers link: zlib (the layer container) and
// bc7decomp (vendor/bc7enc_rdo). Kept out of alpine_lightmap_reader.h so that header needs neither.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include <zlib.h>
#include <bc7decomp.h>

#include "alpine_lightmap_reader.h"

namespace alpine_lightmap
{

// A Bc7BlockDecoder: one 16 byte BC7 block to 16 RGBA8 texels, row major.
inline bool unpack_bc7_rgba(const void* block, std::uint8_t* rgba)
{
    return bc7decomp::unpack_bc7(block, reinterpret_cast<bc7decomp::color_rgba*>(rgba));
}

// Fills `out` with `layer` of the section body `section` (section_len bytes, as read_section accepted it):
// uncompressed_size bytes, inflated when zlib compressed, else copied. False, with `out` empty, when the
// payload does not inflate to exactly that size. zlib_result, when given, receives uncompress's result:
// Z_OK for a copy and for an inflate of the wrong size, Z_DATA_ERROR for a payload past the section.
inline bool decompress_layer(const std::uint8_t* section, std::size_t section_len, const LayerRef& layer,
                             std::vector<std::uint8_t>& out, int* zlib_result = nullptr)
{
    const bool zlib = layer.compression == static_cast<std::uint8_t>(Compression::zlib);
    const std::uint64_t stored = zlib ? layer.stored_size : layer.uncompressed_size;
    bool ok = section && layer.payload_offset <= section_len && stored <= section_len - layer.payload_offset;
    int zr = ok ? Z_OK : Z_DATA_ERROR;
    if (ok) {
        out.assign(layer.uncompressed_size, 0);
        const std::uint8_t* payload = section + layer.payload_offset;
        if (zlib) {
            uLongf out_len = static_cast<uLongf>(out.size());
            zr = uncompress(out.data(), &out_len, payload, static_cast<uLong>(layer.stored_size));
            ok = zr == Z_OK && out_len == out.size();
        }
        else {
            std::memcpy(out.data(), payload, out.size());
        }
    }
    if (zlib_result) {
        *zlib_result = zr;
    }
    if (!ok) {
        out.clear();
    }
    return ok;
}

} // namespace alpine_lightmap
