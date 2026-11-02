#include <algorithm>
#include <cstdint>
#include <iterator>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <xlog/xlog.h>
#include "../bmpman/atx.h"
#include "../bmpman/bmpman.h"
#include "../misc/alpine_settings.h"
#include "../misc/player.h"
#include "../os/console.h"
#include "../rf/bmpman.h"
#include "../rf/physics.h"
#include "../rf/player/player_fpgun.h"
#include "gr.h"
#include "detail_texture.h"

namespace
{
    // Automatic detail (r_detailtextures 2) for textures no ATX gives one
    constexpr const char* auto_metal_texture = "metal_detailmap01.tga";
    constexpr const char* auto_other_texture = "ground_detailmap01.tga";
    constexpr float auto_texels_per_repeat = 32.0f;
    constexpr float auto_strength = 1.0f;

    // Base bitmap handle and fallback material (detail_key) -> its detail, nullopt for none
    std::unordered_map<uint64_t, std::optional<DetailTexture>> g_detail_by_bm;
    // Detail bitmap handle -> 1 / its mean luminance, 0 when it cannot be used
    std::unordered_map<int, float> g_inv_mean_by_bm;
    std::unordered_set<int> g_fpgun_arm_bms;
    bool g_fpgun_arm_bms_known = false;

    uint64_t detail_key(int bm, int fallback_material)
    {
        return (static_cast<uint64_t>(static_cast<uint32_t>(bm)) << 32) | static_cast<uint32_t>(fallback_material);
    }

    float detail_inv_mean(int bm)
    {
        auto [it, inserted] = g_inv_mean_by_bm.try_emplace(bm, 0.0f);
        if (!inserted) {
            return it->second;
        }
        int w = 0, h = 0, num_pixels = 0, levels = 0;
        rf::bm::get_mipmap_info(bm, &w, &h, &num_pixels, &levels);
        std::vector<uint8_t> pixels;
        if (w <= 0 || h <= 0 || !bm_read_8888(bm, w, h, pixels)) {
            xlog::warn("[DetailTexture] Detail texture {} cannot be read", rf::bm::get_filename(bm));
            return 0.0f;
        }
        double sum = 0.0;
        for (size_t i = 0; i < pixels.size(); i += 4) {
            sum += 0.114 * pixels[i] + 0.587 * pixels[i + 1] + 0.299 * pixels[i + 2];
        }
        const double mean = sum / (255.0 * w * h);
        // Divided by a mean near 0, its few bright texels would blow out
        if (mean < 1.0 / 64.0) {
            xlog::warn("[DetailTexture] Detail texture {} is too dark", rf::bm::get_filename(bm));
            return 0.0f;
        }
        it->second = static_cast<float>(1.0 / mean);
        return it->second;
    }

    // `cacheable` is cleared when the answer can still change this level
    std::optional<DetailTexture> resolve(int bm, int fallback_material, bool& cacheable)
    {
        // User bitmaps are written at run time: render targets, monitors, fonts
        if (rf::bm::get_type(bm) == rf::bm::TYPE_USER) {
            return std::nullopt;
        }
        const int slot = rf::bm::handle_to_index(bm);
        if (!rf::bm::bitmaps || slot < 0 || slot >= rf::bm::num_cache_slots) {
            return std::nullopt;
        }
        const rf::bm::BitmapEntry& entry = rf::bm::bitmaps[slot];
        if (entry.handle != bm) {
            return std::nullopt;
        }

        DetailTexture detail{};
        const bool is_atx = entry.bm_type == rf::bm::TYPE_ATX;
        // An ATX entry kept across a level change gets its controller back on its next lock
        if (is_atx && !atx_has_controller(atx_canonical_handle(entry.name))) {
            cacheable = false;
        }
        const AtxSpec::Detail* atx = is_atx ? atx_detail_texture(entry) : nullptr;
        if (atx) {
            if (!atx->enabled || atx->strength <= 0.0f) {
                return std::nullopt;
            }
            int w = 0, h = 0;
            rf::bm::get_dimensions(bm, &w, &h);
            detail.bm = rf::bm::load(atx->texture.c_str(), -1, true);
            detail.texels_per_repeat = static_cast<float>(std::max(w, 1)) / atx->scale;
            detail.strength = atx->strength;
        }
        else if (g_alpine_game_config.detail_textures == AlpineGameSettings::detail_textures_auto
                 && fallback_material != detail_no_auto) {
            int material = rf::bm_get_surface_material(bm);
            if (material == rf::MATERIAL_DEFAULT) {
                material = fallback_material;
            }
            if (material == rf::MATERIAL_WATER || material == rf::MATERIAL_LAVA) {
                return std::nullopt;
            }
            const bool metal = material == rf::MATERIAL_METAL;
            detail.bm = rf::bm::load(metal ? auto_metal_texture : auto_other_texture, -1, true);
            detail.texels_per_repeat = auto_texels_per_repeat;
            detail.strength = auto_strength;
        }
        else {
            return std::nullopt;
        }
        // Placeholders and render targets are user bitmaps; an ATX could show a live feed
        const rf::bm::Type detail_type = detail.bm < 0 ? rf::bm::TYPE_NONE : rf::bm::get_type(detail.bm);
        if (detail_type == rf::bm::TYPE_NONE || detail_type == rf::bm::TYPE_USER || detail_type == rf::bm::TYPE_ATX) {
            return std::nullopt;
        }
        detail.inv_mean = detail_inv_mean(detail.bm);
        if (detail.inv_mean <= 0.0f) {
            return std::nullopt;
        }
        return detail;
    }

    ConsoleCommand2 detail_textures_cmd{
        "r_detailtextures",
        [](std::optional<int> mode) {
            if (mode) {
                g_alpine_game_config.set_detail_textures(mode.value());
                detail_texture_level_reset();
            }
            static constexpr const char* mode_names[] = {"off", "ATX only", "ATX and automatic"};
            rf::console::print("Detail textures: {} (Direct3D 11 renderer only)",
                               mode_names[g_alpine_game_config.detail_textures]);
        },
        "Sets detail textures: 0 off, 1 only those an ATX file names, 2 also automatic ones by surface material "
        "(Direct3D 11 renderer only)",
        "r_detailtextures [0|1|2]",
    };
}

std::optional<DetailTexture> detail_texture_find(int bm_handle, int fallback_material)
{
    if (g_alpine_game_config.detail_textures == AlpineGameSettings::detail_textures_off || bm_handle < 0) {
        return std::nullopt;
    }
    // A live feed shows a camera, not the texture
    if (atx_lookup_live_feed(bm_handle) >= 0) {
        return std::nullopt;
    }
    const uint64_t key = detail_key(bm_handle, fallback_material);
    if (auto it = g_detail_by_bm.find(key); it != g_detail_by_bm.end()) {
        return it->second;
    }
    bool cacheable = true;
    std::optional<DetailTexture> detail = resolve(bm_handle, fallback_material, cacheable);
    if (cacheable) {
        g_detail_by_bm.emplace(key, detail);
    }
    return detail;
}

int detail_texture_first_person_material(int bm_handle)
{
    if (!g_fpgun_arm_bms_known) {
        g_fpgun_arm_bms_known = true;
        const int count = std::clamp(rf::num_fpgun_arms, 0, static_cast<int>(std::size(rf::fpgun_arms)));
        for (int i = 0; i < count; i++) {
            g_fpgun_arm_bms.insert(std::begin(rf::fpgun_arms[i].arm_bitmaps), std::end(rf::fpgun_arms[i].arm_bitmaps));
        }
        player_fpgun_collect_override_arm_bitmaps(g_fpgun_arm_bms);
    }
    return g_fpgun_arm_bms.contains(bm_handle) ? rf::MATERIAL_FLESH : rf::MATERIAL_METAL;
}

void detail_texture_level_reset()
{
    g_detail_by_bm.clear();
    g_inv_mean_by_bm.clear();
    g_fpgun_arm_bms.clear();
    g_fpgun_arm_bms_known = false;
    gr_invalidate_texture_cache();
}

void detail_texture_register_commands()
{
    detail_textures_cmd.register_cmd();
}
