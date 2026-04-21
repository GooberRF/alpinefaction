#include <windows.h>
#include <SDL3/SDL.h>
#include <cwchar>
#include <cwctype>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include "../rf/os/os.h"
#include "../rf/multi.h"
#include "../rf/input.h"
#include "../input/input.h"
#include "../misc/alpine_settings.h"
#include "../rf/crt.h"
#include "../main/main.h"
#include "../multi/multi.h"
#include "os.h"
#include "win32_console.h"

#include <timeapi.h>

const char* get_win_msg_name(UINT msg);

static bool g_sdl_video_initialized = false;
SDL_Window* g_sdl_window = nullptr;

FunHook<void()> os_poll_hook{
    0x00524B60,
    []() {
        // Note: When using dedicated server we get WM_PAINT messages all the time
        MSG msg;
        constexpr int limit = 4;
        for (int i = 0; i < limit; ++i) {
            if (!PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
                break;
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
            // xlog::info("msg {}\n", msg.message);
        }

        if (win32_console_is_enabled()) {
            win32_console_poll_input();
        }

        if (g_sdl_video_initialized) {
            keyboard_sdl_poll();
            mouse_sdl_poll();
        }
    },
};

LRESULT WINAPI wnd_proc(HWND wnd_handle, UINT msg, WPARAM w_param, LPARAM l_param)
{
    // extern const char* get_win_msg_name(UINT msg);
    // xlog::trace("{:08x}: msg {} {:x} {:x}", GetTickCount64(), get_win_msg_name(msg), w_param, l_param);
    if (rf::main_wnd && wnd_handle != rf::main_wnd) {
        xlog::warn("Got unknown window in the window procedure: hwnd {} msg {}",
            static_cast<void*>(wnd_handle), msg);
    }

    for (int i = 0; i < rf::num_msg_handlers; ++i) {
        rf::msg_handlers[i](msg, w_param, l_param);
    }

    switch (msg) {
    case WM_ACTIVATE:
        if (is_headless_mode()) {
            // In headless mode, the console window will have focus and WM_ACTIVATE for the
            // hidden game window may report inactive. Keep active state pinned so client
            // simulation/network timing does not fall into background-throttled behavior.
            rf::is_main_wnd_active = true;
            return 0;
        }

        if (g_sdl_video_initialized)
            mouse_on_focus_changed(w_param != 0);

        rf::is_main_wnd_active = w_param;
        return 0; //DefWindowProcA(wnd_handle, msg, w_param, l_param);

    case WM_WINDOWPOSCHANGING:
        if (is_headless_mode() && l_param) {
            // Prevent any late startup/system path from re-showing the hidden client window.
            auto* wp = reinterpret_cast<WINDOWPOS*>(l_param);
            wp->flags &= ~SWP_SHOWWINDOW;
            wp->flags |= SWP_HIDEWINDOW | SWP_NOACTIVATE;
        }
        return DefWindowProcA(wnd_handle, msg, w_param, l_param);

    case WM_SHOWWINDOW:
        if (is_headless_mode() && w_param) {
            return 0;
        }
        return DefWindowProcA(wnd_handle, msg, w_param, l_param);

    case WM_SYSCOMMAND:
        if ((w_param & 0xFFF0) == SC_KEYMENU)
            return 0;
        return DefWindowProcA(wnd_handle, msg, w_param, l_param);

    case WM_QUIT:
    case WM_CLOSE:
    case WM_DESTROY:
        rf::close_app_req = 1;
        break;

    case WM_PAINT:
        if (rf::is_dedicated_server)
            ++rf::console_redraw_counter;
        return DefWindowProcA(wnd_handle, msg, w_param, l_param);

    default:
        return DefWindowProcA(wnd_handle, msg, w_param, l_param);
    }

    return 0;
}

static FunHook<void(const char *, const char *, bool, bool)> os_init_window_server_hook{
    0x00524B70,
    [](const char *wclass, const char *title, bool hooks, bool server_console) {
        const bool bot_headless = is_headless_mode();
        win32_console_set_forced(bot_headless);
        if (server_console || bot_headless) {
            win32_console_init();
        }
        if (server_console && win32_console_is_enabled()) {
            return;
        }

        os_init_window_server_hook.call_target(wclass, title, hooks, server_console);

        if (bot_headless && rf::main_wnd) {
            // Keep client-side message/window plumbing (for networking reliability),
            // but do not show the regular game window in headless mode.
            LONG wnd_style = GetWindowLongA(rf::main_wnd, GWL_STYLE);
            if (wnd_style & WS_VISIBLE) {
                SetWindowLongA(rf::main_wnd, GWL_STYLE, wnd_style & ~WS_VISIBLE);
            }
            SetWindowPos(
                rf::main_wnd,
                nullptr,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW
            );
            ShowWindow(rf::main_wnd, SW_HIDE);
            rf::is_main_wnd_active = true;
            if (HWND console_wnd = GetConsoleWindow()) {
                SetActiveWindow(console_wnd);
                SetForegroundWindow(console_wnd);
            }
        }
    },
};

bool cmdline_token_equals_ci(const wchar_t* token_begin, std::size_t token_len, const wchar_t* expected)
{
    if (!token_begin || !expected || !*expected) {
        return false;
    }

    const auto expected_len = std::wcslen(expected);
    if (token_len == expected_len) {
        return _wcsnicmp(token_begin, expected, expected_len) == 0;
    }
    if (token_len > expected_len && _wcsnicmp(token_begin, expected, expected_len) == 0) {
        const wchar_t suffix_delim = token_begin[expected_len];
        return suffix_delim == L'=' || suffix_delim == L':';
    }
    return false;
}

bool cmdline_has_switch_token(const wchar_t* cmdline, const wchar_t* switch_name)
{
    if (!cmdline || !*cmdline || !switch_name || !*switch_name) {
        return false;
    }

    const wchar_t* p = cmdline;
    while (*p) {
        while (*p && std::iswspace(*p)) {
            ++p;
        }
        if (!*p) {
            break;
        }

        const bool quoted = (*p == L'"');
        if (quoted) {
            ++p;
        }

        const wchar_t* token_begin = p;
        while (*p && ((quoted && *p != L'"') || (!quoted && !std::iswspace(*p)))) {
            ++p;
        }
        const std::size_t token_len = static_cast<std::size_t>(p - token_begin);
        if (cmdline_token_equals_ci(token_begin, token_len, switch_name)) {
            return true;
        }

        if (quoted && *p == L'"') {
            ++p;
        }
    }
    return false;
}

bool raw_command_line_has_switch(const wchar_t* switch_name)
{
    const wchar_t* cmdline = GetCommandLineW();
    return cmdline_has_switch_token(cmdline, switch_name);
}

bool is_client_bot_requested_from_cmdline()
{
    if (rf::is_dedicated_server) {
        return false;
    }
    return raw_command_line_has_switch(L"-bot");
}

bool is_client_debugbot_requested_from_cmdline()
{
    if (rf::is_dedicated_server) {
        return false;
    }
    return raw_command_line_has_switch(L"-debugbot");
}

bool headless_bot_requested_from_raw_cmdline()
{
    const bool has_bot = raw_command_line_has_switch(L"-bot");
    const bool has_debugbot = raw_command_line_has_switch(L"-debugbot");
    return has_bot && !has_debugbot;
}

bool awpgen_requested_from_raw_cmdline()
{
    return raw_command_line_has_switch(L"-awpgen");
}

bool headless_requested_from_raw_cmdline()
{
    return headless_bot_requested_from_raw_cmdline() || awpgen_requested_from_raw_cmdline();
}

static FunHook<void()> os_close_hook{
    0x00525240,
    []() {
        os_close_hook.call_target();
        win32_console_close();
        SDL_Quit();
    },
};

static FunHook<void(char*, bool)> os_parse_params_hook{
    0x00523320,
    [](char *cmdline, bool skip_first) {
        std::string buf;
        bool quote = false;
        while (true) {
            char c = *cmdline;
            ++cmdline;

            if ((!quote && c == ' ') || c == '\0') {
                if (skip_first) {
                    skip_first = false;
                } else {
                    rf::CmdArg &cmd_arg = rf::cmdline_args[rf::cmdline_num_args++];
                    cmd_arg.arg = static_cast<char*>(rf::operator_new(buf.size() + 1));
                    std::strcpy(cmd_arg.arg, buf.c_str());
                    cmd_arg.is_done = false;
                }
                buf.clear();
                if (!c) {
                    break;
                }
            } else if (c == '"') {
                quote = !quote;
            } else {
                buf += c;
            }
        }
    },
};

void wait_for(const float ms, const WaitableTimer& timer) {
    if (ms <= .0f) {
        return;
    }

    if (!timer.handle) {
    SLEEP:
        static const MMRESULT res = timeBeginPeriod(1);
        if (res != TIMERR_NOERROR) {
            ERR_ONCE(
                "The frame rate may be unstable, because `timeBeginPeriod` failed ({})",
                res
            );
        }
        Sleep(static_cast<DWORD>(ms));
    } else {
        // SetWaitableTimer requires 100-nanosecond intervals.
        // Negative values indicate relative time.
        LARGE_INTEGER dur{
            .QuadPart = -static_cast<LONGLONG>(static_cast<double>(ms) * 10'000.)
        };

        if (!SetWaitableTimer(timer.handle, &dur, 0, nullptr, nullptr, FALSE)) {
            ERR_ONCE("`SetWaitableTimer` in `wait_for` failed ({})", GetLastError());
            goto SLEEP;
        }

        if (WaitForSingleObject(timer.handle, INFINITE) != WAIT_OBJECT_0) {
            ERR_ONCE("`WaitForSingleObject` in `wait_for` failed ({})", GetLastError());
            goto SLEEP;
        }
    }
}

void os_init_sdl_video()
{
    if (rf::is_dedicated_server)
        return;
    g_sdl_video_initialized = SDL_Init(SDL_INIT_VIDEO);
    if (!g_sdl_video_initialized)
        xlog::error("SDL_Init(SDL_INIT_VIDEO) failed: {}", SDL_GetError());
}

void os_init_sdl_window()
{
    // Deferred after D3D device creation so SDL's hidden helper window does not
    // interfere with exclusive fullscreen acquisition.
    os_init_sdl_video();

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER, rf::main_wnd);
    g_sdl_window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (!g_sdl_window) {
        xlog::error("SDL_CreateWindowWithProperties failed: {}", SDL_GetError());
        return;
    }
}

void os_apply_patch()
{
    if (!is_d3d11()) {
        // Fix bitmap scaling on legacy D3D8/9 renderer
        if (auto* set_dpi_ctx = reinterpret_cast<BOOL(WINAPI*)(HANDLE)>(
                GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext"))) {
            set_dpi_ctx(reinterpret_cast<HANDLE>(-1)); // DPI_AWARENESS_CONTEXT_UNAWARE
        }
    }

    // SDL video and window init is deferred to os_init_sdl_window, after D3D device creation,
    // to avoid SDL's hidden helper window interfering with exclusive fullscreen acquisition.

    // Process messages in the same thread as DX processing (alternative: D3DCREATE_MULTITHREADED)
    AsmWriter(0x00524C48, 0x00524C83).nop(); // disable msg loop thread
    AsmWriter(0x00524C48).call(0x00524E40);  // os_create_main_window
    os_poll_hook.install();

    // Subclass window
    write_mem_ptr(0x00524E66, &wnd_proc);

    // Disable keyboard hooks (they were supposed to block alt-tab; they does not work in modern OSes anyway)
    write_mem<u8>(0x00524C98, asm_opcodes::jmp_rel_short);

    // Support `rf::close_app_req` for clients in addition to dedicated servers.
    AsmWriter{0x004B2DE2}.nop(2);

    // Hooks for win32 console support
    os_init_window_server_hook.install();
    os_close_hook.install();

    // Fix quotes support in cmdline parsing
    os_parse_params_hook.install();

    // Apply patches from other files in 'os' dir
    void frametime_apply_patch();
    void timer_apply_patch();
    frametime_apply_patch();
    timer_apply_patch();

    win32_console_pre_init();
}
