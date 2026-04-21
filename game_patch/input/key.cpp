#include <cctype>
#include <string>
#include <SDL3/SDL.h>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include "../hud/multi_spectate.h"
#include "../hud/hud.h"
#include "../misc/player.h"
#include "../misc/achievements.h"
#include "../misc/alpine_settings.h"
#include "../misc/waypoints_utils.h"
#include "../multi/multi.h"
#include "../multi/endgame_votes.h"
#include "input.h"
#include "../rf/input.h"
#include "../rf/entity.h"
#include "../rf/multi.h"
#include "../rf/gameseq.h"
#include "../rf/player/control_config.h"
#include "../rf/player/player.h"
#include "../rf/os/console.h"
#include "../rf/os/os.h"
#include "../rf/ui.h"
#include "../multi/alpine_packets.h"
#include "../os/console.h"

static int starting_alpine_control_index = -1;
static int g_pending_extra_key_rebind = -1; // pending scan code for sentinel rebind pattern
static std::string g_sdl_pending_text;

rf::String get_action_bind_name(int action)
{
    auto& config_item = rf::local_player->settings.controls.bindings[action];
    rf::String name;
    if (config_item.scan_codes[0] >= 0) {
        rf::control_config_get_key_name(&name, config_item.scan_codes[0]);
    }
    else if (config_item.mouse_btn_id >= 0) {
        rf::control_config_get_mouse_button_name(&name, config_item.mouse_btn_id);
    }
    else {
        name = "?";
    }
    return name;
}

rf::ControlConfigAction get_af_control(rf::AlpineControlConfigAction alpine_control)
{
    return static_cast<rf::ControlConfigAction>(starting_alpine_control_index + static_cast<int>(alpine_control));
}

static SDL_Scancode rf_key_to_sdl_scancode(int key); // defined below

FunHook<int(int16_t)> key_to_ascii_hook{
    0x0051EFC0,
    [](int16_t key) {
        using namespace rf;
        constexpr int empty_result = 0xFF;
        if (!key)
            return empty_result;

        // Numpad arithmetic keys: same in all modes
        switch (key & KEY_MASK) {
            case KEY_PADMULTIPLY: return static_cast<int>('*');
            case KEY_PADMINUS:    return static_cast<int>('-');
            case KEY_PADPLUS:     return static_cast<int>('+');
            case KEY_PADENTER:    return empty_result; // game not prepared for newline from numpad
            case CTRL_EXTRA_KEY_SCAN_BASE + EXTRA_KEY_KP_DIVIDE: return static_cast<int>('/');
        }

        if (g_alpine_game_config.input_mode == 2) {
            SDL_Keymod sdl_mods = SDL_GetModState();

            // Treat pure Ctrl shortcuts as non-text, but allow AltGr (often Ctrl+Alt or SDL_KMOD_MODE)
            if (key & KEY_CTRLED) {
                bool is_altgr =
                    (sdl_mods & SDL_KMOD_MODE) != 0 ||
                    ((sdl_mods & SDL_KMOD_CTRL) && (sdl_mods & SDL_KMOD_ALT));
                if (!is_altgr)
                    return empty_result;
            }

            if (sdl_mods & SDL_KMOD_NUM) {
                switch (key & KEY_MASK) {
                    case KEY_PAD7: return static_cast<int>('7');
                    case KEY_PAD8: return static_cast<int>('8');
                    case KEY_PAD9: return static_cast<int>('9');
                    case KEY_PAD4: return static_cast<int>('4');
                    case KEY_PAD5: return static_cast<int>('5');
                    case KEY_PAD6: return static_cast<int>('6');
                    case KEY_PAD1: return static_cast<int>('1');
                    case KEY_PAD2: return static_cast<int>('2');
                    case KEY_PAD3: return static_cast<int>('3');
                    case KEY_PAD0: return static_cast<int>('0');
                    case KEY_PADPERIOD: return static_cast<int>('.');
                }
            }

            SDL_Scancode sc = rf_key_to_sdl_scancode(key);
            if (sc == SDL_SCANCODE_UNKNOWN)
                return empty_result;

            SDL_Keymod mods = SDL_KMOD_NONE;
            if (key & KEY_SHIFTED) mods |= SDL_KMOD_SHIFT;
            if (key & KEY_ALTED)   mods |= SDL_KMOD_RALT; // AltGr on most non-US layouts

            SDL_Keycode kc = SDL_GetKeyFromScancode(sc, mods, false);
            if (kc == SDLK_UNKNOWN || kc < 0x20 || kc > 0x7E)
                return empty_result;

            return static_cast<int>(kc);
        } else {
            // Note: broken on Wine with non-US layout (MAPVK_VSC_TO_VK_EX mapping issue)
            if (GetKeyState(VK_NUMLOCK) & 1) {
                switch (key & KEY_MASK) {
                    case KEY_PAD7: return static_cast<int>('7');
                    case KEY_PAD8: return static_cast<int>('8');
                    case KEY_PAD9: return static_cast<int>('9');
                    case KEY_PAD4: return static_cast<int>('4');
                    case KEY_PAD5: return static_cast<int>('5');
                    case KEY_PAD6: return static_cast<int>('6');
                    case KEY_PAD1: return static_cast<int>('1');
                    case KEY_PAD2: return static_cast<int>('2');
                    case KEY_PAD3: return static_cast<int>('3');
                    case KEY_PAD0: return static_cast<int>('0');
                    case KEY_PADPERIOD: return static_cast<int>('.');
                }
            }
            BYTE key_state[256] = {0};
            if (key & KEY_SHIFTED) key_state[VK_SHIFT]   = 0x80;
            if (key & KEY_ALTED)   key_state[VK_MENU]    = 0x80;
            if (key & KEY_CTRLED)  key_state[VK_CONTROL] = 0x80;
            int scan_code = key & 0x7F;
            auto vk = MapVirtualKeyA(scan_code, MAPVK_VSC_TO_VK);
            WCHAR unicode_chars[3];
            auto num_unicode_chars = ToUnicode(vk, scan_code, key_state, unicode_chars, std::size(unicode_chars), 0);
            if (num_unicode_chars < 1)
                return empty_result;
            if (static_cast<char16_t>(unicode_chars[0]) >= 0x80 || !std::isprint(unicode_chars[0]))
                return empty_result;
            return static_cast<int>(unicode_chars[0]);
        }
    },
};

static SDL_Scancode rf_key_to_sdl_scancode(int key)
{
    using namespace rf;
    switch (key & KEY_MASK) {
        case KEY_ESC:          return SDL_SCANCODE_ESCAPE;
        case KEY_1:            return SDL_SCANCODE_1;
        case KEY_2:            return SDL_SCANCODE_2;
        case KEY_3:            return SDL_SCANCODE_3;
        case KEY_4:            return SDL_SCANCODE_4;
        case KEY_5:            return SDL_SCANCODE_5;
        case KEY_6:            return SDL_SCANCODE_6;
        case KEY_7:            return SDL_SCANCODE_7;
        case KEY_8:            return SDL_SCANCODE_8;
        case KEY_9:            return SDL_SCANCODE_9;
        case KEY_0:            return SDL_SCANCODE_0;
        case KEY_MINUS:        return SDL_SCANCODE_MINUS;
        case KEY_EQUAL:        return SDL_SCANCODE_EQUALS;
        case KEY_BACKSP:       return SDL_SCANCODE_BACKSPACE;
        case KEY_TAB:          return SDL_SCANCODE_TAB;
        case KEY_Q:            return SDL_SCANCODE_Q;
        case KEY_W:            return SDL_SCANCODE_W;
        case KEY_E:            return SDL_SCANCODE_E;
        case KEY_R:            return SDL_SCANCODE_R;
        case KEY_T:            return SDL_SCANCODE_T;
        case KEY_Y:            return SDL_SCANCODE_Y;
        case KEY_U:            return SDL_SCANCODE_U;
        case KEY_I:            return SDL_SCANCODE_I;
        case KEY_O:            return SDL_SCANCODE_O;
        case KEY_P:            return SDL_SCANCODE_P;
        case KEY_LBRACKET:     return SDL_SCANCODE_LEFTBRACKET;
        case KEY_RBRACKET:     return SDL_SCANCODE_RIGHTBRACKET;
        case KEY_ENTER:        return SDL_SCANCODE_RETURN;
        case KEY_LCTRL:        return SDL_SCANCODE_LCTRL;
        case KEY_A:            return SDL_SCANCODE_A;
        case KEY_S:            return SDL_SCANCODE_S;
        case KEY_D:            return SDL_SCANCODE_D;
        case KEY_F:            return SDL_SCANCODE_F;
        case KEY_G:            return SDL_SCANCODE_G;
        case KEY_H:            return SDL_SCANCODE_H;
        case KEY_J:            return SDL_SCANCODE_J;
        case KEY_K:            return SDL_SCANCODE_K;
        case KEY_L:            return SDL_SCANCODE_L;
        case KEY_SEMICOL:      return SDL_SCANCODE_SEMICOLON;
        case KEY_RAPOSTRO:     return SDL_SCANCODE_APOSTROPHE;
        case KEY_LAPOSTRO_DBG: return SDL_SCANCODE_GRAVE;
        case KEY_LSHIFT:       return SDL_SCANCODE_LSHIFT;
        case KEY_SLASH:        return SDL_SCANCODE_BACKSLASH;
        case KEY_Z:            return SDL_SCANCODE_Z;
        case KEY_X:            return SDL_SCANCODE_X;
        case KEY_C:            return SDL_SCANCODE_C;
        case KEY_V:            return SDL_SCANCODE_V;
        case KEY_B:            return SDL_SCANCODE_B;
        case KEY_N:            return SDL_SCANCODE_N;
        case KEY_M:            return SDL_SCANCODE_M;
        case KEY_COMMA:        return SDL_SCANCODE_COMMA;
        case KEY_PERIOD:       return SDL_SCANCODE_PERIOD;
        case KEY_DIVIDE:       return SDL_SCANCODE_SLASH;
        case KEY_RSHIFT:       return SDL_SCANCODE_RSHIFT;
        case KEY_PADMULTIPLY:  return SDL_SCANCODE_KP_MULTIPLY;
        case KEY_LALT:         return SDL_SCANCODE_LALT;
        case KEY_SPACEBAR:     return SDL_SCANCODE_SPACE;
        case KEY_CAPSLOCK:     return SDL_SCANCODE_CAPSLOCK;
        case KEY_F1:           return SDL_SCANCODE_F1;
        case KEY_F2:           return SDL_SCANCODE_F2;
        case KEY_F3:           return SDL_SCANCODE_F3;
        case KEY_F4:           return SDL_SCANCODE_F4;
        case KEY_F5:           return SDL_SCANCODE_F5;
        case KEY_F6:           return SDL_SCANCODE_F6;
        case KEY_F7:           return SDL_SCANCODE_F7;
        case KEY_F8:           return SDL_SCANCODE_F8;
        case KEY_F9:           return SDL_SCANCODE_F9;
        case KEY_F10:          return SDL_SCANCODE_F10;
        case KEY_PAUSE:        return SDL_SCANCODE_PAUSE;
        case KEY_SCROLLLOCK:   return SDL_SCANCODE_SCROLLLOCK;
        case KEY_PAD7:         return SDL_SCANCODE_KP_7;
        case KEY_PAD8:         return SDL_SCANCODE_KP_8;
        case KEY_PAD9:         return SDL_SCANCODE_KP_9;
        case KEY_PADMINUS:     return SDL_SCANCODE_KP_MINUS;
        case KEY_PAD4:         return SDL_SCANCODE_KP_4;
        case KEY_PAD5:         return SDL_SCANCODE_KP_5;
        case KEY_PAD6:         return SDL_SCANCODE_KP_6;
        case KEY_PADPLUS:      return SDL_SCANCODE_KP_PLUS;
        case KEY_PAD1:         return SDL_SCANCODE_KP_1;
        case KEY_PAD2:         return SDL_SCANCODE_KP_2;
        case KEY_PAD3:         return SDL_SCANCODE_KP_3;
        case KEY_PAD0:         return SDL_SCANCODE_KP_0;
        case KEY_PADPERIOD:    return SDL_SCANCODE_KP_PERIOD;
        case KEY_F11:          return SDL_SCANCODE_F11;
        case KEY_F12:          return SDL_SCANCODE_F12;
        case KEY_PADENTER:     return SDL_SCANCODE_KP_ENTER;
        case KEY_RCTRL:        return SDL_SCANCODE_RCTRL;
        case KEY_PRINT_SCRN:   return SDL_SCANCODE_PRINTSCREEN;
        case KEY_RALT:         return SDL_SCANCODE_RALT;
        case KEY_NUMLOCK:      return SDL_SCANCODE_NUMLOCKCLEAR;
        case KEY_BREAK:        return SDL_SCANCODE_PAUSE;
        case KEY_HOME:         return SDL_SCANCODE_HOME;
        case KEY_UP:           return SDL_SCANCODE_UP;
        case KEY_PAGEUP:       return SDL_SCANCODE_PAGEUP;
        case KEY_LEFT:         return SDL_SCANCODE_LEFT;
        case KEY_RIGHT:        return SDL_SCANCODE_RIGHT;
        case KEY_END:          return SDL_SCANCODE_END;
        case KEY_DOWN:         return SDL_SCANCODE_DOWN;
        case KEY_PAGEDOWN:     return SDL_SCANCODE_PAGEDOWN;
        case KEY_INSERT:       return SDL_SCANCODE_INSERT;
        case KEY_DELETE:       return SDL_SCANCODE_DELETE;
        // Extra keyboard keys (custom scan codes not in RF's original DInput table)
        case CTRL_EXTRA_KEY_SCAN_BASE + EXTRA_KEY_KP_DIVIDE:      return SDL_SCANCODE_KP_DIVIDE;
        case CTRL_EXTRA_KEY_SCAN_BASE + EXTRA_KEY_NONUSBACKSLASH:  return SDL_SCANCODE_NONUSBACKSLASH;
        case CTRL_EXTRA_KEY_SCAN_BASE + EXTRA_KEY_F13:             return SDL_SCANCODE_F13;
        case CTRL_EXTRA_KEY_SCAN_BASE + EXTRA_KEY_F14:             return SDL_SCANCODE_F14;
        case CTRL_EXTRA_KEY_SCAN_BASE + EXTRA_KEY_F15:             return SDL_SCANCODE_F15;
        case CTRL_EXTRA_KEY_SCAN_BASE + EXTRA_KEY_F16:             return SDL_SCANCODE_F16;
        case CTRL_EXTRA_KEY_SCAN_BASE + EXTRA_KEY_F17:             return SDL_SCANCODE_F17;
        case CTRL_EXTRA_KEY_SCAN_BASE + EXTRA_KEY_F18:             return SDL_SCANCODE_F18;
        case CTRL_EXTRA_KEY_SCAN_BASE + EXTRA_KEY_F19:             return SDL_SCANCODE_F19;
        case CTRL_EXTRA_KEY_SCAN_BASE + EXTRA_KEY_F20:             return SDL_SCANCODE_F20;
        case CTRL_EXTRA_KEY_SCAN_BASE + EXTRA_KEY_F21:             return SDL_SCANCODE_F21;
        case CTRL_EXTRA_KEY_SCAN_BASE + EXTRA_KEY_F22:             return SDL_SCANCODE_F22;
        case CTRL_EXTRA_KEY_SCAN_BASE + EXTRA_KEY_F23:             return SDL_SCANCODE_F23;
        case CTRL_EXTRA_KEY_SCAN_BASE + EXTRA_KEY_F24:             return SDL_SCANCODE_F24;
        default:               return SDL_SCANCODE_UNKNOWN;
    }
}

static rf::Key sdl_scancode_to_rf_key(SDL_Scancode sc)
{
    static rf::Key table[SDL_SCANCODE_COUNT] = {};
    static bool built = false;
    if (!built) {
        for (int k = 1; k <= static_cast<int>(rf::KEY_MASK); ++k) {
            SDL_Scancode mapped = rf_key_to_sdl_scancode(k);
            if (mapped != SDL_SCANCODE_UNKNOWN && table[mapped] == rf::KEY_NONE) {
                table[mapped] = static_cast<rf::Key>(k);
            }
        }
        built = true;
    }
    if (sc == SDL_SCANCODE_UNKNOWN || static_cast<int>(sc) >= SDL_SCANCODE_COUNT)
        return rf::KEY_NONE;
    return table[static_cast<int>(sc)];
}

// Returns true for scan codes that RF's rebind UI ignores (extra keys + Alt).
static bool scan_needs_rebind_sentinel(int scan)
{
    if (scan >= CTRL_EXTRA_KEY_SCAN_BASE &&
        scan < CTRL_EXTRA_KEY_SCAN_BASE + CTRL_EXTRA_KEY_SCAN_COUNT)
        return true;
    return scan == rf::KEY_LALT || scan == rf::KEY_RALT;
}

void keyboard_sdl_poll()
{
    if (SDL_IsMainThread())
        SDL_PumpEvents();

    SDL_Event events[16];
    int n;
    while ((n = SDL_PeepEvents(events, static_cast<int>(std::size(events)),
                               SDL_GETEVENT, SDL_EVENT_KEY_DOWN, SDL_EVENT_TEXT_EDITING_CANDIDATES)) > 0) {
        for (int i = 0; i < n; ++i) {
            const auto& evt = events[i];
            switch (evt.type) {
            case SDL_EVENT_TEXT_INPUT:
                // Always buffer text — on-screen keyboard works in all input modes.
                g_sdl_pending_text += evt.text.text;
                break;
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                if (g_alpine_game_config.input_mode != 2)
                    break; // Win32 keyboard handles key events in modes 0/1
                if (evt.key.repeat)
                    break; // ignore OS key repeat; RF tracks state itself
                const bool down = (evt.type == SDL_EVENT_KEY_DOWN);
                const rf::Key rf_key = sdl_scancode_to_rf_key(evt.key.scancode);
                if (rf_key == rf::KEY_NONE)
                    break;
                int scan = static_cast<int>(rf_key);
                // Keys that RF's rebind UI can't handle use the sentinel pattern.
                if (scan_needs_rebind_sentinel(scan) && down && g_pending_extra_key_rebind < 0 &&
                    rf::ui::options_controls_waiting_for_key) {
                    g_pending_extra_key_rebind = scan;
                    rf::key_process_event(CTRL_REBIND_SENTINEL, 1, 0);
                } else {
                    rf::key_process_event(scan, down ? 1 : 0, 0);
                }
                break;
            }
            default:
                break;
            }
        }
    }
    if (n < 0)
        xlog::warn("SDL Events error: {}", SDL_GetError());
}

int key_take_pending_extra_rebind()
{
    int sc = g_pending_extra_key_rebind;
    g_pending_extra_key_rebind = -1;
    return sc;
}

std::string keyboard_take_pending_text()
{
    return std::exchange(g_sdl_pending_text, {});
}

int get_key_name(int key, char* buf, size_t buf_len)
{
    // Extra mouse buttons (Mouse 4+)
    if (key >= CTRL_EXTRA_MOUSE_SCAN_BASE && key < CTRL_EXTRA_MOUSE_SCAN_BASE + CTRL_EXTRA_MOUSE_SCAN_COUNT) {
        int n = std::snprintf(buf, buf_len, "Mouse %d", (key - CTRL_EXTRA_MOUSE_SCAN_BASE) + 4);
        return n > 0 ? n : 0;
    }
    // Extra keyboard keys
    if (key >= CTRL_EXTRA_KEY_SCAN_BASE && key < CTRL_EXTRA_KEY_SCAN_BASE + CTRL_EXTRA_KEY_SCAN_COUNT) {
        static const char* names[] = {
            "Keypad /", "ISO \\",
            "F13", "F14", "F15", "F16", "F17", "F18",
            "F19", "F20", "F21", "F22", "F23", "F24",
        };
        int n = std::snprintf(buf, buf_len, "%s", names[key - CTRL_EXTRA_KEY_SCAN_BASE]);
        return n > 0 ? n : 0;
    }
    if (g_alpine_game_config.input_mode == 2) {
        SDL_Scancode sc = rf_key_to_sdl_scancode(key);
        if (sc == SDL_SCANCODE_UNKNOWN) {
            buf[0] = '\0';
            return 0;
        }
        const char* name = SDL_GetKeyName(SDL_GetKeyFromScancode(sc, SDL_KMOD_NONE, false));
        if (!name || name[0] == '\0') {
            buf[0] = '\0';
            return 0;
        }
        SDL_strlcpy(buf, name, buf_len);
        return static_cast<int>(SDL_strlen(name));
    }
    // Modes 0/1: use Win32 key names
    // Note: it seems broken on Wine with non-US layout due to MAPVK_VSC_TO_VK_EX mapping
    LONG lparam = (key & 0x7F) << 16;
    if (key & 0x80) {
        lparam |= 1 << 24;
    }
    int ret = GetKeyNameTextA(lparam, buf, buf_len);
    if (ret <= 0) {
        WARN_ONCE("GetKeyNameTextA failed for 0x{:X}", key);
        buf[0] = '\0';
    }
    else {
        xlog::trace("key 0x{:x} name {}", key, buf);
    }
    return ret;
}

FunHook<int(rf::String&, int)> get_key_name_hook{
    0x0043D930,
    [](rf::String& out_name, int key) {
        static char buf[32] = "";
        int result = 0;
        if (key < 0 || get_key_name(key, buf, std::size(buf)) <= 0) {
            result = -1;
        }
        out_name = buf;
        return result;
    },
};

CodeInjection key_name_in_options_patch{
    0x00450328,
    [](auto& regs) {
        static char buf[32];
        int key = regs.edx;
        get_key_name(key, buf, std::size(buf));
        regs.edi = buf;
        regs.eip = 0x0045032F;
    },
};

FunHook<rf::Key()> key_get_hook{
    0x0051F000,
    [] {
        // Process messages here because when watching videos main loop is not running
        rf::os_poll();

        const rf::Key key = key_get_hook.call_target();

        if (rf::close_app_req) {
            goto MAYBE_CANCEL_BINK;
        }

        if ((key & rf::KEY_MASK) == rf::KEY_ESC
            && key & rf::KEY_SHIFTED
            && g_alpine_game_config.quick_exit) {
            rf::gameseq_set_state(rf::GameState::GS_QUITING, false);
        MAYBE_CANCEL_BINK:
            // If we are playing a video, cancel it.
            const int bink_handle = addr_as_ref<int>(0x018871E4);
            return bink_handle ? rf::KEY_ESC : rf::KEY_NONE;
        }

        return key;
    }
};

ConsoleCommand2 key_quick_exit_cmd{
    "key_quick_exit",
    [] {
        g_alpine_game_config.quick_exit =
            !g_alpine_game_config.quick_exit;
        rf::console::print(
            "Shift+Esc to quit out of Red Faction is {}",
            g_alpine_game_config.quick_exit ? "enabled" : "disabled"
        );
    },
    "Toggle Shift+Esc to quit out of Red Faction",
};

void alpine_control_config_add_item(rf::ControlConfig* config, const char* name, bool press_mode,
    int16_t key1, int16_t key2, int16_t mouse_button, rf::AlpineControlConfigAction alpine_control)
{
    if (config->num_bindings >= 128) {
        return; // hard upper limit
    }

    int binding_index = starting_alpine_control_index + static_cast<int>(alpine_control);

    // Reference the current binding for clarity
    auto& binding = config->bindings[binding_index];

    // Set initial binding values
    binding.scan_codes[0] = key1;
    binding.scan_codes[1] = key2;
    binding.mouse_btn_id = mouse_button;
    binding.press_mode = press_mode;
    binding.name = name;

    // set "Factory Default" binding values
    binding.default_scan_codes[0] = key1;
    binding.default_scan_codes[1] = key2;
    binding.default_mouse_btn_id = mouse_button;    

    // Increment num_bindings (for control indices)
    config->num_bindings++;

    //xlog::warn("added {}, {}, {}", binding.name.c_str(), binding_index, config->bindings[binding_index].name);

    return;
}

// add new controls after all stock ones
CodeInjection control_config_init_patch{
    0x0043D329,
    [] (auto& regs) {
        rf::ControlConfig* ccp = regs.esi;

        // set the starting index for Alpine controls
        // needed because some mods have a different number of weapons, so we can't hardcode the indices
        // overall limit on number of controls (stock + weapons + alpine) is 128
        if (starting_alpine_control_index == -1) {
            starting_alpine_control_index = ccp->num_bindings;
        }

        alpine_control_config_add_item(ccp, "Toggle Headlamp", 0, rf::KEY_F, -1, -1,
                                       rf::AlpineControlConfigAction::AF_ACTION_FLASHLIGHT);
        alpine_control_config_add_item(ccp, "Skip Cutscene", 0, rf::KEY_L, -1, -1,
                                       rf::AlpineControlConfigAction::AF_ACTION_SKIP_CUTSCENE);
        alpine_control_config_add_item(ccp, "Respawn", 0, rf::KEY_K, -1, -1,
                                       rf::AlpineControlConfigAction::AF_ACTION_SELF_KILL);
        alpine_control_config_add_item(ccp, "Vote Yes", 0, rf::KEY_F1, -1, -1,
                                       rf::AlpineControlConfigAction::AF_ACTION_VOTE_YES);
        alpine_control_config_add_item(ccp, "Vote No", 0, rf::KEY_F2, -1, -1,
                                       rf::AlpineControlConfigAction::AF_ACTION_VOTE_NO);
        alpine_control_config_add_item(ccp, "Ready For Match", 0, rf::KEY_F3, -1, -1,
                                       rf::AlpineControlConfigAction::AF_ACTION_READY);
        alpine_control_config_add_item(ccp, "Drop Flag", 0, rf::KEY_G, -1, -1,
                                       rf::AlpineControlConfigAction::AF_ACTION_DROP_FLAG);
        alpine_control_config_add_item(ccp, "Radio Message Menu", 0, rf::KEY_V, -1, -1,
                                       rf::AlpineControlConfigAction::AF_ACTION_CHAT_MENU);
        alpine_control_config_add_item(ccp, "Taunt Menu", 0, rf::KEY_B, -1, -1,
                                       rf::AlpineControlConfigAction::AF_ACTION_TAUNT_MENU);
        alpine_control_config_add_item(ccp, "Command Menu", 0, rf::KEY_N, -1, -1,
                                       rf::AlpineControlConfigAction::AF_ACTION_COMMAND_MENU);
        alpine_control_config_add_item(ccp, "Ping Location", 0, -1, -1, 2, // Mouse 3
                                       rf::AlpineControlConfigAction::AF_ACTION_PING_LOCATION);
        alpine_control_config_add_item(ccp, "Spectate Mode Menu", 0, rf::KEY_COMMA, -1, -1,
                                       rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_MENU);
        alpine_control_config_add_item(ccp, "Suppress Autoswitch", 0, -1, -1, -1,
                                       rf::AlpineControlConfigAction::AF_ACTION_NO_AUTOSWITCH);
        alpine_control_config_add_item(ccp, "Remote Server Config", false, rf::KEY_F5, -1, -1,
                                       rf::AlpineControlConfigAction::AF_ACTION_REMOTE_SERVER_CFG);
        alpine_control_config_add_item(ccp, "Inspect Weapon", false, rf::KEY_I, -1, -1,
                                       rf::AlpineControlConfigAction::AF_ACTION_INSPECT_WEAPON);
        alpine_control_config_add_item(ccp, "Cycle Spectate Modes", false, rf::KEY_PERIOD, -1, -1,
                                       rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_TOGGLE_FREELOOK);
        alpine_control_config_add_item(ccp, "Toggle Spectate", false, rf::KEY_DIVIDE, -1, -1,
                                       rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_TOGGLE);
    },
};

// alpine controls that activate only when local player is alive (multi or single)
CodeInjection player_execute_action_patch{
    0x004A6283,
    [](auto& regs) {
        rf::ControlConfigAction action = regs.ebp;
        int action_index = static_cast<int>(action);
        //xlog::warn("executing action {}", action_index);

        // only intercept alpine controls
        if (action_index >= starting_alpine_control_index) {
            if (action_index == static_cast<int>(get_af_control(rf::AlpineControlConfigAction::AF_ACTION_FLASHLIGHT))
                && !rf::is_multi) {
                if (g_headlamp_toggle_enabled) {
                    (rf::entity_headlamp_is_on(rf::local_player_entity))
                        ? rf::entity_headlamp_turn_off(rf::local_player_entity)
                        : rf::entity_headlamp_turn_on(rf::local_player_entity);
                    grant_achievement_sp(AchievementName::UseFlashlight);
                }
            }
            else if (action_index == starting_alpine_control_index +
                static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_SELF_KILL) &&
                rf::is_multi) {
                rf::player_kill_self(rf::local_player);
                if (gt_is_run()) {
                    multi_hud_reset_run_gt_timer(true);
                }
            }
            else if (action_index == starting_alpine_control_index +
                static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_DROP_FLAG) &&
                rf::is_multi && !rf::is_server) {
                send_chat_line_packet("/dropflag", nullptr);
            }
            else if (action_index == starting_alpine_control_index +
                static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_PING_LOCATION)) {
                ping_looked_at_location();
            }
            else if (action_index == starting_alpine_control_index +
                static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_INSPECT_WEAPON)) {
                fpgun_play_random_idle_anim();
            }
        }
    },
};

// alpine controls that activate in active multiplayer game (player spawned or not, but not during limbo)
CodeInjection player_execute_action_patch2{
    0x004A624B,
    [] (const auto& regs) {
        const rf::ControlConfigAction action = regs.ebp;
        int action_index = static_cast<int>(action);
        // xlog::warn("executing action {}", action_index);

        // only intercept alpine controls
        if (action_index >= starting_alpine_control_index) {
            const int alpine_action_index = action_index - starting_alpine_control_index;
            if (alpine_action_index
                == static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_VOTE_YES)
                && rf::is_multi
                && !rf::is_server) {
                send_chat_line_packet("/vote yes", nullptr);
                remove_hud_vote_notification();
            } else if (alpine_action_index
                == static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_VOTE_NO)
                && rf::is_multi
                && !rf::is_server) {
                send_chat_line_packet("/vote no", nullptr);
                remove_hud_vote_notification();
            } else if (alpine_action_index
                == static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_READY)
                && rf::is_multi
                && !rf::is_server) {
                send_chat_line_packet("/ready", nullptr);
                draw_hud_ready_notification(false);
            } else if (alpine_action_index
                == static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_CHAT_MENU)
                && rf::is_multi
                && !rf::is_dedicated_server) {
                toggle_chat_menu(ChatMenuType::Comms);
            } else if (alpine_action_index
                == static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_TAUNT_MENU)
                && rf::is_multi
                && !rf::is_dedicated_server) {
                toggle_chat_menu(ChatMenuType::Taunts);
            } else if (alpine_action_index
                == static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_COMMAND_MENU)
                && rf::is_multi
                && !rf::is_dedicated_server) {
                toggle_chat_menu(ChatMenuType::Commands);
            }
        }
    },
};

// alpine controls that activate any time in multiplayer 
CodeInjection player_execute_action_patch3{
    0x004A6233,
    [] (const auto& regs) {
        rf::ControlConfigAction action = regs.ebp;
        int action_index = static_cast<int>(action);
        // xlog::warn("executing action {}", action_index);

        // only intercept alpine controls
        if (action_index >= starting_alpine_control_index) {
            const int alpine_action_index = action_index - starting_alpine_control_index;
            if (alpine_action_index
                == static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_VOTE_YES)
                && rf::gameseq_get_state() == rf::GS_MULTI_LIMBO
                && !rf::is_server) {
                multi_attempt_endgame_vote(true);
            } else if (alpine_action_index
                == static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_VOTE_NO)
                && rf::gameseq_get_state() == rf::GS_MULTI_LIMBO
                && !rf::is_server) {
                multi_attempt_endgame_vote(false);
            } else if (alpine_action_index
                == static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_MENU) &&
                !rf::is_dedicated_server
                && multi_spectate_is_spectating()) {
                toggle_chat_menu(ChatMenuType::Spectate);
            } else if (alpine_action_index
                == static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_REMOTE_SERVER_CFG)
                && is_server_minimum_af_version(1, 2)) {
                g_remote_server_cfg_popup.toggle();
            } else if (alpine_action_index
                == static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_TOGGLE_FREELOOK)
                && !rf::is_dedicated_server
                && multi_spectate_is_spectating()) {
                multi_spectate_toggle_freelook();
            } else if (alpine_action_index
                == static_cast<int>(rf::AlpineControlConfigAction::AF_ACTION_SPECTATE_TOGGLE)
                && !rf::is_dedicated_server) {
                multi_spectate_toggle();
            }
        }
    },
};

// allow processing alpine controls
CodeInjection controls_process_patch{
    0x00430E4C,
    [](auto& regs) {
        int index = regs.edi;
        if (index >= starting_alpine_control_index &&
            index <= static_cast<int>(rf::AlpineControlConfigAction::_AF_ACTION_LAST_VARIANT)) {
            //xlog::warn("passing control {}", index);
            regs.eip = 0x00430E24;
        }
    },
};

CodeInjection controls_process_chat_menu_patch{
    0x00430E19,
    [](auto& regs) {
        const bool chat_menu_numeric_capture_active =
            get_chat_menu_is_active()
            && !rf::console::console_is_visible()
            && !rf::multi_chat_is_say_visible();
        const bool waypoint_link_numeric_capture_active =
            waypoints_utils_link_editor_text_input_active();

        // Consume top-row number keys for active overlay input modes so they do
        // not trigger gameplay bindings.
        if (chat_menu_numeric_capture_active || waypoint_link_numeric_capture_active) {
            for (int key = rf::KEY_1; key <= rf::KEY_0; ++key) {
                const int count =
                    rf::key_get_and_reset_down_counter(static_cast<rf::Key>(key));
                if (count <= 0) {
                    continue;
                }
                if (waypoint_link_numeric_capture_active) {
                    waypoints_utils_capture_numeric_key(key, count);
                }
                else {
                    chat_menu_action_handler(static_cast<rf::Key>(key));
                }
            }
        }
    },
};

CodeInjection item_touch_weapon_autoswitch_patch{
    0x0045AA50,
    [](auto& regs) {
        rf::Player* player = regs.edi;
        bool should_suppress_autoswitch = false;

        // check dedicated bind
        if (rf::control_is_control_down(&player->settings.controls, get_af_control(rf::AlpineControlConfigAction::AF_ACTION_NO_AUTOSWITCH))) {
            should_suppress_autoswitch = true;
        }

        // check alias
        if (!should_suppress_autoswitch && g_alpine_game_config.suppress_autoswitch_alias >= 0 &&
            rf::control_is_control_down(&player->settings.controls, static_cast<rf::ControlConfigAction>(g_alpine_game_config.suppress_autoswitch_alias))) {
            should_suppress_autoswitch = true;
        }

        // check fire wait autoswitch suppression
        if (!should_suppress_autoswitch && g_alpine_game_config.suppress_autoswitch_fire_wait > 0) {
            if (rf::Entity* entity = rf::entity_from_handle(player->entity_handle)) {
                if (entity->last_fired_timestamp.time_since() < g_alpine_game_config.suppress_autoswitch_fire_wait) {
                    should_suppress_autoswitch = true;
                }
            }
        }

        // Suppress autoswitch if applicable
        if (should_suppress_autoswitch) {
            regs.eip = 0x0045AA9B;
        }
    }
};

FunHook<void(int, int, int)> key_msg_handler_hook{
    0x0051EBA0,
    [] (const int msg, const int w_param, int l_param) {
        switch (msg) {
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYUP:
                if (g_alpine_game_config.input_mode == 2
                    && (SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) != 0
                    && SDL_GetKeyboardFocus() != nullptr)
                    return; // SDL handles keyboard in mode 2

                // RF requires KF_EXTENDED for numpad-derived navigation keys
                if (w_param == VK_PRIOR || w_param == VK_NEXT
                    || w_param == VK_END || w_param == VK_HOME)
                    l_param |= KF_EXTENDED << 16;

                // Sentinel pattern for keys RF's rebind UI rejects (Alt, extra keys)
                if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)
                    && rf::ui::options_controls_waiting_for_key
                    && g_pending_extra_key_rebind < 0) {
                    int win32_scan = (l_param >> 16) & 0x1FF;
                    int rf_scan = (win32_scan & 0x7F) | ((win32_scan & 0x100) ? 0x80 : 0);
                    if (scan_needs_rebind_sentinel(rf_scan)) {
                        g_pending_extra_key_rebind = rf_scan;
                        rf::key_process_event(CTRL_REBIND_SENTINEL, 1, 0);
                        return;
                    }
                }
                break;
        }
        key_msg_handler_hook.call_target(msg, w_param, l_param);
    },
};

void key_apply_patch()
{
    // Handle Alpine chat menus
    controls_process_chat_menu_patch.install();

    // Handle Alpine controls
    control_config_init_patch.install();
    player_execute_action_patch.install();
    player_execute_action_patch2.install();
    player_execute_action_patch3.install();
    controls_process_patch.install();

    // Support non-US keyboard layouts
    key_to_ascii_hook.install();
    get_key_name_hook.install();
    key_name_in_options_patch.install();

    // Disable broken numlock handling
    write_mem<int8_t>(0x004B14B2 + 1, 0);

    // win32 console support and addition of os_poll
    key_get_hook.install();

    key_quick_exit_cmd.register_cmd();

    // Support suppress autoswitch bind
    item_touch_weapon_autoswitch_patch.install();

    key_msg_handler_hook.install();
}