#include <format>
#include <optional>
#include <string>
#include <utility>
#include <common/utils/string-utils.h>
#include <xlog/xlog.h>
#include "server_switch.h"
#include "network.h"
#include "server_browser_data.h"
#include "demo/demo.h"
#include "../misc/alpine_settings.h"
#include "../misc/misc.h"
#include "../misc/settings_registry.h"
#include "../os/console.h"
#include "../rf/gameseq.h"
#include "../rf/input.h"
#include "../rf/localize.h"
#include "../rf/os/os.h"
#include "../rf/ui.h"

namespace
{

// The console's join, waiting on the stock popup's answer.
struct ConsoleJoin
{
    rf::NetAddr addr;
    std::string password;
    ServerJoinConfirmation confirmation;
};
std::optional<ConsoleJoin> g_console_join;

void console_join_confirmed()
{
    if (const std::optional<ConsoleJoin> join = std::exchange(g_console_join, std::nullopt)) {
        server_join(join->addr, join->password);
    }
}

void console_join_canceled()
{
    g_console_join.reset();
}

// It would stay over the confirmation and the level change.
void close_console()
{
    if (rf::console::console_is_visible()) {
        rf::console::console_close();
    }
}

std::string server_display_name(const rf::NetAddr& addr)
{
    const int index = server_browser_find(addr);
    if (index >= 0 && !server_browser_entries()[index].name.empty()) {
        return server_browser_entries()[index].name;
    }
    return std::format("{}", addr);
}

ConsoleCommand2 connect_cmd{
    "connect",
    [](std::string address, std::optional<std::string> password) {
        if (rf::is_dedicated_server) {
            rf::console::print("This command is not available on dedicated servers");
            return;
        }
        const std::optional<rf::NetAddr> addr = parse_server_address(address);
        if (!addr) {
            rf::console::print("Not a server address: {}. Give it as a.b.c.d or a.b.c.d:port.", address);
            return;
        }
        if (demo_playback_active()) {
            rf::console::print("Stop the demo before joining a server.");
            return;
        }
        // As the Multi menu's JOIN, never over a single player game.
        if (!rf::is_multi && (rf::gameseq_in_gameplay() || rf::ui::mainmenu_game_beneath)) {
            rf::console::print("Leave the single player game before joining a server.");
            return;
        }
        if (server_browser_is_current_server(*addr)) {
            rf::console::print("You are already on that server.");
            return;
        }
        // What the browser last heard from the server, if it has.
        if (const int index = server_browser_find(*addr); index >= 0 && server_browser_entries()[index].has_info) {
            const ServerBrowserEntry& entry = server_browser_entries()[index];
            if (entry.incompatible) {
                rf::console::print("{}", rf::strings::incompatible_version);
                return;
            }
            if (const std::optional<std::string> refusal = server_join_mod_mismatch(entry.mod)) {
                rf::console::print("{}", *refusal);
                return;
            }
            if (entry.unknown_game_type) {
                show_unsupported_game_type_popup();
                return;
            }
        }
        const std::string pw = password.value_or("");
        if (!server_join_needs_confirmation()) {
            rf::console::print("Connecting to {}...", *addr);
            close_console();
            server_join(*addr, pw);
            return;
        }
        if (rf::ui::popup_is_active()) {
            rf::console::print("Close the open popup first.");
            return;
        }
        close_console();
        g_console_join = ConsoleJoin{*addr, pw, server_join_confirmation(server_display_name(*addr))};
        const char* choices[2] = {"CANCEL", "JOIN"};
        void (*callbacks[2])() = {console_join_canceled, console_join_confirmed};
        int keys[2] = {rf::KEY_ESC, rf::KEY_J};
        rf::ui::popup_custom(g_console_join->confirmation.title.c_str(), g_console_join->confirmation.text.c_str(), 2,
                             choices, callbacks, 0, keys);
    },
    "Join a server, leaving the game in progress",
    "connect <ip[:port]> [password]",
};

ConsoleCommand2 confirm_server_switch_cmd{
    "cl_confirm_server_switch",
    [](std::optional<bool> enabled) {
        setting_console_set_bool("confirm_server_switch", enabled);
        rf::console::print("Confirming a server switch is {}",
                           g_alpine_game_config.confirm_server_switch ? "enabled" : "disabled");
    },
    "Ask before leaving a game to join another server (a listen server host is always asked)",
    "cl_confirm_server_switch [bool]",
};

} // namespace

std::optional<std::string> server_join_mod_mismatch(const std::string& server_mod)
{
    const std::string running = tc_mod_is_loaded() ? rf::mod_param.get_arg() : "";
    if (!server_mod.empty() && !string_iequals(server_mod, running)) {
        return string_replace(rf::strings::mod_not_running, "%s", server_mod);
    }
    if (server_mod.empty() && !running.empty()) {
        return string_replace(rf::strings::mod_running, "%s", running);
    }
    return std::nullopt;
}

bool server_join_needs_confirmation()
{
    return rf::is_multi && (rf::is_server || g_alpine_game_config.confirm_server_switch);
}

ServerJoinConfirmation server_join_confirmation(const std::string& server_name)
{
    if (rf::is_server) {
        return {"End Your Game?",
                std::format("You are hosting this game. Joining {} ends it for everyone playing on it.", server_name)};
    }
    return {"Switch Server?", std::format("Leave {} and join {}?", rf::netgame.name.c_str(), server_name)};
}

void server_join(const rf::NetAddr& addr, const std::string& password)
{
    xlog::info("[ServerBrowser] Joining {}", addr);
    if (rf::gameseq_get_state() == rf::GS_MULTI_SERVER_LIST) {
        multi_join_game(addr, password);
        return;
    }
    start_join_multi_game_sequence(addr, password);
    if (rf::is_multi) {
        // The engine's own way out of a game, as a lost connection takes it.
        rf::multi_stop();
        rf::gameseq_set_state(rf::GS_END_GAME, true);
        rf::multi_clear_current_server_addr();
    }
    else {
        // The jump to the server list runs from the main menu's init.
        rf::gameseq_set_state(rf::GS_MAIN_MENU, true);
    }
}

void server_switch_apply_patch()
{
    connect_cmd.register_cmd();
    confirm_server_switch_cmd.register_cmd();
}
