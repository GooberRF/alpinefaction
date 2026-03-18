#include <algorithm>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include "../os/console.h"
#include "../rf/input.h"
#include "../rf/os/os.h"
#include "../rf/gr/gr.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../misc/alpine_settings.h"
#include "../main/main.h"
#include "input.h"

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
    "Toggles input mode",
};

ConsoleCommand2 ms_cmd{
    "ms",
    [](std::optional<float> value_opt) {
        if (value_opt) {
            float value = value_opt.value();
            value = std::clamp(value, 0.0f, 1.0f);
            rf::local_player->settings.controls.mouse_sensitivity = value;
        }
        rf::console::print("Mouse sensitivity: {:.4f}", rf::local_player->settings.controls.mouse_sensitivity);
    },
    "Sets mouse sensitivity",
    "ms <value>",
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
}
