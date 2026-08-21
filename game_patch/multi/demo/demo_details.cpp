#include <algorithm>
#include <optional>
#include <common/rfproto.h>
#include "demo_details.h"
#include "demo_file.h"
#include "../gametype.h"

namespace
{
    // Bounds-checked little-endian cursor readers; nullopt aborts decoding of the
    // current packet (the scan continues with the next record).
    std::optional<uint8_t> read_u8(const uint8_t* data, size_t len, size_t& pos)
    {
        if (pos + 1 > len) {
            return std::nullopt;
        }
        return data[pos++];
    }

    std::optional<uint16_t> read_u16(const uint8_t* data, size_t len, size_t& pos)
    {
        if (pos + 2 > len) {
            return std::nullopt;
        }
        auto val = static_cast<uint16_t>(data[pos] | (data[pos + 1] << 8));
        pos += 2;
        return val;
    }

    std::optional<int16_t> read_i16(const uint8_t* data, size_t len, size_t& pos)
    {
        auto val = read_u16(data, len, pos);
        if (!val) {
            return std::nullopt;
        }
        return static_cast<int16_t>(*val);
    }

    bool skip(size_t count, size_t len, size_t& pos)
    {
        if (pos + count > len) {
            return false;
        }
        pos += count;
        return true;
    }

    std::optional<std::string> read_cstr(const uint8_t* data, size_t len, size_t& pos)
    {
        size_t end = pos;
        while (end < len && data[end] != 0) {
            ++end;
        }
        if (end >= len) {
            return std::nullopt; // missing NUL terminator
        }
        // Cap the stored length: these become player names fed to hud_fit_string, whose
        // per-row cost is O(n^2) in the name length (H3). Stock names are <= 31 bytes on
        // the wire. The cursor still advances past the full string so later fields parse.
        constexpr size_t max_name_len = 64;
        const size_t stored_len = std::min(end - pos, max_name_len);
        std::string str{reinterpret_cast<const char*>(data + pos), stored_len};
        pos = end + 1;
        return str;
    }

    DemoScoreRow& upsert_row(std::vector<DemoScoreRow>& rows, uint8_t id)
    {
        for (auto& row : rows) {
            if (row.id == id) {
                return row;
            }
        }
        rows.emplace_back();
        rows.back().id = id;
        return rows.back();
    }

    int normalize_team(uint8_t team)
    {
        return team <= 1 ? team : -1;
    }

    // players 0x06: up to 8 entries of {u8 flags, u8 id, u32 unknown2, u32 ip, u16 port,
    // char name[] NUL, u8 team}, then a trailer byte (1 = another players packet follows,
    // 2 = roster complete). flags & 1 implies an extra u32 after id (never sent by stock).
    void decode_players(const uint8_t* data, size_t len, size_t pos, std::vector<DemoScoreRow>& rows)
    {
        while (len - pos >= 14) {
            auto flags = read_u8(data, len, pos);
            auto id = read_u8(data, len, pos);
            if (!flags || !id) {
                return;
            }
            if ((*flags & 1) && !skip(4, len, pos)) {
                return;
            }
            if (!skip(4 + 4 + 2, len, pos)) { // unknown2, ip, port
                return;
            }
            auto name = read_cstr(data, len, pos);
            auto team = read_u8(data, len, pos);
            if (!name || !team) {
                return;
            }
            auto& row = upsert_row(rows, *id);
            row.name = *name;
            row.team = normalize_team(*team);
        }
    }

    // new_player 0x05: {u8 id, u32 ip, u16 port, u32 flags, u32 rate, char name[] NUL}
    void decode_new_player(const uint8_t* data, size_t len, size_t pos, std::vector<DemoScoreRow>& rows)
    {
        auto id = read_u8(data, len, pos);
        if (!id || !skip(4 + 2 + 4 + 4, len, pos)) {
            return;
        }
        auto name = read_cstr(data, len, pos);
        if (!name) {
            return;
        }
        upsert_row(rows, *id).name = *name;
    }

    // netgame_update 0x1A: {u8 =0x07, u8 count}, then count * {u8 id, u16 ping, u8 unk,
    // i16 score, u8 caps, u8 unk2}, then {f32 level_time, f32 time_limit}. The last update
    // in the stream wins, which is exactly the final scoreboard.
    void decode_netgame_update(const uint8_t* data, size_t len, size_t pos, std::vector<DemoScoreRow>& rows)
    {
        auto subtype = read_u8(data, len, pos);
        auto count = read_u8(data, len, pos);
        if (!subtype || *subtype != 0x07 || !count) {
            return;
        }
        for (int i = 0; i < *count; ++i) {
            auto id = read_u8(data, len, pos);
            auto ping = read_u16(data, len, pos);
            if (!skip(1, len, pos)) {
                return;
            }
            auto score = read_i16(data, len, pos);
            auto caps = read_u8(data, len, pos);
            if (!skip(1, len, pos)) {
                return;
            }
            if (!id || !ping || !score || !caps) {
                return;
            }
            auto& row = upsert_row(rows, *id);
            row.ping = *ping;
            row.score = *score;
            row.caps = *caps;
            row.has_stats = true;
        }
    }
}

DemoDetails demo_scan_details(const std::string& name)
{
    DemoDetails details;

    DemoFileReader reader;
    const auto open_result = reader.open(demo_file_resolve_path(name));
    if (open_result != DemoFileReader::OpenResult::ok
        && open_result != DemoFileReader::OpenResult::missing_features) {
        return details;
    }
    details.readable = true;
    details.requires_newer = open_result == DemoFileReader::OpenResult::missing_features;
    details.header = reader.header();

    uint32_t last_t_ms = 0;
    DemoRecord rec;
    while (reader.next_record(rec)) {
        last_t_ms = rec.t_ms;
        if (!rec.is_packet()) {
            continue;
        }
        const uint8_t* data = rec.packet_data();
        const size_t len = rec.packet_len();
        size_t pos = 3; // past the {u8 type, u16 size} packet header
        switch (rec.packet_type()) {
        case RF_GPT_PLAYERS:
            decode_players(data, len, pos, details.rows);
            break;
        case RF_GPT_NEW_PLAYER:
            decode_new_player(data, len, pos, details.rows);
            break;
        case RF_GPT_LEFT_GAME:
            // Final scoreboard mirrors who was present at the end (like the live limbo screen)
            if (auto id = read_u8(data, len, pos)) {
                std::erase_if(details.rows, [&](const DemoScoreRow& row) { return row.id == *id; });
            }
            break;
        case RF_GPT_NAME_CHANGE:
            if (auto id = read_u8(data, len, pos)) {
                if (auto new_name = read_cstr(data, len, pos)) {
                    upsert_row(details.rows, *id).name = *new_name;
                }
            }
            break;
        case RF_GPT_TEAM_CHANGE:
            if (auto id = read_u8(data, len, pos)) {
                if (auto team = read_u8(data, len, pos)) {
                    upsert_row(details.rows, *id).team = normalize_team(*team);
                }
            }
            break;
        case RF_GPT_NETGAME_UPDATE:
            decode_netgame_update(data, len, pos, details.rows);
            break;
        case RF_GPT_TEAM_SCORES:
            if (auto red = read_u16(data, len, pos)) {
                if (auto blue = read_u16(data, len, pos)) {
                    details.red_score = *red;
                    details.blue_score = *blue;
                    details.team_scores_known = true;
                }
            }
            break;
        case RF_GPT_CTF_FLAG_CAPTURED:
            // {u8 team, u8 player_id, u8 flags_red, u8 flags_blue}
            if (skip(2, len, pos)) {
                if (auto red = read_u8(data, len, pos)) {
                    if (auto blue = read_u8(data, len, pos)) {
                        details.red_score = *red;
                        details.blue_score = *blue;
                        details.team_scores_known = true;
                    }
                }
            }
            break;
        default:
            break;
        }
    }

    details.has_footer = reader.has_footer();
    details.duration_ms = details.has_footer ? reader.footer().duration_ms : last_t_ms;

    // The recorder's virtual player leaks into netgame_update (it iterates the full
    // player_list); it is never part of the visible roster.
    std::erase_if(details.rows, [&](const DemoScoreRow& row) {
        return row.id == details.header.demo_player_id || (row.name.empty() && !row.has_stats);
    });
    for (auto& row : details.rows) {
        if (row.name.empty()) {
            row.name = "Player " + std::to_string(row.id);
        }
    }

    const bool team_mode = multi_game_type_is_team_type(static_cast<rf::NetGameType>(details.header.game_type));
    std::stable_sort(details.rows.begin(), details.rows.end(), [team_mode](const DemoScoreRow& a, const DemoScoreRow& b) {
        // In team modes unknown team (-1) sorts last; otherwise team is meaningless.
        // Then higher score first, then by name.
        if (team_mode) {
            const int team_a = a.team < 0 ? 2 : a.team;
            const int team_b = b.team < 0 ? 2 : b.team;
            if (team_a != team_b) {
                return team_a < team_b;
            }
        }
        if (a.score != b.score) {
            return a.score > b.score;
        }
        return a.name < b.name;
    });

    return details;
}
