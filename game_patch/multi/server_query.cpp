#include <winsock2.h>
#include <algorithm>
#include <charconv>
#include <cstring>
#include <common/rfproto.h>
#include <common/utils/byte-io.h>
#include <common/utils/string-utils.h>
#include "server_query.h"
#include "alpine_packets.h"
#include "../purefaction/pf_packets.h"

namespace
{

// The stock reader's buffers (BufferOverflowPatch in network.cpp).
constexpr size_t max_game_info_string_len = 255;

constexpr uint8_t tracker_magic = 0x06;
// What the stock client puts in packet_len of its own tracker packets (multi_tracker_init).
constexpr uint16_t tracker_request_len = 10;

template<typename T>
void append_bytes(std::vector<uint8_t>& out, const T& value)
{
    const auto* p = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), p, p + sizeof(value));
}

// A NUL-terminated string of at most max_len characters.
bool read_cstring(ByteReader& r, std::string& out, size_t max_len)
{
    const size_t avail = r.remaining();
    const auto* const start = r.cur();
    const void* const nul = avail ? std::memchr(start, 0, avail) : nullptr;
    if (!nul) {
        return false;
    }
    const size_t len = static_cast<size_t>(static_cast<const uint8_t*>(nul) - start);
    if (len > max_len) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(start), len);
    r.skip(len + 1);
    return r.ok();
}

std::vector<uint8_t> make_tracker_packet(uint16_t type, uint32_t seq)
{
    RF_TrackerUnknown pkt{};
    pkt.unknown = tracker_magic;
    pkt.type = type;
    pkt.seq = seq;
    pkt.packet_len = tracker_request_len;
    std::vector<uint8_t> out{static_cast<uint8_t>(RF_TRACKER)};
    append_bytes(out, pkt);
    return out;
}

} // namespace

std::optional<GameInfoAnswer> parse_game_info(std::span<const uint8_t> payload)
{
    if (payload.empty()) {
        return std::nullopt;
    }
    const uint8_t* r = payload.data();
    const uint8_t* const end = r + payload.size();

    GameInfoAnswer info;
    const uint8_t protocol_version = *r++;
    if (protocol_version != RF_VER_10_11 && protocol_version != RF_VER_12 && protocol_version != RF_VER_13) {
        return info;
    }
    info.compatible = true;

    auto read_string = [&](std::string& dst) {
        const uint8_t* const start = r;
        while (r < end && *r != '\0') {
            ++r;
        }
        if (r >= end) {
            return false;
        }
        dst.assign(reinterpret_cast<const char*>(start),
                   std::min(static_cast<size_t>(r - start), max_game_info_string_len));
        ++r;
        return true;
    };

    // name\0, game_type, players, max_players, level\0, mod\0, flags
    if (!read_string(info.name) || end - r < 3) {
        return std::nullopt;
    }
    const uint8_t raw_game_type = *r++;
    info.unknown_game_type = raw_game_type >= RF_GT_UNK;
    info.game_type = std::min<uint8_t>(raw_game_type, RF_GT_UNK);
    info.players = *r++;
    info.max_players = *r++;
    if (!read_string(info.level) || !read_string(info.mod) || r >= end) {
        return std::nullopt;
    }
    info.flags = *r++;

    // AF extension v2: [sig][ver*4][af_flags][filename\0][bot_counts*4]
    constexpr size_t pre_sz = af_game_info_ext_v2::wire_pre_fname_size;
    constexpr size_t post_sz = af_game_info_ext_v2::wire_post_fname_size;
    if (static_cast<size_t>(end - r) >= pre_sz + 1 + post_sz) {
        uint32_t sig;
        std::memcpy(&sig, r, sizeof(sig));
        // r[4] == 19 is the AF 1.3 beta layout below
        if (sig == ALPINE_FACTION_SIGNATURE && r[4] != 19) {
            AFGameInfoExtra extra{};
            const uint8_t* ext_r = r + sizeof(sig);
            extra.version_major = *ext_r++;
            extra.version_minor = *ext_r++;
            extra.version_patch = *ext_r++;
            extra.version_type = *ext_r++;
            std::memcpy(&extra.af_flags, ext_r, sizeof(extra.af_flags));
            ext_r += sizeof(extra.af_flags);

            const uint8_t* const fname_start = ext_r;
            const uint8_t* const fname_limit = end - post_sz;
            while (ext_r < fname_limit && *ext_r != '\0') {
                ++ext_r;
            }
            if (ext_r < fname_limit) {
                extra.level_filename.assign(reinterpret_cast<const char*>(fname_start), ext_r - fname_start);
                ++ext_r;
                extra.num_bots = *ext_r++;
                extra.num_human_players = *ext_r++;
                extra.num_browsers = *ext_r++;
                extra.num_total_clients = *ext_r++;
                extra.unknown_game_type = info.unknown_game_type;
                info.af = std::move(extra);
            }
        }
    }

    // DEPRECATED: AF 1.3 beta compat, remove when no 1.3 beta servers remain.
    // [sig][ext_size(2)][ext_version(1)][ver*4][af_flags][bot_counts*4][filename\0][AFFooter]
    if (!info.af && static_cast<size_t>(end - r) >= 19 + 1 + sizeof(AFFooter)) {
        uint32_t sig;
        std::memcpy(&sig, r, sizeof(sig));
        if (sig == ALPINE_FACTION_SIGNATURE && r[4] == 19) {
            AFGameInfoExtra extra{};
            const uint8_t* ext_r = r + sizeof(sig);
            ext_r += 3; // ext_size, ext_version
            extra.version_major = *ext_r++;
            extra.version_minor = *ext_r++;
            extra.version_patch = *ext_r++;
            extra.version_type = *ext_r++;
            std::memcpy(&extra.af_flags, ext_r, sizeof(extra.af_flags));
            ext_r += sizeof(extra.af_flags);
            extra.num_bots = *ext_r++;
            extra.num_human_players = *ext_r++;
            extra.num_browsers = *ext_r++;
            extra.num_total_clients = *ext_r++;
            const uint8_t* const fname_start = ext_r;
            const uint8_t* const footer_start = end - sizeof(AFFooter);
            while (ext_r < footer_start && *ext_r != '\0') {
                ++ext_r;
            }
            if (ext_r < footer_start) {
                extra.level_filename.assign(reinterpret_cast<const char*>(fname_start), ext_r - fname_start);
            }
            extra.unknown_game_type = info.unknown_game_type;
            info.af = std::move(extra);
        }
    }
    return info;
}

std::optional<PfPlayersSegment> parse_pf_players_segment(std::span<const uint8_t> payload)
{
    ByteReader r{payload.data(), payload.size()};
    const uint8_t version = r.u8();
    PfPlayersSegment seg;
    seg.response_id = r.u8();
    seg.sequence = r.u8();
    seg.total_segments = r.u8();
    if (!r.ok() || version != af_player_info_packet_version || seg.total_segments == 0
        || seg.sequence >= seg.total_segments) {
        return std::nullopt;
    }
    seg.slice = payload.subspan(payload.size() - r.remaining());
    return seg;
}

std::optional<ServerRoster> parse_pf_players(std::span<const uint8_t> joined)
{
    ByteReader r{joined.data(), joined.size()};
    af_player_info_payload_header preamble{};
    if (!r.get(preamble)) {
        return std::nullopt;
    }
    ServerRoster roster;
    roster.red_score = preamble.red_score;
    roster.blue_score = preamble.blue_score;
    roster.time_left_seconds = preamble.time_left_seconds;
    roster.af_flags = preamble.af_flags;
    roster.game_type = preamble.game_type;
    if (!read_cstring(r, roster.level_filename, RF_MAX_LEVEL_NAME_LEN)) {
        return std::nullopt;
    }
    while (r.remaining() > 0) {
        if (roster.players.size() >= static_cast<size_t>(rf::multi_max_player_slots)) {
            return std::nullopt;
        }
        ServerRosterPlayer player;
        player.flags = r.u8();
        r.get(player.score);
        player.kills = r.u16();
        player.deaths = r.u16();
        player.caps = r.u16();
        if (!r.ok() || !read_cstring(r, player.name, k_max_player_name_len)) {
            return std::nullopt;
        }
        roster.players.push_back(std::move(player));
    }
    return roster;
}

std::optional<TrackerPacket> parse_tracker_packet(std::span<const uint8_t> datagram)
{
    RF_TrackerHeader hdr{};
    if (datagram.size() < 1 + sizeof(hdr) || datagram[0] != RF_TRACKER) {
        return std::nullopt;
    }
    std::memcpy(&hdr, datagram.data() + 1, sizeof(hdr));
    // packet_len counts the main packet type too (multi_tracker_do_frame)
    if (hdr.unknown != tracker_magic || hdr.packet_len != datagram.size()) {
        return std::nullopt;
    }
    TrackerPacket pkt;
    pkt.type = hdr.type;
    pkt.seq = hdr.seq;
    if (pkt.type == RF_TPT_SERVER_LIST) {
        ByteReader r{datagram.data() + 1 + sizeof(hdr), datagram.size() - 1 - sizeof(hdr)};
        const uint8_t count = r.u8();
        r.u32(); // server_count
        for (uint8_t i = 0; i < count; ++i) {
            RF_TrackerServerAddress wire{};
            if (!r.get(wire)) {
                return std::nullopt;
            }
            rf::NetAddr addr{};
            addr.ip_addr.inner = ntohl(wire.ip);
            addr.port = ntohs(wire.port);
            pkt.servers.push_back(addr);
        }
    }
    return pkt;
}

std::vector<uint8_t> make_game_info_request()
{
    // Matches send_game_info_req_packet_hook: an empty stock request with the AF tail.
    const AFGameInfoReq core{ALPINE_FACTION_SIGNATURE, af_game_info_req_ext_ver};
    const AFFooter footer{static_cast<uint16_t>(sizeof(core)), AF_FOOTER_MAGIC};
    const RF_GamePacketHeader hdr{RF_GPT_GAME_INFO_REQUEST, static_cast<uint16_t>(sizeof(core) + sizeof(footer))};
    std::vector<uint8_t> out{static_cast<uint8_t>(RF_GAME)};
    append_bytes(out, hdr);
    append_bytes(out, core);
    append_bytes(out, footer);
    return out;
}

std::vector<uint8_t> make_pf_players_request()
{
    const rf_packet_header hdr{static_cast<uint8_t>(pf_packet_type::players_request), 0};
    std::vector<uint8_t> out{static_cast<uint8_t>(RF_GAME)};
    append_bytes(out, hdr);
    return out;
}

std::vector<uint8_t> make_tracker_list_request()
{
    return make_tracker_packet(RF_TPT_SERVER_LIST_REQUEST, 0);
}

std::vector<uint8_t> make_tracker_ack(uint32_t seq)
{
    return make_tracker_packet(RF_TPT_REPLY, seq);
}

std::optional<rf::NetAddr> parse_server_address(std::string_view text)
{
    text = trim(text);
    const size_t colon = text.find(':');
    const std::string_view host = text.substr(0, colon);

    uint32_t ip = 0;
    const char* p = host.data();
    const char* const host_end = p + host.size();
    for (int i = 0; i < 4; ++i) {
        if (i > 0) {
            if (p == host_end || *p != '.') {
                return std::nullopt;
            }
            ++p;
        }
        unsigned octet = 0;
        const auto [next, ec] = std::from_chars(p, host_end, octet);
        if (ec != std::errc{} || next - p > 3 || octet > 255) {
            return std::nullopt;
        }
        ip = (ip << 8) | octet;
        p = next;
    }
    if (p != host_end || ip == 0 || ip == INADDR_NONE) {
        return std::nullopt;
    }

    uint16_t port = default_server_port;
    if (colon != std::string_view::npos) {
        const std::string_view port_text = text.substr(colon + 1);
        unsigned value = 0;
        const auto [next, ec] = std::from_chars(port_text.data(), port_text.data() + port_text.size(), value);
        if (ec != std::errc{} || next != port_text.data() + port_text.size() || value == 0 || value > 65535) {
            return std::nullopt;
        }
        port = static_cast<uint16_t>(value);
    }

    rf::NetAddr addr{};
    addr.ip_addr.inner = ip;
    addr.port = port;
    return addr;
}
