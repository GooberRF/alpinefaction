#include "server_config_snapshot.h"

#include <algorithm>
#include <type_traits>

#include "alpine_packets.h"
#include "bagman.h"
#include "gametype.h"
#include "server_internal.h"
#include "../rf/multi.h"

namespace server_config {

namespace {

std::vector<MutatorRecord> build_mutators()
{
    std::vector<MutatorRecord> out;
    for (const MutatorDeclaration& decl : g_alpine_server_config_active_rules.mutators.declarations) {
        MutatorRecord record;
        // Mutator names, option keys, and string values are operator-authored config,
        // so they go through sanitize_string like every other external string.
        record.name = sanitize_string(decl.name, k_max_string_len);
        for (const auto& [key, value] : decl.options) {
            std::string clean_key = sanitize_string(key, k_max_string_len);
            std::visit(
                [&record, &clean_key](const auto& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, bool>) {
                        record.settings.emplace_back(clean_key, SettingValue{v});
                    }
                    else if constexpr (std::is_same_v<T, int32_t>) {
                        record.settings.emplace_back(clean_key, SettingValue{static_cast<int64_t>(v)});
                    }
                    else if constexpr (std::is_same_v<T, float>) {
                        record.settings.emplace_back(clean_key, SettingValue{static_cast<double>(v)});
                    }
                    else {
                        record.settings.emplace_back(clean_key,
                                                     SettingValue{sanitize_string(v, k_max_string_len)});
                    }
                },
                value);
        }
        out.push_back(std::move(record));
    }
    return out;
}

std::vector<std::pair<std::string, SettingValue>> build_gametype_settings(rf::NetGameType type)
{
    const auto& rules = g_alpine_server_config_active_rules;
    std::vector<std::pair<std::string, SettingValue>> out;
    const auto add = [&out](const char* key, int64_t value) {
        out.emplace_back(key, SettingValue{value});
    };

    switch (type) {
        case rf::NG_TYPE_BAG:
        case rf::NG_TYPE_TBAG:
            add("score_tick_ms", bagman_get_score_tick_ms());
            add("bag_return_time_ms", rules.bagman.bag_return_time_ms);
            add("bag_spawn_delay_ms", rules.bagman.bag_spawn_delay_ms);
            break;
        case rf::NG_TYPE_SAL:
            add("flag_spawn_delay_ms", rules.salvage.flag_spawn_delay_ms);
            add("flag_capture_respawn_delay_ms", rules.salvage.flag_capture_respawn_delay_ms);
            add("flag_return_time_ms", rules.salvage.flag_return_time_ms);
            break;
        case rf::NG_TYPE_CTF:
            add("flag_return_time_ms", rules.ctf_flag_return_time_ms);
            break;
        case rf::NG_TYPE_KOTH:
        case rf::NG_TYPE_DC:
        case rf::NG_TYPE_REV:
        case rf::NG_TYPE_ESC:
            add("grow_rate", g_koth_info.rules.grow_rate);
            add("drain_empty_rate", g_koth_info.rules.drain_empty_rate);
            add("drain_defended_rate", g_koth_info.rules.drain_defended_rate);
            add("ms_per_point", g_koth_info.rules.ms_per_point);
            break;
        case rf::NG_TYPE_PIT:
        case rf::NG_TYPE_WO:
            add("max_rounds", rules.rounds.max_rounds);
            add("round_time_s", rules.rounds.round_time);
            add("post_round_time_s", rules.rounds.post_round_time);
            add("intermission_time_s", rules.rounds.intermission_time);
            break;
        default:
            break;
    }
    return out;
}

uint8_t build_rf_flags()
{
    // Recomputed rather than read back from build_af_server_info_packet, which is
    // not idempotent.
    uint8_t flags = 0;
    if (rf::netgame.flags & rf::NG_FLAG_WEAPON_STAY) {
        flags |= rf_server_info_flags::RFSIF_WEAPON_STAY;
    }
    if (rf::netgame.flags & rf::NG_FLAG_FORCE_RESPAWN) {
        flags |= rf_server_info_flags::RFSIF_FORCE_RESPAWN;
    }
    if (rf::netgame.flags & rf::NG_FLAG_TEAM_DAMAGE) {
        flags |= rf_server_info_flags::RFSIF_TEAM_DAMAGE;
    }
    if (rf::netgame.flags & rf::NG_FLAG_FALL_DAMAGE) {
        flags |= rf_server_info_flags::RFSIF_FALL_DAMAGE;
    }
    if (rf::netgame.flags & rf::NG_FLAG_BALANCE_TEAMS) {
        flags |= rf_server_info_flags::RFSIF_BALANCE_TEAMS;
    }
    return flags;
}

} // namespace

std::string sanitize_string(std::string_view in, size_t max_len)
{
    // Guarantees valid UTF-8 output: every codepoint that reaches JSON is a whole,
    // well-formed sequence, so json::dump() can never throw on this string (the
    // build enables nlohmann exceptions and a throw here would unwind into the stock
    // game loop). max_len is a byte cap; truncation stops on a sequence boundary so a
    // codepoint is never bisected. C0 controls and DEL are stripped as before.
    std::string out;
    out.reserve(std::min(in.size(), max_len));
    const size_t n = in.size();
    size_t i = 0;
    while (i < n) {
        const unsigned char c0 = static_cast<unsigned char>(in[i]);
        size_t seq_len = 0;
        uint32_t cp = 0;
        if (c0 < 0x80) {
            seq_len = 1;
            cp = c0;
        }
        else if ((c0 & 0xE0) == 0xC0) {
            seq_len = 2;
            cp = c0 & 0x1Fu;
        }
        else if ((c0 & 0xF0) == 0xE0) {
            seq_len = 3;
            cp = c0 & 0x0Fu;
        }
        else if ((c0 & 0xF8) == 0xF0) {
            seq_len = 4;
            cp = c0 & 0x07u;
        }

        bool valid = seq_len != 0 && i + seq_len <= n;
        for (size_t k = 1; valid && k < seq_len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(in[i + k]);
            if ((cc & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (valid) {
            // Reject overlong encodings, UTF-16 surrogates, and out-of-range values.
            static constexpr uint32_t min_cp[5] = {0, 0x0, 0x80, 0x800, 0x10000};
            if (cp < min_cp[seq_len] || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
                valid = false;
            }
        }

        if (!valid) {
            // Replace exactly one bad byte and resync; never emit a partial sequence.
            if (out.size() + 1 > max_len) {
                break;
            }
            out.push_back('?');
            ++i;
            continue;
        }
        if (seq_len == 1 && (cp < 0x20 || cp == 0x7F)) {
            ++i;
            continue;
        }
        if (out.size() + seq_len > max_len) {
            break;
        }
        out.append(in.substr(i, seq_len));
        i += seq_len;
    }
    return out;
}

ServerConfigSnapshot capture_server_config_snapshot()
{
    ServerConfigSnapshot snapshot;
    snapshot.rf_flags = build_rf_flags();
    snapshot.gi_flags = server_get_game_info_flags().game_info_flags_to_uint32();
    snapshot.match_state = af_match_state_for_stats();
    snapshot.mutators = build_mutators();
    snapshot.gametype_settings = build_gametype_settings(rf::multi_get_game_type());
    return snapshot;
}

} // namespace server_config
