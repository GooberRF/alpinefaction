#include <xlog/xlog.h>
#include <patch_common/CodeInjection.h>
#include "../rf/multi.h"
#include "../rf/entity.h"
#include "../rf/gr/gr.h"
#include "../rf/os/frametime.h"
#include "../rf/vmesh.h"
#include "gr_internal.h"
#include "../misc/alpine_settings.h"

bool is_find_static_lights = false;
std::vector<ExplosionFlashLight> g_active_explosion_flash_lights;
std::vector<ExplosionFlashVclipEntry> g_explosion_flash_vclip_entries_weapon;
std::vector<ExplosionFlashVclipEntry> g_explosion_flash_vclip_entries_env;
float g_explosion_flash_flicker_phase = 0.0f;
float g_fire_light_flicker_phase = 0.0f;

void explosion_flash_lights_init()
{
    if (!is_d3d11()) {
        return;
    }

    g_explosion_flash_vclip_entries_weapon.clear();
    for (const auto& vclip_spec : k_explosion_flash_vclip_specs_weapon) {
        const int vclip_id = rf::vclip_lookup(vclip_spec.name);
        if (vclip_id >= 0) {
            g_explosion_flash_vclip_entries_weapon.push_back({
                vclip_id,
                vclip_spec.radius_scale,
                vclip_spec.intensity,
                vclip_spec.red,
                vclip_spec.green,
                vclip_spec.blue,
                vclip_spec.duration_ms
            });
        }
    }

    g_explosion_flash_vclip_entries_env.clear();
    for (const auto& vclip_spec : k_explosion_flash_vclip_specs_env) {
        const int vclip_id = rf::vclip_lookup(vclip_spec.name);
        if (vclip_id >= 0) {
            g_explosion_flash_vclip_entries_env.push_back({
                vclip_id,
                vclip_spec.radius_scale,
                vclip_spec.intensity,
                vclip_spec.red,
                vclip_spec.green,
                vclip_spec.blue,
                vclip_spec.duration_ms
            });
        }
    }
}

void explosion_flash_lights_level_init()
{
    g_active_explosion_flash_lights.clear();
}

bool explosion_flash_vclip_lookup(bool from_weapon, int vclip_id, float* radius_scale, float* intensity, float* r, float* g, float* b, int* duration_ms)
{
    const auto& entries = from_weapon
        ? g_explosion_flash_vclip_entries_weapon
        : g_explosion_flash_vclip_entries_env;

    const auto it = std::find_if(entries.begin(), entries.end(),
        [vclip_id](const ExplosionFlashVclipEntry& entry) {
            return entry.vclip_id == vclip_id;
        });

    if (it == entries.end()) {
        return false;
    }

    const ExplosionFlashVclipEntry& e = *it;
    if (radius_scale)
        *radius_scale = e.radius_scale;
    if (intensity)
        *intensity = e.intensity;
    if (r)
        *r = e.red;
    if (g)
        *g = e.green;
    if (b)
        *b = e.blue;
    if (duration_ms)
        *duration_ms = e.duration_ms;

    return true;
}

bool vclip_should_do_explosion_flash(bool from_weapon, int vclip_id, float* radius_scale, float* intensity, float* r, float* g, float* b, int* duration_ms)
{
    return explosion_flash_vclip_lookup(from_weapon, vclip_id, radius_scale, intensity, r, g, b, duration_ms);
}

float explosion_flash_light_intensity(const ExplosionFlashLight& light)
{
    if (light.duration_ms <= 0) {
        return 0.0f;
    }

    const float progress = std::clamp(light.elapsed_ms / static_cast<float>(light.duration_ms), 0.0f, 1.0f);
    if (light.fade_only) {
        float phase = g_explosion_flash_flicker_phase / 0.15f;
        float flicker = 1.0f + 0.05f * std::sin(2.0f * 3.14159265358979323846f * phase);
        float fade = light.max_intensity * (1.0f - progress);
        return fade * flicker;
    }

    constexpr float ramp_up_portion = 0.2f;
    if (progress < ramp_up_portion) {
        const float ramp = progress / ramp_up_portion;
        return light.max_intensity * (0.3f + 0.7f * ramp);
    }

    const float fade = (progress - ramp_up_portion) / (1.0f - ramp_up_portion);
    return light.max_intensity * (1.0f - fade);
}

void explosion_flash_light_create(const rf::Vector3& pos, float radius, float intensity, float r, float g, float b, int duration_ms, bool fade_only)
{
    g_active_explosion_flash_lights.emplace_back(ExplosionFlashLight{
        .pos = pos,
        .radius = radius,
        .max_intensity = intensity,
        .red = r,
        .green = g,
        .blue = b,
        .duration_ms = duration_ms,
        .fade_only = fade_only,
    });
}

void explosion_flash_light_create_offset(const rf::Vector3& pos, const rf::Vector3& dir, float offset_meters, float radius,
    float intensity, float r, float g, float b, int duration_ms)
{
    rf::Vector3 offset_pos = pos;
    if (offset_meters != 0.0f) {
        rf::Vector3 normalized_dir = dir;
        const float len = normalized_dir.len();
        if (len > 0.0001f) {
            normalized_dir /= len;
            offset_pos += normalized_dir * offset_meters;
        }
    }

    explosion_flash_light_create(offset_pos, radius, intensity, r, g, b, duration_ms, false);
}

void explosion_flash_lights_do_frame()
{
    if (!is_d3d11() || g_active_explosion_flash_lights.empty()) {
        return;
    }

    const float delta_ms = rf::frametime * 1000.0f;
    g_explosion_flash_flicker_phase += rf::frametime;
    if (g_explosion_flash_flicker_phase > 0.15f) {
        g_explosion_flash_flicker_phase -= 0.15f;
    }
    for (auto& light : g_active_explosion_flash_lights) {
        if (light.light_handle > -1) {
            rf::gr::light_delete(light.light_handle, 0);
            light.light_handle = -1;
        }

        const float intensity = explosion_flash_light_intensity(light);
        if (intensity > 0.0f) {
            light.light_handle = rf::gr::light_create_point(
                &light.pos,
                light.radius,
                intensity,
                light.red,
                light.green,
                light.blue,
                true,
                light.shadow_condition,
                light.attenuation_algorithm
            );
        }

        light.elapsed_ms += delta_ms;
    }

    g_active_explosion_flash_lights.erase(
        std::remove_if(
            g_active_explosion_flash_lights.begin(),
            g_active_explosion_flash_lights.end(),
            [](ExplosionFlashLight& light) {
                if (light.elapsed_ms < light.duration_ms) {
                    return false;
                }
                if (light.light_handle > -1) {
                    rf::gr::light_delete(light.light_handle, 0);
                    light.light_handle = -1;
                }
                return true;
            }),
        g_active_explosion_flash_lights.end());
}

CodeInjection entity_process_post_fire_injection{
    0x0041EBC1,
    [](auto& regs) {
        if (!is_d3d11() || !g_alpine_game_config.burning_entity_lights || rf::is_multi) {
            return;
        }
        rf::Entity* ep = regs.esi;
        const bool on_fire = ep->entity_fire_handle != nullptr;

        if (ep->powerup_light_handle != -1) {
            rf::gr::light_delete(ep->powerup_light_handle, 0);
            ep->powerup_light_handle = -1;
        }

        if (on_fire) {
            g_fire_light_flicker_phase += rf::frametime;
            if (g_fire_light_flicker_phase > 0.15f) {
                g_fire_light_flicker_phase -= 0.15f;
            }

            float phase = g_fire_light_flicker_phase / 0.15f;
            float flicker = 1.0f + 0.05f * std::sin(2.0f * 3.14159265358979323846f * phase);
            float radius = 8.0f * (0.95f + 0.1f * flicker);

            // attach a flickering light to the burning entity
            // this light will be removed when the entity transitions to corpse
            ep->powerup_light_handle = rf::gr::light_create_point(
                &ep->pos,
                radius,
                flicker,
                0.95f,
                0.72f,
                0.16f,
                true,
                rf::gr::LightShadowcastCondition::SHADOWCAST_EDITOR,
                0);
            return;
        }
    },
};

CodeInjection entity_fire_on_dead_light_injection{
    0x0041937A,
    [](auto& regs) {
        if (!is_d3d11() || !g_alpine_game_config.burning_entity_lights || rf::is_multi) {
            return;
        }
        rf::EntityFireInfo* ef = regs.eax;
        if (!ef) {
            return;
        }
        rf::Object* obj = rf::obj_from_handle(ef->parent_hobj);
        if (!obj) {
            return;
        }

        // spawn a throwaway light at the position of the flaming corpse and fade it out over time as the flames dissipate
        explosion_flash_light_create(
            obj->pos,
            8.0f,   // flickers in explosion_flash_lights_do_frame
            1.0f,   // flickers in explosion_flash_lights_do_frame
            0.95f,
            0.72f,
            0.16f,
            12500,  // 12.5 seconds is roughly the duration of the fire on the corpse
            true);
    },
};

void gr_light_use_static(bool use_static)
{
    // Enable some experimental flag that causes static lights to be included in computations
    //auto& experimental_alloc_and_lighting = addr_as_ref<bool>(0x00879AF8);
    auto& gr_light_state = addr_as_ref<int>(0x00C96874);
    //experimental_alloc_and_lighting = use_static;
    is_find_static_lights = use_static;
    // Increment light cache key to trigger cache invalidation
    gr_light_state++;
}

void gr_light_apply_patch()
{
    // Change the variable that is used to determine what light list is searched (by default is_editor
    // variable determines that). It is needed for static mesh lighting.
    write_mem_ptr(0x004D96CD + 1, &is_find_static_lights); // gr_light_find_all_in_room
    write_mem_ptr(0x004D9761 + 1, &is_find_static_lights); // gr_light_find_all_in_room
    write_mem_ptr(0x004D98B3 + 1, &is_find_static_lights); // gr_light_find_all_by_gsolid
    
    // Add dynamic fire glow around burning entities
    entity_process_post_fire_injection.install();
    entity_fire_on_dead_light_injection.install();
}
