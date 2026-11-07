#include "../misc.h"
#include "../../main/main.h"
#include "../../graphics/gr.h"
#include "../../graphics/detail_texture.h"
#include "../../graphics/d3d11/gr_d3d11_hooks.h"
#include "../../object/object.h"
#include "../../os/os.h"
#include "../../rf/character.h"
#include "../../rf/gr/gr.h"
#include "../../rf/level.h"
#include "../../rf/ui.h"
#include "settings_rows.h"

namespace
{

void apply_graphics_commit()
{
    if (rf::local_player) {
        rf::player_settings_apply_graphics_options(rf::local_player);
    }
}

void apply_picmip()
{
    // Mips are generated at upload only while picmip > 1, so crossing that gate leaves cached
    // textures in the wrong state; evict them to re-upload on next use. User bitmaps (fonts, HUD
    // atlases, render targets) are kept: their pixels live only in the D3D11 texture.
    static bool mips_generated = false;
    const bool generate_mips = g_alpine_game_config.picmip > 1;
    if (generate_mips != mips_generated && is_d3d11()) {
        gr::d3d11::texture_flush_non_user_cache();
    }
    mips_generated = generate_mips;
    gr_update_texture_filtering();
}

void apply_mesh_lighting()
{
    gr::d3d11::evaluate_mesh_lighting(rf::level.filename);
    recalc_mesh_static_lighting();
}

void apply_pixel_light_overbright()
{
    gr::d3d11::evaluate_pixel_light_overbright(rf::level.filename);
}

struct DetailPreset
{
    int character_detail;
    int texture_detail;
    bool dynamic_light;
    bool decals;
    bool shadows;
    bool bilinear;
};

// Mirrors the preset table in options_video_details_level_change 0x00451390, which reads the stock
// Video panel's gadget globals rather than PlayerSettings.
constexpr DetailPreset detail_presets[] = {
    {0, 0, true, true, true, false},   // High
    {1, 1, true, true, true, true},    // Medium
    {1, 2, false, false, false, true}, // Low
};
constexpr int detail_preset_custom = static_cast<int>(std::size(detail_presets));

void apply_detail_preset()
{
    rf::Player* const player = rf::local_player;
    if (!player) {
        return;
    }
    auto& settings = player->settings;
    const int level = settings.detail_level;
    if (level >= 0 && level < detail_preset_custom) {
        const DetailPreset& p = detail_presets[level];
        settings.character_detail_level = p.character_detail;
        settings.textures_resolution_level = p.texture_detail;
        settings.dynamic_lightining_enabled = p.dynamic_light;
        settings.decals_enabled = p.decals;
        settings.shadows_enabled = p.shadows;
        settings.bilinear_filtering = p.bilinear;
    }
    // Custom keeps whatever the individual rows hold.
    rf::player_settings_apply_graphics_options(player);
}

// A member that no longer matches its preset turns the preset to Custom, or the next preset write
// would undo the edit. As an apply hook it runs on every write path.
void apply_detail_preset_member()
{
    if (rf::Player* const player = rf::local_player) {
        auto& settings = player->settings;
        const int level = settings.detail_level;
        if (level >= 0 && level < detail_preset_custom) {
            const DetailPreset& p = detail_presets[level];
            if (settings.character_detail_level != p.character_detail
                || settings.textures_resolution_level != p.texture_detail
                || settings.dynamic_lightining_enabled != p.dynamic_light || settings.decals_enabled != p.decals
                || settings.shadows_enabled != p.shadows || settings.bilinear_filtering != p.bilinear) {
                settings.detail_level = detail_preset_custom;
            }
        }
    }
    apply_graphics_commit();
}

const char* const labels_detail_preset[] = {"High", "Medium", "Low", "Custom"};
static_assert(std::size(labels_detail_preset) == detail_preset_custom + 1);
const char* const labels_high_med_low[] = {"High", "Medium", "Low"};
const char* const labels_mip_filter[] = {"Trilinear", "Bilinear"};
const char* const labels_mesh_lighting[] = {"Ambient", "Vertex", "Pixel"};
const char* const labels_shadow_distance[] = {"Lowest", "Low", "Medium", "High", "Very high", "Maximum"};
const char* const labels_shadow_quality[] = {"Blob", "Low", "Medium", "High", "Very high", "Maximum"};
const char* const labels_underwater_fx[] = {"Off", "Caustics", "Caustics + fog", "Full"};
const char* const labels_detail_textures[] = {"Off", "ATX only", "ATX and automatic"};

bool sample_count_offered(const int value)
{
    return gr_supports_sample_count(static_cast<uint32_t>(value));
}

const char* const labels_msaa[] = {"Off", "MSAA 2x", "MSAA 4x", "MSAA 8x"};
const int values_msaa[] = {1, 2, 4, 8};
const char* const labels_colorblind[] = {"Off", "Protanopia", "Deuteranopia", "Tritanopia"};

const SettingRow rows[] = {
    {
        .id = "vsync",
        .key = "VerticalSync",
        .label = "Vertical sync",
        .tooltip = "Synchronize frames to the monitor refresh rate.",
        .category = Cat::Graphics,
        .subcategory = "Display",
        .widget = Wid::Toggle,
        .access = AccCoreBool{&ASC::vsync},
        .persistence = Pers::Afcc,
        .apply = gr_d3d_update_vsync,
    },
    {
        .id = "max_fps",
        .key = "MaxFPS",
        .label = "Max FPS",
        .tooltip = "Frame-rate cap (servers may limit this to 240).",
        .category = Cat::Graphics,
        .subcategory = "Display",
        .widget = Wid::Stepper,
        .access = AccUint{&AGS::max_fps, &AGS::set_max_fps, AGS::min_fps_limit, AGS::max_fps_limit},
        .flags = SF_NOT_DEDICATED | SF_APPLY_ON_LOAD,
        .apply = apply_maximum_fps,
        .console_cmd = "maxfps",
    },
    {
        .id = "reduced_speed_in_background",
        .key = "ReducedSpeedInBackground",
        .label = "Slow down when unfocused",
        .tooltip = "Reduce the frame rate while the game window is in the background.",
        .category = Cat::Graphics,
        .subcategory = "Display",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::reduced_speed_in_background},
        .flags = SF_APPLY_ON_LOAD | SF_ADVANCED,
        .apply = apply_reduced_speed_in_background,
    },
    {
        .id = "sample_count",
        .key = "SampleCount",
        .label = "Anti-aliasing",
        .tooltip = "Multisample anti-aliasing level.",
        .category = Cat::Graphics,
        .subcategory = "Display",
        .widget = Wid::Enum,
        .access = AccUint{&AGS::sample_count},
        .flags = SF_NOT_DEDICATED,
        .apply = gr_flush_frame_buffers,
        ENUM_LABELS(labels_msaa),
        .enum_values = values_msaa,
        .enum_offered = sample_count_offered,
        .console_cmd = "r_antialiasing_mode",
    },
    {
        .id = "gamma",
        .key = "Gamma",
        .label = "Gamma",
        .tooltip = "Overall screen brightness.",
        .category = Cat::Graphics,
        .subcategory = "Display",
        .widget = Wid::Slider,
        .access = AccFnFloat{[](rf::Player*) { return rf::gr::gamma; },
            [](rf::Player*, float v) {
                rf::gr::set_gamma(v);
                // The stock Video panel reads gamma into its slider only when the panel is built.
                if (legacy_menus_enabled()) {
                    rf::ui::video_gamma_slider.set_value(v);
                }
            }, 0.7f, 2.2f},
        .console_cmd = "r_gamma",
        .default_str = "1",
        .ui_min = 0.7f,
        .ui_max = 2.2f,
        .ui_step = 0.05f,
        .ui_decimals = 2,
    },
    {
        .id = "colorblind_mode",
        .key = "ColorblindMode",
        .label = "Color-blind filter",
        .tooltip = "Apply a color-vision compensation filter.",
        .category = Cat::Graphics,
        .subcategory = "Display",
        .widget = Wid::Enum,
        .access = AccInt{&AGS::colorblind_mode, &AGS::set_colorblind_mode,
            AGS::min_colorblind_mode, AGS::max_colorblind_mode},
        .flags = SF_D3D11_ONLY,
        ENUM_LABELS(labels_colorblind),
        .console_cmd = "r_colorblind",
    },
    {
        .id = "low_frame_latency",
        .key = "D3D11_LowFrameLatency",
        .label = "Low-latency swap chain",
        .tooltip = "Experimental: waitable swap chain for lower input latency (restart required).",
        .category = Cat::Graphics,
        .subcategory = "Display",
        .widget = Wid::Toggle,
        .access = AccCoreBool{&ASC::low_frame_latency},
        .flags = SF_D3D11_ONLY | SF_HIDDEN | SF_RESTART_REQUIRED,
        .persistence = Pers::Afcc,
    },
    {
        .id = "allow_tearing",
        .key = "D3D11_AllowTearing",
        .label = "Allow tearing",
        .tooltip = "Experimental: allow tearing when vsync is off (restart required).",
        .category = Cat::Graphics,
        .subcategory = "Display",
        .widget = Wid::Toggle,
        .access = AccCoreBool{&ASC::allow_tearing},
        .flags = SF_D3D11_ONLY | SF_HIDDEN | SF_RESTART_REQUIRED,
        .persistence = Pers::Afcc,
    },
    {
        .id = "skip_gamma_pass",
        .key = "D3D11_SkipGammaPass",
        .label = "Skip gamma pass",
        .tooltip = "Experimental: bypass the gamma-correction pass (restart required).",
        .category = Cat::Graphics,
        .subcategory = "Display",
        .widget = Wid::Toggle,
        .access = AccCoreBool{&ASC::skip_gamma_pass},
        .flags = SF_D3D11_ONLY | SF_HIDDEN | SF_RESTART_REQUIRED,
        .persistence = Pers::Afcc,
    },
    {
        .id = "rendering_enabled",
        .key = "EnableRendering",
        .label = "Rendering enabled",
        .tooltip = "Debug: disable all rendering.",
        .category = Cat::Graphics,
        .subcategory = "Debug",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::rendering_enabled},
        .flags = SF_HIDDEN,
        .console_cmd = "dbg_togglerendering",
    },
    {
        .id = "detail_level",
        .key = "DetailLevel",
        .label = "Detail preset",
        .tooltip = "Quality preset; it shows Custom once the options below no longer match it.",
        .category = Cat::Graphics,
        .subcategory = "Quality",
        .widget = Wid::Enum,
        .access = PS_INT(detail_level, 0, 0, detail_preset_custom),
        .flags = SF_HIDDEN,
        .apply = apply_detail_preset,
        ENUM_LABELS(labels_detail_preset),
        .default_str = "0",
    },
    {
        .id = "anisotropic_filtering",
        .key = "AnisotropicFiltering",
        .label = "Anisotropic filtering",
        .tooltip = "Sharper textures at oblique viewing angles.",
        .category = Cat::Graphics,
        .subcategory = "Quality",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::anisotropic_filtering},
        .flags = SF_APPLY_ON_LOAD,
        .apply = gr_update_texture_filtering,
    },
    {
        .id = "shadows_enabled",
        .key = "ShowShadows",
        .label = "Shadows",
        .tooltip = "Draw character shadows.",
        .category = Cat::Graphics,
        .subcategory = "Shadows",
        .widget = Wid::Toggle,
        .access = PS_BOOL(shadows_enabled, true),
        .apply = apply_detail_preset_member,
        .default_str = "1",
    },
    {
        .id = "decals_enabled",
        .key = "ShowDecals",
        .label = "Decals",
        .tooltip = "Show bullet holes, scorch marks and other decals.",
        .category = Cat::Graphics,
        .subcategory = "Effects",
        .widget = Wid::Toggle,
        .access = PS_BOOL(decals_enabled, true),
        .apply = apply_detail_preset_member,
        .default_str = "1",
    },
    {
        .id = "dynamic_lighting",
        .key = "ShowDynamicLights",
        .label = "Dynamic lighting",
        .tooltip = "Lights from weapons, explosions and effects.",
        .category = Cat::Graphics,
        .subcategory = "Lighting",
        .widget = Wid::Toggle,
        .access = PS_BOOL(dynamic_lightining_enabled, true),
        .apply = apply_detail_preset_member,
        .default_str = "1",
    },
    {
        .id = "texture_resolution",
        .key = "TextureDetailLevel",
        .label = "Texture resolution",
        .tooltip = "Resolution of all textures; lower settings use less video memory.",
        .category = Cat::Graphics,
        .subcategory = "Quality",
        .widget = Wid::Enum,
        .access = PS_INT(textures_resolution_level, 0, 0, 2),
        .apply = apply_detail_preset_member,
        ENUM_LABELS(labels_high_med_low),
        .default_str = "0",
    },
    {
        .id = "character_detail",
        .key = "CharacterDetailLevel",
        .label = "Character detail",
        .tooltip = "How detailed character models stay; lower settings simplify them sooner.",
        .category = Cat::Graphics,
        .subcategory = "Quality",
        .widget = Wid::Enum,
        .access = PS_INT(character_detail_level, 0, 0, 2),
        .apply = apply_detail_preset_member,
        ENUM_LABELS(labels_high_med_low),
        .default_str = "0",
    },
    {
        .id = "bilinear_filtering",
        .key = "BilinearFiltering",
        .label = "Mip filtering",
        .tooltip = "Texture mip filtering (legacy renderers only).",
        .category = Cat::Graphics,
        .subcategory = "Quality",
        .widget = Wid::Enum,
        .access = PS_INT(bilinear_filtering, 1, 0, 1),
        .flags = SF_LEGACY_RENDERER_ONLY | SF_HIDDEN,
        .apply = apply_detail_preset_member,
        ENUM_LABELS(labels_mip_filter),
        .default_str = "1",
    },
    {
        .id = "nearest_texture_filtering",
        .key = "NearestTextureFiltering",
        .label = "Nearest neighbor texture filtering",
        .tooltip = "Use unfiltered, pixelated textures.",
        .category = Cat::Graphics,
        .subcategory = "Quality",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::nearest_texture_filtering},
        .flags = SF_APPLY_ON_LOAD,
        .apply = gr_update_texture_filtering,
        .console_cmd = "r_nearest",
    },
    {
        .id = "picmip",
        .key = "Picmip",
        .label = "Texture downscale",
        .tooltip = "Divide texture resolution by this factor (1 = full).",
        .category = Cat::Graphics,
        .subcategory = "Expert",
        .widget = Wid::Stepper,
        .access = AccInt{&AGS::picmip, &AGS::set_picmip, AGS::min_picmip, AGS::max_picmip},
        .flags = SF_D3D11_ONLY | SF_APPLY_ON_LOAD | SF_ADVANCED,
        .apply = apply_picmip,
        .console_cmd = "r_picmip",
    },
    {
        .id = "detail_textures",
        .key = "DetailTextures",
        .label = "Detail textures",
        .tooltip = "Fine surface detail up close: only where an ATX file names one, or also automatic ones by "
                   "surface material.",
        .category = Cat::Graphics,
        .subcategory = "Quality",
        .widget = Wid::Enum,
        .access = AccInt{&AGS::detail_textures, &AGS::set_detail_textures, AGS::detail_textures_off,
            AGS::detail_textures_auto},
        .flags = SF_D3D11_ONLY,
        .apply = detail_texture_level_reset,
        ENUM_LABELS(labels_detail_textures),
        .console_cmd = "r_detailtextures",
    },
    {
        .id = "precache_rooms",
        .key = "PrecacheRooms",
        .label = "Precache rooms",
        .tooltip = "Build all rooms at load time: longer loads, fewer hitches.",
        .category = Cat::Graphics,
        .subcategory = "Quality",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::precache_rooms},
        .flags = SF_D3D11_ONLY | SF_APPLIES_NEXT_LEVEL | SF_ADVANCED,
        .console_cmd = "r_precache_rooms",
    },
    {
        .id = "lod_dist_scale",
        .key = "LODDistanceScale",
        .label = "Model detail distance",
        .tooltip = "Higher values keep detailed models visible farther away.",
        .category = Cat::Graphics,
        .subcategory = "Quality",
        .widget = Wid::Slider,
        .access = AccFloat{&AGS::lod_dist_scale, &AGS::set_lod_dist_scale, AGS::min_lod_dist_scale,
            AGS::max_lod_dist_scale},
        .console_cmd = "r_lodscale",
        .ui_min = 1.0f,
        .ui_max = 40.0f,
        .ui_step = 1.0f,
        .ui_decimals = 0,
    },
    {
        .id = "multi_no_character_lod",
        .key = "DisableMultiCharacterLOD",
        .label = "Full-detail players",
        .tooltip = "Use single-player detail distances for characters; stock multiplayer lowers their detail sooner.",
        .category = Cat::Graphics,
        .subcategory = "Quality",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::multi_no_character_lod},
        .flags = SF_MP_ONLY,
    },
    {
        .id = "entity_sim_distance",
        .key = "SimulationDistance",
        .label = "Simulation distance",
        .tooltip = "Distance beyond which entities stop being simulated.",
        .category = Cat::Graphics,
        .subcategory = "Quality",
        .widget = Wid::Stepper,
        .access = AccFloat{&AGS::entity_sim_distance, &AGS::set_entity_sim_distance, AGS::min_entity_sim_distance,
            AGS::max_entity_sim_distance},
        .flags = SF_APPLY_ON_LOAD | SF_ADVANCED,
        .apply = apply_entity_sim_distance,
        .ui_step = 10.0f,
        .ui_decimals = 0,
        .ui_suffix = " m",
    },
    {
        .id = "monitor_resolution_scale",
        .key = "MonitorResolutionScale",
        .label = "Security monitor resolution",
        .tooltip = "Render scale of security monitors and mirrors.",
        .category = Cat::Graphics,
        .subcategory = "Quality",
        .widget = Wid::Stepper,
        .access = AccInt{&AGS::monitor_resolution_scale, &AGS::set_monitor_resolution_scale,
            AGS::min_monitor_resolution_scale, AGS::max_monitor_resolution_scale},
        .flags = SF_APPLIES_NEXT_LEVEL,
        .console_cmd = "r_monitorscale",
        .ui_suffix = "x",
    },
    {
        .id = "fast_animations",
        .key = "FastAnimations",
        .label = "Fast animations",
        .tooltip = "Cheaper character animation; reduces animation smoothness at far distances.",
        .category = Cat::Graphics,
        .subcategory = "Expert",
        .widget = Wid::Toggle,
        .access = AccFnBool{[](rf::Player*) { return static_cast<bool>(rf::g_fast_animations); },
            [](rf::Player*, bool v) { rf::g_fast_animations = v; }},
        .flags = SF_ADVANCED,
        .console_cmd = "r_fastanim",
        .default_str = "0",
    },
    {
        .id = "weather",
        .key = "Weather",
        .label = "Weather effects",
        .tooltip = "Render rain and snow weather regions.",
        .category = Cat::Graphics,
        .subcategory = "Effects",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::weather},
        .console_cmd = "r_weather",
    },
    {
        .id = "underwater_fx",
        .key = "UnderwaterFx",
        .label = "Underwater effects",
        .tooltip = "Underwater rendering: caustics, then water fog, tint and waterline, then screen distortion.",
        .category = Cat::Graphics,
        .subcategory = "Effects",
        .widget = Wid::Enum,
        .access = AccInt{&AGS::underwater_fx, &AGS::set_underwater_fx, AGS::min_underwater_fx, AGS::max_underwater_fx},
        .flags = SF_D3D11_ONLY,
        ENUM_LABELS(labels_underwater_fx),
        .console_cmd = "r_underwater",
    },
    {
        .id = "particle_sim_cull",
        .key = "ParticleSimCull",
        .label = "Cull hidden particles",
        .tooltip = "Skip simulating level particle emitters that are not visible.",
        .category = Cat::Graphics,
        .subcategory = "Effects",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::particle_sim_cull},
        .flags = SF_ADVANCED,
        .console_cmd = "cl_particlecull",
    },
    {
        .id = "particle_sim_fixed_step",
        .key = "ParticleSimFixedStep",
        .label = "Fixed-rate particles",
        .tooltip = "Step cosmetic particles at a fixed rate with render interpolation.",
        .category = Cat::Graphics,
        .subcategory = "Effects",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::particle_sim_fixed_step},
        .flags = SF_ADVANCED,
        .console_cmd = "cl_particlefixedstep",
    },
    {
        .id = "particle_sim_rate",
        .key = "ParticleSimRate",
        .label = "Particle rate",
        .tooltip = "Fixed particle simulation rate.",
        .category = Cat::Graphics,
        .subcategory = "Effects",
        .widget = Wid::Stepper,
        .access = AccInt{&AGS::particle_sim_rate, &AGS::set_particle_sim_rate, AGS::min_particle_sim_rate,
            AGS::max_particle_sim_rate},
        .flags = SF_ADVANCED,
        .console_cmd = "cl_particlerate",
        .ui_step = 10.0f,
        .ui_suffix = " Hz",
    },
    {
        .id = "particle_sim_collide_lod",
        .key = "ParticleSimCollideLod",
        .label = "Particle collision LOD",
        .tooltip = "Test cosmetic particle world collision less often with camera distance.",
        .category = Cat::Graphics,
        .subcategory = "Effects",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::particle_sim_collide_lod},
        .flags = SF_ADVANCED,
        .console_cmd = "cl_particlecollidelod",
    },
    {
        .id = "particle_pool_size",
        .key = "ParticlePoolSize",
        .label = "Particle pool size",
        .tooltip = "Capacity of each particle pool. Stock uses the original capacities (500 and 1100).",
        .category = Cat::Graphics,
        .subcategory = "Effects",
        .widget = Wid::Stepper,
        .access = AccInt{&AGS::particle_pool_size, &AGS::set_particle_pool_size, 0, AGS::max_particle_pool_size},
        .flags = SF_APPLIES_NEXT_LEVEL | SF_ADVANCED,
        .console_cmd = "cl_particlepoolsize",
        .ui_step = 2048.0f,
        .ui_zero_label = "Stock",
    },
    {
        .id = "show_glares",
        .key = "ShowGlares",
        .label = "Light glares",
        .tooltip = "Draw glare halos around bright lights.",
        .category = Cat::Graphics,
        .subcategory = "Effects",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::show_glares},
        .console_cmd = "vli",
    },
    {
        .id = "muzzle_flash_lights",
        .key = "DisableMuzzleFlashLights",
        .label = "Muzzle flash lights",
        .tooltip = "Dynamic light from muzzle flashes (servers may enforce this).",
        .category = Cat::Graphics,
        .subcategory = "Effects",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::try_disable_muzzle_flash_lights},
        .flags = SF_INVERTED | SF_ADVANCED,
        .permission = SPerm::NoMuzzleFlash,
        .apply = evaluate_restrict_disable_muzzle_flash,
        .console_cmd = "r_muzzleflash",
    },
    {
        .id = "explosion_weapon_flash_lights",
        .key = "ExplosionFlashLightsWeapons",
        .label = "Weapon explosion flashes",
        .tooltip = "Light flash from weapon explosions.",
        .category = Cat::Graphics,
        .subcategory = "Effects",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::explosion_weapon_flash_lights},
        .flags = SF_D3D11_ONLY | SF_NOT_DEDICATED,
        .console_cmd = "cl_explosionflashweapons",
    },
    {
        .id = "explosion_env_flash_lights",
        .key = "ExplosionFlashLightsEnv",
        .label = "Environment flashes",
        .tooltip = "Light flash from environmental explosions.",
        .category = Cat::Graphics,
        .subcategory = "Effects",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::explosion_env_flash_lights},
        .flags = SF_D3D11_ONLY | SF_NOT_DEDICATED,
        .console_cmd = "cl_explosionflashenv",
    },
    {
        .id = "burning_entity_lights",
        .key = "BurningEntityLights",
        .label = "Burning glow",
        .tooltip = "Burning enemies emit light in single player.",
        .category = Cat::Graphics,
        .subcategory = "Effects",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::burning_entity_lights},
        .flags = SF_D3D11_ONLY | SF_SP_ONLY,
        .console_cmd = "cl_burningentityglow",
    },
    {
        .id = "mesh_lighting_mode",
        .key = "MeshLightingMode",
        .label = "Model lighting",
        .tooltip = "How character and object meshes receive level lighting.",
        .category = Cat::Graphics,
        .subcategory = "Lighting",
        .widget = Wid::Enum,
        .access = AccInt{&AGS::mesh_lighting_mode, nullptr, 0, 2},
        .flags = SF_APPLY_ON_LOAD,
        .apply = apply_mesh_lighting,
        ENUM_LABELS(labels_mesh_lighting),
        .console_cmd = "r_meshlighting",
    },
    {
        .id = "dynamic_light_ndotl",
        .key = "DynamicLightNdotL",
        .label = "Dynamic light angle shading",
        .tooltip = "How much a level surface's angle to a dynamic light dims it; 0 lights level geometry fully.",
        .category = Cat::Graphics,
        .subcategory = "Expert",
        .widget = Wid::Slider,
        .access = AccFloat{&AGS::dynamic_light_ndotl, &AGS::set_dynamic_light_ndotl, 0.0f,
            AGS::max_dynamic_light_ndotl},
        .flags = SF_D3D11_ONLY | SF_ADVANCED,
        .console_cmd = "r_dynamiclightndotl",
        .ui_min = 0.0f,
        .ui_max = 1.0f,
        .ui_step = 0.05f,
        .ui_decimals = 2,
    },
    {
        .id = "pixel_light_overbright",
        .key = "PixelLightOverbright",
        .label = "Lighting overbright",
        .tooltip = "How far above full brightness lit surfaces may go with pixel lighting.",
        .category = Cat::Graphics,
        .subcategory = "Expert",
        .widget = Wid::Slider,
        .access = AccFloat{&AGS::pixel_light_overbright, &AGS::set_pixel_light_overbright, 0.0f,
            AGS::max_pixel_light_overbright},
        .flags = SF_D3D11_ONLY | SF_ADVANCED,
        .apply = apply_pixel_light_overbright,
        .console_cmd = "r_pixellightoverbright",
        .ui_min = 0.0f,
        .ui_max = 3.0f,
        .ui_step = 0.1f,
        .ui_decimals = 2,
    },
    {
        .id = "full_range_lighting",
        .key = "FullRangeLighting",
        .label = "Full lightmap range",
        .tooltip = "Use the full lightmap brightness range (next level).",
        .category = Cat::Graphics,
        .subcategory = "Lighting",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::full_range_lighting},
        .flags = SF_APPLIES_NEXT_LEVEL | SF_ADVANCED,
        .console_cmd = "r_fullrangelighting",
    },
    {
        .id = "always_clamp_official_lightmaps",
        .key = "AlwaysClampOfficialLightmaps",
        .label = "Clamp official lightmaps",
        .tooltip = "Force legacy lightmap clamping on official levels.",
        .category = Cat::Graphics,
        .subcategory = "Expert",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::always_clamp_official_lightmaps},
        .flags = SF_APPLIES_NEXT_LEVEL | SF_ADVANCED,
        .console_cmd = "r_clampofficiallightmaps",
    },
    {
        .id = "ignore_tbl_vertex_lighting",
        .key = "IgnoreTblVertexLighting",
        .label = "Ignore map vertex light",
        .tooltip = "Ignore per-map vertex-lighting overrides.",
        .category = Cat::Graphics,
        .subcategory = "Expert",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::ignore_tbl_vertex_lighting},
        .flags = SF_D3D11_ONLY | SF_ADVANCED,
        .apply = apply_mesh_lighting,
        .console_cmd = "cl_ignore_tbl_vertex_lighting",
    },
    {
        .id = "ignore_tbl_pixel_light_overbright",
        .key = "IgnoreTblPixelLightOverbright",
        .label = "Ignore map overbright",
        .tooltip = "Ignore per-map overbright overrides.",
        .category = Cat::Graphics,
        .subcategory = "Expert",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::ignore_tbl_pixel_light_overbright},
        .flags = SF_D3D11_ONLY | SF_ADVANCED,
        .apply = apply_pixel_light_overbright,
        .console_cmd = "cl_ignore_tbl_pixel_light_overbright",
    },
    {
        .id = "ignore_tbl_lightmap_clamping",
        .key = "IgnoreTblLightmapClamping",
        .label = "Ignore map lightmap clamping",
        .tooltip = "Ignore per-map lightmap clamp overrides.",
        .category = Cat::Graphics,
        .subcategory = "Expert",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::ignore_tbl_lightmap_clamping},
        .flags = SF_APPLIES_NEXT_LEVEL | SF_ADVANCED,
        .console_cmd = "cl_ignore_tbl_lightmap_clamping",
    },
    {
        .id = "fullbright_characters",
        .key = "FullbrightCharacters",
        .label = "Fullbright models",
        .tooltip = "Render character models unlit (servers may forbid this).",
        .category = Cat::Graphics,
        .subcategory = "Lighting",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::try_fullbright_characters},
        .permission = SPerm::FullbrightMeshes,
        .apply = evaluate_fullbright_meshes,
        .console_cmd = "r_fullbright",
    },
    {
        .id = "lightmaps_only",
        .key = "DisableTextures",
        .label = "Display only lightmaps, no textures",
        .tooltip = "Draw level geometry with lightmaps only (servers may forbid this).",
        .category = Cat::Graphics,
        .subcategory = "Lighting",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::try_disable_textures},
        .flags = SF_ADVANCED,
        .permission = SPerm::LightmapsOnly,
        .apply = evaluate_lightmaps_only,
        .console_cmd = "r_lightmaps",
    },
    {
        .id = "shadow_corpses",
        .key = "ShadowCorpses",
        .label = "Corpse shadows",
        .tooltip = "Dead bodies cast shadows.",
        .category = Cat::Graphics,
        .subcategory = "Shadows",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::shadow_corpses},
        .flags = SF_D3D11_ONLY,
        .console_cmd = "r_shadowcorpses",
    },
    {
        .id = "shadow_items",
        .key = "ShadowItems",
        .label = "Item shadows",
        .tooltip = "Pickups cast shadows.",
        .category = Cat::Graphics,
        .subcategory = "Shadows",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::shadow_items},
        .flags = SF_D3D11_ONLY,
        .console_cmd = "r_shadowitems",
    },
    {
        .id = "shadow_distance",
        .key = "ShadowDistance",
        .label = "Shadow distance",
        .tooltip = "How far away shadow casters are drawn.",
        .category = Cat::Graphics,
        .subcategory = "Shadows",
        .widget = Wid::Enum,
        .access = AccInt{&AGS::shadow_distance, &AGS::set_shadow_distance,
            AGS::min_shadow_distance, AGS::max_shadow_distance},
        .flags = SF_D3D11_ONLY,
        ENUM_LABELS(labels_shadow_distance),
        .console_cmd = "r_shadowdistance",
    },
    {
        .id = "shadow_quality",
        .key = "ShadowQuality",
        .label = "Shadow quality",
        .tooltip = "Shadow-map resolution and filtering preset (Blob = classic).",
        .category = Cat::Graphics,
        .subcategory = "Shadows",
        .widget = Wid::Enum,
        .access = AccInt{&AGS::shadow_quality, &AGS::set_shadow_quality,
            AGS::min_shadow_quality, AGS::max_shadow_quality},
        .flags = SF_D3D11_ONLY,
        ENUM_LABELS(labels_shadow_quality),
        .console_cmd = "r_shadowquality",
    },
    {
        .id = "shadow_frame_lag",
        .key = "ShadowFrameLag",
        .label = "Shadow update interval",
        .tooltip = "Reuse the shadow map for N frames to save GPU time.",
        .category = Cat::Graphics,
        .subcategory = "Shadows",
        .widget = Wid::Stepper,
        .access = AccInt{&AGS::shadow_frame_lag, &AGS::set_shadow_frame_lag,
            AGS::min_shadow_frame_lag, AGS::max_shadow_frame_lag},
        .flags = SF_D3D11_ONLY | SF_ADVANCED,
        .console_cmd = "r_shadowupdateinterval",
    },
    {
        .id = "outlines",
        .key = "Outlines",
        .label = "Player outlines",
        .tooltip = "Draw outlines around players (needs server permission).",
        .category = Cat::Graphics,
        .subcategory = "Outlines",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::try_outlines},
        .flags = SF_D3D11_ONLY | SF_MP_ONLY,
        .permission = SPerm::Outlines,
        .console_cmd = "r_outlines",
    },
    {
        .id = "outlines_spectator",
        .key = "OutlinesSpectator",
        .label = "Outlines while spectating",
        .tooltip = "Draw player outlines while spectating.",
        .category = Cat::Graphics,
        .subcategory = "Outlines",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::outlines_spectator},
        .flags = SF_D3D11_ONLY | SF_MP_ONLY,
        .console_cmd = "r_outlines_spectator",
    },
    {
        .id = "outlines_team_xray",
        .key = "OutlinesTeamXray",
        .label = "Teammate X-ray",
        .tooltip = "Show teammate outlines through walls.",
        .category = Cat::Graphics,
        .subcategory = "Outlines",
        .widget = Wid::Toggle,
        .access = AccBool{&AGS::try_outlines_team_xray},
        .flags = SF_D3D11_ONLY | SF_MP_ONLY,
        .permission = SPerm::OutlinesXray,
        .console_cmd = "r_outlines_team_xray",
    },
    {
        .id = "outlines_color",
        .key = "OutlinesColor",
        .label = "Outline color",
        .tooltip = "Outline color in non-team modes.",
        .category = Cat::Graphics,
        .subcategory = "Outlines",
        .widget = Wid::Color,
        .access = AccColor{&AGS::outlines_color},
        .flags = SF_D3D11_ONLY | SF_ADVANCED,
        .console_cmd = "r_outlines_color",
    },
    {
        .id = "outlines_color_team_r",
        .key = "OutlinesColorTeamR",
        .label = "Red team outline",
        .tooltip = "Outline color for the red team.",
        .category = Cat::Graphics,
        .subcategory = "Outlines",
        .widget = Wid::Color,
        .access = AccColor{&AGS::outlines_color_team_r},
        .flags = SF_D3D11_ONLY | SF_ADVANCED,
        .console_cmd = "r_outlines_color_team_r",
    },
    {
        .id = "outlines_color_team_b",
        .key = "OutlinesColorTeamB",
        .label = "Blue team outline",
        .tooltip = "Outline color for the blue team.",
        .category = Cat::Graphics,
        .subcategory = "Outlines",
        .widget = Wid::Color,
        .access = AccColor{&AGS::outlines_color_team_b},
        .flags = SF_D3D11_ONLY | SF_ADVANCED,
        .console_cmd = "r_outlines_color_team_b",
    },
    {
        .id = "outlines_color_enemy",
        .key = "OutlinesColorEnemy",
        .label = "Enemy outline override",
        .tooltip = "Override the outline color used for enemies.",
        .category = Cat::Graphics,
        .subcategory = "Outlines",
        .widget = Wid::OptionalColor,
        .access = AccOptColor{&AGS::outlines_color_enemy},
        .flags = SF_D3D11_ONLY | SF_ADVANCED,
        .console_cmd = "r_outlines_color_enemy",
    },
    {
        .id = "outlines_color_team",
        .key = "OutlinesColorTeam",
        .label = "Teammate outline override",
        .tooltip = "Override the outline color used for teammates.",
        .category = Cat::Graphics,
        .subcategory = "Outlines",
        .widget = Wid::OptionalColor,
        .access = AccOptColor{&AGS::outlines_color_team},
        .flags = SF_D3D11_ONLY | SF_ADVANCED,
        .console_cmd = "r_outlines_color_team",
    },
};

} // namespace

std::span<const SettingRow> settings_rows_graphics()
{
    return {rows, std::size(rows)};
}
