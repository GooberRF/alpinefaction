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
#include "../os/os.h"
#include "mouse.h"
#include "../multi/multi.h"

// SDL window and mouse motion state (used in SDL input mode only)
static bool g_relative_mouse_mode_window_missing_logged = false;
static float g_sdl_mouse_dx_rem = 0.0f, g_sdl_mouse_dy_rem = 0.0f;
static float g_sdl_mouse_wheel_rem = 0.0f; // sub-notch accumulator for smooth-scroll devices
static int g_sdl_mouse_dx = 0, g_sdl_mouse_dy = 0;

// Extra mouse button rebind state (Mouse 4-8, used with SDL input mode)
static int g_pending_mouse_extra_btn_rebind = -1;

// Raw mouse delta accumulators — captured in mouse_get_delta_hook, then consumed
// by consume_raw_mouse_deltas() (via linear_pitch_patch for the player entity, or
// directly for the freelook camera) which writes scaled values into RF's control
// pipeline so the controlled entity/camera picks them up.
static int g_camera_mouse_dx = 0, g_camera_mouse_dy = 0;

static bool is_freelook_camera()
{
    return rf::local_player && rf::local_player->cam
        && rf::local_player->cam->mode == rf::CameraMode::CAMERA_FREELOOK;
}

// Sub-pixel remainder accumulators for vehicle mouse sensitivity scaling.
static float g_vehicle_mouse_dx_rem = 0.0f, g_vehicle_mouse_dy_rem = 0.0f;

static float scope_sensitivity_value = 0.25f;
static float scanner_sensitivity_value = 0.25f;

static void reset_mouse_delta_accumulators()
{
    g_camera_mouse_dx = 0;
    g_camera_mouse_dy = 0;
    g_vehicle_mouse_dx_rem = 0.0f;
    g_vehicle_mouse_dy_rem = 0.0f;
}

static float applied_static_sensitivity_value = 0.25f; // value written by AsmWriter
static float applied_dynamic_sensitivity_value = 1.0f; // value written by AsmWriter

// Converts accumulated raw mouse deltas to camera angle deltas (radians).
// For the player entity, this is called from linear_pitch_patch inside the entity
// control function where timing is guaranteed. For the freelook camera, it's called
// from mouse_get_delta_hook since the freelook camera has a separate control path.
void consume_raw_mouse_deltas(float& out_pitch, float& out_yaw, bool apply_scope_sens)
{
    out_pitch = 0.0f;
    out_yaw = 0.0f;

    if (g_camera_mouse_dx == 0 && g_camera_mouse_dy == 0) {
        return;
    }
    if (!rf::local_player) {
        g_camera_mouse_dx = 0;
        g_camera_mouse_dy = 0;
        return;
    }

    float sens = rf::local_player->settings.controls.mouse_sensitivity;
    constexpr float deg2rad = 3.14159265f / 180.0f;
    constexpr float id_tech_deg_per_pixel = 0.022f;
    float scale = (g_alpine_game_config.mouse_scale == 1)
        ? deg2rad
        : id_tech_deg_per_pixel * deg2rad;

    if (apply_scope_sens) {
        if (rf::local_player->fpgun_data.scanning_for_target) {
            sens *= scanner_sensitivity_value;
        } else {
            float zoom = rf::local_player->fpgun_data.zoom_factor;
            if (zoom > 1.0f) {
                if (g_alpine_game_config.scope_static_sensitivity) {
                    // Static: flat multiplier regardless of zoom level
                    sens *= scope_sensitivity_value;
                } else {
                    // Dynamic: proportional to zoom level, matches stock formula
                    constexpr float zoom_scale = 30.0f;
                    float divisor = (zoom - 1.0f) * applied_dynamic_sensitivity_value * zoom_scale;
                    if (divisor > 1.0f) {
                        sens /= divisor;
                    }
                }
            }
        }
    }

    float dy = static_cast<float>(g_camera_mouse_dy);
    if (rf::local_player->settings.controls.axes[1].invert)
        dy = -dy;

    out_pitch = -dy * sens * scale;
    out_yaw = static_cast<float>(g_camera_mouse_dx) * sens * scale;

    g_camera_mouse_dx = 0;
    g_camera_mouse_dy = 0;
}

// For the freelook camera, consume accumulated deltas and write directly to the
// camera entity's control_data fields (read by camera_do_frame). The player entity
// path is handled by linear_pitch_patch inside the entity control function instead.
static void flush_freelook_mouse_deltas()
{
    if (g_camera_mouse_dx == 0 && g_camera_mouse_dy == 0) {
        return;
    }
    if (!is_freelook_camera() || !rf::local_player || !rf::local_player->cam) {
        return; // Not in freelook — deltas consumed by linear_pitch_patch instead
    }
    rf::Entity* cam_entity = rf::local_player->cam->camera_entity;
    if (!cam_entity) {
        return;
    }

    float pitch = 0.0f, yaw = 0.0f;
    consume_raw_mouse_deltas(pitch, yaw, false);
    cam_entity->control_data.eye_phb.x += pitch;
    cam_entity->control_data.phb.y += yaw;
}

bool set_direct_input_enabled(bool enabled)
{
    auto direct_input_initialized = addr_as_ref<bool>(0x01885460);

    if (is_headless_mode()) {
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
        if (g_sdl_window) {
            if (rf::keep_mouse_centered)
                SDL_SetWindowRelativeMouseMode(g_sdl_window, mode == 2);
            else
                SDL_SetWindowMouseGrab(g_sdl_window, g_game_config.wnd_mode == GameConfig::STRETCHED);
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
        if (is_headless_mode()) {
            return;
        }

        if (!rf::os_foreground() && !g_alpine_game_config.background_mouse) {
            g_sdl_mouse_dx_rem = 0.0f;
            g_sdl_mouse_dy_rem = 0.0f;
            return;
        }

        if (g_alpine_game_config.input_mode == 2) {
            g_sdl_mouse_dx = static_cast<int>(g_sdl_mouse_dx_rem);
            g_sdl_mouse_dy = static_cast<int>(g_sdl_mouse_dy_rem);
            g_sdl_mouse_dx_rem -= g_sdl_mouse_dx;
            g_sdl_mouse_dy_rem -= g_sdl_mouse_dy;
        }

        mouse_eval_deltas_hook.call_target();

        // Cursor centering fallback for SDL mode when relative mouse mode is unavailable (e.g. no SDL window)
        if (rf::keep_mouse_centered && g_alpine_game_config.input_mode == 2 && (!g_sdl_window || !SDL_GetWindowRelativeMouseMode(g_sdl_window))) {
            RECT rect{};
            GetClientRect(rf::main_wnd, &rect);
            POINT pt{rect.right / 2, rect.bottom / 2};
            ClientToScreen(rf::main_wnd, &pt);
            SetCursorPos(pt.x, pt.y);
            SDL_FlushEvents(SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_MOTION);
        }
    },
};

FunHook<void()> mouse_eval_deltas_di_hook{
    0x0051DEB0,
    []() {
        if (is_headless_mode()) {
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

FunHook<void(bool)> mouse_set_visible_hook{
    0x0051E680,
    [](bool visible) {
        mouse_set_visible_hook.call_target(visible);
        if (g_sdl_window) {
            if (visible)
                SDL_ShowCursor();
            else
                SDL_HideCursor();
        }
    },
};

FunHook<void()> mouse_keep_centered_enable_hook{
    0x0051E690,
    []() {
        if (is_headless_mode()) {
            rf::keep_mouse_centered = false;
            if (g_sdl_window)
                SDL_SetWindowRelativeMouseMode(g_sdl_window, false);
            set_direct_input_enabled(false);
            return;
        }

        // keep_mouse_centered is still false here; call_target sets it
        if (!rf::keep_mouse_centered && !rf::is_dedicated_server) {
            // Release menu grab when entering gameplay; only set in stretched (borderless) mode.
            if (g_sdl_window && g_game_config.wnd_mode == GameConfig::STRETCHED)
                SDL_SetWindowMouseGrab(g_sdl_window, false);
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
        if (is_headless_mode()) {
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
            // Confine cursor in stretched (borderless) mode; SDL cursor navigation is active in all input modes.
            if (g_sdl_window && g_game_config.wnd_mode == GameConfig::STRETCHED)
                SDL_SetWindowMouseGrab(g_sdl_window, true);
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
            dx = g_sdl_mouse_dx;
            dy = g_sdl_mouse_dy;
            g_sdl_mouse_dx = 0;
            g_sdl_mouse_dy = 0;
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

        // Suppress mouse look while viewing a security camera
        if (rf::local_player && rf::local_player->view_from_handle != -1) {
            reset_mouse_delta_accumulators();
            dx = 0;
            dy = 0;
            return;
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

        // For freelook camera, apply deltas now (its control path doesn't go
        // through linear_pitch_patch). Player entity deltas are consumed later
        // by linear_pitch_patch inside the entity control function.
        flush_freelook_mouse_deltas();
    },
};

ConsoleCommand2 input_mode_cmd{
    "inputmode",
    [](std::optional<int> mode_opt) {
        static constexpr const char* mode_names[] = {"Legacy", "DirectInput", "SDL"};

        if (is_headless_mode()) {
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
        int scan_code = CTRL_EXTRA_MOUSE_SCAN_BASE + (rf_btn - 3);
        rf::key_process_event(scan_code, ev.button.down ? 1 : 0, 0);
    }
}

void mouse_sdl_poll()
{
    if (!g_sdl_window) return;

    if (SDL_IsMainThread())
        SDL_PumpEvents();

    SDL_Event events[16];
    int n;
    while ((n = SDL_PeepEvents(events, static_cast<int>(std::size(events)),
                               SDL_GETEVENT, SDL_EVENT_MOUSE_MOTION,
                               SDL_EVENT_MOUSE_WHEEL)) > 0) {
        for (int i = 0; i < n; ++i) {
            const SDL_Event& ev = events[i];
            switch (ev.type) {
            case SDL_EVENT_MOUSE_MOTION:
                if (ev.motion.which == SDL_TOUCH_MOUSEID || ev.motion.which == SDL_PEN_MOUSEID) {
                    // Touch/pen: sync Win32 cursor position in menus.
                    if (!rf::keep_mouse_centered) {
                        POINT pt{static_cast<LONG>(ev.motion.x), static_cast<LONG>(ev.motion.y)};
                        ClientToScreen(rf::main_wnd, &pt);
                        SetCursorPos(pt.x, pt.y);
                    }
                } else if (g_alpine_game_config.input_mode == 2 && rf::os_foreground()) {
                    g_sdl_mouse_dx_rem += ev.motion.xrel;
                    g_sdl_mouse_dy_rem += ev.motion.yrel;
                }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (ev.button.which == SDL_TOUCH_MOUSEID || ev.button.which == SDL_PEN_MOUSEID) {
                    // Touch/pen taps: synthesise a left-click at the tap position in menus.
                    if (!rf::keep_mouse_centered && ev.button.button == SDL_BUTTON_LEFT) {
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
    if (n < 0)
        xlog::warn("SDL Events error: {}", SDL_GetError());
}

int mouse_take_pending_rebind()
{
    int btn = g_pending_mouse_extra_btn_rebind;
    g_pending_mouse_extra_btn_rebind = -1;
    return btn;
}

void mouse_on_focus_changed(bool focused)
{
    if (!g_sdl_window)
        return;

    // Reset SDL delta accumulators on focus change to prevent stale deltas replaying.
    g_sdl_mouse_dx = 0;
    g_sdl_mouse_dy = 0;
    g_sdl_mouse_dx_rem = 0.0f;
    g_sdl_mouse_dy_rem = 0.0f;

    if (focused) {
        // Gameplay (mode 2): re-enable relative mouse mode; SDL3 may have cleared it on focus loss.
        if (g_alpine_game_config.input_mode == 2 && rf::keep_mouse_centered)
            SDL_SetWindowRelativeMouseMode(g_sdl_window, true);
        // Menu: confine cursor in stretched (borderless) mode to prevent escape to other monitors.
        if (!rf::keep_mouse_centered && g_game_config.wnd_mode == GameConfig::STRETCHED)
            SDL_SetWindowMouseGrab(g_sdl_window, true);
        SDL_FlushEvents(SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_MOTION);
    } else {
        // Show cursor so it's visible in other applications after alt-tab.
        SDL_ShowCursor();
        // Only release grab in stretched (borderless) mode; fullscreen conflicts with D3D11's exclusive swap chain.
        if (g_game_config.wnd_mode == GameConfig::STRETCHED)
            SDL_SetWindowMouseGrab(g_sdl_window, false);
    }
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

    // Sync SDL cursor visibility with the game's own cursor show/hide calls
    mouse_set_visible_hook.install();

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
