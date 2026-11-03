#include <windows.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include <xlog/xlog.h>
#include <common/utils/list-utils.h>
#include <common/version/version.h>
#include "discord_presence.h"
#include "alpine_settings.h"
#include "misc.h"
#include "../hud/hud.h"
#include "../hud/multi_spectate.h"
#include "../multi/demo/demo.h"
#include "../multi/gametype.h"
#include "../multi/multi.h"
#include "../multi/server_internal.h"
#include "../multi/vote_client.h"
#include "../os/console.h"
#include "../rf/cutscene.h"
#include "../rf/gameseq.h"
#include "../rf/level.h"
#include "../rf/multi.h"
#include "../rf/os/os.h"
#include "../rf/player/player.h"

namespace
{
using json = nlohmann::json;

// Application ID of the "Alpine Faction" app in the Discord developer portal. Its rich presence art
// assets must include one named by large_image_key.
constexpr const char* discord_client_id = "1565119498611068988";
constexpr const char* large_image_key = "logo";
constexpr std::string_view join_url_base = "https://join.alpinefaction.com/";

// Discord's 2-128 character limit for activity strings, applied to bytes.
constexpr size_t max_activity_str_len = 128;
// Discord drops SET_ACTIVITY calls beyond 5 per 20 seconds.
constexpr auto min_update_interval = std::chrono::seconds{4};
constexpr auto reconnect_interval = std::chrono::seconds{15};
constexpr auto handshake_timeout = std::chrono::seconds{5};
constexpr DWORD state_poll_interval_ms = 1000;
// Discord's own RPC library caps frames at 64 KiB.
constexpr uint32_t max_frame_len = 64 * 1024;
// Remaining-time drift (time limit votes, level time resync) tolerated before the end timestamp moves.
constexpr int64_t end_time_tolerance_ms = 5000;

enum class DiscordOpcode : uint32_t
{
    handshake = 0,
    frame = 1,
    close = 2,
    ping = 3,
    pong = 4,
};

struct DiscordFrameHeader
{
    uint32_t opcode;
    uint32_t length;
};
static_assert(sizeof(DiscordFrameHeader) == 8);

// json::value() throws on a present key of another type, and command responses carry "evt": null.
std::string json_str(const json& obj, const char* key)
{
    const auto it = obj.find(key);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

// The game console log appender isn't thread-safe, so the worker queues its log lines for the main thread.
struct WorkerLog
{
    std::mutex mutex;
    std::vector<std::pair<xlog::Level, std::string>> lines;
};

WorkerLog g_worker_log;
std::unordered_set<std::string> g_logged_lines;

void worker_log(xlog::Level level, std::string line)
{
    std::lock_guard lock{g_worker_log.mutex};
    g_worker_log.lines.emplace_back(level, std::move(line));
}

// Logs each distinct line once per worker run, so a reconnect loop can't spam the console.
void flush_worker_log()
{
    std::vector<std::pair<xlog::Level, std::string>> lines;
    {
        std::lock_guard lock{g_worker_log.mutex};
        lines.swap(g_worker_log.lines);
    }
    for (const auto& [level, line] : lines) {
        if (g_logged_lines.insert(line).second) {
            xlog::log(level, "[Discord] {}", line);
        }
    }
}

// Owned and used only by the worker thread.
class DiscordIpc
{
public:
    ~DiscordIpc()
    {
        close();
    }

    bool is_open() const
    {
        return pipe_ != INVALID_HANDLE_VALUE;
    }

    bool open()
    {
        // Discord listens on the first free pipe of discord-ipc-0..9.
        for (int i = 0; i < 10; ++i) {
            const auto name = std::format("\\\\.\\pipe\\discord-ipc-{}", i);
            pipe_ = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe_ != INVALID_HANDLE_VALUE) {
                ready_ = false;
                return true;
            }
        }
        return false;
    }

    void close()
    {
        if (pipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
        ready_ = false;
    }

    bool is_ready() const
    {
        return ready_;
    }

    bool send(DiscordOpcode opcode, const std::string& payload)
    {
        std::string frame(sizeof(DiscordFrameHeader) + payload.size(), '\0');
        const DiscordFrameHeader header{static_cast<uint32_t>(opcode), static_cast<uint32_t>(payload.size())};
        std::memcpy(frame.data(), &header, sizeof(header));
        std::memcpy(frame.data() + sizeof(header), payload.data(), payload.size());
        DWORD written = 0;
        if (!WriteFile(pipe_, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr)
            || written != frame.size()) {
            close();
            return false;
        }
        return true;
    }

    bool drain()
    {
        while (is_open()) {
            DWORD available = 0;
            if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &available, nullptr)) {
                close();
                return false;
            }
            if (available < sizeof(DiscordFrameHeader)) {
                return true;
            }
            DiscordFrameHeader header{};
            std::string payload;
            if (!read_exact(&header, sizeof(header))) {
                return false;
            }
            if (header.length > max_frame_len) {
                worker_log(xlog::Level::warn, "Oversized frame");
                close();
                return false;
            }
            payload.resize(header.length);
            if (header.length > 0 && !read_exact(payload.data(), header.length)) {
                return false;
            }
            try {
                handle_frame(static_cast<DiscordOpcode>(header.opcode), payload);
            }
            catch (const std::exception& e) {
                worker_log(xlog::Level::warn, std::format("Bad frame: {}", e.what()));
                close();
                return false;
            }
        }
        return false;
    }

private:
    bool read_exact(void* buf, DWORD size)
    {
        DWORD read = 0;
        if (!ReadFile(pipe_, buf, size, &read, nullptr) || read != size) {
            close();
            return false;
        }
        return true;
    }

    void handle_frame(DiscordOpcode opcode, const std::string& payload)
    {
        if (opcode == DiscordOpcode::ping) {
            send(DiscordOpcode::pong, payload);
            return;
        }
        const json msg = json::parse(payload, nullptr, false);
        if (opcode == DiscordOpcode::close) {
            worker_log(xlog::Level::warn, std::format("Connection closed: {}",
                msg.is_object() ? json_str(msg, "message") : payload));
            close();
            return;
        }
        if (opcode != DiscordOpcode::frame || !msg.is_object()) {
            return;
        }
        const std::string evt = json_str(msg, "evt");
        if (evt == "READY") {
            ready_ = true;
            worker_log(xlog::Level::info, "Connected");
        }
        else if (evt == "ERROR") {
            // Not the raw payload: its nonce would make every line distinct to flush_worker_log.
            const auto data = msg.find("data");
            worker_log(xlog::Level::warn, std::format("{} failed: {}", json_str(msg, "cmd"),
                data != msg.end() && data->is_object() ? json_str(*data, "message") : std::string{}));
        }
    }

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    bool ready_ = false;
};

struct PresenceChannel
{
    std::mutex mutex;
    std::condition_variable_any cv;
    json activity;
    uint64_t revision = 0;
};

PresenceChannel g_channel;
std::jthread g_worker;

struct SessionState
{
    std::string key;
    int64_t start_ms = 0;
    int64_t end_ms = 0;
};

SessionState g_session;
std::string g_last_activity_dump;
DWORD g_last_poll_ticks = 0;

struct JoinPassword
{
    rf::NetAddr addr{};
    bool has_password = false;
};

JoinPassword g_last_join;

void presence_worker(std::stop_token stop)
{
    DiscordIpc ipc;
    uint64_t sent_revision = 0;
    uint64_t nonce = 0;
    auto last_send = std::chrono::steady_clock::now() - min_update_interval;
    std::unique_lock lock{g_channel.mutex};

    while (!stop.stop_requested()) {
        if (!ipc.is_open()) {
            lock.unlock();
            const bool opened = ipc.open();
            if (opened) {
                const json handshake{{"v", 1}, {"client_id", discord_client_id}};
                ipc.send(DiscordOpcode::handshake, handshake.dump());
                const auto deadline = std::chrono::steady_clock::now() + handshake_timeout;
                while (ipc.drain() && !ipc.is_ready() && std::chrono::steady_clock::now() < deadline
                    && !stop.stop_requested()) {
                    Sleep(50);
                }
                if (!ipc.is_ready()) {
                    ipc.close();
                }
            }
            lock.lock();
            if (!ipc.is_open()) {
                g_channel.cv.wait_for(lock, stop, reconnect_interval, [] { return false; });
                continue;
            }
            sent_revision = 0;
        }

        // The periodic wake answers pings and notices a closed pipe.
        g_channel.cv.wait_for(lock, stop, reconnect_interval, [&] { return g_channel.revision != sent_revision; });
        if (stop.stop_requested()) {
            break;
        }

        lock.unlock();
        ipc.drain();
        lock.lock();
        if (!ipc.is_open() || g_channel.revision == sent_revision) {
            continue;
        }

        const auto next_send = last_send + min_update_interval;
        if (std::chrono::steady_clock::now() < next_send) {
            g_channel.cv.wait_until(lock, stop, next_send, [] { return false; });
            continue;
        }

        const json cmd{
            {"cmd", "SET_ACTIVITY"},
            {"args", {{"pid", GetCurrentProcessId()}, {"activity", g_channel.activity}}},
            {"nonce", std::to_string(++nonce)},
        };
        sent_revision = g_channel.revision;
        lock.unlock();
        ipc.send(DiscordOpcode::frame, cmd.dump());
        last_send = std::chrono::steady_clock::now();
        lock.lock();
    }
}

void start_worker()
{
    if (!g_worker.joinable()) {
        g_worker = std::jthread{presence_worker};
    }
}

void stop_worker()
{
    if (g_worker.joinable()) {
        g_worker.request_stop();
        g_worker.join();
        g_worker_log.lines.clear();
        g_logged_lines.clear();
    }
    g_last_activity_dump.clear();
    g_session = {};
}

// RF strings are Windows-1252; Discord requires UTF-8.
std::string activity_str(std::string_view text)
{
    std::string utf8;
    if (!text.empty()) {
        const int wide_len = MultiByteToWideChar(1252, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring wide(wide_len, L'\0');
        MultiByteToWideChar(1252, 0, text.data(), static_cast<int>(text.size()), wide.data(), wide_len);
        const int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_len, nullptr, 0, nullptr, nullptr);
        utf8.resize(utf8_len);
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_len, utf8.data(), utf8_len, nullptr, nullptr);
    }
    if (utf8.size() > max_activity_str_len) {
        size_t cut = max_activity_str_len;
        while (cut > 0 && (static_cast<unsigned char>(utf8[cut]) & 0xC0) == 0x80) {
            --cut;
        }
        utf8.resize(cut);
    }
    if (utf8.size() < 2) {
        utf8.resize(2, ' ');
    }
    return utf8;
}

int64_t unix_now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

bool is_publicly_routable(rf::IpAddr ip)
{
    const uint32_t addr = ip.inner;
    const uint8_t a = addr >> 24;
    const uint8_t b = (addr >> 16) & 0xFF;
    return addr != 0
        && a != 10
        && a != 127
        && !(a == 169 && b == 254)
        && !(a == 172 && b >= 16 && b <= 31)
        && !(a == 192 && b == 168)
        && !(a == 100 && b >= 64 && b <= 127);
}

bool server_has_password()
{
    // An unknown join (none recorded for this address) counts as passworded.
    return !(g_last_join.addr == rf::netgame.server_addr) || g_last_join.has_password;
}

std::string_view current_level_name()
{
    const char* const name = rf::level.name.c_str();
    return name[0] ? std::string_view{name} : std::string_view{rf::level.filename.c_str()};
}

void update_session(const std::string& key, std::optional<int64_t> remaining_ms)
{
    const int64_t now = unix_now_ms();
    if (g_session.key != key) {
        g_session = {key, now, 0};
    }
    if (!remaining_ms) {
        g_session.end_ms = 0;
        return;
    }
    const int64_t end_ms = now + *remaining_ms;
    if (std::abs(end_ms - g_session.end_ms) > end_time_tolerance_ms) {
        g_session.end_ms = end_ms;
    }
}

std::string_view difficulty_name(rf::GameDifficultyLevel level)
{
    switch (level) {
        case rf::GameDifficultyLevel::DIFFICULTY_EASY:
            return "Easy";
        case rf::GameDifficultyLevel::DIFFICULTY_MEDIUM:
            return "Medium";
        case rf::GameDifficultyLevel::DIFFICULTY_HARD:
            return "Hard";
        default:
            return "Impossible";
    }
}

std::string_view activity_mode_prefix()
{
    // The listen-server host runs the match itself and gets no match state packets.
    const bool match_live = rf::is_server ? g_match_info.match_active : vote_client_match_live();
    const bool pre_match = rf::is_server ? g_match_info.pre_match_active : get_local_pre_match_active();
    const bool spectating = multi_spectate_is_spectating() || (!rf::is_server && vote_client_spectating_match());
    if (match_live) {
        return spectating ? "Spectating match: " : "Match: ";
    }
    if (pre_match) {
        return "Match starting: ";
    }
    return spectating ? "Spectating " : "";
}

int count_participants()
{
    int count = 0;
    for (const rf::Player& player : SinglyLinkedList{rf::player_list}) {
        if (!player.is_non_participant()) {
            ++count;
        }
    }
    return count;
}

json build_multi_activity()
{
    const rf::NetGameType game_type = rf::multi_get_game_type();
    const bool level_loaded = rf::level.flags & rf::LEVEL_LOADED;
    const rf::NetAddr server_addr = rf::netgame.server_addr;
    const std::string server_name = rf::netgame.name.c_str();

    json activity;
    if (!level_loaded) {
        activity["details"] = activity_str("Changing level");
        activity["state"] = activity_str(server_name);
    }
    else {
        activity["details"] = activity_str(std::format("{}{} - {}", activity_mode_prefix(),
            multi_game_type_name(game_type), current_level_name()));
        // REV and ESC have no running team score; the scoreboard shows none for them either.
        if (multi_game_type_is_team_type(game_type) && game_type != rf::NG_TYPE_REV && game_type != rf::NG_TYPE_ESC) {
            int red = 0;
            int blue = 0;
            multi_get_team_scores(game_type, red, blue);
            activity["state"] = activity_str(std::format("Red {} - {} Blue | {}", red, blue, server_name));
        }
        else {
            activity["state"] = activity_str(server_name);
        }
    }

    std::optional<int64_t> remaining_ms;
    const float time_left = rf::multi_time_limit - rf::level.time;
    if (level_loaded && rf::multi_time_limit > 0.0f && time_left > 0.0f && !gt_uses_rounds()) {
        remaining_ms = static_cast<int64_t>(time_left * 1000.0f);
    }
    update_session(std::format("multi {} {}", server_addr, server_name), remaining_ms);
    activity["timestamps"] = g_session.end_ms ? json{{"end", g_session.end_ms}} : json{{"start", g_session.start_ms}};

    activity["assets"] = {
        {"large_image", large_image_key},
        {"large_text", activity_str(level_loaded ? rf::level.filename.c_str() : PRODUCT_NAME)},
    };

    const int max_players = rf::netgame.max_players;
    if (max_players > 0) {
        activity["party"] = {
            {"id", std::format("{:x}", std::hash<rf::NetAddr>{}(server_addr))},
            {"size", {std::clamp(count_participants(), 1, max_players), max_players}},
        };
    }

    if (g_alpine_game_config.discord_join_button && !rf::is_server && is_publicly_routable(server_addr.ip_addr)
        && (rf::netgame.flags & rf::NG_FLAG_NOT_LAN_ONLY) && !server_has_password()) {
        const std::string url = std::format("{}{}/{}", join_url_base, server_addr.ip_addr, server_addr.port);
        activity["buttons"] = json::array({{{"label", "Join server"}, {"url", url}}});
    }
    return activity;
}

json build_activity()
{
    if (demo_playback_active()) {
        update_session("demo", std::nullopt);
        return {
            {"details", activity_str("Watching a demo")},
            {"state", activity_str(current_level_name())},
            {"timestamps", {{"start", g_session.start_ms}}},
            {"assets", {{"large_image", large_image_key}, {"large_text", activity_str(rf::level.filename.c_str())}}},
        };
    }
    if (rf::is_multi) {
        return build_multi_activity();
    }

    json activity{{"assets", {{"large_image", large_image_key}, {"large_text", PRODUCT_NAME}}}};
    const rf::GameState state = rf::gameseq_get_state();
    // Esc pushes the main menu over gameplay as the pause menu.
    const bool paused = gameseq_is_stacked(rf::GS_MAIN_MENU) && gameseq_is_deep_stacked(rf::GS_GAMEPLAY);
    if ((rf::level.flags & rf::LEVEL_LOADED) && (state != rf::GS_MAIN_MENU || paused)) {
        update_session("single", std::nullopt);
        const char* const mod = rf::mod_param.found() ? rf::mod_param.get_arg() : nullptr;
        activity["details"] = activity_str(std::format("{} \xB7 {}", mod && mod[0] ? mod : "Single player",
            difficulty_name(rf::game_get_skill_level())));
        const std::string_view prefix = paused ? "Paused: " : rf::cutscene_is_playing() ? "Cutscene: " : "";
        activity["state"] = activity_str(std::format("{}{}", prefix, current_level_name()));
        activity["assets"]["large_text"] = activity_str(rf::level.filename.c_str());
    }
    else {
        update_session("menu", std::nullopt);
        activity["details"] = activity_str(state == rf::GS_MULTI_SERVER_LIST ? "Browsing servers" : "In the menus");
    }
    activity["timestamps"] = {{"start", g_session.start_ms}};
    return activity;
}

ConsoleCommand2 discord_presence_cmd{
    "cl_discord_presence",
    [](std::optional<bool> enabled) {
        g_alpine_game_config.discord_rich_presence = enabled.value_or(!g_alpine_game_config.discord_rich_presence);
        rf::console::print("Discord rich presence is {}",
            g_alpine_game_config.discord_rich_presence ? "enabled" : "disabled");
    },
    "Show what you are playing, including the server, map and player count in multiplayer, as your Discord activity",
    "cl_discord_presence [bool]",
};

ConsoleCommand2 discord_join_button_cmd{
    "cl_discord_join_button",
    [](std::optional<bool> enabled) {
        g_alpine_game_config.discord_join_button = enabled.value_or(!g_alpine_game_config.discord_join_button);
        rf::console::print("Discord rich presence join button is {}",
            g_alpine_game_config.discord_join_button ? "enabled" : "disabled");
    },
    "Offer a join link to public, password-free servers in your Discord activity",
    "cl_discord_join_button [bool]",
};
} // namespace

void discord_presence_init()
{
    discord_presence_cmd.register_cmd();
    discord_join_button_cmd.register_cmd();
}

void discord_presence_do_frame()
{
    // A client bot would replace the player's own presence.
    if (rf::is_dedicated_server || is_headless_mode() || client_bot_launch_enabled()) {
        return;
    }
    if (!g_alpine_game_config.discord_rich_presence) {
        stop_worker();
        return;
    }
    const DWORD now = GetTickCount();
    if (g_worker.joinable() && now - g_last_poll_ticks < state_poll_interval_ms) {
        return;
    }
    g_last_poll_ticks = now;
    flush_worker_log();
    start_worker();

    json activity = build_activity();
    std::string dump = activity.dump();
    if (dump == g_last_activity_dump) {
        return;
    }
    g_last_activity_dump = std::move(dump);
    {
        std::lock_guard lock{g_channel.mutex};
        g_channel.activity = std::move(activity);
        ++g_channel.revision;
    }
    g_channel.cv.notify_all();
}

void discord_presence_shutdown()
{
    stop_worker();
}

void discord_presence_on_join_request(const rf::NetAddr& addr, bool has_password)
{
    g_last_join = {addr, has_password};
}
