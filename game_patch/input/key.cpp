#include <cctype>
#include <patch_common/FunHook.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include "../hud/multi_spectate.h"
#include "../hud/hud.h"
#include "../misc/player.h"
#include "../misc/achievements.h"
#include "../misc/alpine_settings.h"
#include "../multi/multi.h"
#include "../multi/endgame_votes.h"
#include "../rf/input.h"
#include "../rf/entity.h"
#include "../rf/multi.h"
#include "../rf/gameseq.h"
#include "../rf/player/control_config.h"
#include "../rf/player/player.h"
#include "../rf/os/console.h"
#include "../rf/os/os.h"
#include "../multi/alpine_packets.h"
#include "../os/console.h"

static int starting_alpine_control_index = -1;

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

FunHook<int(int16_t)> key_to_ascii_hook{
    0x0051EFC0,
    [](int16_t key) {
        using namespace rf;
        constexpr int empty_result = 0xFF;
        if (!key) {
            return empty_result;
        }
        // special handling for Num Lock (because ToAscii API does not support it)
        switch (key & KEY_MASK) {
            // Numpad keys that always work
            case KEY_PADMULTIPLY: return static_cast<int>('*');
            case KEY_PADMINUS: return static_cast<int>('-');
            case KEY_PADPLUS: return static_cast<int>('+');
            // Disable Numpad Enter key because game is not prepared for getting new line character from this function
            case KEY_PADENTER: return empty_result;
        }
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
        if (key & KEY_SHIFTED) {
            key_state[VK_SHIFT] = 0x80;
        }
        if (key & KEY_ALTED) {
            key_state[VK_MENU] = 0x80;
        }
        if (key & KEY_CTRLED) {
            key_state[VK_CONTROL] = 0x80;
        }
        int scan_code = key & 0x7F;
        auto vk = MapVirtualKeyA(scan_code, MAPVK_VSC_TO_VK);
        WCHAR unicode_chars[3];
        auto num_unicode_chars = ToUnicode(vk, scan_code, key_state, unicode_chars, std::size(unicode_chars), 0);
        if (num_unicode_chars < 1) {
            return empty_result;
        }
        char ansi_char;
#if 0 // Windows-1252 codepage support - disabled because callers of this function expects ASCII
        int num_ansi_chars = WideCharToMultiByte(1252, 0, unicode_chars, num_unicode_chars,
            &ansi_char, sizeof(ansi_char), nullptr, nullptr);
        if (num_ansi_chars == 0) {
            return empty_result;
        }
#else
        if (static_cast<char16_t>(unicode_chars[0]) >= 0x80 || !std::isprint(unicode_chars[0])) {
            return empty_result;
        }
        ansi_char = static_cast<char>(unicode_chars[0]);
#endif
        xlog::trace("vk {:x} ({}) char {}", vk, vk, ansi_char);
        return static_cast<int>(ansi_char);
    },
};

int get_key_name(int key, char* buf, size_t buf_len)
{
     LONG lparam = (key & 0x7F) << 16;
    if (key & 0x80) {
        lparam |= 1 << 24;
    }
    // Note: it seems broken on Wine with non-US layout (most likely broken MAPVK_VSC_TO_VK_EX mapping is responsible)
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

void alpine_control_config_add_item(rf::ControlConfig* config, const char* name, bool is_repeat,
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
    binding.is_repeat = is_repeat;
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
            }
        }
    },
};

// allow processing alpine controls
CodeInjection controls_process_patch{
    0x00430E4C,
    [](auto& regs) {
        int index = regs.edi;

        // C++ doesn't have a way to dynamically get the last enum index, so just update this when adding new controls
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

        // only consume numline keys if a chat menu is active + chat box and console are hidden
        if (get_chat_menu_is_active() && !rf::console::console_is_visible() && !rf::multi_chat_is_say_visible()) {
            for (int key = rf::KEY_1; key <= rf::KEY_0; ++key) {
                if (rf::key_get_and_reset_down_counter(static_cast<rf::Key>(key)) > 0) {
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

CodeInjection key_down_handler_injection{
    0x0051E9AC,
    [] (auto& regs) {
        const int virtual_key = addr_as_ref<int>(regs.esp + 4);
        // For numeric keypads, we need to fix these keys' scan codes.
        auto& scan_code = regs.eax;
        if (virtual_key == VK_PRIOR) {
            scan_code = rf::KEY_PAGEUP;
        } else if (virtual_key == VK_NEXT) {
            scan_code = rf::KEY_PAGEDOWN;
        } else if (virtual_key == VK_END) {
            scan_code = rf::KEY_END;
        } else if (virtual_key == VK_HOME) {
            scan_code = rf::KEY_HOME;
        }
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

    key_down_handler_injection.install();
}
