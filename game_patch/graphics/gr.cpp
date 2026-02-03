#include <algorithm>
#include <cctype>
#include <common/error/d3d-error.h>
#include <common/utils/list-utils.h>
#include <common/utils/os-utils.h>
#include <common/config/BuildConfig.h>
#include <common/ComPtr.h>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/ShortTypes.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include "../os/console.h"
#include "../main/main.h"
#include "../multi/multi.h"
#include "../misc/alpine_settings.h"
#include "../rf/gr/gr.h"
#include "../rf/gameseq.h"
#include "../rf/level.h"
#include "../rf/geometry.h"
#include "../rf/player/player.h"
#include "../rf/multi.h"
#include "../rf/os/os.h"
#include "../rf/os/frametime.h"
#include "../rf/item.h"
#include "../rf/clutter.h"
#include "gr.h"
#include "gr_internal.h"
#include "legacy/gr_d3d.h"
#include "d3d11/gr_d3d11_hooks.h"

namespace df::gr::d3d11
{
    bool set_render_target(int bm_handle);
    void update_window_mode();
    void bitmap_float(int bitmap_handle, float x, float y, float w, float h, float sx, float sy, float sw, float sh, bool flip_x, bool flip_y, rf::gr::Mode mode);
}

CodeInjection gr_init_stretched_window_injection{
    0x0050C464,
    [](auto& regs) {
        if (g_game_config.wnd_mode == GameConfig::STRETCHED) {
            // Make sure stretched window is always full screen
            auto cx = GetSystemMetrics(SM_CXSCREEN);
            auto cy = GetSystemMetrics(SM_CYSCREEN);
            SetWindowLongA(rf::main_wnd, GWL_STYLE, WS_POPUP | WS_SYSMENU);
            SetWindowLongA(rf::main_wnd, GWL_EXSTYLE, 0);
            SetWindowPos(rf::main_wnd, HWND_NOTOPMOST, 0, 0, cx, cy, SWP_SHOWWINDOW);
            rf::gr::screen.aspect = static_cast<float>(cx) / static_cast<float>(cy) * 0.75f;
            regs.eip = 0x0050C551;
        }
    },
};

CodeInjection gr_init_injection{
    0x0050C556,
    []() {
        // Make sure pixel aspect ratio is set to 1 so the frame is not stretched
        rf::gr::screen.aspect = 1.0f;
    },
};

static inline void set_uv(rf::gr::Vertex& v, float u, float w)
{
    v.u1 = u;
    v.v1 = w;
    v.u2 = u;
    v.v2 = w;
}

bool gr_3d_bitmap_oriented_wh(const rf::Vector3* pnt, const rf::Matrix3* M, float half_w, float half_h, rf::gr::Mode mode)
{
    if (half_w <= 1e-5f || half_h <= 1e-5f)
        return false;

    const rf::Vector3& R = M->rvec;
    const rf::Vector3& U = M->uvec;

    rf::gr::Vertex va{}; // (-w,+h)
    rf::gr::Vertex vb{}; // (+w,+h)
    rf::gr::Vertex vc{}; // (+w,-h)
    rf::gr::Vertex vd{}; // (-w,-h)

    auto prep = [](rf::gr::Vertex& out, const rf::Vector3& pos, float u, float v) {
        rf::Vector3 tmp = pos;
        rf::gr::rotate_vertex(&out, &tmp);
        set_uv(out, u, v);
        // keep colors sane
        out.r = out.g = out.b = out.a = 255;
    };

    // build corners
    prep(va, *pnt + (R * -half_w) + (U * +half_h), 1.f, 0.f);
    prep(vb, *pnt + (R * +half_w) + (U * +half_h), 0.f, 0.f);
    prep(vc, *pnt + (R * +half_w) + (U * -half_h), 0.f, 1.f);
    prep(vd, *pnt + (R * -half_w) + (U * -half_h), 1.f, 1.f);

    // winding
    rf::gr::Vertex* verts[4] = {&vd, &vc, &vb, &va};
    return rf::gr::poly(4, verts, rf::gr::TMapperFlags::TMAP_FLAG_TEXTURED, mode, 0, 0.0f);
}

float gr_scale_fov_hor_plus(float horizontal_fov)
{
    // Use Hor+ FOV scaling method to improve user experience for wide screens
    // Assume provided FOV makes sense on a 4:3 screen
    float s = static_cast<float>(rf::gr::screen.max_w) / rf::gr::screen.max_h * 0.75f;
    constexpr float pi = 3.141592f;
    float h_fov_rad = horizontal_fov / 180.0f * pi;
    float x = std::tan(h_fov_rad / 2.0f);
    float y = x * s;
    h_fov_rad = 2.0f * std::atan(y);
    horizontal_fov = h_fov_rad / pi * 180.0f;
    // Clamp the value to avoid artifacts when the view is very stretched
    horizontal_fov = std::min<float>(horizontal_fov, g_alpine_game_config.max_fov);
    return horizontal_fov;
}

float gr_scale_world_fov(float horizontal_fov = 90.0f)
{
    if (g_alpine_game_config.horz_fov > 0.0f) {
        // Use user provided factor
        // Note: 90 is the default FOV for RF
        horizontal_fov *= g_alpine_game_config.horz_fov / 90.0f;
    }
    else {
        horizontal_fov = gr_scale_fov_hor_plus(horizontal_fov);
    }

    const auto& server_info_opt = get_af_server_info();
    if (server_info_opt && server_info_opt.value().max_fov) {
        horizontal_fov = std::min(horizontal_fov, server_info_opt.value().max_fov.value());
    }
    return horizontal_fov;
}

CodeInjection gameplay_render_frame_fov_injection{
    0x00431BA1,
    []() {
        // Scale world FOV
        auto& rf_fov = addr_as_ref<float>(0x0059613C);
        rf_fov = gr_scale_world_fov(rf_fov);
    },
};

CallHook<void(rf::Matrix3&, rf::Vector3&, float, bool, bool)> gr_setup_3d_railgun_hook{
    {
        0x00431CE9, // railgun
        0x004ADDB4,
    },
    [](rf::Matrix3& viewer_orient, rf::Vector3& viewer_pos, float horizontal_fov, bool zbuffer_flag, bool z_scale) {
        horizontal_fov = gr_scale_fov_hor_plus(horizontal_fov);
        gr_setup_3d_railgun_hook.call_target(viewer_orient, viewer_pos, horizontal_fov, zbuffer_flag, z_scale);
    },
};

ConsoleCommand2 fov_cmd{
    "r_fov",
    [](std::optional<float> fov_opt) {
        if (fov_opt) {
            g_alpine_game_config.set_horz_fov(std::max(0.0f, fov_opt.value()));
        }
        rf::console::print("Horizontal FOV: {:.2f}", gr_scale_world_fov());

        const auto& server_info_opt = get_af_server_info();
        if (server_info_opt && server_info_opt.value().max_fov) {
            rf::console::print("Server FOV limit: {:.2f}", server_info_opt.value().max_fov.value());
        }
    },
    "Sets horizontal FOV (field of view) in degrees. Use 0 to enable automatic FOV scaling.",
    "fov [degrees]",
};

ConsoleCommand2 gamma_cmd{
    "r_gamma",
    [](std::optional<float> value_opt) {
        if (value_opt) {
            rf::gr::set_gamma(value_opt.value());
        }
        rf::console::print("Gamma: {:.2f}", rf::gr::gamma);
    },
    "Sets gamma.",
    "gamma [value]",
};

ConsoleCommand2 colorblind_cmd{
    "r_colorblind",
    [](std::optional<std::string> mode_opt) {
        static const char* names[] = {"off", "protanopia", "deuteranopia", "tritanopia"};
        if (!mode_opt) {
            rf::console::print("Colorblind mode: {} (Direct3D 11 mode only)", names[g_alpine_game_config.colorblind_mode]);
            return;
        }
        std::string mode = mode_opt.value();
        std::transform(mode.begin(), mode.end(), mode.begin(), [](unsigned char c) { return std::tolower(c); });
        int new_mode = g_alpine_game_config.colorblind_mode;
        if (mode == "off" || mode == "0") new_mode = 0;
        else if (mode == "protanopia" || mode == "1") new_mode = 1;
        else if (mode == "deuteranopia" || mode == "2") new_mode = 2;
        else if (mode == "tritanopia" || mode == "3") new_mode = 3;
        else {
            rf::console::print("Usage: r_colorblind <off|protanopia|deuteranopia|tritanopia>");
            return;
        }
        g_alpine_game_config.set_colorblind_mode(new_mode);
        rf::console::print("Colorblind mode set to {} (Direct3D 11 renderer only)", names[g_alpine_game_config.colorblind_mode]);
    },
    "Configure colorblind mode rendering filter (Direct3D 11 renderer only)",
    "r_colorblind <off|protanopia|deuteranopia|tritanopia>",
};

ConsoleCommand2 precache_rooms_cmd{
    "r_precache_rooms",
    []() {
        g_alpine_game_config.precache_rooms = !g_alpine_game_config.precache_rooms;
        rf::console::print("D3D11 room precaching is {}", g_alpine_game_config.precache_rooms ? "enabled" : "disabled");
    },
    "Toggle room precaching in D3D11 renderer (increases initial level load time, reduces risk of hitches during gameplay)",
    "r_precache_rooms",
};

void evaluate_lightmaps_only()
{
    bool server_side_restrict_lightmaps_only =
        rf::is_multi && !rf::is_server && get_af_server_info() && !get_af_server_info()->allow_lmap;

    if (server_side_restrict_lightmaps_only) {
        if (g_alpine_game_config.try_disable_textures) {
            rf::console::print("This server does not allow you to use lightmap only mode!");
            rf::gr::show_lightmaps = false;
            rf::g_cache_clear();
        }
    }
    else {
        if (rf::gr::show_lightmaps != g_alpine_game_config.try_disable_textures) {
            rf::gr::show_lightmaps = g_alpine_game_config.try_disable_textures;
            rf::g_cache_clear();
        }
    }
}

ConsoleCommand2 lightmaps_only_cmd{
    "r_lightmaps",
    []() {
        g_alpine_game_config.try_disable_textures = !g_alpine_game_config.try_disable_textures;

        evaluate_lightmaps_only();

        rf::console::print("Lightmap only mode is {}", g_alpine_game_config.try_disable_textures ?
            "enabled. In multiplayer, this will only apply if the server allows it." : "disabled.");
    },
    "Render only lightmaps for level geometry (no textures). In multiplayer, this is only available if the server allows it.",
    "r_lightmaps",
};

ConsoleCommand2 disable_rendering_cmd{
    "dbg_togglerendering",
    []() {
        g_alpine_game_config.rendering_enabled = !g_alpine_game_config.rendering_enabled;
        rf::console::print("Rendering is now {}.", g_alpine_game_config.rendering_enabled ? "enabled" : "disabled");
    },
    "Toggle all game rendering.",
    "dbg_togglerendering",
};

FunHook<void(rf::Player*, int)> gameplay_render_frame_hook{
    0x00431A00,
    [](rf::Player* pp, int flags) {
        if (!g_alpine_game_config.rendering_enabled) {
            return;
        }

        gameplay_render_frame_hook.call_target(pp, flags);
    },
};

FunHook<void()> gameplay_render_frame_pre_hook{
    0x00431820,
    []() {
        if (!g_alpine_game_config.rendering_enabled) {
            return;
        }

        gameplay_render_frame_pre_hook.call_target();
    },
};

FunHook<float(const rf::Vector3&)> gr_get_apparent_distance_from_camera_hook{
    0x005182F0,
    [](const rf::Vector3& pos) {
        return gr_get_apparent_distance_from_camera_hook.call_target(pos) / g_alpine_game_config.lod_dist_scale;
    },
};

FunHook<void()> vclip_init_hook{
    0x004C12A0,
    []() {
        vclip_init_hook.call_target();
        explosion_flash_lights_init();
    },
};

FunHook<void()> explosion_do_frame_hook{
    0x0048E290,
    []() {
        explosion_do_frame_hook.call_target();
        explosion_flash_lights_do_frame();
    },
};

CallHook<int(int index, rf::GRoom* src_room, rf::Vector3* src_pos, rf::Vector3* pos,
    float radius, int parent_handle, rf::Vector3* dir, bool play_sound)> vclip_play_3d_weapon_hook{
    0x004C8A5C,
    [](int index, rf::GRoom* src_room, rf::Vector3* src_pos, rf::Vector3* pos,
        float radius, int parent_handle, rf::Vector3* dir, bool play_sound) {
        int success = vclip_play_3d_weapon_hook.call_target(index, src_room, src_pos, pos, radius, parent_handle, dir, play_sound);

        if (!rf::is_dedicated_server &&
            is_d3d11() &&
            g_alpine_game_config.explosion_weapon_flash_lights &&
            rf::gameseq_get_state() != rf::GameState::GS_MULTI_LIMBO &&
            success >= 0) {
            float radius_scale, intensity, r, g, b;
            int duration_ms;
            if (vclip_should_do_explosion_flash(true, index, &radius_scale, &intensity, &r, &g, &b, &duration_ms)) {
                explosion_flash_light_create_offset(*pos, *dir, 0.5f, radius * radius_scale, intensity, r, g, b, duration_ms);
            }
        }

        return success;
    },
};

CallHook<int(int index, rf::GRoom* src_room, rf::Vector3* src_pos, rf::Vector3* pos,
    float radius, int parent_handle, rf::Vector3* dir, bool play_sound)> vclip_play_3d_env_hook{
    0x004364BC,
    [](int index, rf::GRoom* src_room, rf::Vector3* src_pos, rf::Vector3* pos,
        float radius, int parent_handle, rf::Vector3* dir, bool play_sound) {
        int success = vclip_play_3d_env_hook.call_target(index, src_room, src_pos, pos, radius, parent_handle, dir, play_sound);

        if (!rf::is_dedicated_server &&
            is_d3d11() &&
            g_alpine_game_config.explosion_env_flash_lights &&
            rf::gameseq_get_state() != rf::GameState::GS_MULTI_LIMBO &&
            success >= 0) {
            float radius_scale, intensity, r, g, b;
            int duration_ms;
            if (vclip_should_do_explosion_flash(false, index, &radius_scale, &intensity, &r, &g, &b, &duration_ms)) {
                explosion_flash_light_create(*pos, radius * radius_scale, intensity, r, g, b, duration_ms, false);
            }
        }

        return success;
    },
};

bool gr_is_texture_format_supported(rf::bm::Format format)
{
    if (rf::gr::screen.mode == rf::gr::DIRECT3D) {
        if (is_d3d11()) {
            return true;
        }
        else {
            return gr_d3d_is_texture_format_supported(format);
        }
    }
    return false;
}

bool gr_set_render_target(int bm_handle)
{
    if (rf::gr::screen.mode == rf::gr::DIRECT3D) {
        if (is_d3d11()) {
            return df::gr::d3d11::set_render_target(bm_handle);
        }
        else {
            return gr_d3d_set_render_target(bm_handle);
        }
    }
    return false;
}

void gr_bitmap_scaled_float(int bitmap_handle, float x, float y, float w, float h,
                            float sx, float sy, float sw, float sh, bool flip_x, bool flip_y, rf::gr::Mode mode)
{
    if (rf::gr::screen.mode == rf::gr::DIRECT3D) {
        if (is_d3d11()) {
            df::gr::d3d11::bitmap_float(bitmap_handle, x, y, w, h, sx, sy, sw, sh, flip_x, flip_y, mode);
        }
        else {
            gr_d3d_bitmap_float(bitmap_handle, x, y, w, h, sx, sy, sw, sh, flip_x, flip_y, mode);
        }
    }
}

void gr_set_window_mode(rf::gr::WindowMode window_mode)
{
    if (rf::gr::screen.mode == rf::gr::DIRECT3D) {
        rf::gr::screen.window_mode = window_mode;
        if (is_d3d11()) {
            df::gr::d3d11::update_window_mode();
        }
        else {
            gr_d3d_update_window_mode();
        }
    }
}

void gr_update_texture_filtering()
{
    if (rf::gr::screen.mode == rf::gr::DIRECT3D) {
        if (is_d3d11()) {
            df::gr::d3d11::update_texture_filtering();
        }
        else {
            gr_d3d_update_texture_filtering();
        }
    }
}

ConsoleCommand2 fullscreen_cmd{
    "d_fullscreen",
    []() {
        gr_set_window_mode(rf::gr::FULLSCREEN);
    },
};

ConsoleCommand2 windowed_cmd{
    "d_windowed",
    []() {
        gr_set_window_mode(rf::gr::WINDOWED);
    },
};

ConsoleCommand2 nearest_texture_filtering_cmd{
    "r_nearest",
    []() {
        g_alpine_game_config.nearest_texture_filtering = !g_alpine_game_config.nearest_texture_filtering;
        gr_update_texture_filtering();
        rf::console::print("Nearest texture filtering is {}", g_alpine_game_config.nearest_texture_filtering ? "enabled" : "disabled");
    },
    "Toggle nearest texture filtering",
};

ConsoleCommand2 picmip_cmd{
    "r_picmip",
    [](std::optional<int> picmip_opt) {
        if (picmip_opt) {
            int divisor = picmip_opt.value();
            if (divisor < 1) {
                divisor = 1;
            }
            g_alpine_game_config.set_picmip(divisor);
            gr_update_texture_filtering();
        }
        rf::console::print(
            "Texture resolution divisor is set to {} (Direct3D 11 renderer only, 1 = full resolution)",
            g_alpine_game_config.picmip
        );
    },
    "Sets the global texture resolution divisor (Direct3D 11 renderer only)",
    "r_picmip <divisor>",
};

ConsoleCommand2 lod_distance_scale_cmd{
    "r_lodscale",
    [](std::optional<float> scale_opt) {
        if (scale_opt.has_value()) {
            g_alpine_game_config.set_lod_dist_scale(scale_opt.value());
        }
        rf::console::print("LOD distance scale: {:.2f}", g_alpine_game_config.lod_dist_scale);
    },
    "Sets LOD distance scale factor",
};

CodeInjection gr_d3d_render_lod_vif_injection{
    0x0052FAEA,
    [](auto& regs) {
        if (g_alpine_game_config.multi_no_character_lod) {
            regs.esp += 0x4;
            regs.eip = 0x0052FAFB;
        }
    },
};

void gr_apply_patch()
{
    if (g_game_config.wnd_mode != GameConfig::FULLSCREEN) {
        // Enable windowed mode
        write_mem<u32>(0x004B29A5 + 6, 0xC8);
        gr_init_stretched_window_injection.install();
    }

    // Fix FOV for widescreen
    gr_init_injection.install();
    gameplay_render_frame_fov_injection.install();
    gr_setup_3d_railgun_hook.install();

    // Fix rendering of right and bottom edges of viewport in gameplay_render_frame (part 1)
    write_mem<u8>(0x00431D9F, asm_opcodes::jmp_rel_short);
    write_mem<u8>(0x00431F6B, asm_opcodes::jmp_rel_short);
    write_mem<u8>(0x004328CF, asm_opcodes::jmp_rel_short);
    AsmWriter(0x0043298F).jmp(0x004329DC);

    // Fonts
    gr_font_apply_patch();

    // Lights
    gr_light_apply_patch();

    if (g_game_config.renderer == GameConfig::Renderer::d3d11) {
        void gr_d3d11_apply_patch();
        gr_d3d11_apply_patch();
    }
    else {
        // D3D generic patches
        gr_d3d_apply_patch();

        // D3D texture handling
        gr_d3d_texture_apply_patch();

        // Back-buffer capture or render to texture related code
        gr_d3d_capture_apply_patch();

        // Gamma related code
        gr_d3d_gamma_apply_patch();
    }

    // Bink Video patch
    bink_apply_patch();

    // Do not flush drawing buffers in gr_set_color
    write_mem<u8>(0x0050CFEB, asm_opcodes::jmp_rel_short);

    // Fix details and liquids rendering in railgun scanner. They were unnecessary modulated with very dark green color,
    // which seems to be a left-over from an old implementationof railgun scanner green overlay (currently it is
    // handled by drawing a green rectangle after all the geometry is drawn)
    write_mem<u8>(0x004D33F3, asm_opcodes::jmp_rel_short);
    write_mem<u8>(0x004D410D, asm_opcodes::jmp_rel_short);

    // Use gr_clear instead of gr_rect for faster drawing of the fog background
    AsmWriter(0x00431F99).call(0x0050CDF0);

    // Fix possible divisions by zero
    // Fixes performance issues caused by gr::sceen::fog_far_scaled being initialized to inf
    rf::gr::matrix_scale.set(1.0f, 1.0f, 1.0f);
    rf::gr::one_over_matrix_scale_z = 1.0f;

    // Increase mesh details
    gr_get_apparent_distance_from_camera_hook.install();
    gr_d3d_render_lod_vif_injection.install();

    // Handle explosion dynamic light flashes
    vclip_init_hook.install();
    explosion_do_frame_hook.install();
    vclip_play_3d_weapon_hook.install();
    vclip_play_3d_env_hook.install();

    // Fix gr_rect_border not drawing left border
    AsmWriter{0x0050DF2D}.push(asm_regs::ebp).push(asm_regs::ebx);

    // Allow client to disable rendering
    gameplay_render_frame_hook.install();
    gameplay_render_frame_pre_hook.install();

    // Commands
    fov_cmd.register_cmd();
    gamma_cmd.register_cmd();
    lightmaps_only_cmd.register_cmd();
    fullscreen_cmd.register_cmd();
    windowed_cmd.register_cmd();
    nearest_texture_filtering_cmd.register_cmd();
    lod_distance_scale_cmd.register_cmd();
    picmip_cmd.register_cmd();
    colorblind_cmd.register_cmd();
    precache_rooms_cmd.register_cmd();
    disable_rendering_cmd.register_cmd();

    // Fix `rf::gr::text_2d_mode`.
    AsmWriter{0x0050BB40}.push<int8_t>(rf::gr::FOG_NOT_ALLOWED);
}
