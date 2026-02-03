#include "console.h"
#include "../main/main.h"
#include "../misc/player.h"
#include "../misc/misc.h"
#include "../misc/alpine_settings.h"
#include "../rf/player/player.h"
#include "../rf/gameseq.h"
#include "../rf/input.h"
#include "win32_console.h"
#include <common/config/BuildConfig.h>
#include <common/version/version.h>
#include <patch_common/CodeInjection.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/ShortTypes.h>
#include <algorithm>
#include <cassert>
#include <xlog/xlog.h>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <ctime>
#include <string_view>

// ConsoleDrawClientConsole uses 200 bytes long buffer for: "] ", user input and '\0'
constexpr int max_cmd_line_len = 200 - 2 - 1;

rf::console::Command* g_commands_buffer[CMD_LIMIT];

static std::ofstream g_console_log;
static std::string g_console_log_path;

void console_run_script(const char* filename)
{
    rf::console::run_script(filename);
}

static void console_log_write(std::string_view text)
{
    if (!g_console_log.is_open())
        return;

    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n')
                continue; // skip CR in CRLF pair, newline added next
            g_console_log.put('\n');
        }
        else {
            g_console_log.put(c);
        }
    }

    g_console_log.flush();
}

void console_start_server_log()
{
    if (g_console_log.is_open())
        return;

    namespace fs = std::filesystem;
    fs::create_directories("logs");

    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &t);
    char buf[64];
    std::strftime(buf, sizeof(buf), "logs\\AlpineFaction-console-%Y%m%d-%H%M%S.log", &tm);
    g_console_log_path = buf;
    g_console_log.open(g_console_log_path, std::ios::out | std::ios::trunc);
    if (!g_console_log) {
        xlog::warn("Failed to open console log file {}", g_console_log_path);
    }
}

rf::Player* find_best_matching_player(const char* name)
{
    rf::Player* found_player;
    int num_found = 0;
    find_player(StringMatcher().exact(name), [&](rf::Player* player) {
        found_player = player;
        ++num_found;
    });
    if (num_found == 1)
        return found_player;

    num_found = 0;
    find_player(StringMatcher().infix(name), [&](rf::Player* player) {
        found_player = player;
        ++num_found;
    });

    if (num_found == 1) {
        return found_player;
    }
    if (num_found > 1)
        rf::console::print("Found {} players matching '{}'!", num_found, name);
    else
        rf::console::print("Cannot find player matching '{}'", name);
    return nullptr;
}

FunHook<int()> gameseq_process_hook{
    0x00434230,
    []() {
        int menu_id = gameseq_process_hook.call_target();
        if (menu_id == rf::GS_MULTI_LIMBO) // hide cursor when changing level - hackfixed in RF by changing rendering logic
            rf::mouse_set_visible(false);
        else if (menu_id == rf::GS_MAIN_MENU)
            rf::mouse_set_visible(true);
        return menu_id;
    },
};

CodeInjection ConsoleCommand_init_limit_check_patch{
    0x00509A7E,
    [](auto& regs) {
        if (regs.eax >= CMD_LIMIT) {
            regs.eip = 0x00509ACD;
        }
    },
};

CodeInjection console_run_cmd_call_handler_patch{
    0x00509DBB,
    [](auto& regs) {
        // Make sure command pointer is in ecx register to support thiscall handlers
        regs.ecx = regs.eax;
    },
};

CallHook<void(char*, int)> console_process_kbd_get_text_from_clipboard_hook{
    0x0050A2FD,
    [](char *buf, int max_len) {
        max_len = std::min(max_len, max_cmd_line_len - rf::console::cmd_line_len);
        console_process_kbd_get_text_from_clipboard_hook.call_target(buf, max_len);
    },
};

void console_register_command(rf::console::Command* cmd)
{
    if (rf::console::num_commands < CMD_LIMIT)
        rf::console::Command::init(cmd, cmd->name, cmd->help, cmd->func);
    else
        assert(false);
}

static FunHook<void(const char*, const rf::Color*)> console_output_hook{
    &rf::console::output,
    [](const char* text, const rf::Color* color) {
        if (win32_console_is_enabled()) {
            win32_console_output(text, color);
        }
        else {
            console_output_hook.call_target(text, color);
        }
        std::string_view text_view{text};
        console_log_write(text_view);
        if (g_console_log.is_open() && (text_view.empty() || (text_view.back() != '\n' && text_view.back() != '\r'))) {
            g_console_log.put('\n');
            g_console_log.flush();
        }
    },
};

static FunHook<void()> console_draw_server_hook{
    0x0050A770,
    []() {
        if (win32_console_is_enabled()) {
            win32_console_update();
        }
        else {
            console_draw_server_hook.call_target();
        }
    },
};

static FunHook<void()> console_draw_client_hook{
    0x0050ABE0,
    []() {
        // Make sure clip window is reset before drawing
        // Fixes console rendering in endgame state
        rf::gr::reset_clip();

        console_draw_client_hook.call_target();
    },
};


static CallHook<void(char)> console_put_char_new_line_hook{
    0x0050A081,
    [](char c) {
        if (win32_console_is_enabled()) {
            win32_console_new_line();
        }
        else {
            console_put_char_new_line_hook.call_target(c);
        }
    },
};

void console_clear_input()
{
    rf::console::cmd_line[0] = '\0';
    rf::console::cmd_line_len = 0;
    rf::console::history_current_index = 0;
}

static CodeInjection console_handle_input_injection{
    0x00509FAF,
    [](auto& regs) {
        rf::Key key = regs.eax;
        if (key == (rf::KEY_C | rf::KEY_CTRLED)) {
            console_clear_input();
        }
    },
};

static CodeInjection console_handle_input_history_injection{
    0x0050A09B,
    [](auto& regs) {

        if (rf::console::history_max_index >= 0 &&
            std::strcmp(rf::console::history[rf::console::history_max_index], rf::console::cmd_line) == 0) {
            // Command was repeated - do not add it to the history
            console_clear_input();
            regs.eip = 0x0050A35C;
        }
    },
};

void print_fflink_info() {
    std::string username = g_game_config.fflink_username.value();
    std::string msg = "";
    if (username.empty()) {
        msg = "Not linked to a FactionFiles account!";
    }
    else {
        msg = "Linked to FactionFiles as " + username;
    }
    rf::console::printf("-- %s --", msg);
}

void apply_console_history_setting() {
    rf::console::console_keep_history = g_alpine_game_config.save_console_history;
}

extern void console_commands_apply_patches();
extern void console_auto_complete_apply_patches();
extern void console_commands_init();

void console_apply_patches()
{
    // Console init string
    write_mem_ptr(0x004B2534, "-- " PRODUCT_NAME " Initializing --\n");

    // Console background color
    constexpr rf::Color console_color{0x27, 0x4E, 0x69, 0xC0};
    write_mem<u32>(0x005098D1, console_color.alpha);
    write_mem<u8>(0x005098D6, console_color.blue);
    write_mem<u8>(0x005098D8, console_color.green);
    write_mem<u8>(0x005098DA, console_color.red);

    // Support unsigned hexadecimal arguments.
    AsmWriter{0x0050B0B0}.call(std::strtoul);

    // Fix console rendering when changing level
    AsmWriter(0x0047C490).ret();
    AsmWriter(0x0047C4AA).ret();
    AsmWriter(0x004B2E15).nop(2);
    gameseq_process_hook.install();

    // Change limit of commands
    assert(rf::console::num_commands == 0);
    write_mem_ptr(0x005099AC + 1, g_commands_buffer);
    write_mem_ptr(0x00509A8A + 1, g_commands_buffer);
    write_mem_ptr(0x00509AB0 + 3, g_commands_buffer);
    write_mem_ptr(0x00509AE1 + 3, g_commands_buffer);
    write_mem_ptr(0x00509AF5 + 3, g_commands_buffer);
    write_mem_ptr(0x00509C8F + 1, g_commands_buffer);
    write_mem_ptr(0x00509DB4 + 3, g_commands_buffer);
    write_mem_ptr(0x00509E6F + 1, g_commands_buffer);
    write_mem_ptr(0x0050A648 + 4, g_commands_buffer);
    write_mem_ptr(0x0050A6A0 + 3, g_commands_buffer);
    AsmWriter(0x00509A7E).nop(2);
    ConsoleCommand_init_limit_check_patch.install();

    console_run_cmd_call_handler_patch.install();

    // Fix possible input buffer overflow
    console_process_kbd_get_text_from_clipboard_hook.install();
    write_mem<u32>(0x0050A2D0 + 2, max_cmd_line_len);

    // Win32 console support
    console_output_hook.install();
    console_draw_server_hook.install();
    console_put_char_new_line_hook.install();

    // Additional key handling
    console_handle_input_injection.install();

    // Improve history handling
    console_handle_input_history_injection.install();

    // Reset clip window before rendering console
    console_draw_client_hook.install();

    apply_console_history_setting();
    console_commands_apply_patches();
    console_auto_complete_apply_patches();
}

void console_init()
{
    console_commands_init();
    print_fflink_info();
    initialize_achievement_manager();

}
