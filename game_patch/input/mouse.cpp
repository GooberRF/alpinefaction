#include <algorithm>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include "../os/console.h"
#include "../rf/input.h"
#include "../rf/entity.h"
#include "../rf/os/os.h"
#include "../rf/gr/gr.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../misc/alpine_settings.h"
#include "../main/main.h"
#include "mouse.h"

// Raw mouse delta accumulators for centralized camera angle computation.
static int g_camera_mouse_dx = 0, g_camera_mouse_dy = 0;
// Sub-pixel remainder accumulators for vehicle mouse sensitivity scaling.
static float g_vehicle_mouse_dx_rem = 0.0f, g_vehicle_mouse_dy_rem = 0.0f;

static float scope_sensitivity_value = 0.25f;
static float scanner_sensitivity_value = 0.25f;
static float applied_static_sensitivity_value = 0.25f; // value written by AsmWriter
static float applied_dynamic_sensitivity_value = 1.0f; // value written by AsmWriter

bool set_direct_input_enabled(bool enabled)
{
    auto direct_input_initialized = addr_as_ref<bool>(0x01885460);
    auto mouse_di_init = addr_as_ref<int()>(0x0051E070);
    rf::direct_input_disabled = !enabled;
    if (enabled && !direct_input_initialized) {
        if (mouse_di_init() != 0) {
            xlog::error("Failed to initialize DirectInput");
            rf::direct_input_disabled = true;
            return false;
        }
    }
    if (direct_input_initialized) {
        if (rf::direct_input_disabled)
            rf::di_mouse->Unacquire();
        else
            rf::di_mouse->Acquire();
    }
    return true;
}

FunHook<void()> mouse_eval_deltas_hook{
    0x0051DC70,
    []() {
        // disable mouse when window is not active
        if (rf::os_foreground() || g_alpine_game_config.background_mouse) {
            mouse_eval_deltas_hook.call_target();
        }
    },
};

FunHook<void()> mouse_eval_deltas_di_hook{
    0x0051DEB0,
    []() {
        mouse_eval_deltas_di_hook.call_target();

        // Fix invalid mouse scroll delta, when DirectInput is turned off.
        rf::mouse_old_z = rf::mouse_wheel_pos;

        // center cursor if in game
        if (rf::keep_mouse_centered) {
            POINT pt{rf::gr::screen_width() / 2, rf::gr::screen_height() / 2};
            ClientToScreen(rf::main_wnd, &pt);
            SetCursorPos(pt.x, pt.y);
        }
    },
};

FunHook<void()> mouse_keep_centered_enable_hook{
    0x0051E690,
    []() {
        if (!rf::keep_mouse_centered && !rf::is_dedicated_server)
            set_direct_input_enabled(g_alpine_game_config.direct_input);
        mouse_keep_centered_enable_hook.call_target();
    },
};

FunHook<void()> mouse_keep_centered_disable_hook{
    0x0051E6A0,
    []() {
        if (rf::keep_mouse_centered)
            set_direct_input_enabled(false);
        mouse_keep_centered_disable_hook.call_target();
    },
};

FunHook<void(int&, int&, int&)> mouse_get_delta_hook{
    0x0051E630,
    [](int& dx, int& dy, int& dz) {
        mouse_get_delta_hook.call_target(dx, dy, dz); // fills dz (scroll wheel)

        // Nothing to do in Classic mode or outside gameplay.
        if (!rf::keep_mouse_centered || g_alpine_game_config.mouse_scale == 0)
            return;

        // In Raw/Modern mode: capture raw deltas for centralized angle
        // computation and zero them so RF does not apply its own scaling.
        // Skip when in a vehicle (RF needs the deltas to steer), but scale
        // them down to stay consistent with the camera formula feel.
        bool in_vehicle = rf::local_player_entity &&
            rf::entity_in_vehicle(rf::local_player_entity);
        if (!in_vehicle) {
            g_camera_mouse_dx += dx;
            g_camera_mouse_dy += dy;
            dx = 0;
            dy = 0;
        } else if (g_alpine_game_config.mouse_scale == 2) {
            // Modern mode: scale vehicle steering down to match camera formula feel.
            constexpr float vehicle_sens_scale = 0.08f;
            g_vehicle_mouse_dx_rem += dx * vehicle_sens_scale;
            g_vehicle_mouse_dy_rem += dy * vehicle_sens_scale;
            dx = static_cast<int>(g_vehicle_mouse_dx_rem);
            dy = static_cast<int>(g_vehicle_mouse_dy_rem);
            g_vehicle_mouse_dx_rem -= dx;
            g_vehicle_mouse_dy_rem -= dy;
        }
    },
};

ConsoleCommand2 input_mode_cmd{
    "inputmode",
    []() {
        g_alpine_game_config.direct_input = !g_alpine_game_config.direct_input;

        if (g_alpine_game_config.direct_input) {
            if (!set_direct_input_enabled(g_alpine_game_config.direct_input)) {
                rf::console::print("Failed to initialize DirectInput");
            }
            else {
                set_direct_input_enabled(rf::keep_mouse_centered);
                rf::console::print("DirectInput is enabled");
            }
        }
        else {
            rf::console::print("DirectInput is disabled");
        }
    },
    "Toggles DirectInput mouse mode",
};

ConsoleCommand2 ms_cmd{
    "ms",
    [](std::optional<float> value_opt) {
        if (value_opt) {
            float value = std::max(value_opt.value(), 0.0f);
            rf::local_player->settings.controls.mouse_sensitivity = value;
        }
        rf::console::print("Mouse sensitivity: {:.4f}", rf::local_player->settings.controls.mouse_sensitivity);
    },
    "Sets mouse sensitivity",
    "ms <value>",
};

ConsoleCommand2 ms_scale_cmd{
    "ms_scale",
    [](std::optional<int> value_opt) {
        if (value_opt) {
            g_alpine_game_config.mouse_scale = std::clamp(value_opt.value(), 0, 2);
        }
        static constexpr const char* mode_names[] = {"Classic", "Raw", "Modern"};
        int mode = std::clamp(g_alpine_game_config.mouse_scale, 0, 2);
        rf::console::print("ms_scale: {} ({})", mode, mode_names[mode]);
    },
    "Sets mouse scale mode. 0 = Classic (RF native), 1 = Raw (pure degrees), 2 = Modern (id Tech/Source 0.022 deg/pixel).",
    "ms_scale <0|1|2>",
};

void update_scope_sensitivity()
{
    scope_sensitivity_value = g_alpine_game_config.scope_sensitivity_modifier;

    applied_dynamic_sensitivity_value =
        (1 / (4 * g_alpine_game_config.scope_sensitivity_modifier)) * rf::scope_sensitivity_constant;
}

void update_scanner_sensitivity()
{
    scanner_sensitivity_value = g_alpine_game_config.scanner_sensitivity_modifier;
}

ConsoleCommand2 static_scope_sens_cmd{
    "cl_staticscopesens",
    []() {
        g_alpine_game_config.scope_static_sensitivity = !g_alpine_game_config.scope_static_sensitivity;
        rf::console::print("Scope sensitivity is {}", g_alpine_game_config.scope_static_sensitivity ? "static" : "dynamic");
    },
    "Toggle whether scope mouse sensitivity is static or dynamic (based on zoom level)."
};

ConsoleCommand2 scope_sens_cmd{
    "cl_scopesens",
    [](std::optional<float> value_opt) {
        if (value_opt) {
            g_alpine_game_config.set_scope_sens_mod(value_opt.value());
            update_scope_sensitivity();
        }
        else {
            rf::console::print("Scope sensitivity modifier: {:.2f}", g_alpine_game_config.scope_sensitivity_modifier);
        }
    },
    "Sets mouse sensitivity modifier used while in a scope.",
    "cl_scopesens <value> (valid range: 0.0 - 10.0)",
};

ConsoleCommand2 scanner_sens_cmd{
    "cl_scannersens",
    [](std::optional<float> value_opt) {
        if (value_opt) {
            g_alpine_game_config.set_scanner_sens_mod(value_opt.value());
            update_scanner_sensitivity();
        }
        else {
            rf::console::print("Scanner sensitivity modifier: {:.2f}", g_alpine_game_config.scanner_sensitivity_modifier);
        }
    },
    "Sets mouse sensitivity modifier used while in a scanner.",
    "cl_scannersens <value> (valid range: 0.0 - 10.0)",
};

CodeInjection static_zoom_sensitivity_patch {
    0x004309A2,
    [](auto& regs) {
        if (g_alpine_game_config.scope_static_sensitivity) {
            regs.eip = 0x004309D0; // use static sens calculation method for scopes (same as scanner and unscoped)
        }
    },
};

CodeInjection static_zoom_sensitivity_patch2 {
    0x004309D6,
    [](auto& regs) {
        rf::Player* player = regs.edi;

        if (player && rf::player_fpgun_is_zoomed(player)) {
            applied_static_sensitivity_value = scope_sensitivity_value;
            if (g_alpine_game_config.scope_static_sensitivity) {
                regs.al = static_cast<int8_t>(1); // make cmp at 0x004309DA test true
            }
        }
        else {
            applied_static_sensitivity_value = scanner_sensitivity_value;
        }
    },
};

// Converts the per-frame raw mouse pixel deltas captured by mouse_get_delta_hook
// into camera angle deltas (radians).
// Mode 1 (Raw):    pure camera angles — 360 pixels * sens = 360 degree camera turn (angle = raw_pixels * sens * deg2rad)
// Mode 2 (Modern): angle = raw_pixels * sens * 0.022 deg/pixel * deg2rad  (id Tech/Source formula)
// Called from camera.cpp's centralized camera injection point.
void mouse_get_camera(float& pitch_delta, float& yaw_delta)
{
    pitch_delta = 0.0f;
    yaw_delta   = 0.0f;
    if (!rf::local_player || !rf::keep_mouse_centered)
        return;
    float sens = rf::local_player->settings.controls.mouse_sensitivity;
    constexpr float deg2rad = 3.14159265f / 180.0f;
    // Raw (1) is pure degrees; Modern (2) uses 0.022 multiplier (matches id Tech/Source)
    float scale = (g_alpine_game_config.mouse_scale == 1)
        ? deg2rad
        : 0.022f * deg2rad;
    // Apply scope/scanner sensitivity modifiers
    if (rf::local_player->fpgun_data.scanning_for_target)
        sens *= scanner_sensitivity_value;
    else if (rf::player_fpgun_is_zoomed(rf::local_player))
        sens *= scope_sensitivity_value;
    pitch_delta = -static_cast<float>(g_camera_mouse_dy) * sens * scale;
    yaw_delta   =  static_cast<float>(g_camera_mouse_dx) * sens * scale;
    g_camera_mouse_dx = 0;
    g_camera_mouse_dy = 0;
}

void mouse_apply_patch()
{
    // Handle zoom sens customization
    static_zoom_sensitivity_patch.install();
    static_zoom_sensitivity_patch2.install();
    AsmWriter{0x004309DE}.fmul<float>(AsmRegMem{&applied_static_sensitivity_value});
    AsmWriter{0x004309B1}.fmul<float>(AsmRegMem{&applied_dynamic_sensitivity_value});
    update_scope_sensitivity();
    update_scanner_sensitivity();

    // Disable mouse when window is not active
    mouse_eval_deltas_hook.install();

    // Add DirectInput mouse support
    mouse_eval_deltas_di_hook.install();
    mouse_keep_centered_enable_hook.install();
    mouse_keep_centered_disable_hook.install();
    mouse_get_delta_hook.install();

    // Do not limit the cursor to the game window if in menu (Win32 mouse)
    AsmWriter(0x0051DD7C).jmp(0x0051DD8E);

    // Use exclusive DirectInput mode so cursor cannot exit game window
    //write_mem<u8>(0x0051E14B + 1, 5); // DISCL_EXCLUSIVE|DISCL_FOREGROUND

    // Commands
    input_mode_cmd.register_cmd();
    ms_cmd.register_cmd();
    static_scope_sens_cmd.register_cmd();
    scope_sens_cmd.register_cmd();
    scanner_sens_cmd.register_cmd();
    ms_scale_cmd.register_cmd();
}