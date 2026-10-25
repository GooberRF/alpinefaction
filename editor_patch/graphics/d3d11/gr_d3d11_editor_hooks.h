#pragma once

#include <cstdint>
#include <source_location>
#include <string>
#include <vector>

namespace alpine_lightmap
{
    struct ReadResult;
}

struct EditorLevelStyle;

// Replaces RED's Direct3D 8 backend with the Direct3D 11 renderer. Installed at startup instead of the Direct3D 8
// patches, before RED's gr_init.
void editor_d3d11_apply_patch();

// The core's fatal errors (its fatal_error binding). A lost device closes RED as a failed Present does, and any other
// failure the same way, the level saved first; a failure while the renderer starts closes it with the Direct3D 9
// advice. headless_bake.h lists the exit codes.
[[noreturn]] void editor_d3d11_fatal_error(const std::string& text, const std::source_location& loc);

namespace gr::d3d11
{
    // The Alpine lightmap atlas, as the game's renderer holds it. Upload fails without a live renderer.
    bool upload_af_lightmap_atlas(const alpine_lightmap::ReadResult& section, const std::vector<std::uint8_t>& blocks);
    void release_af_lightmap_atlas();
    bool af_lightmap_atlas_live();

    // The editor_gr.h operations
    void flush();
    bool batch_pending();
    EditorLevelStyle level_style();
}
