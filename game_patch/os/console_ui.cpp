// Replacement renderer and line editor for the in-game (~) console.
// Stock keeps owning visibility, the open/close animation, command dispatch, history and Tab
// completion. We mirror output into our own scrollback, draw it with consolefont.ttf, and take
// over cursor editing, scrolling and search.
#include "console.h"
#include "os.h"
#include "win32_console.h"
#include "../main/main.h"
#include "../misc/alpine_settings.h"
#include "../input/control_input_filter.h"
#include "../misc/vote_panel.h"
#include "../multi/gametype.h"
#include "../multi/multi.h"
#include "../rf/gr/gr.h"
#include "../rf/gameseq.h"
#include "../rf/gr/gr_font.h"
#include "../rf/input.h"
#include "../rf/level.h"
#include "../rf/multi.h"
#include "../rf/player/player.h"
#include "../rf/sound/sound.h"
#include "../sound/sound.h"
#include <common/config/GameConfig.h>
#include <common/utils/string-utils.h>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/MemUtils.h>
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>
#include <deque>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace
{
// Range of rf::console::client_height while it animates.
constexpr int stock_height_closed = -11;
constexpr int stock_height_open = 192;

struct Line
{
    std::string text;
    rf::Color color;
    size_t prefix_len = 0; // leading chars drawn in prefix_color (chat sender names)
    rf::Color prefix_color{};
    bool match = true; // contains g_filter; only meaningful while filter_active()
};

// Default colors per kind of output. All are >= 4.5:1 contrast against the default console
// background (39, 78, 105) and distinct from the default white text.
namespace palette
{
constexpr rf::Color command{140, 235, 140};    // command names and the typed-command echo
constexpr rf::Color description{180, 195, 210}; // " - help text" after a command name
constexpr rf::Color error{255, 180, 130};
constexpr rf::Color red_team{255, 165, 165};
constexpr rf::Color blue_team{150, 200, 255};
constexpr rf::Color chat{255, 255, 255};
constexpr rf::Color chat_name{255, 240, 170}; // sender with no team
constexpr rf::Color server{255, 210, 80};
constexpr rf::Color event{205, 190, 255}; // kills, joins, flag events
} // namespace palette

int g_chat_color = -1; // rf::ChatMsgColor of the multi_chat_print in progress, -1 = none
std::string g_chat_prefix;
bool g_user_scrolled = false; // wheel/PgUp/PgDn/Ctrl+Home/End this frame; plays the scroll tick
int g_shown_scroll = 0;
int g_console_bottom = 0; // screen y of the console's lower edge last frame, 0 = closed

constexpr size_t max_lines = 4000;
constexpr int wheel_rows = 3;
constexpr int pad = 4;

std::deque<Line> g_lines;
int g_scroll = 0;       // wrapped rows above the bottom; 0 = following new output
int g_unseen = 0;       // lines that arrived while scrolled up
int g_cursor = INT_MAX; // byte index into cmd_line; INT_MAX = end (clamped on use)
int g_cols = 80;        // wrap width in chars, from the last render
int g_page_rows = 10;
bool g_filter_mode = false;
std::string g_filter;
int g_font = -1;
int g_font_bold = -1;
int g_font_px = 0;

// Mouse selection. Lines are addressed by a stable id (g_first_id + deque index) so a selection
// survives old lines being dropped from the front.
struct TextPos
{
    uint32_t id;
    size_t off;
    auto operator<=>(const TextPos&) const = default;
};
uint32_t g_first_id = 0;
bool g_cursor_on = false; // toggled cursor (gameplay only; menus already have one)
bool g_swallow_attack = false; // after the cursor closes, keep attacks blocked until buttons are released
bool g_selecting = false;
TextPos g_sel_a{};
TextPos g_sel_b{};
DWORD g_copied_until = 0;
size_t g_copied_chars = 0;

// While the console is open over a menu, clicks on it must not reach the menu buttons underneath.
// The console reads the real state through call_target.
bool console_eats_mouse()
{
    if (!rf::console::console_is_visible()) {
        return false;
    }
    if (g_selecting) {
        return true;
    }
    if (g_console_bottom <= 0 || rf::gameseq_get_state() == rf::GS_GAMEPLAY) {
        return false;
    }
    int mx = 0;
    int my = 0;
    int mz = 0;
    rf::mouse_get_pos(mx, my, mz);
    return my < g_console_bottom;
}

FunHook<bool(int)> mouse_was_button_pressed_hook{
    &rf::mouse_was_button_pressed,
    [](int btn) { return !console_eats_mouse() && mouse_was_button_pressed_hook.call_target(btn); },
};

FunHook<bool(int)> mouse_button_is_down_hook{
    &rf::mouse_button_is_down,
    [](int btn) { return !console_eats_mouse() && mouse_button_is_down_hook.call_target(btn); },
};

bool mouse_pressed(int btn)
{
    return mouse_was_button_pressed_hook.call_target(btn);
}

bool mouse_down(int btn)
{
    return mouse_button_is_down_hook.call_target(btn);
}

void play_console_sound(int sound_id)
{
    if (g_alpine_game_config.console_sounds) {
        rf::snd_play(sound_id, 0, 0.0f, 1.0f);
    }
}

// Saturating, because Ctrl+Home parks g_scroll at INT_MAX until the next render clamps it.
void scroll_by(int rows)
{
    g_scroll = rows > 0 && g_scroll > INT_MAX - rows ? INT_MAX : g_scroll + rows;
}

int rows_for(const std::string& s)
{
    return std::max(1, (static_cast<int>(s.size()) + g_cols - 1) / g_cols);
}

bool filter_active()
{
    return g_filter_mode && !g_filter.empty();
}

bool line_matches(const Line& line)
{
    return !filter_active() || line.match;
}

void exit_filter()
{
    if (filter_active()) {
        g_scroll = 0; // it counted rows of the filtered view
    }
    g_filter_mode = false;
    g_filter.clear();
}

void refilter()
{
    if (!filter_active()) {
        return;
    }
    for (Line& line : g_lines) {
        line.match = string_icontains(line.text, g_filter);
    }
}

int cursor()
{
    g_cursor = std::clamp(g_cursor, 0, rf::console::cmd_line_len);
    return g_cursor;
}

void set_cmd_line(const char* s)
{
    std::strncpy(rf::console::cmd_line, s, max_cmd_line_len);
    rf::console::cmd_line[max_cmd_line_len] = '\0';
    rf::console::cmd_line_len = static_cast<int>(std::strlen(rf::console::cmd_line));
    g_cursor = INT_MAX;
}

// Shortest command whose name extends what's typed (no args yet, cursor at end).
rf::console::Command* ghost_suggestion()
{
    const int len = rf::console::cmd_line_len;
    if (len == 0 || cursor() != len || std::strchr(rf::console::cmd_line, ' ')) {
        return nullptr;
    }
    const std::string_view typed{rf::console::cmd_line, static_cast<size_t>(len)};
    rf::console::Command* best = nullptr;
    for (int i = 0; i < rf::console::num_commands; ++i) {
        rf::console::Command* cmd = g_commands_buffer[i];
        if (std::strlen(cmd->name) > typed.size() && string_istarts_with(cmd->name, typed)
            && (!best || std::strlen(cmd->name) < std::strlen(best->name))) {
            best = cmd;
        }
    }
    return best;
}

bool has_selection()
{
    return g_sel_a != g_sel_b;
}

void copy_selection()
{
    const TextPos lo = std::min(g_sel_a, g_sel_b);
    const TextPos hi = std::max(g_sel_a, g_sel_b);
    std::string out;
    bool first = true;
    for (uint32_t id = std::max(lo.id, g_first_id); id <= hi.id && id - g_first_id < g_lines.size(); ++id) {
        const Line& line = g_lines[id - g_first_id];
        if (!line_matches(line)) {
            continue; // in search mode, copy only what's shown
        }
        const std::string& text = line.text;
        const size_t s = id == lo.id ? std::min(lo.off, text.size()) : 0;
        const size_t e = id == hi.id ? std::min(hi.off, text.size()) : text.size();
        if (!first) {
            out += "\r\n";
        }
        first = false;
        out.append(text, s, e > s ? e - s : 0);
    }
    write_clipboard_text(out);
    g_copied_chars = out.size();
    g_copied_until = GetTickCount() + 1500;
    play_console_sound(stock_sound_id::panel_button_click);
}

// Ctrl+C with a selection. Dropping it afterwards lets the next Ctrl+C clear the input line again.
bool copy_and_drop_selection()
{
    if (!has_selection()) {
        return false;
    }
    copy_selection();
    g_sel_b = g_sel_a;
    return true;
}

// Plain click (no drag) on a command name in the scrollback, e.g. from `help` or a Tab listing:
// put it on the input line ready for arguments.
void prefill_clicked_command()
{
    if (g_sel_a.id < g_first_id || g_sel_a.id - g_first_id >= g_lines.size()) {
        return;
    }
    const std::string& text = g_lines[g_sel_a.id - g_first_id].text;
    const auto is_name_char = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    size_t s = std::min(g_sel_a.off, text.size());
    size_t e = s;
    // hit_test rounds to the nearest gap, so a click on a word's last half lands just past it.
    if (s > 0 && (s == text.size() || !is_name_char(text[s])) && is_name_char(text[s - 1])) {
        --s;
        --e;
    }
    while (s > 0 && is_name_char(text[s - 1])) {
        --s;
    }
    while (e < text.size() && is_name_char(text[e])) {
        ++e;
    }
    if (s == e) {
        return;
    }
    const std::string_view word{text.data() + s, e - s};
    for (int i = 0; i < rf::console::num_commands; ++i) {
        if (string_iequals(g_commands_buffer[i]->name, word)) {
            set_cmd_line((std::string{g_commands_buffer[i]->name} + ' ').c_str());
            play_console_sound(stock_sound_id::menu_select);
            return;
        }
    }
}

void release_cursor()
{
    if (!g_cursor_on) {
        return;
    }
    g_cursor_on = false;
    g_swallow_attack = true;
    gameplay_overlay_release_mouse();
}

bool console_open_in_gameplay()
{
    return rf::console::console_is_visible() && rf::gameseq_get_state() == rf::GS_GAMEPLAY;
}

// Same toggle as the scoreboard cursor. The binding is vetoed while the console is open in
// gameplay, so the toggling click never fires (see console_cursor_blocks_action).
bool cursor_toggle_pressed()
{
    bool pressed = false;
    if (rf::local_player) {
        control_input_filter_check_pressed_unfiltered(
            &rf::local_player->settings.controls, rf::CC_ACTION_SECONDARY_ATTACK, &pressed);
    }
    return pressed;
}

bool is_word_char(char c)
{
    return c != ' ';
}

// Returns true if the key was consumed.
bool handle_edit_key(int key)
{
    const int base = key & rf::KEY_MASK;
    const bool ctrl = key & rf::KEY_CTRLED;
    const bool alt = key & rf::KEY_ALTED;
    char* line = rf::console::cmd_line;
    int& len = rf::console::cmd_line_len;
    int cur = cursor();

    if (ctrl && base == rf::KEY_C) {
        if (!copy_and_drop_selection()) {
            console_clear_input();
            g_cursor = 0;
        }
        return true;
    }
    if (ctrl && (base == rf::KEY_U || base == rf::KEY_K)) {
        const int from = base == rf::KEY_U ? 0 : cur;
        const int to = base == rf::KEY_U ? cur : len;
        std::memmove(line + from, line + to, len - to + 1); // includes NUL
        len -= to - from;
        g_cursor = from;
        return true;
    }
    switch (base) {
        case rf::KEY_LEFT:
            if (ctrl) {
                while (cur > 0 && !is_word_char(line[cur - 1])) {
                    --cur;
                }
                while (cur > 0 && is_word_char(line[cur - 1])) {
                    --cur;
                }
            }
            else if (cur > 0) {
                --cur;
            }
            g_cursor = cur;
            return true;
        case rf::KEY_RIGHT:
            if (auto* cmd = ghost_suggestion()) {
                set_cmd_line(cmd->name);
                return true;
            }
            if (ctrl) {
                while (cur < len && is_word_char(line[cur])) {
                    ++cur;
                }
                while (cur < len && !is_word_char(line[cur])) {
                    ++cur;
                }
            }
            else if (cur < len) {
                ++cur;
            }
            g_cursor = cur;
            return true;
        case rf::KEY_HOME:
            g_cursor = 0;
            return true;
        case rf::KEY_END:
            if (len == 0) {
                g_scroll = 0;
                g_unseen = 0;
            }
            g_cursor = len;
            return true;
        case rf::KEY_DELETE: {
            int end = cur < len ? cur + 1 : cur;
            if (ctrl) {
                end = cur;
                while (end < len && is_word_char(line[end])) {
                    ++end;
                }
                while (end < len && !is_word_char(line[end])) {
                    ++end;
                }
            }
            std::memmove(line + cur, line + end, len - end + 1); // includes NUL
            len -= end - cur;
            return true;
        }
        case rf::KEY_BACKSP: {
            int start = cur > 0 ? cur - 1 : 0;
            if (ctrl) {
                start = cur;
                while (start > 0 && !is_word_char(line[start - 1])) {
                    --start;
                }
                while (start > 0 && is_word_char(line[start - 1])) {
                    --start;
                }
            }
            std::memmove(line + start, line + cur, len - cur + 1);
            len -= cur - start;
            g_cursor = start;
            return true;
        }
        default:
            break;
    }
    // Esc and the console key go through stock (they close the console).
    if (ctrl || alt || base == rf::KEY_ESC || base == rf::KEY_LAPOSTRO_DBG) {
        return false;
    }
    const int c = rf::key_to_ascii(static_cast<int16_t>(key));
    if (c == 0xFF || c < 0x20) {
        return false;
    }
    if (len < max_cmd_line_len) {
        std::memmove(line + cur + 1, line + cur, len - cur + 1);
        line[cur] = static_cast<char>(c);
        ++len;
        g_cursor = cur + 1;
    }
    return true;
}

bool handle_filter_key(int key)
{
    const int base = key & rf::KEY_MASK;
    if (base == rf::KEY_ESC || base == rf::KEY_ENTER || base == rf::KEY_PADENTER) {
        exit_filter();
        return true;
    }
    if (base == rf::KEY_BACKSP) {
        if (!g_filter.empty()) {
            g_filter.pop_back();
            refilter();
        }
        return true;
    }
    // Consumed even without a selection: console.cpp's Ctrl+C clear would wipe the hidden input line.
    if ((key & rf::KEY_CTRLED) && base == rf::KEY_C) {
        copy_and_drop_selection();
        return true;
    }
    if ((key & (rf::KEY_CTRLED | rf::KEY_ALTED)) || base == rf::KEY_LAPOSTRO_DBG) {
        return false;
    }
    const int c = rf::key_to_ascii(static_cast<int16_t>(key));
    if (c == 0xFF || c < 0x20) {
        return false;
    }
    g_filter.push_back(static_cast<char>(c));
    refilter();
    g_scroll = 0;
    return true;
}

// Wraps the key_get() call at the top of console_handle_input (0x00509F90). Returning 0 takes
// the stock "no key this frame" path, so consumed keys never reach the stock switch.
CallHook<int()> console_handle_input_key_get_hook{
    0x00509F96,
    []() {
        const int raw = console_handle_input_key_get_hook.call_target();
        // Server consoles show none of this UI, and the Win32 one echoes commands itself.
        if (win32_console_is_enabled() || rf::is_dedicated_server) {
            return raw;
        }
        const int key = raw & 0x7FFF; // stock strips the same bit right after this call
        if (key <= 0) {
            return raw;
        }
        const int base = key & rf::KEY_MASK;
        const bool ctrl = key & rf::KEY_CTRLED;

        if (ctrl && base == rf::KEY_F) {
            g_filter_mode = !g_filter_mode;
            g_filter.clear();
            g_scroll = 0;
            return 0;
        }
        if (base == rf::KEY_PAGEUP || base == rf::KEY_PAGEDOWN) {
            scroll_by(base == rf::KEY_PAGEUP ? g_page_rows : -g_page_rows);
            g_user_scrolled = true;
            return 0;
        }
        if (ctrl && (base == rf::KEY_HOME || base == rf::KEY_END)) {
            g_scroll = base == rf::KEY_HOME ? INT_MAX : 0;
            g_user_scrolled = true;
            return 0;
        }
        if (g_filter_mode ? handle_filter_key(key) : handle_edit_key(key)) {
            return 0;
        }
        if (!g_filter_mode && (base == rf::KEY_ENTER || base == rf::KEY_PADENTER) && rf::console::cmd_line_len > 0) {
            const std::string echo = std::string{"> "} + rf::console::cmd_line;
            rf::console::output(echo.c_str(), &palette::command);
        }
        // Stock handles it (Enter, history, Tab, paste, Esc/~); all of those leave the cursor at the end.
        g_cursor = INT_MAX;
        return raw;
    },
};

std::string format_time_left(int secs)
{
    return secs >= 3600 ? std::format("{}:{:02}:{:02}", secs / 3600, secs / 60 % 60, secs % 60)
                        : std::format("{}:{:02}", secs / 60, secs % 60);
}

std::string build_statusline()
{
    std::vector<std::string> parts;
    if (rf::is_multi && rf::local_player) {
        parts.emplace_back(rf::local_player->name.c_str());
        if (multi_local_player_is_spectating()) {
            parts.emplace_back("Spectating");
        }
        else if (multi_game_type_is_team_type(rf::multi_get_game_type())) {
            parts.emplace_back(rf::local_player->team == rf::TEAM_RED ? "Team Red" : "Team Blue");
        }
    }
    if (rf::level.flags & rf::LEVEL_LOADED) {
        parts.emplace_back(level_display_name());
    }
    if (rf::is_multi) {
        parts.push_back(rf::is_server ? std::format("hosting {}", rf::netgame.name.c_str())
                                      : std::string{rf::netgame.name.c_str()});
        int bots = 0;
        const int players = multi_count_participants(&bots);
        parts.push_back(bots ? std::format("{}/{} players ({} bots)", players, rf::netgame.max_players, bots)
                             : std::format("{}/{} players", players, rf::netgame.max_players));
        if (const std::optional<float> time_left = multi_time_left()) {
            parts.push_back(format_time_left(static_cast<int>(*time_left)) + " left");
        }
    }
    const std::string ff_user = g_game_config.fflink_username.value();
    parts.push_back(ff_user.empty() ? "FF: not linked" : "FF: " + ff_user);

    std::string out;
    for (const auto& p : parts) {
        if (!out.empty()) {
            out += "  |  ";
        }
        out += p;
    }
    return out;
}

void ensure_font()
{
    const int px = g_alpine_game_config.console_font_size;
    if (px != g_font_px) {
        // Each size builds a font atlas that is never freed; the setting's clamp bounds how many.
        g_font = rf::gr::load_font(std::format("consolefont.ttf:{}", px).c_str());
        g_font_bold = rf::gr::load_font(std::format("consolefont-bold.ttf:{}", px).c_str());
        g_font_px = px;
    }
}

void draw_text(int x, int y, const std::string& s, const rf::Color& color, bool bold = false)
{
    rf::gr::set_color(color);
    rf::gr::string(x, y, s.c_str(), bold ? g_font_bold : g_font);
}

struct Row
{
    const Line* line;
    uint32_t id;
    size_t start;
    size_t len;
    int y;
};

// Map a screen point to a text position using the rows drawn this frame (rows[0] is the bottom).
TextPos hit_test(const std::vector<Row>& rows, int mx, int my, int cw, int lh)
{
    if (my < rows.back().y) {
        return {rows.back().id, rows.back().start};
    }
    const Row* row = &rows.front();
    if (my >= row->y + lh) {
        return {row->id, row->start + row->len};
    }
    for (const Row& r : rows) {
        if (my >= r.y && my < r.y + lh) {
            row = &r;
            break;
        }
    }
    const int col = std::clamp((mx - pad + cw / 2) / cw, 0, static_cast<int>(row->len));
    return {row->id, row->start + static_cast<size_t>(col)};
}

void update_mouse(const std::vector<Row>& rows, int area_top, int area_h, int text_right, int cw, int lh)
{
    const bool visible = rf::console::console_is_visible();
    const bool in_gameplay = rf::gameseq_get_state() == rf::GS_GAMEPLAY;
    if (!visible || !in_gameplay) {
        release_cursor();
    }
    else if (cursor_toggle_pressed()) {
        if (g_cursor_on) {
            release_cursor();
        }
        else {
            g_cursor_on = true;
        }
    }
    if (g_cursor_on) {
        gameplay_overlay_apply_mouse(true); // re-assert: respawns and camera changes reset mouse mode
    }
    // Menus already show a cursor, so selection works there without the toggle.
    if (!visible || (in_gameplay && !g_cursor_on) || rows.empty()) {
        g_selecting = false;
        return;
    }
    int mx = 0;
    int my = 0;
    int mz = 0;
    rf::mouse_get_pos(mx, my, mz);
    if (mouse_pressed(0)) {
        g_selecting = my >= area_top && my < area_top + area_h && mx < text_right;
        g_sel_a = g_sel_b = g_selecting ? hit_test(rows, mx, my, cw, lh) : TextPos{};
    }
    if (g_selecting) {
        g_sel_b = hit_test(rows, mx, my, cw, lh);
        if (!mouse_down(0)) {
            g_selecting = false;
            if (has_selection()) {
                copy_selection();
            }
            else {
                prefill_clicked_command();
            }
        }
    }
}

// Clicks on the console must not fire weapons.
bool console_cursor_blocks_action(rf::ControlConfig* ccp, rf::ControlConfigAction action)
{
    if (!rf::local_player || ccp != &rf::local_player->settings.controls
        || (action != rf::CC_ACTION_PRIMARY_ATTACK && action != rf::CC_ACTION_SECONDARY_ATTACK)) {
        return false;
    }
    if (g_swallow_attack && !mouse_down(0) && !mouse_down(1)) {
        g_swallow_attack = false;
    }
    if (action == rf::CC_ACTION_SECONDARY_ATTACK && console_open_in_gameplay()) {
        return true;
    }
    return g_cursor_on || g_swallow_attack;
}

// Turns everything a failed command printed into an error.
FunHook<int(const char*)> console_do_command_hook{
    &rf::console::do_command,
    [](const char* text) {
        const uint32_t start_id = g_first_id + static_cast<uint32_t>(g_lines.size());
        const int result = console_do_command_hook.call_target(text);
        if (result != 0) {
            for (uint32_t id = std::max(start_id, g_first_id); id - g_first_id < g_lines.size(); ++id) {
                g_lines[id - g_first_id].color = palette::error;
            }
        }
        return result;
    },
};
} // namespace

void console_ui_add_output(const char* text, const rf::Color* color)
{
    // Mirror the stock writer: split on '\n', no extra line for a trailing newline.
    rf::Color c = color ? *color : rf::console::default_text_color;
    rf::Color prefix_color{};
    size_t prefix_len = 0;
    if (g_chat_color >= 0) {
        const bool named = !g_chat_prefix.empty();
        c = palette::chat;
        switch (static_cast<rf::ChatMsgColor>(g_chat_color)) {
            case rf::ChatMsgColor::red_white:
                prefix_color = palette::red_team;
                break;
            case rf::ChatMsgColor::blue_white:
                prefix_color = palette::blue_team;
                break;
            case rf::ChatMsgColor::red_red:
                c = prefix_color = palette::red_team;
                break;
            case rf::ChatMsgColor::blue_blue:
                c = prefix_color = palette::blue_team;
                break;
            case rf::ChatMsgColor::gold_white:
                prefix_color = palette::server;
                break;
            case rf::ChatMsgColor::white_white:
                if (named) {
                    prefix_color = palette::chat_name;
                    break;
                }
                [[fallthrough]];
            default:
                c = prefix_color = palette::event;
                break;
        }
        if (named && std::strncmp(text, g_chat_prefix.c_str(), g_chat_prefix.size()) == 0) {
            prefix_len = g_chat_prefix.size();
        }
    }
    else if (!color) {
        // Command listings (help, Tab): "name - description" or a bare name. Stock `help` indents
        // its listing by one space; drop it so both listings look the same.
        const char* t = text[0] == ' ' ? text + 1 : text;
        for (int i = 0; i < rf::console::num_commands; ++i) {
            const char* name = g_commands_buffer[i]->name;
            const size_t n = std::strlen(name);
            if (string_istarts_with(t, name)
                && (t[n] == '\0' || t[n] == '\n' || std::strncmp(t + n, " - ", 3) == 0)) {
                prefix_color = palette::command;
                prefix_len = n;
                c = palette::description;
                text = t;
                break;
            }
        }
    }
    const char* end = text + std::strlen(text);
    const char* p = text;
    while (p < end) {
        const char* nl = std::find(p, end, '\n');
        std::string s{p, nl};
        std::erase(s, '\r');
        const bool match = !filter_active() || string_icontains(s, g_filter);
        if (g_scroll > 0 && match) {
            // Keep the view anchored while the user reads back.
            scroll_by(rows_for(s));
            ++g_unseen;
        }
        g_lines.push_back({std::move(s), c, prefix_len, prefix_color, match});
        prefix_len = 0; // only the first line starts with the sender
        if (g_lines.size() > max_lines) {
            g_lines.pop_front();
            ++g_first_id;
        }
        p = nl + 1;
    }
}

void console_ui_render()
{
    const float open = std::clamp(static_cast<float>(rf::console::client_height - stock_height_closed)
                                      / static_cast<float>(stock_height_open - stock_height_closed), 0.0f, 1.0f);
    const int sw = rf::gr::screen_width();
    const int full_h = rf::gr::screen_height() * g_alpine_game_config.console_height_pct / 100;
    const int h = static_cast<int>(static_cast<float>(full_h) * open);
    g_console_bottom = std::max(h, 0);
    if (h <= 0) {
        // Also covers a long frame that finished the close animation before update_mouse saw it.
        release_cursor();
        g_selecting = false;
        exit_filter();
        return;
    }
    ensure_font();
    const int lh = rf::gr::get_font_height(g_font);
    const int cw = std::max(1, rf::gr::get_char_size('M', g_font).first);
    const int y0 = h - full_h; // slides the full-height layout down from above the screen

    rf::gr::set_color(rf::console::background_color);
    rf::gr::rect(0, 0, sw, h);

    // Bottom-up layout: input, statusline, scrollback. With the statusline off, scrollback takes its
    // row and the occasional notice (search count, "Copied") draws over the bottom row instead.
    const bool statusline = g_alpine_game_config.console_statusline;
    const int input_y = y0 + full_h - pad - lh;
    const int status_y = input_y - pad - lh;
    const int area_top = y0 + pad;
    const int area_h = (statusline ? status_y - pad : input_y - pad) - area_top;
    const int vis_rows = std::max(1, area_h / lh);
    const int scrollbar_w = 4;
    g_cols = std::max(10, (sw - 2 * pad - scrollbar_w - pad) / cw);
    g_page_rows = std::max(1, vis_rows - 1);

    // Mouse wheel (positive = away from the user = back in history).
    if (console_ui_takes_mouse_wheel() && rf::mouse_dz != 0) {
        scroll_by(rf::mouse_dz > 0 ? wheel_rows : -wheel_rows);
        g_user_scrolled = true;
    }

    int total_rows = 0;
    int matches = 0;
    for (const Line& l : g_lines) {
        if (line_matches(l)) {
            total_rows += rows_for(l.text);
            ++matches;
        }
    }
    g_scroll = std::clamp(g_scroll, 0, std::max(0, total_rows - vis_rows));
    if (g_user_scrolled && g_scroll != g_shown_scroll) {
        play_console_sound(g_scroll > g_shown_scroll ? stock_sound_id::list_scroll_up
                                                     : stock_sound_id::list_scroll_down);
    }
    g_user_scrolled = false;
    g_shown_scroll = g_scroll;
    if (g_scroll == 0) {
        g_unseen = 0;
    }

    std::vector<Row> rows;
    int skip = g_scroll;
    uint32_t id = g_first_id + static_cast<uint32_t>(g_lines.size());
    int row_y = area_top + area_h - lh;
    for (auto it = g_lines.rbegin(); it != g_lines.rend() && static_cast<int>(rows.size()) < vis_rows; ++it) {
        --id;
        if (!line_matches(*it)) {
            continue;
        }
        const int n = rows_for(it->text);
        for (int r = n - 1; r >= 0 && static_cast<int>(rows.size()) < vis_rows; --r) {
            if (skip > 0) {
                --skip;
                continue;
            }
            const size_t start = static_cast<size_t>(r) * g_cols;
            const size_t len = std::min<size_t>(g_cols, it->text.size() - std::min(start, it->text.size()));
            rows.push_back({&*it, id, start, len, row_y});
            row_y -= lh;
        }
    }

    update_mouse(rows, area_top, area_h, sw - pad - scrollbar_w, cw, lh);
    const TextPos sel_lo = std::min(g_sel_a, g_sel_b);
    const TextPos sel_hi = std::max(g_sel_a, g_sel_b);

    for (const Row& row : rows) {
        const int y = row.y;
        const std::string seg = row.line->text.substr(row.start, row.len);
        if (has_selection() && row.id >= sel_lo.id && row.id <= sel_hi.id) {
            const size_t s = std::max(row.start, row.id == sel_lo.id ? sel_lo.off : 0);
            const size_t e = std::min(row.start + row.len, row.id == sel_hi.id ? sel_hi.off : SIZE_MAX);
            if (e > s) {
                rf::gr::set_color(80, 140, 255, 120);
                rf::gr::rect(pad + static_cast<int>(s - row.start) * cw, y, static_cast<int>(e - s) * cw, lh);
            }
        }
        if (filter_active()) {
            rf::gr::set_color(255, 200, 0, 90);
            const std::string_view seg_view{seg};
            for (size_t i = 0; i + g_filter.size() <= seg.size(); ++i) {
                if (string_istarts_with(seg_view.substr(i), g_filter)) {
                    rf::gr::rect(pad + static_cast<int>(i) * cw, y, static_cast<int>(g_filter.size()) * cw, lh);
                }
            }
        }
        const size_t pre =
            row.start < row.line->prefix_len ? std::min(row.line->prefix_len - row.start, seg.size()) : 0;
        if (pre > 0) {
            // Prefix = command name or chat sender. Inconsolata Bold has the same 500-unit advance as Regular,
            // so the column math (cw) still holds.
            draw_text(pad, y, seg.substr(0, pre), row.line->prefix_color, true);
        }
        draw_text(pad + static_cast<int>(pre) * cw, y, seg.substr(pre), row.line->color);
    }

    if (total_rows > vis_rows) {
        const int track_x = sw - pad - scrollbar_w;
        const int thumb_h = std::max(lh / 2, area_h * vis_rows / total_rows);
        const int thumb_y =
            area_top + (area_h - thumb_h) * (total_rows - vis_rows - g_scroll) / (total_rows - vis_rows);
        rf::gr::set_color(255, 255, 255, 40);
        rf::gr::rect(track_x, area_top, scrollbar_w, area_h);
        rf::gr::set_color(255, 255, 255, 150);
        rf::gr::rect(track_x, thumb_y, scrollbar_w, thumb_h);
    }
    if (g_scroll > 0) {
        const std::string msg = g_unseen > 0
            ? std::format("-- {} new line{} below (Ctrl+End) --", g_unseen, g_unseen == 1 ? "" : "s")
            : std::string{"-- scrolled up (Ctrl+End) --"};
        const int w = static_cast<int>(msg.size()) * cw;
        const int x = sw - pad * 3 - scrollbar_w - w;
        const int iy = area_top + area_h - lh;
        rf::gr::set_color(0, 0, 0, 180);
        rf::gr::rect(x - pad, iy, w + 2 * pad, lh);
        draw_text(x, iy, msg, {255, 220, 100, 255});
    }

    // Statusline (search count and copy notice still show when it's disabled).
    std::string status = statusline ? build_statusline() : std::string{};
    const auto prepend = [&](const std::string& s) { status = status.empty() ? s : s + "  |  " + status; };
    if (g_filter_mode) {
        prepend(std::format("{} match{}", matches, matches == 1 ? "" : "es"));
    }
    if (GetTickCount() < g_copied_until) {
        prepend(std::format("Copied {} chars", g_copied_chars));
    }
    if (!status.empty()) {
        rf::gr::set_color(0, 0, 0, statusline ? 100 : 220);
        rf::gr::rect(0, status_y - pad / 2, sw, lh + pad);
        draw_text(pad, status_y, status, {170, 200, 230, 255});
    }

    const rf::Color text_color = rf::console::default_text_color;
    const std::string prompt = g_filter_mode ? "find: " : "] ";
    const std::string input = g_filter_mode ? g_filter : std::string{rf::console::cmd_line};
    const int input_x = pad + static_cast<int>(prompt.size()) * cw;
    draw_text(pad, input_y, prompt, text_color);
    draw_text(input_x, input_y, input, text_color);
    if (!g_filter_mode) {
        if (auto* cmd = ghost_suggestion()) {
            const int gx = input_x + static_cast<int>(input.size()) * cw;
            const std::string rest = cmd->name + input.size();
            draw_text(gx, input_y, rest, {255, 255, 255, 90});
            if (cmd->help && cmd->help[0]) {
                draw_text(gx + static_cast<int>(rest.size() + 2) * cw, input_y, cmd->help, {255, 255, 255, 55});
            }
        }
    }
    if ((GetTickCount() / 500) % 2 == 0) {
        const int caret = g_filter_mode ? static_cast<int>(input.size()) : cursor();
        rf::gr::set_color(text_color);
        rf::gr::rect(input_x + caret * cw, input_y, 2, lh);
    }
}

bool console_ui_cursor_active()
{
    return g_cursor_on;
}

bool console_ui_takes_mouse_wheel()
{
    return !legacy_console_enabled() && g_alpine_game_config.console_mouse_wheel
        && rf::console::console_is_visible();
}

void console_ui_begin_chat(int color, const char* prefix)
{
    g_chat_color = color;
    g_chat_prefix = prefix ? prefix : "";
}

void console_ui_end_chat()
{
    g_chat_color = -1;
}

void console_ui_apply_patches()
{
    console_do_command_hook.install();
    mouse_was_button_pressed_hook.install();
    mouse_button_is_down_hook.install();
    console_handle_input_key_get_hook.install();
    control_input_filter_add_veto(&console_cursor_blocks_action);
}
