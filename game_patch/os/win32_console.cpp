#include <common/config/BuildConfig.h>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include <xlog/xlog.h>
#include <windows.h>
#include <thread>
#include <algorithm>
#include <cstring>
#include <format>
#include "../rf/os/console.h"
#include "../rf/multi.h"
#include "../rf/input.h"
#include "../rf/os/os.h"
#include "os.h"

static bool win32_console_enabled = false;
static bool win32_console_forced = false;
static bool win32_console_input_line_printed = false;
static int win32_console_printed_input_len = 0;
static HANDLE win32_console_input_handle;
static HANDLE win32_console_output_handle;
static bool win32_console_is_output_redirected = false;
static bool win32_console_is_input_redirected = false;
// Armed on every output line; the input line reprint waits until output has been quiet for a while
static HighResTimer win32_console_output_pause_timer;

bool win32_console_is_enabled()
{
    return win32_console_enabled;
}

void win32_console_set_forced(bool forced)
{
    win32_console_forced = forced;
}

static void reset_console_cursor_column(bool clear)
{
    if (win32_console_is_output_redirected) {
        return;
    }
    // The input line is the only text printed without a trailing newline, so the cursor column
    // equals the printed input line length tracked in win32_console_printed_input_len.
    // Blank it with a single write ('\r' resets the column thanks to ENABLE_PROCESSED_OUTPUT)
    // instead of querying the console and writing space by space: each console API call is a
    // double-hop wineserver/conhost RPC under Wine.
    if (win32_console_printed_input_len == 0) {
        return;
    }
    if (clear) {
        std::string buf;
        buf.reserve(2 + win32_console_printed_input_len);
        buf += '\r';
        buf.append(win32_console_printed_input_len, ' ');
        buf += '\r';
        WriteConsoleA(win32_console_output_handle, buf.data(), buf.size(), nullptr, nullptr);
        win32_console_input_line_printed = false;
        win32_console_printed_input_len = 0;
    }
    else {
        WriteConsoleA(win32_console_output_handle, "\r", 1, nullptr, nullptr);
    }
}

static void print_cmd_input_line()
{
    if (win32_console_is_output_redirected) {
        return;
    }
    CONSOLE_SCREEN_BUFFER_INFO scr_buf_info;
    GetConsoleScreenBufferInfo(win32_console_output_handle, &scr_buf_info);
    unsigned offset = std::max(0, rf::console::cmd_line_len - scr_buf_info.dwSize.X + 3);
    std::string line;
    line.reserve(2 + rf::console::cmd_line_len - offset);
    line += "] ";
    line.append(rf::console::cmd_line + offset, rf::console::cmd_line_len - offset);
    WriteConsoleA(win32_console_output_handle, line.data(), line.size(), nullptr, nullptr);
    win32_console_input_line_printed = true;
    win32_console_printed_input_len = static_cast<int>(line.size());
}

static BOOL WINAPI console_ctrl_handler([[maybe_unused]] DWORD ctrl_type)
{
    xlog::info("Quiting after Console CTRL");
    rf::close_app_req = 1;
    return TRUE;
}

// void input_thread_proc()
// {
//     while (true) {
//         INPUT_RECORD input_record;
//         DWORD num_read = 0;
//         ReadConsoleInput(GetStdHandle(STD_INPUT_HANDLE), &input_record, 1, &num_read);
//     }
// }

static rf::CmdLineParam& get_win32_console_cmd_line_param()
{
    static rf::CmdLineParam win32_console_param{"-win32-console", "", false};
    return win32_console_param;
}

void win32_console_pre_init()
{
    // register cmdline param
    get_win32_console_cmd_line_param();
}

void win32_console_init()
{
    win32_console_enabled = win32_console_forced || get_win32_console_cmd_line_param().found();
    if (!win32_console_enabled) {
        return;
    }

    win32_console_input_handle = GetStdHandle(STD_INPUT_HANDLE);
    win32_console_output_handle = GetStdHandle(STD_OUTPUT_HANDLE);

    char buf[256];
    if (GetFinalPathNameByHandleA(win32_console_output_handle, buf, std::size(buf), 0) == 0) {
        char* ptr = std::format_to(buf, "(error {})", GetLastError());
        *ptr = '\0';
    }
    xlog::info("Standard output info: path_name {}, file_type: {}, handle {}",
        buf, GetFileType(win32_console_output_handle), win32_console_output_handle);

    if (!GetFileType(win32_console_output_handle)) {
        if (!AllocConsole()) {
            xlog::warn("AllocConsole failed, error {}", GetLastError());
        }
        win32_console_input_handle = GetStdHandle(STD_INPUT_HANDLE);
        win32_console_output_handle = GetStdHandle(STD_OUTPUT_HANDLE);
        // We are using native console functions but in case some code tried stdio API reopen standard streams
        std::freopen("CONOUT$", "w", stdout);
        std::freopen("CONOUT$", "w", stderr);
        std::freopen("CONIN$", "r", stdin);
        xlog::info("Allocated new console, standard output: file_type {}, handle {}",
            GetFileType(win32_console_output_handle), win32_console_output_handle);
    }

    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    DWORD mode;
    win32_console_is_output_redirected = !GetConsoleMode(win32_console_output_handle, &mode);
    win32_console_is_input_redirected = !GetConsoleMode(win32_console_input_handle, &mode);
    if (!win32_console_is_input_redirected) {
        SetConsoleMode(win32_console_input_handle, mode & ~ ENABLE_ECHO_INPUT);
    }

    // std::thread input_thread(input_thread_proc);
    // input_thread.detach();
}

static void write_console_output(std::string_view str)
{
    HANDLE output_handle = win32_console_output_handle;
    if (!win32_console_is_output_redirected) {
        WriteConsoleA(output_handle, str.data(), str.size(), nullptr, nullptr);
    }
    else {
        // Convert LF to CRLF
        DWORD bytes_written;
        size_t start_pos = 0;
        while (true) {
            size_t end_pos = str.find('\n', start_pos);
            if (end_pos == std::string_view::npos) {
                WriteFile(output_handle, str.data() + start_pos, str.size() - start_pos, &bytes_written, nullptr);
                break;
            }

            size_t next_pos = end_pos + 1;
            if (end_pos > 0 && str[end_pos - 1] == '\r') {
                --end_pos;
            }
            if (end_pos - start_pos > 0) {
                WriteFile(output_handle, str.data() + start_pos, end_pos - start_pos, &bytes_written, nullptr);
            }
            WriteFile(output_handle, "\r\n", 2, &bytes_written, nullptr);
            start_pos = next_pos;
        }
    }
}

void win32_console_output(const char* text, [[maybe_unused]] const rf::Color* color)
{
    constexpr WORD red_attr = FOREGROUND_RED | FOREGROUND_INTENSITY;
    constexpr WORD blue_attr = FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    constexpr WORD white_attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
    constexpr WORD gray_attr = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    HANDLE output_handle = win32_console_output_handle;
    WORD current_attr = 0;

    if (win32_console_input_line_printed) {
        reset_console_cursor_column(true);
    }

    constexpr std::string_view color_prefix{"[$"};
    constexpr std::string_view color_suffix{"]"};
    std::string_view text_sv{text};
    size_t pos = 0;
    while (pos < text_sv.size()) {
        std::string color;
        if (text_sv.substr(pos, color_prefix.size()) == color_prefix) {
            size_t color_name_pos = pos + color_prefix.size();
            size_t color_suffix_pos = text_sv.find(color_suffix, color_name_pos);
            if (color_suffix_pos != std::string_view::npos) {
                color = text_sv.substr(color_name_pos, color_suffix_pos - color_name_pos);
                pos = color_suffix_pos + color_suffix.size();
            }
        }
        size_t end_pos = text_sv.find(color_prefix, pos);
        if (end_pos == std::string_view::npos) {
            end_pos = text_sv.size();
        }
        std::string_view text_part = text_sv.substr(pos, end_pos - pos);

        WORD attr;
        if (color == "Red")
            attr = red_attr;
        else if (color == "Blue")
            attr = blue_attr;
        else if (color == "White")
            attr = white_attr;
        else {
            if (!color.empty())
                xlog::error("unknown color {}", color);
            attr = gray_attr;
        }

        if (current_attr != attr && !win32_console_is_output_redirected) {
            current_attr = attr;
            SetConsoleTextAttribute(output_handle, attr);
        }

        write_console_output(text_part);
        pos = end_pos;
    }

    if (!text_sv.empty() && text_sv[text_sv.size() - 1] != '\n') {
        write_console_output("\n");
    }

    if (current_attr != gray_attr && !win32_console_is_output_redirected) {
        SetConsoleTextAttribute(output_handle, gray_attr);
    }

    win32_console_output_pause_timer.set_ms(100);
}

void win32_console_new_line()
{
    DWORD bytes_written;
    WriteFile(win32_console_output_handle, "\r\n", 2, &bytes_written, nullptr);
    print_cmd_input_line();
}

void win32_console_update()
{
    static char prev_cmd_line[sizeof(rf::console::cmd_line)];
    bool cmd_line_changed = std::strncmp(rf::console::cmd_line, prev_cmd_line, std::size(prev_cmd_line)) != 0;
    // Reprint the input line when the user edited it, or after output has been quiet for a while.
    // Reprinting right after every output line causes console API call bursts during log storms;
    // the prompt staying hidden while output is flowing is an accepted tradeoff.
    bool output_paused = !win32_console_output_pause_timer.valid() || win32_console_output_pause_timer.elapsed();
    if (cmd_line_changed || (!win32_console_input_line_printed && output_paused)) {
        reset_console_cursor_column(true);
        print_cmd_input_line();
        std::strcpy(prev_cmd_line, rf::console::cmd_line);
    }
}

void win32_console_poll_input()
{
    if (win32_console_is_input_redirected) {
        return;
    }
    HANDLE input_handle = win32_console_input_handle;
    INPUT_RECORD input_record;
    DWORD num_read = 0;
    while (true) {
        if (!PeekConsoleInput(input_handle, &input_record, 1, &num_read) || num_read == 0)
            break;
        if (!ReadConsoleInput(input_handle, &input_record, 1, &num_read) || num_read == 0)
            break;
        if (input_record.EventType == KEY_EVENT) {
            rf::key_process_event(input_record.Event.KeyEvent.wVirtualScanCode, input_record.Event.KeyEvent.bKeyDown, 0);
        }
    }
}

void win32_console_close()
{
    if (win32_console_enabled) {
        FreeConsole();
    }
}
