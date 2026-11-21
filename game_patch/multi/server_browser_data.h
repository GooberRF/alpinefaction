#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "network.h"
#include "server_query.h"
#include "../rf/multi.h"

// The Join Game browser's servers, queried on a UDP socket of its own (tracker, game_info,
// pf_players, LAN broadcast). The list, the AF data and the rosters are kept for the whole
// process; favorites live in favlist.adr and the last tracker list in svrlist.adr, as in stock.
// Main thread only.

struct ServerBrowserEntry
{
    rf::NetAddr addr{};
    // Where the server was seen, in any combination.
    bool listed = false;    // the tracker's latest list
    bool favorite = false;
    bool lan = false;       // answered the latest LAN query
    bool added = false;     // typed in with Add Server
    // The latest game_info answer (or the list files' copy until one arrives).
    bool has_info = false;
    bool incompatible = false; // a protocol this build cannot join
    std::string name;
    std::string level;
    std::string mod;
    int game_type = rf::NG_TYPE_DM; // rf::NG_TYPE_UNK for a type this build does not know
    bool unknown_game_type = false;
    int players = 0;
    int max_players = 0;
    bool dedicated = false;
    bool passworded = false;
    std::optional<AFGameInfoExtra> af;
    int ping_ms = -1; // -1: no answer to the latest query
    std::optional<ServerRoster> roster;
};

enum class ServerTab : uint8_t
{
    internet,   // the tracker's list, plus favorites and added servers that answer
    favorites,
    lan,
};

enum class ServerBrowserActivity : uint8_t
{
    idle,
    getting_servers, // the tracker's name being resolved or its list arriving
    searching_lan,
    pinging,
};

enum class TrackerResult : uint8_t
{
    none,
    timed_out,   // the list stopped arriving; what came is kept
    unreachable, // the tracker's name did not resolve
};

struct ServerBrowserStatus
{
    ServerBrowserActivity activity = ServerBrowserActivity::idle;
    int pinged = 0;  // pinging: answered or given up
    int to_ping = 0; // pinging: the batch's size
    TrackerResult tracker = TrackerResult::none; // the latest Get Servers
};

enum class ServerRosterState : uint8_t
{
    none,      // never asked
    asking,
    no_answer, // the server does not list its players
    empty,
    ok,
};

enum class AddServerResult : uint8_t
{
    added,
    invalid,         // not "a.b.c.d[:port]"
    favorites_full,
};

// ─── Lifetime ───

void server_browser_apply_patch();
// Opens the socket and gets the servers until a Get Servers has finished; after that, re-pings `tab` if `reping`
// or if the last close cut pings short.
void server_browser_open(ServerTab tab, bool reping);
// Saves the list files and closes the socket; the list stays.
void server_browser_close();
void server_browser_frame();

// ─── Queries (while open) ───

// Ignored while busy.
void server_browser_get_servers();
// Favorites first; a broadcast for LAN. Queued while busy.
void server_browser_refresh(ServerTab tab);
// At most every 5 s, without touching the status. Call freely for the hovered or selected server.
void server_browser_refresh_quiet(const rf::NetAddr& addr);
// At most every 3 s.
void server_browser_request_roster(const rf::NetAddr& addr);
ServerRosterState server_browser_roster_state(const rf::NetAddr& addr);

ServerBrowserStatus server_browser_status();
bool server_browser_busy();

// ─── The list ───

// Indices stay valid for the process: entries are never removed or reordered.
const std::vector<ServerBrowserEntry>& server_browser_entries();
// -1 when not listed.
int server_browser_find(const rf::NetAddr& addr);
// The listed name, else the address.
std::string server_browser_display_name(const rf::NetAddr& addr);
// False when the favorites are full (adding) or the server is not listed.
bool server_browser_set_favorite(const rf::NetAddr& addr, bool favorite);
AddServerResult server_browser_add_server(std::string_view text);

// ─── Sorting and filtering (pure) ───

enum class ServerSortColumn : uint8_t
{
    name,
    game_type,
    map,
    players, // players_ranks
    ping,
};

enum class ServerModFilter : uint8_t
{
    any,
    unmodded,
    modded,
};

struct ServerFilter
{
    ServerTab tab = ServerTab::internet;
    std::string search; // every word in the name, map, game type, mod or address
    bool hide_empty = false;
    bool hide_full = false;
    bool hide_passworded = false;
    bool alpine_only = false;
    ServerModFilter mod = ServerModFilter::any;
    int game_type = -1; // rf::NetGameType, -1 for all
};

// Indices into `entries` that pass the filter, sorted. Answered servers come first whatever the order.
std::vector<int> server_browser_view(std::span<const ServerBrowserEntry> entries, const ServerFilter& filter,
                                     ServerSortColumn column, bool reverse);

int server_entry_humans(const ServerBrowserEntry& entry);
int server_entry_bots(const ServerBrowserEntry& entry);
bool server_entry_is_modded(const ServerBrowserEntry& entry);
// "1.5.0", "1.5.0-beta", ...
std::string server_af_version_label(const AFGameInfoExtra& af);

// ─── Joining ───

// True while in a game on that server (a client of it, not its host).
bool server_browser_is_current_server(const rf::NetAddr& addr);
