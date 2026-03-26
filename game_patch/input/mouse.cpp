#include <algorithm>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include <SDL3/SDL.h>
#include "input.h"
#include "../os/console.h"
#include "../rf/input.h"
#include "../rf/entity.h"
#include "../rf/os/os.h"
#include "../rf/gr/gr.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/player/camera.h"
#include "../rf/ui.h"
#include "../misc/alpine_settings.h"
#include "../main/main.h"
#include "mouse.h"
#include "../multi/multi.h"

// SDL window and mouse motion state (used in SDL input mode only)
static SDL_Window* g_sdl_window = nullptr;
static bool g_relative_mouse_mode_window_missing_logged = false;
static float g_sdl_mouse_dx_rem = 0.0f, g_sdl_mouse_dy_rem = 0.0f;
static float g_sdl_mouse_wheel_rem = 0.0f; // sub-notch accumulator for smooth-scroll devices
static int g_sdl_mouse_dx = 0, g_sdl_mouse_dy = 0;

// Extra mouse button rebind state (Mouse 4-8, used with SDL input mode)
static int g_pending_mouse_extra_btn_rebind = -1;

static bool is_freelook_camera()
{
    return rf::local_player && rf::local_player->cam
        && rf::local_player->cam->mode == rf::CAMERA_FREELOOK;
}

// Per-frame raw mouse deltas captured for centralized camera angle computation.
// Populated by mouse_get_delta_hook during gameplay; consumed by mouse_get_camera.
static int g_camera_mouse_dx = 0, g_camera_mouse_dy = 0;

// Sub-pixel remainder accumulators for vehicle mouse sensitivity scaling.
static float g_vehicle_mouse_dx_rem = 0.0f, g_vehicle_mouse_dy_rem = 0.0f;

static void reset_mouse_delta_accumulators()
{
    g_camera_mouse_dx = 0;
    g_camera_mouse_dy = 0;
    g_vehicle_mouse_dx_rem = 0.0f;
    g_vehicle_mouse_dy_rem = 0.0f;
}

static float scope_sensitivity_value = 0.25f;
static float scanner_sensitivity_value = 0.25f;
static float applied_static_sensitivity_value = 0.25f; // value written by AsmWriter
static float applied_dynamic_sensitivity_value = 1.0f; // value written by AsmWriter

bool set_direct_input_enabled(bool enabled)
{
    auto direct_input_initialized = addr_as_ref<bool>(0x01885460);

    if (client_bot_headless_enabled()) {
        rf::direct_input_disabled = true;
        if (direct_input_initialized && rf::di_mouse) {
            rf::di_mouse->Unacquire();
        }
        return true;
    }

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

void set_input_mode(int mode)
{
    mode = std::clamp(mode, 0, 2);

    const int old_mode = g_alpine_game_config.input_mode;
    g_alpine_game_config.input_mode = mode;

    if (!rf::is_dedicated_server) {
        // Handle SDL relative mouse mode
        if (g_sdl_window) {
            SDL_SetWindowRelativeMouseMode(g_sdl_window, mode == 2 && rf::keep_mouse_centered);
        }

        // Handle DirectInput transitions
        if (mode == 1 && rf::keep_mouse_centered) {
            set_direct_input_enabled(true);
        } else if (old_mode == 1 && mode != 1) {
            set_direct_input_enabled(false);
        }
    }

    // Clear SDL state when leaving SDL mode
    if (old_mode == 2 && mode != 2) {
        g_sdl_mouse_dx = 0;
        g_sdl_mouse_dy = 0;
        g_sdl_mouse_dx_rem = 0.0f;
        g_sdl_mouse_dy_rem = 0.0f;
        g_sdl_mouse_wheel_rem = 0.0f;
    }

    // Release held extra scan codes so they don't stay stuck after mode switch
    if (old_mode != mode) {
        for (int i = 0; i < CTRL_EXTRA_MOUSE_SCAN_COUNT; ++i)
            rf::key_process_event(CTRL_EXTRA_MOUSE_SCAN_BASE + i, 0, 0);
        for (int i = 0; i < CTRL_EXTRA_KEY_SCAN_COUNT; ++i)
            rf::key_process_event(CTRL_EXTRA_KEY_SCAN_BASE + i, 0, 0);
        g_pending_mouse_extra_btn_rebind = -1;
    }

    // Keep the Alpine Settings UI button label in sync
    ui_refresh_input_mode_label();
}

FunHook<void()> mouse_eval_deltas_hook{
    0x0051DC70,
    []() {
        if (client_bot_headless_enabled()) {
            return;
        }

        if (!rf::os_foreground() && !g_alpine_game_config.background_mouse) {
            // Discard any SDL motion that accumulated while unfocused
            g_sdl_mouse_dx_rem = 0.0f;
            g_sdl_mouse_dy_rem = 0.0f;
            return;
        }

        if (g_alpine_game_config.input_mode == 2) {
            // SDL mode: accumulate SDL motion events into integer deltas for this frame
            g_sdl_mouse_dx = static_cast<int>(g_sdl_mouse_dx_rem);
            g_sdl_mouse_dy = static_cast<int>(g_sdl_mouse_dy_rem);
            g_sdl_mouse_dx_rem -= g_sdl_mouse_dx;
            g_sdl_mouse_dy_rem -= g_sdl_mouse_dy;
            // Do NOT touch mouse_old_z here — mouse_eval_deltas_di_hook uses the
            // difference between mouse_wheel_pos (updated by SDL poll) and mouse_old_z
            // (from the previous frame) to compute mouse_dz. Setting it here before
            // call_target() would zero out the delta every frame.
        }

        mouse_eval_deltas_hook.call_target();

        // Cursor centering fallback for SDL mode when relative mouse mode is unavailable (e.g. no SDL window)
        if (rf::keep_mouse_centered && g_alpine_game_config.input_mode == 2 && (!g_sdl_window || !SDL_GetWindowRelativeMouseMode(g_sdl_window))) {
            RECT rect{};
            GetClientRect(rf::main_wnd, &rect);
            POINT pt{rect.right / 2, rect.bottom / 2};
            ClientToScreen(rf::main_wnd, &pt);
            SetCursorPos(pt.x, pt.y);
            SDL_PumpEvents();
            SDL_FlushEvents(SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_MOTION);
        }
    },
};

// Handles scroll-wheel delta fix and Win32 cursor centering for Legacy/DInput modes (0 and 1).
// In SDL mode (2) this hook fires but we skip its extra work — SDL manages it instead.
FunHook<void()> mouse_eval_deltas_di_hook{
    0x0051DEB0,
    []() {
        if (client_bot_headless_enabled()) {
            rf::mouse_dz = 0;
            rf::mouse_old_z = rf::mouse_wheel_pos;
            return;
        }

        mouse_eval_deltas_di_hook.call_target();

        if (g_alpine_game_config.input_mode == 2 && rf::keep_mouse_centered) {
            rf::mouse_dz = rf::mouse_wheel_pos - rf::mouse_old_z;
            rf::mouse_old_z = rf::mouse_wheel_pos;
            return;
        }

        // In SDL menu mode, allow the original code path to continue so UI cursor behavior works.

        // Fix invalid mouse scroll delta when DirectInput is not active (mode 0 or SDL menu mode)
        if (g_alpine_game_config.input_mode != 1)
            rf::mouse_old_z = rf::mouse_wheel_pos;

        // Keep Win32 cursor at window centre so delta-from-centre aiming stays accurate
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
        if (client_bot_headless_enabled()) {
            rf::keep_mouse_centered = false;
            if (g_sdl_window)
                SDL_SetWindowRelativeMouseMode(g_sdl_window, false);
            set_direct_input_enabled(false);
            return;
        }

        // keep_mouse_centered is still false here; call_target sets it
        if (!rf::keep_mouse_centered && !rf::is_dedicated_server) {
            switch (g_alpine_game_config.input_mode) {
            case 1: // DirectInput mouse
                set_direct_input_enabled(true);
                break;
            case 2: // SDL mouse
                if (g_sdl_window) {
                    SDL_SetWindowRelativeMouseMode(g_sdl_window, true);
                } else if (!g_relative_mouse_mode_window_missing_logged) {
                    xlog::warn("mouse_keep_centered_enable_hook: SDL window is null, cannot enable relative mouse mode");
                    g_relative_mouse_mode_window_missing_logged = true;
                }
                break;
            }
        }
        mouse_keep_centered_enable_hook.call_target();
    },
};

FunHook<void()> mouse_keep_centered_disable_hook{
    0x0051E6A0,
    []() {
        if (client_bot_headless_enabled()) {
            rf::keep_mouse_centered = false;
            if (g_sdl_window)
                SDL_SetWindowRelativeMouseMode(g_sdl_window, false);
            set_direct_input_enabled(false);
            return;
        }

        // keep_mouse_centered is still true here; call_target clears it
        if (rf::keep_mouse_centered && !rf::is_dedicated_server) {
            switch (g_alpine_game_config.input_mode) {
            case 1: // DirectInput mouse
                set_direct_input_enabled(false);
                break;
            case 2: // SDL mouse
                if (g_sdl_window) {
                    SDL_SetWindowRelativeMouseMode(g_sdl_window, false);
                } else if (!g_relative_mouse_mode_window_missing_logged) {
                    xlog::warn("mouse_keep_centered_disable_hook: SDL window is null, cannot disable relative mouse mode");
                    g_relative_mouse_mode_window_missing_logged = true;
                }
                break;
            }
            reset_mouse_delta_accumulators();
        }
        mouse_keep_centered_disable_hook.call_target();
    },
};

FunHook<void(int&, int&, int&)> mouse_get_delta_hook{
    0x0051E630,
    [](int& dx, int& dy, int& dz) {
        mouse_get_delta_hook.call_target(dx, dy, dz); // fills dz (scroll wheel)

        if (g_alpine_game_config.input_mode == 2 && g_sdl_window) {
            // SDL mode: override dx/dy with SDL-sourced deltas when SDL window is available
            dx = g_sdl_mouse_dx;
            dy = g_sdl_mouse_dy;
            g_sdl_mouse_dx = 0;
            g_sdl_mouse_dy = 0;

            // Also pass through wheel delta for weapon switching/menu scroll actions.
            dz = rf::mouse_dz;
        }

        // Nothing to do in Classic mode or outside gameplay.
        if (!rf::keep_mouse_centered || g_alpine_game_config.mouse_scale == 0) {
            reset_mouse_delta_accumulators();
            return;
        }

        // If the player entity is not valid (dead/spawn transition), pause raw delta.
        // Exception: spectator freelook camera should still receive mouse input.
        if (!rf::local_player_entity || rf::entity_is_dying(rf::local_player_entity)) {
            if (!is_freelook_camera()) {
                reset_mouse_delta_accumulators();
                dx = 0;
                dy = 0;
                return;
            }
        }

        // In Raw/Modern mode: capture raw deltas for centralized angle
        // computation and zero them so RF does not apply its own scaling.
        // Skip zeroing when in a vehicle (RF needs the deltas to steer), but scale
        // them down to stay consistent with the camera formula feel.
        bool in_vehicle = rf::local_player_entity && rf::entity_in_vehicle(rf::local_player_entity);
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
    [](std::optional<int> mode_opt) {
        static constexpr const char* mode_names[] = {"Legacy", "DirectInput", "SDL"};

        if (client_bot_headless_enabled()) {
            set_input_mode(0);
            rf::console::print("Input mode is disabled in headless bot mode");
            return;
        }

        if (!mode_opt) {
            // No argument: print current mode
            int cur = g_alpine_game_config.input_mode;
            rf::console::print("Input mode: {} ({})", cur, mode_names[cur]);
            return;
        }

        int new_mode = std::clamp(mode_opt.value(), 0, 2);
        set_input_mode(new_mode);
        rf::console::print("Input mode: {} ({})", new_mode, mode_names[new_mode]);
    },
    "Gets or sets input mode: 0=Legacy Win32, 1=DirectInput mouse+Legacy keyboard, 2=SDL mouse+keyboard",
    "inputmode [0|1|2]",
};

ConsoleCommand2 ms_cmd{
    "ms",
    [](std::optional<float> value_opt) {
        if (!rf::local_player) return;
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
            if (g_alpine_game_config.mouse_scale == 0) {
                reset_mouse_delta_accumulators();
            }
        }
        static constexpr const char* mode_names[] = {"Classic", "Raw", "Modern"};
        int mode = std::clamp(g_alpine_game_config.mouse_scale, 0, 2);
        rf::console::print("ms_scale: {} ({})", mode, mode_names[mode]);
    },
    "Sets mouse scale mode. 0 = Classic (RF native), 1 = Raw (pure degrees), 2 = Modern (id Tech/Source style).",
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

// Handle an SDL extra mouse button event (Mouse 4-8).
// Maps SDL button indices to custom scan codes and injects them into RF's key layer.
// Only active in SDL input mode (mode 2).
static void handle_extra_mouse_button(const SDL_Event& ev)
{
    if (g_alpine_game_config.input_mode != 2)
        return;

    if (ev.button.button < SDL_BUTTON_X1 ||
        ev.button.button >= SDL_BUTTON_X1 + CTRL_EXTRA_MOUSE_SCAN_COUNT)
        return;

    int rf_btn = static_cast<int>(ev.button.button) - 1; // SDL 4→rf 3, SDL 5→rf 4 ...

    if (ev.button.down && g_pending_mouse_extra_btn_rebind < 0
        && rf::ui::options_controls_waiting_for_key) {
        // Rebind UI active: inject the sentinel key so RF processes the rebind,
        // then ui.cpp's falling-edge handler replaces it with our custom scan code.
        g_pending_mouse_extra_btn_rebind = rf_btn;
        rf::key_process_event(CTRL_REBIND_SENTINEL, 1, 0);
    } else {
        // Inject our custom scan code directly into RF's key state.
        int scan_code = CTRL_EXTRA_MOUSE_SCAN_BASE + (rf_btn - 3);
        rf::key_process_event(scan_code, ev.button.down ? 1 : 0, 0);
    }
}

void mouse_sdl_poll()
{
    if (!g_sdl_window) return;

    SDL_Event events[16];
    int n;
    // Consume motion, button, and wheel events; leave device-change events in the queue.
    while ((n = SDL_PeepEvents(events, static_cast<int>(std::size(events)),
                               SDL_GETEVENT, SDL_EVENT_MOUSE_MOTION,
                               SDL_EVENT_MOUSE_WHEEL)) > 0) {
        for (int i = 0; i < n; ++i) {
            const SDL_Event& ev = events[i];
            switch (ev.type) {
            case SDL_EVENT_MOUSE_MOTION:
                if (ev.motion.which == SDL_TOUCH_MOUSEID || ev.motion.which == SDL_PEN_MOUSEID) {
                    // Touch/pen: move the Win32 cursor to the contact point in menus.
                    // Works in all input modes; ignored during gameplay
                    if (!rf::keep_mouse_centered) {
                        POINT pt{static_cast<LONG>(ev.motion.x), static_cast<LONG>(ev.motion.y)};
                        ClientToScreen(rf::main_wnd, &pt);
                        SetCursorPos(pt.x, pt.y);
                    }
                } else if (g_alpine_game_config.input_mode == 2) {
                    // Real mouse: accumulate relative deltas for the camera (SDL mode only).
                    g_sdl_mouse_dx_rem += ev.motion.xrel;
                    g_sdl_mouse_dy_rem += ev.motion.yrel;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (ev.button.which == SDL_TOUCH_MOUSEID || ev.button.which == SDL_PEN_MOUSEID) {
                    // Touch/pen taps: synthesise a left-click in menus; ignored during gameplay.
                    // Works in all input modes; not part of the binding system.
                    if (!rf::keep_mouse_centered && ev.button.button == SDL_BUTTON_LEFT) {
                        // Sync cursor to the exact tap position first.
                        POINT pt{static_cast<LONG>(ev.button.x), static_cast<LONG>(ev.button.y)};
                        ClientToScreen(rf::main_wnd, &pt);
                        SetCursorPos(pt.x, pt.y);
                        UINT  wmsg = (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) ? WM_LBUTTONDOWN : WM_LBUTTONUP;
                        WPARAM wp  = (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) ? MK_LBUTTON : 0;
                        PostMessageA(rf::main_wnd, wmsg, wp,
                            MAKELPARAM(static_cast<int>(ev.button.x), static_cast<int>(ev.button.y)));
                    }
                } else {
                    handle_extra_mouse_button(ev);
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                // Feed scroll into rf::mouse_wheel_pos (120 units per notch, matching Win32/DInput).
                // SDL y > 0 = scroll up = positive delta, same sign as RF expects.
                if (g_alpine_game_config.input_mode == 2
                    && ev.wheel.which != SDL_TOUCH_MOUSEID
                    && ev.wheel.which != SDL_PEN_MOUSEID) {
                    float dy = (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
                        ? -ev.wheel.y : ev.wheel.y;
                    g_sdl_mouse_wheel_rem += dy;
                    int notches = static_cast<int>(g_sdl_mouse_wheel_rem);
                    if (notches != 0) {
                        rf::mouse_wheel_pos += notches * 120;
                        g_sdl_mouse_wheel_rem -= static_cast<float>(notches);
                    }
                }
                break;
            default:
                break;
            }
        }
    }
}

int mouse_take_pending_rebind()
{
    int btn = g_pending_mouse_extra_btn_rebind;
    g_pending_mouse_extra_btn_rebind = -1;
    return btn;
}

void mouse_init_sdl_window()
{
    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, rf::main_wnd);
    g_sdl_window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!g_sdl_window) {
        xlog::error("SDL_CreateWindowWithProperties failed: {}", SDL_GetError());
        return;
    }
}

// Converts the per-frame raw mouse pixel deltas captured into camera angle deltas (radians).
// Mode 1 (Raw):    pure camera angles — 360 pixels * sens = 360 degree camera turn (angle = raw_pixels * sens * deg2rad)
// Mode 2 (Modern): angle = raw_pixels * sens * 0.022 deg/pixel * deg2rad  (id Tech/Source formula)
// Called from camera.cpp's centralized camera injection point.
void mouse_get_camera(float& pitch_delta, float& yaw_delta)
{
    pitch_delta = 0.0f;
    yaw_delta   = 0.0f;

    const bool has_player_entity = rf::local_player_entity && !rf::entity_is_dying(rf::local_player_entity);
    const bool is_freelook = !has_player_entity && is_freelook_camera();
    if (!rf::local_player || !rf::keep_mouse_centered || (!has_player_entity && !is_freelook)) {
        reset_mouse_delta_accumulators();
        return;
    }

    if (g_camera_mouse_dx == 0 && g_camera_mouse_dy == 0) {
        // No raw mouse movement accumulated
        return;
    }

    float sens = rf::local_player->settings.controls.mouse_sensitivity;
    constexpr float deg2rad = 3.14159265f / 180.0f;
    constexpr float id_tech_deg_per_pixel = 0.022f;
    // Raw (1) is pure degrees; Modern (2) uses id Tech/Source multiplier
    float scale = (g_alpine_game_config.mouse_scale == 1)
        ? deg2rad
        : id_tech_deg_per_pixel * deg2rad;
    // Apply scope/scanner sensitivity modifiers
    if (has_player_entity) {
        if (rf::local_player->fpgun_data.scanning_for_target)
            sens *= scanner_sensitivity_value;
        else if (rf::player_fpgun_is_zoomed(rf::local_player))
            sens *= scope_sensitivity_value;
    }
    // Mouse Y-Invert setting (axes[1].invert) for Raw/Modern modes
    float dy = static_cast<float>(g_camera_mouse_dy);
    if (rf::local_player->settings.controls.axes[1].invert)
        dy = -dy;
    pitch_delta = -dy * sens * scale;
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

    // Scroll-wheel fix and Win32 cursor centering for Legacy/DInput modes (0 and 1)
    mouse_eval_deltas_di_hook.install();

    // Mouse mode hooks (DInput or SDL depending on input_mode)
    mouse_keep_centered_enable_hook.install();
    mouse_keep_centered_disable_hook.install();
    mouse_get_delta_hook.install();

    // Do not limit the cursor to the game window if in menu (Win32 mouse)
    AsmWriter(0x0051DD7C).jmp(0x0051DD8E);

    // Commands
    input_mode_cmd.register_cmd();
    ms_cmd.register_cmd();
    ms_scale_cmd.register_cmd();
    static_scope_sens_cmd.register_cmd();
    scope_sens_cmd.register_cmd();
    scanner_sens_cmd.register_cmd();
}
