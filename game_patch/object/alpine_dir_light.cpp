#include <string>
#include <vector>
#include <xlog/xlog.h>
#include "../misc/level.h"
#include "alpine_dir_light.h"

namespace adl = alpine_dir_light;

namespace
{
std::vector<AlpineDirLight> g_dir_lights;
uint32_t g_dir_light_generation = 0;
} // namespace

void alpine_dir_light_load_chunk(rf::File& file, std::size_t chunk_len, [[maybe_unused]] int content_version)
{
    std::size_t remaining = chunk_len;

    rf::File::ChunkGuard chunk_guard{file, remaining};

    AlpineChunkReader reader{file, remaining};

    uint32_t count = 0;
    if (!reader.read(count)) {
        xlog::warn("[AlpineDirLight] Failed to read count from chunk (len={})", chunk_len);
        return;
    }
    const uint32_t capacity = adl::max_lights - static_cast<uint32_t>(g_dir_lights.size());
    if (count > capacity) {
        xlog::warn("[AlpineDirLight] Chunk declares {} directional lights, reading the first {}", count, capacity);
        count = capacity;
    }

    g_dir_light_generation++;
    int corrected = 0;
    for (uint32_t i = 0; i < count; i++) {
        adl::Record rec;
        std::string script_name;
        if (!adl::read_record(reader, rec, script_name)) {
            xlog::warn("[AlpineDirLight] Chunk is truncated, keeping {} directional light(s)", g_dir_lights.size());
            break;
        }
        if (adl::sanitize_record(rec)) {
            corrected++;
        }

        AlpineDirLight& light = g_dir_lights.emplace_back();
        light.uid = rec.uid;
        light.color[0] = rec.color_r / 255.0f;
        light.color[1] = rec.color_g / 255.0f;
        light.color[2] = rec.color_b / 255.0f;
        light.intensity = rec.intensity;
        light.on = rec.initially_on != 0;
        light.affects_meshes = rec.affects_meshes != 0;
        light.mesh_mode = rec.mesh_mode;
        light.volume = adl::make_volume(rec);
        light.bounding_radius = adl::volume_bounding_radius(light.volume);
    }

    if (corrected) {
        xlog::warn("[AlpineDirLight] {} directional light(s) had out of range properties corrected", corrected);
    }
    xlog::info("[AlpineDirLight] Loaded {} directional light(s)", g_dir_lights.size());
}

void alpine_dir_light_clear_state()
{
    g_dir_lights.clear();
    g_dir_light_generation++;
}

const std::vector<AlpineDirLight>& alpine_dir_light_get_all()
{
    return g_dir_lights;
}

uint32_t alpine_dir_light_generation()
{
    return g_dir_light_generation;
}

bool alpine_dir_light_set_on(int uid, bool on)
{
    bool found = false;
    for (AlpineDirLight& light : g_dir_lights) {
        if (light.uid == uid) {
            light.on = on;
            found = true;
        }
    }
    if (found) {
        g_dir_light_generation++;
    }
    return found;
}

bool alpine_dir_light_set_color(int uid, float r, float g, float b)
{
    bool found = false;
    for (AlpineDirLight& light : g_dir_lights) {
        if (light.uid == uid) {
            light.color[0] = r;
            light.color[1] = g;
            light.color[2] = b;
            found = true;
        }
    }
    if (found) {
        g_dir_light_generation++;
    }
    return found;
}
