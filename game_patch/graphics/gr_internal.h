#pragma once

#include <array>
#include "../rf/gr/gr_light.h"

struct ExplosionFlashVclipSpec
{
    const char* name = nullptr;
    float radius_scale = 0.0f;
    float intensity = 0.0f;
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    int duration_ms = 0;
};

struct ExplosionFlashVclipEntry
{
    int vclip_id = -1;
    float radius_scale = 0.0f;
    float intensity = 0.0f;
    float red = 0.0f;
    float green = 0.0f;
    float blue = 0.0f;
    int duration_ms = 0;
};

struct ExplosionFlashLight
{
    rf::Vector3 pos;
    float radius = 0.0f;
    float max_intensity = 0.0f;
    float red = 1.0f;
    float green = 1.0f;
    float blue = 1.0f;
    int duration_ms = 1000;
    float elapsed_ms = 0.0f;
    bool fade_only = false;
    rf::gr::LightShadowcastCondition shadow_condition = rf::gr::SHADOWCAST_EDITOR;
    int attenuation_algorithm = 0;
    int light_handle = -1;
};

constexpr std::array<ExplosionFlashVclipSpec, 9> k_explosion_flash_vclip_specs_weapon = {
    ExplosionFlashVclipSpec{"charge_explode", 4.0f, 1.5f, 0.95f, 0.72f, 0.16f, 450},            // remote charge
    ExplosionFlashVclipSpec{"shoulder_cannon_explode", 2.8f, 1.5f, 0.95f, 0.72f, 0.16f, 1500},  // fusion
    ExplosionFlashVclipSpec{"rocket_impact", 5.3f, 1.5f, 0.95f, 0.72f, 0.16f, 450},             // rocket launcher and grenade
    ExplosionFlashVclipSpec{"flame_can_explode", 1.5f, 1.5f, 0.95f, 0.8f, 0.09f, 3650},         // flamethrower
    ExplosionFlashVclipSpec{"tribeam hit", 2.5f, 1.5f, 0.05f, 1.0f, 0.1f, 800},                 // spike tribeam laser
    ExplosionFlashVclipSpec{"Laser Hit", 3.0f, 1.5f, 0.95f, 0.07f, 0.07f, 500},                 // red laser hit
    ExplosionFlashVclipSpec{"NanoAttackHit", 4.0f, 1.5f, 0.65f, 0.97f, 0.98f, 650},             // capek cane hit
    ExplosionFlashVclipSpec{"big_charge_explode", 2.25f, 1.5f, 0.95f, 0.72f, 0.16f, 450},       // drone and tankbot rockets
    ExplosionFlashVclipSpec{"rail_rifle_hit", 4.0f, 1.5f, 0.65f, 0.97f, 0.98f, 500},            // rail driver hit
};

constexpr std::array<ExplosionFlashVclipSpec, 10> k_explosion_flash_vclip_specs_env = {
    ExplosionFlashVclipSpec{"yellboom", 10.0f, 1.5f, 0.95f, 0.72f, 0.16f, 175},                 // most lights
    ExplosionFlashVclipSpec{"yellboom_metal", 9.0f, 1.5f, 0.8f, 0.87f, 0.97f, 200},             // some lights, some medical equipment, small robots
    ExplosionFlashVclipSpec{"objectboom", 3.5f, 1.5f, 0.8f, 0.87f, 0.97f, 200},                 // generators, computers, cable spool
    ExplosionFlashVclipSpec{"oil drum explode", 4.0f, 1.5f, 0.95f, 0.72f, 0.16f, 250},          // oil drums
    ExplosionFlashVclipSpec{"screenboom", 5.0f, 1.5f, 0.65f, 0.97f, 0.98f, 150},                // screens and mirrors
    ExplosionFlashVclipSpec{"objectboom3", 3.0f, 1.5f, 0.95f, 0.72f, 0.16f, 250},               // welding equip
    ExplosionFlashVclipSpec{"objectboom2", 4.5f, 1.5f, 0.95f, 0.72f, 0.16f, 250},               // fuel tank
    ExplosionFlashVclipSpec{"explosion 1", 4.0f, 1.5f, 0.95f, 0.72f, 0.16f, 250},               // jeep, APC, fighter, driller, some robots
    ExplosionFlashVclipSpec{"sub_explode", 1.25f, 1.5f, 0.65f, 0.97f, 0.98f, 250},              // sub
    ExplosionFlashVclipSpec{"drone explode", 2.0f, 1.5f, 0.95f, 0.72f, 0.16f, 250},             // drone, tankbot, masako_fighter
};

void explosion_flash_lights_init();
bool vclip_should_do_explosion_flash(bool from_weapon, int vclip_id, float* radius_scale, float* intensity, float* r, float* g, float* b, int* duration_ms);
void explosion_flash_light_create(
    const rf::Vector3& pos, float radius, float intensity, float r, float g, float b, int duration_ms, bool fade_only);
void explosion_flash_light_create_offset(
    const rf::Vector3& pos, const rf::Vector3& dir, float offset_meters, float radius, float intensity, float r, float g, float b, int duration_ms);
void explosion_flash_lights_do_frame();
void gr_font_apply_patch();
void gr_light_apply_patch();
void bink_apply_patch();
