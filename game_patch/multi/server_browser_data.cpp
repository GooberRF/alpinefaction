#include <winsock2.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <exception>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <thread>
#include <unordered_map>
#include <utility>
#include <common/rfproto.h>
#include <common/utils/string-utils.h>
#include <xlog/xlog.h>
#include "server_browser_data.h"
#include "multi.h"
#include "tracker.h"
#include "../misc/alpine_settings.h"
#include "../misc/settings_registry.h"
#include "../os/console.h"
#include "../rf/file/file.h"
#include "../rf/localize.h"
#include "../rf/ui.h"
#include "../purefaction/pf_packets.h"

namespace
{

using Clock = std::chrono::steady_clock;

constexpr size_t max_entries = 0x2000;
constexpr size_t max_favorites = 0x200;
constexpr size_t max_lan_entries = 0x200;
constexpr int max_in_flight = 16;
constexpr int max_attempts = 2;
constexpr std::chrono::milliseconds request_gap{20};
constexpr std::chrono::milliseconds answer_timeout{2000};
constexpr std::chrono::milliseconds lan_window{2000};
constexpr std::chrono::seconds dns_timeout{4};
constexpr std::chrono::seconds tracker_timeout{10};
constexpr std::chrono::seconds quiet_refresh_interval{5};
constexpr std::chrono::seconds roster_interval{3};
constexpr std::chrono::seconds roster_wait{2};
constexpr int max_datagrams_per_frame = 512;
constexpr uint8_t max_roster_segments = 32;

enum class PingState : uint8_t
{
    idle,
    queued,
    in_flight,
};

struct EntryState
{
    PingState ping_state = PingState::idle;
    bool counts_for_batch = false;
    bool queried = false;
    int attempts = 0;
    Clock::time_point sent_at{};
    std::optional<Clock::time_point> last_quiet;
    uint32_t tracker_gen = 0;
    std::optional<Clock::time_point> roster_first_sent;
    std::optional<Clock::time_point> roster_sent;
    uint8_t roster_response_id = 0;
    std::vector<std::optional<std::vector<uint8_t>>> roster_slices;
};

struct DnsJob
{
    std::atomic<bool> done{false};
    std::atomic<uint32_t> ip{0}; // host byte order, 0 on failure; written before `done`
};

enum class TrackerPhase : uint8_t
{
    idle,
    resolving,
    listing,
};

struct TrackerState
{
    TrackerPhase phase = TrackerPhase::idle;
    // Kept until it finishes, so a lookup that hangs is waited on again instead of started twice.
    std::shared_ptr<DnsJob> dns;
    std::string dns_host;
    Clock::time_point dns_started{};
    std::string host;
    // The engine's own tracker address when it has one for the host, else resolved once a session.
    std::string resolved_host;
    std::optional<rf::NetAddr> addr;
    Clock::time_point last_chunk{};
    uint32_t gen = 0;
    TrackerResult result = TrackerResult::none;
};

struct Batch
{
    bool active = false;
    std::vector<int> order;
    size_t next = 0;
    int done = 0;
};

SOCKET g_socket = INVALID_SOCKET;
// A Get Servers ran to its end; until then, an open starts one.
bool g_fetched = false;
bool g_files_loaded = false;
std::vector<ServerBrowserEntry> g_entries;
std::vector<EntryState> g_states;
std::unordered_map<rf::NetAddr, int> g_index;
// Retries, quiet refreshes and added servers: sent before the batch.
std::deque<int> g_queue;
Batch g_batch;
int g_in_flight = 0;
Clock::time_point g_next_send{};
Clock::time_point g_lan_sent{};
std::optional<Clock::time_point> g_lan_until;
TrackerState g_tracker;
std::optional<ServerTab> g_pending_refresh;
// A close cut pings short, so the next open re-pings even with auto refresh off.
bool g_pings_cut_off = false;

int elapsed_ms(Clock::time_point from, Clock::time_point to)
{
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(to - from).count();
    return static_cast<int>(std::max<long long>(us + 500, 0) / 1000);
}

std::string list_file_path(const char* name)
{
    return std::string{rf::root_path} + name;
}

int find_or_add(const rf::NetAddr& addr)
{
    if (const auto it = g_index.find(addr); it != g_index.end()) {
        return it->second;
    }
    if (g_entries.size() >= max_entries) {
        return -1;
    }
    const int index = static_cast<int>(g_entries.size());
    ServerBrowserEntry entry;
    entry.addr = addr;
    g_entries.push_back(std::move(entry));
    g_states.emplace_back();
    g_index.emplace(addr, index);
    return index;
}

size_t count_entries(bool ServerBrowserEntry::* flag)
{
    return static_cast<size_t>(std::ranges::count_if(g_entries, [&](const ServerBrowserEntry& e) { return e.*flag; }));
}

// ─── Sorting and filtering ───

int compare_text(std::string_view a, std::string_view b)
{
    if (a.empty() != b.empty()) {
        return a.empty() ? 1 : -1;
    }
    return string_icompare(a, b);
}

std::string_view game_type_short_name(const ServerBrowserEntry& entry)
{
    return multi_game_type_name_short(static_cast<rf::NetGameType>(entry.game_type));
}

int compare_column(const ServerBrowserEntry& a, const ServerBrowserEntry& b, ServerSortColumn column)
{
    switch (column) {
    case ServerSortColumn::name:
        return compare_text(a.name, b.name);
    case ServerSortColumn::game_type:
        if (const int c = string_icompare(game_type_short_name(a), game_type_short_name(b)); c != 0) {
            return c;
        }
        return string_icompare(a.mod, b.mod);
    case ServerSortColumn::map:
        return compare_text(a.level, b.level);
    case ServerSortColumn::players:
        return 0; // ranked over the whole view: players_ranks
    case ServerSortColumn::ping:
        return a.ping_ms == b.ping_ms ? 0 : (a.ping_ms < b.ping_ms ? -1 : 1);
    }
    return 0;
}

bool server_entry_in_tab(const ServerBrowserEntry& entry, ServerTab tab)
{
    switch (tab) {
    case ServerTab::internet:
        return entry.listed || entry.favorite || entry.added;
    case ServerTab::favorites:
        return entry.favorite;
    case ServerTab::lan:
        return entry.lan;
    }
    return false;
}

bool server_entry_passes_filter(const ServerBrowserEntry& entry, const ServerFilter& filter)
{
    if (!server_entry_in_tab(entry, filter.tab)) {
        return false;
    }
    // A favorite the tracker doesn't list shows on the Internet tab only while it answers; the batch still pings it.
    if (filter.tab == ServerTab::internet && !entry.listed && entry.ping_ms < 0) {
        return false;
    }
    if (filter.hide_empty && server_entry_humans(entry) == 0) {
        return false;
    }
    if (filter.hide_full && entry.max_players > 0 && entry.players >= entry.max_players) {
        return false;
    }
    if (filter.hide_passworded && entry.passworded) {
        return false;
    }
    if (filter.alpine_only && !entry.af) {
        return false;
    }
    if ((filter.mod == ServerModFilter::unmodded && server_entry_is_modded(entry))
        || (filter.mod == ServerModFilter::modded && !server_entry_is_modded(entry))) {
        return false;
    }
    if (filter.game_type >= 0 && entry.game_type != filter.game_type) {
        return false;
    }
    if (!string_split(filter.search).empty()) {
        const auto type = static_cast<rf::NetGameType>(entry.game_type);
        const std::string address = std::format("{}", entry.addr);
        const bool found = all_words_match(filter.search, [&](std::string_view word) {
            return string_icontains(entry.name, word)
                || string_icontains(entry.level, word)
                || (entry.af && string_icontains(entry.af->level_filename, word))
                || string_icontains(entry.mod, word)
                || string_icontains(multi_game_type_name(type), word)
                || string_icontains(multi_game_type_name_short(type), word)
                || string_icontains(address, word);
        });
        if (!found) {
            return false;
        }
    }
    return true;
}

// The players order, as a rank for each entry of `view` (indexed like `entries`, lower first): AF servers by humans
// then bots, more first; legacy servers by their count; each legacy server right before the first AF server whose
// humans and bots add up to fewer than its count, as it may count bots of its own. Ranked over the whole view because
// no rule between two servers can keep all three without a loop: an AF server with more humans but fewer in all than
// another and a legacy count between their totals. There the legacy server goes ahead of both.
std::vector<std::pair<int, int>> players_ranks(std::span<const ServerBrowserEntry> entries, std::span<const int> view)
{
    std::vector<std::pair<int, int>> groups; // AF humans and bots, most first
    int max_count = 0;
    for (const int i : view) {
        if (const auto& af = entries[i].af) {
            groups.emplace_back(af->num_human_players, af->num_bots);
        }
        else {
            max_count = std::max(max_count, entries[i].players);
        }
    }
    std::ranges::sort(groups, std::greater{});
    groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
    // For each legacy count, the first group with fewer in all, the group count for none.
    const int num_groups = static_cast<int>(groups.size());
    std::vector<int> first_fewer(max_count + 1, num_groups);
    for (int g = 0; g < num_groups; ++g) {
        for (int count = groups[g].first + groups[g].second + 1; count <= max_count; ++count) {
            if (first_fewer[count] == num_groups) {
                first_fewer[count] = g;
            }
        }
    }
    // An AF group's place, or the place before one, then a legacy count, more first.
    std::vector<std::pair<int, int>> ranks(entries.size());
    for (const int i : view) {
        if (const auto& af = entries[i].af) {
            const std::pair<int, int> key{af->num_human_players, af->num_bots};
            const auto group = std::ranges::lower_bound(groups, key, std::greater{});
            ranks[i] = {2 * static_cast<int>(group - groups.begin()) + 1, 0};
        }
        else {
            const int count = std::max(entries[i].players, 0);
            ranks[i] = {2 * first_fewer[count], -count};
        }
    }
    return ranks;
}

// `column_cmp` is the sort column's order of `a` and `b`, negative when `a` comes first.
int server_entry_compare(const ServerBrowserEntry& a, const ServerBrowserEntry& b, int column_cmp, bool reverse)
{
    const bool answered_a = a.ping_ms >= 0;
    const bool answered_b = b.ping_ms >= 0;
    if (answered_a != answered_b) {
        return answered_a ? -1 : 1;
    }
    if (column_cmp != 0) {
        return reverse ? -column_cmp : column_cmp;
    }
    if (const int c = compare_text(a.name, b.name); c != 0) {
        return c;
    }
    const auto addr_order = [](const ServerBrowserEntry& e) {
        return std::pair{e.addr.ip_addr.inner, e.addr.port};
    };
    return addr_order(a) == addr_order(b) ? 0 : (addr_order(a) < addr_order(b) ? -1 : 1);
}


// ─── List files ───

std::vector<rf::ui::ServerListRecord> read_records(const std::string& path)
{
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return {};
    }
    const std::vector<char> data{std::istreambuf_iterator<char>{file}, {}};
    if (data.size() % sizeof(rf::ui::ServerListRecord) != 0) {
        xlog::warn("[ServerBrowser] Ignoring {}: not a whole number of records", path);
        return {};
    }
    std::vector<rf::ui::ServerListRecord> records(data.size() / sizeof(rf::ui::ServerListRecord));
    if (!records.empty()) {
        std::memcpy(records.data(), data.data(), data.size());
    }
    return records;
}

std::string record_string(const char* field, size_t size)
{
    return std::string{field, strnlen(field, size)};
}

void copy_record_string(char* field, size_t size, const std::string& value)
{
    std::memcpy(field, value.data(), std::min(value.size(), size - 1));
}

// A record only fills an entry no answer has filled yet. Its ping is left out: the server has not answered.
void apply_record(ServerBrowserEntry& entry, const rf::ui::ServerListRecord& rec)
{
    if (entry.has_info) {
        return;
    }
    entry.name = record_string(rec.name, sizeof(rec.name));
    entry.level = record_string(rec.level, sizeof(rec.level));
    entry.mod = record_string(rec.mod, sizeof(rec.mod));
    entry.unknown_game_type = rec.game_type < 0 || rec.game_type >= rf::NG_TYPE_UNK;
    entry.game_type = entry.unknown_game_type ? static_cast<int>(rf::NG_TYPE_UNK) : rec.game_type;
    entry.players = rec.players;
    entry.max_players = rec.max_players;
    entry.dedicated = (rec.flags & rf::ui::SERVER_LIST_RECORD_DEDICATED) != 0;
    entry.passworded = (rec.flags & rf::ui::SERVER_LIST_RECORD_PASSWORD) != 0;
    entry.has_info = !entry.name.empty();
}

rf::ui::ServerListRecord make_record(const ServerBrowserEntry& entry)
{
    rf::ui::ServerListRecord rec{};
    rec.ip = entry.addr.ip_addr.inner;
    rec.port = entry.addr.port;
    copy_record_string(rec.name, sizeof(rec.name), entry.name);
    copy_record_string(rec.level, sizeof(rec.level), entry.level);
    copy_record_string(rec.mod, sizeof(rec.mod), entry.mod);
    rec.game_type = entry.game_type;
    rec.players = static_cast<uint8_t>(entry.players);
    rec.max_players = static_cast<uint8_t>(entry.max_players);
    rec.ping = static_cast<int16_t>(std::clamp(entry.ping_ms, -1, 0x7FFF));
    rec.flags = (entry.dedicated ? rf::ui::SERVER_LIST_RECORD_DEDICATED : 0u)
        | (entry.passworded ? rf::ui::SERVER_LIST_RECORD_PASSWORD : 0u);
    return rec;
}

void write_records(const char* name, bool ServerBrowserEntry::* flag)
{
    std::vector<rf::ui::ServerListRecord> records;
    for (const ServerBrowserEntry& entry : g_entries) {
        if (entry.*flag) {
            records.push_back(make_record(entry));
        }
    }
    const std::string path = list_file_path(name);
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (file) {
        file.write(reinterpret_cast<const char*>(records.data()),
                   static_cast<std::streamsize>(records.size() * sizeof(rf::ui::ServerListRecord)));
    }
    if (!file) {
        xlog::warn("[ServerBrowser] Cannot write {}", path);
    }
}

void load_list_files()
{
    g_files_loaded = true;
    for (const rf::ui::ServerListRecord& rec : read_records(list_file_path("svrlist.adr"))) {
        rf::NetAddr addr{};
        addr.ip_addr.inner = rec.ip;
        addr.port = rec.port;
        if (const int index = find_or_add(addr); index >= 0) {
            g_entries[index].listed = true;
            apply_record(g_entries[index], rec);
        }
    }
    size_t favorites = 0;
    for (const rf::ui::ServerListRecord& rec : read_records(list_file_path("favlist.adr"))) {
        if (favorites >= max_favorites) {
            break;
        }
        rf::NetAddr addr{};
        addr.ip_addr.inner = rec.ip;
        addr.port = rec.port;
        if (const int index = find_or_add(addr); index >= 0 && !g_entries[index].favorite) {
            g_entries[index].favorite = true;
            apply_record(g_entries[index], rec);
            ++favorites;
        }
    }
}

void save_favorites()
{
    if (g_files_loaded) {
        write_records("favlist.adr", &ServerBrowserEntry::favorite);
    }
}

void save_list_files()
{
    if (g_files_loaded) {
        write_records("svrlist.adr", &ServerBrowserEntry::listed);
        save_favorites();
    }
}

// ─── Socket ───

bool open_socket()
{
    if (g_socket != INVALID_SOCKET) {
        return true;
    }
    const SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        xlog::warn("[ServerBrowser] Cannot create the socket: {}", WSAGetLastError());
        return false;
    }
    u_long non_blocking = 1;
    const BOOL broadcast = TRUE;
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = 0;
    if (ioctlsocket(sock, FIONBIO, &non_blocking) != 0
        || setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcast), sizeof(broadcast)) != 0
        || bind(sock, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        xlog::warn("[ServerBrowser] Cannot set up the socket: {}", WSAGetLastError());
        closesocket(sock);
        return false;
    }
    g_socket = sock;
    return true;
}

void send_to(const rf::NetAddr& addr, const std::vector<uint8_t>& data)
{
    if (g_socket == INVALID_SOCKET) {
        return;
    }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(addr.ip_addr.inner);
    to.sin_port = htons(addr.port);
    if (sendto(g_socket, reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
               reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == SOCKET_ERROR) {
        xlog::trace("[ServerBrowser] sendto {} failed: {}", addr, WSAGetLastError());
    }
}

// ─── Pings ───

void finish_ping(int index, bool answered, Clock::time_point now)
{
    EntryState& state = g_states[index];
    if (state.ping_state == PingState::in_flight) {
        --g_in_flight;
    }
    g_entries[index].ping_ms = answered ? elapsed_ms(state.sent_at, now) : -1;
    state.ping_state = PingState::idle;
    if (state.counts_for_batch) {
        state.counts_for_batch = false;
        ++g_batch.done;
    }
}

void queue_ping(int index)
{
    EntryState& state = g_states[index];
    if (state.ping_state != PingState::idle) {
        return;
    }
    state.ping_state = PingState::queued;
    state.attempts = 0;
    g_queue.push_back(index);
}

int next_to_send()
{
    while (!g_queue.empty()) {
        const int index = g_queue.front();
        g_queue.pop_front();
        if (g_states[index].ping_state == PingState::queued) {
            return index;
        }
    }
    while (g_batch.active && g_batch.next < g_batch.order.size()) {
        const int index = g_batch.order[g_batch.next++];
        EntryState& state = g_states[index];
        state.counts_for_batch = true;
        if (state.ping_state == PingState::idle) {
            state.attempts = 0;
            return index;
        }
        // Already queued or out: its answer counts for the batch.
    }
    return -1;
}

void send_pings(Clock::time_point now)
{
    while (g_in_flight < max_in_flight && now >= g_next_send) {
        const int index = next_to_send();
        if (index < 0) {
            return;
        }
        EntryState& state = g_states[index];
        state.ping_state = PingState::in_flight;
        state.queried = true;
        state.sent_at = now;
        ++state.attempts;
        ++g_in_flight;
        send_to(g_entries[index].addr, make_game_info_request());
        if (!g_entries[index].lan) {
            g_next_send = now + request_gap;
        }
    }
}

void expire_pings(Clock::time_point now)
{
    for (size_t i = 0; i < g_states.size(); ++i) {
        EntryState& state = g_states[i];
        if (state.ping_state != PingState::in_flight || now - state.sent_at <= answer_timeout) {
            continue;
        }
        if (state.attempts < max_attempts) {
            --g_in_flight;
            state.ping_state = PingState::queued;
            g_queue.push_back(static_cast<int>(i));
        }
        else {
            finish_ping(static_cast<int>(i), false, now);
        }
    }
    if (g_batch.active && g_batch.next >= g_batch.order.size()
        && g_batch.done >= static_cast<int>(g_batch.order.size())) {
        g_batch.active = false;
    }
}

void start_batch(ServerTab tab)
{
    if (g_pending_refresh == tab) {
        g_pending_refresh.reset();
    }
    for (EntryState& state : g_states) {
        state.counts_for_batch = false;
    }
    g_batch = Batch{};
    g_batch.active = true;
    for (int pass = 0; pass < 2; ++pass) {
        const bool favorites = pass == 0;
        for (size_t i = 0; i < g_entries.size(); ++i) {
            if (g_entries[i].favorite == favorites && server_entry_in_tab(g_entries[i], tab)) {
                g_batch.order.push_back(static_cast<int>(i));
            }
        }
    }
}

void lan_query(Clock::time_point now)
{
    if (g_pending_refresh == ServerTab::lan) {
        g_pending_refresh.reset();
    }
    for (ServerBrowserEntry& entry : g_entries) {
        entry.lan = false;
    }
    // Alpine's request rather than stock's bare one, so Alpine servers answer with their extension.
    rf::NetAddr broadcast{};
    broadcast.ip_addr.inner = INADDR_BROADCAST;
    broadcast.port = default_server_port;
    send_to(broadcast, make_game_info_request());
    g_lan_sent = now;
    g_lan_until = now + lan_window;
}

// ─── Answers ───

void apply_game_info(ServerBrowserEntry& entry, const GameInfoAnswer& info)
{
    entry.has_info = true;
    entry.incompatible = !info.compatible;
    if (!info.compatible) {
        entry.name = rf::strings::incompatible_version;
        entry.level.clear();
        entry.mod.clear();
        entry.game_type = rf::NG_TYPE_DM;
        entry.unknown_game_type = false;
        entry.players = 0;
        entry.max_players = 0;
        entry.dedicated = false;
        entry.passworded = false;
        entry.af.reset();
        return;
    }
    entry.name = info.name;
    entry.level = info.level;
    entry.mod = info.mod;
    entry.game_type = info.game_type;
    entry.unknown_game_type = info.unknown_game_type;
    entry.players = info.players;
    entry.max_players = info.max_players;
    entry.dedicated = (info.flags & RF_SF_DEDICATED) != 0;
    entry.passworded = (info.flags & RF_SF_PASSWORD) != 0;
    entry.af = info.af;
}

void handle_game_info(const rf::NetAddr& from, int index, std::span<const uint8_t> payload, bool lan_live,
                      Clock::time_point now)
{
    const std::optional<GameInfoAnswer> info = parse_game_info(payload);
    if (!info) {
        return;
    }
    if (index < 0) {
        if (count_entries(&ServerBrowserEntry::lan) >= max_lan_entries) {
            return;
        }
        index = find_or_add(from);
        if (index < 0) {
            return;
        }
    }
    apply_game_info(g_entries[index], *info);
    // The join path reads the AF data (unknown game type, stats) from network.cpp's map.
    update_server_browser_extra(from, &*info);

    const PingState ping_state = g_states[index].ping_state;
    if (ping_state == PingState::in_flight || (ping_state == PingState::queued && g_states[index].attempts > 0)) {
        finish_ping(index, true, now);
    }
    else if (lan_live) {
        g_entries[index].lan = true;
        g_entries[index].ping_ms = elapsed_ms(g_lan_sent, now);
    }
}

void handle_roster_segment(int index, std::span<const uint8_t> payload)
{
    EntryState& state = g_states[index];
    const std::optional<PfPlayersSegment> seg = parse_pf_players_segment(payload);
    if (!seg || seg->total_segments > max_roster_segments) {
        return;
    }
    if (state.roster_slices.size() != seg->total_segments || state.roster_response_id != seg->response_id) {
        state.roster_response_id = seg->response_id;
        state.roster_slices.assign(seg->total_segments, std::nullopt);
    }
    state.roster_slices[seg->sequence].emplace(seg->slice.begin(), seg->slice.end());
    std::vector<uint8_t> joined;
    for (const auto& slice : state.roster_slices) {
        if (!slice) {
            return;
        }
        joined.insert(joined.end(), slice->begin(), slice->end());
    }
    state.roster_slices.clear();
    if (std::optional<ServerRoster> roster = parse_pf_players(joined)) {
        g_entries[index].roster = std::move(roster);
    }
    else {
        xlog::debug("[ServerBrowser] Malformed player list from {}", g_entries[index].addr);
    }
}

void finish_tracker(bool timed_out)
{
    g_fetched = true;
    g_tracker.phase = TrackerPhase::idle;
    g_tracker.result = timed_out ? TrackerResult::timed_out : TrackerResult::none;
    // A list that stopped arriving may be partial: only a complete one drops servers.
    if (!timed_out) {
        for (size_t i = 0; i < g_entries.size(); ++i) {
            if (g_entries[i].listed && g_states[i].tracker_gen != g_tracker.gen) {
                g_entries[i].listed = false;
            }
        }
    }
    start_batch(ServerTab::internet);
}

void handle_tracker(std::span<const uint8_t> datagram, Clock::time_point now)
{
    const std::optional<TrackerPacket> pkt = parse_tracker_packet(datagram);
    if (!pkt || g_tracker.phase != TrackerPhase::listing) {
        return;
    }
    if (pkt->type == RF_TPT_SERVER_LIST) {
        for (const rf::NetAddr& addr : pkt->servers) {
            if (addr.ip_addr.inner == 0 || addr.port == 0) {
                continue;
            }
            if (const int index = find_or_add(addr); index >= 0) {
                g_entries[index].listed = true;
                g_states[index].tracker_gen = g_tracker.gen;
            }
        }
        g_tracker.last_chunk = now;
        send_to(*g_tracker.addr, make_tracker_ack(pkt->seq));
    }
    else if (pkt->type == RF_TPT_SERVER_LIST_END) {
        send_to(*g_tracker.addr, make_tracker_ack(pkt->seq));
        finish_tracker(false);
    }
}

void handle_datagram(const rf::NetAddr& from, std::span<const uint8_t> data, Clock::time_point now)
{
    if (data[0] == RF_TRACKER) {
        if (g_tracker.addr && from == *g_tracker.addr) {
            handle_tracker(data, now);
        }
        return;
    }
    if (data[0] != RF_GAME) {
        return;
    }
    // Only servers we asked, plus anything answering a live LAN query.
    const bool lan_live = g_lan_until.has_value();
    int index = server_browser_find(from);
    const bool asked = index >= 0 && (g_states[index].queried || g_states[index].roster_first_sent);
    if (!asked && !lan_live) {
        return;
    }
    // [type][u16 size][payload]...
    std::span<const uint8_t> rest = data.subspan(1);
    while (rest.size() >= sizeof(RF_GamePacketHeader)) {
        RF_GamePacketHeader hdr{};
        std::memcpy(&hdr, rest.data(), sizeof(hdr));
        rest = rest.subspan(sizeof(hdr));
        if (hdr.size > rest.size()) {
            return;
        }
        const std::span<const uint8_t> payload = rest.first(hdr.size);
        rest = rest.subspan(hdr.size);
        if (hdr.type == RF_GPT_GAME_INFO) {
            handle_game_info(from, index, payload, lan_live, now);
            index = server_browser_find(from);
        }
        else if (hdr.type == static_cast<uint8_t>(pf_packet_type::players) && index >= 0
                 && g_states[index].roster_first_sent) {
            handle_roster_segment(index, payload);
        }
    }
}

void receive_all(Clock::time_point now)
{
    uint8_t buf[2048];
    for (int i = 0; i < max_datagrams_per_frame; ++i) {
        sockaddr_in from{};
        int from_len = sizeof(from);
        const int len = recvfrom(g_socket, reinterpret_cast<char*>(buf), sizeof(buf), 0,
                                 reinterpret_cast<sockaddr*>(&from), &from_len);
        if (len == SOCKET_ERROR) {
            const int err = WSAGetLastError();
            // ICMP port unreachable from an earlier send, or an oversized datagram: skip it.
            if (err == WSAECONNRESET || err == WSAEMSGSIZE) {
                continue;
            }
            return;
        }
        if (len <= 0 || from.sin_family != AF_INET) {
            continue;
        }
        rf::NetAddr addr{};
        addr.ip_addr.inner = ntohl(from.sin_addr.s_addr);
        addr.port = ntohs(from.sin_port);
        handle_datagram(addr, std::span<const uint8_t>{buf, static_cast<size_t>(len)}, now);
    }
}

// ─── Tracker ───

void ask_tracker(Clock::time_point now)
{
    send_to(*g_tracker.addr, make_tracker_list_request());
    g_tracker.phase = TrackerPhase::listing;
    g_tracker.last_chunk = now;
}

void tracker_unreachable()
{
    xlog::warn("[ServerBrowser] Cannot resolve the tracker {}", g_tracker.host);
    g_fetched = true;
    g_tracker.phase = TrackerPhase::idle;
    g_tracker.result = TrackerResult::unreachable;
    start_batch(ServerTab::internet);
}

void tracker_frame(Clock::time_point now)
{
    if (g_tracker.phase == TrackerPhase::resolving) {
        if (g_tracker.dns->done.load(std::memory_order_acquire)) {
            const uint32_t ip = g_tracker.dns->ip.load();
            g_tracker.dns.reset();
            if (ip == 0) {
                tracker_unreachable();
                return;
            }
            rf::NetAddr addr{};
            addr.ip_addr.inner = ip;
            addr.port = RF_TRACKER_PORT;
            g_tracker.addr = addr;
            g_tracker.resolved_host = g_tracker.host;
            ask_tracker(now);
        }
        else if (now - g_tracker.dns_started > dns_timeout) {
            tracker_unreachable();
        }
    }
    else if (g_tracker.phase == TrackerPhase::listing && now - g_tracker.last_chunk > tracker_timeout) {
        xlog::warn("[ServerBrowser] The tracker's list timed out");
        finish_tracker(true);
    }
}

void reset_queries()
{
    for (EntryState& state : g_states) {
        state.ping_state = PingState::idle;
        state.counts_for_batch = false;
        state.roster_slices.clear();
    }
    g_queue.clear();
    g_batch = Batch{};
    g_in_flight = 0;
    g_lan_until.reset();
    g_pending_refresh.reset();
    g_tracker.phase = TrackerPhase::idle;
}

ConsoleCommand2 server_browser_details_cmd{
    "cl_server_browser_details",
    [](std::optional<bool> enabled) {
        setting_console_set_bool("server_browser_details", enabled);
        rf::console::print("Server browser details are {}",
                           g_alpine_game_config.server_browser_details ? "enabled" : "disabled");
    },
    "Show the server browser's details card for the hovered or selected server",
    "cl_server_browser_details [bool]",
};

ConsoleCommand2 server_browser_auto_refresh_cmd{
    "cl_server_browser_auto_refresh",
    [](std::optional<bool> enabled) {
        setting_console_set_bool("server_browser_auto_refresh", enabled);
        rf::console::print("Server browser auto refresh is {}",
                           g_alpine_game_config.server_browser_auto_refresh ? "enabled" : "disabled");
    },
    "Re-query the server shown in the server browser, and re-ping the list when the browser opens",
    "cl_server_browser_auto_refresh [bool]",
};

} // namespace

// ─── Lifetime ───

void server_browser_apply_patch()
{
    server_browser_details_cmd.register_cmd();
    server_browser_auto_refresh_cmd.register_cmd();
}

void server_browser_open(ServerTab tab, bool reping)
{
    if (!g_files_loaded) {
        load_list_files();
    }
    if (!open_socket()) {
        return;
    }
    const bool cut_off = std::exchange(g_pings_cut_off, false);
    // Also when an earlier open closed before the list arrived, as a join or a level start out of the browser
    // does. The fetch ends in an Internet batch, which pings the favorites too.
    if (!g_fetched) {
        server_browser_get_servers();
        if (tab == ServerTab::lan) {
            lan_query(Clock::now());
        }
        return;
    }
    if (reping || cut_off) {
        server_browser_refresh(tab);
    }
}

void server_browser_close()
{
    save_list_files();
    g_pings_cut_off = g_pings_cut_off || g_batch.active || !g_queue.empty() || g_in_flight > 0
        || g_lan_until.has_value();
    // A Get Servers cut short runs again on the next open.
    if (g_tracker.phase != TrackerPhase::idle) {
        g_fetched = false;
    }
    if (g_socket != INVALID_SOCKET) {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }
    reset_queries();
}

void server_browser_frame()
{
    if (g_socket == INVALID_SOCKET) {
        return;
    }
    const Clock::time_point now = Clock::now();
    receive_all(now);
    tracker_frame(now);
    if (g_lan_until && now >= *g_lan_until) {
        g_lan_until.reset();
    }
    expire_pings(now);
    send_pings(now);
    if (g_pending_refresh && !server_browser_busy()) {
        server_browser_refresh(*g_pending_refresh);
    }
}

// ─── Queries ───

void server_browser_get_servers()
{
    if (server_browser_busy() || g_socket == INVALID_SOCKET) {
        return;
    }
    const Clock::time_point now = Clock::now();
    g_tracker.host = g_alpine_game_config.multiplayer_tracker;
    ++g_tracker.gen;
    g_tracker.result = TrackerResult::none;
    if (const std::optional<rf::NetAddr> engine_addr = tracker_resolved_addr(g_tracker.host)) {
        g_tracker.addr = engine_addr;
        g_tracker.resolved_host = g_tracker.host;
    }
    if (g_tracker.addr && g_tracker.resolved_host == g_tracker.host) {
        ask_tracker(now);
        return;
    }
    g_tracker.addr.reset();
    if (!g_tracker.dns || g_tracker.dns_host != g_tracker.host) {
        auto job = std::make_shared<DnsJob>();
        try {
            std::thread{[job, host = g_tracker.host] {
                job->ip.store(tracker_dns_resolve(host, 0).ip);
                job->done.store(true, std::memory_order_release);
            }}.detach();
        }
        catch (const std::exception& e) {
            xlog::warn("[ServerBrowser] Cannot start the tracker lookup: {}", e.what());
            tracker_unreachable();
            return;
        }
        g_tracker.dns = std::move(job);
        g_tracker.dns_host = g_tracker.host;
    }
    g_tracker.dns_started = now;
    g_tracker.phase = TrackerPhase::resolving;
}

void server_browser_refresh(ServerTab tab)
{
    if (g_socket == INVALID_SOCKET) {
        return;
    }
    if (server_browser_busy()) {
        g_pending_refresh = tab;
        return;
    }
    if (tab == ServerTab::lan) {
        lan_query(Clock::now());
    }
    else {
        start_batch(tab);
    }
}

void server_browser_refresh_quiet(const rf::NetAddr& addr)
{
    const int index = server_browser_find(addr);
    if (index < 0 || g_socket == INVALID_SOCKET) {
        return;
    }
    EntryState& state = g_states[index];
    const Clock::time_point now = Clock::now();
    if (state.last_quiet && now - *state.last_quiet < quiet_refresh_interval) {
        return;
    }
    state.last_quiet = now;
    queue_ping(index);
}

void server_browser_request_roster(const rf::NetAddr& addr)
{
    const int index = server_browser_find(addr);
    if (index < 0 || g_socket == INVALID_SOCKET) {
        return;
    }
    EntryState& state = g_states[index];
    const Clock::time_point now = Clock::now();
    if (state.roster_sent && now - *state.roster_sent < roster_interval) {
        return;
    }
    state.roster_sent = now;
    if (!state.roster_first_sent) {
        state.roster_first_sent = now;
    }
    send_to(addr, make_pf_players_request());
}

ServerRosterState server_browser_roster_state(const rf::NetAddr& addr)
{
    const int index = server_browser_find(addr);
    if (index < 0) {
        return ServerRosterState::none;
    }
    if (const auto& roster = g_entries[index].roster) {
        return roster->players.empty() ? ServerRosterState::empty : ServerRosterState::ok;
    }
    const auto& first_sent = g_states[index].roster_first_sent;
    if (!first_sent) {
        return ServerRosterState::none;
    }
    return Clock::now() - *first_sent < roster_wait ? ServerRosterState::asking : ServerRosterState::no_answer;
}

ServerBrowserStatus server_browser_status()
{
    ServerBrowserStatus status;
    status.tracker = g_tracker.result;
    if (g_tracker.phase != TrackerPhase::idle) {
        status.activity = ServerBrowserActivity::getting_servers;
    }
    else if (g_lan_until) {
        status.activity = ServerBrowserActivity::searching_lan;
    }
    else if (g_batch.active) {
        status.activity = ServerBrowserActivity::pinging;
        status.pinged = g_batch.done;
        status.to_ping = static_cast<int>(g_batch.order.size());
    }
    return status;
}

bool server_browser_busy()
{
    return server_browser_status().activity != ServerBrowserActivity::idle;
}

// ─── The list ───

const std::vector<ServerBrowserEntry>& server_browser_entries()
{
    return g_entries;
}

int server_browser_find(const rf::NetAddr& addr)
{
    const auto it = g_index.find(addr);
    return it != g_index.end() ? it->second : -1;
}

bool server_browser_set_favorite(const rf::NetAddr& addr, bool favorite)
{
    const int index = server_browser_find(addr);
    if (index < 0) {
        return false;
    }
    ServerBrowserEntry& entry = g_entries[index];
    if (entry.favorite == favorite) {
        return true;
    }
    if (favorite && count_entries(&ServerBrowserEntry::favorite) >= max_favorites) {
        return false;
    }
    entry.favorite = favorite;
    save_favorites();
    return true;
}

AddServerResult server_browser_add_server(std::string_view text)
{
    const std::optional<rf::NetAddr> addr = parse_server_address(text);
    if (!addr) {
        return AddServerResult::invalid;
    }
    const int existing = server_browser_find(*addr);
    if ((existing < 0 || !g_entries[existing].favorite)
        && count_entries(&ServerBrowserEntry::favorite) >= max_favorites) {
        return AddServerResult::favorites_full;
    }
    const int index = existing >= 0 ? existing : find_or_add(*addr);
    if (index < 0) {
        return AddServerResult::favorites_full;
    }
    g_entries[index].added = true;
    g_entries[index].favorite = true;
    save_favorites();
    if (g_socket != INVALID_SOCKET) {
        queue_ping(index);
    }
    return AddServerResult::added;
}

std::vector<int> server_browser_view(std::span<const ServerBrowserEntry> entries, const ServerFilter& filter,
                                     ServerSortColumn column, bool reverse)
{
    std::vector<int> view;
    for (size_t i = 0; i < entries.size(); ++i) {
        if (server_entry_passes_filter(entries[i], filter)) {
            view.push_back(static_cast<int>(i));
        }
    }
    const std::vector<std::pair<int, int>> ranks =
        column == ServerSortColumn::players ? players_ranks(entries, view) : std::vector<std::pair<int, int>>{};
    std::stable_sort(view.begin(), view.end(), [&](int a, int b) {
        const int c = ranks.empty() ? compare_column(entries[a], entries[b], column)
                                    : (ranks[a] == ranks[b] ? 0 : (ranks[a] < ranks[b] ? -1 : 1));
        return server_entry_compare(entries[a], entries[b], c, reverse) < 0;
    });
    return view;
}

int server_entry_humans(const ServerBrowserEntry& entry)
{
    return entry.af ? entry.af->num_human_players : entry.players;
}

int server_entry_bots(const ServerBrowserEntry& entry)
{
    return entry.af ? entry.af->num_bots : 0;
}

bool server_entry_is_modded(const ServerBrowserEntry& entry)
{
    return !entry.mod.empty() || (entry.af && (entry.af->af_flags & AF_GI_FLAG_MODDED_SERVER));
}

std::string server_af_version_label(const AFGameInfoExtra& af)
{
    std::string label = std::format("{}.{}.{}", static_cast<int>(af.version_major),
                                    static_cast<int>(af.version_minor), static_cast<int>(af.version_patch));
    switch (af.version_type) {
    case VERSION_TYPE_DEV:
        label += "-dev";
        break;
    case VERSION_TYPE_ALPHA:
        label += "-alpha";
        break;
    case VERSION_TYPE_BETA:
        label += "-beta";
        break;
    case VERSION_TYPE_RC:
        label += "-rc";
        break;
    default:
        break;
    }
    return label;
}

// ─── Joining ───

bool server_browser_is_current_server(const rf::NetAddr& addr)
{
    return rf::is_multi && !rf::is_server && rf::netgame.server_addr == addr;
}
