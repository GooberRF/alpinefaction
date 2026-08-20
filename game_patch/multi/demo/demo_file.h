#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <common/afd_format.h>
#include "../server_config_snapshot.h"

// .afd demo file container (see research/demo-system-prd.md §3).
// Entire file is a gzip stream. Inside:
//   [Fixed prelude]  magic u32 'AFDM', format_major u16, format_minor u16, header_len u32
//   [TLV header]     header_len bytes of {type u8, len u8, value} entries
//   [Record stream]  records of {t_ms u32, type u8, payload_len u16, payload}
// Record payloads:
//   type 0x00 (packet): {flags u8, packet bytes} — bytes are the logical game packet
//                       exactly as passed to multi_io_send* ({u8 type, u16 size, payload})
//   type 0x02 (server_info): a self-describing versioned block (block_version u8, then the
//                       server's base config: mutators, gametype settings, a few flags) —
//                       written once as the first record of a segment. Display-only metadata
//                       (see demo_server_config.h); readers that don't know it skip it.
//   type 0xFF (footer): {duration_ms u32, packet_count u32} — clean-close marker; a
//                       missing footer means "unclean but playable"
// Unknown TLV types and unknown record types are skipped (forward compatibility).
//
// Compatibility policy (mirrors the .rfl chunk approach - everything is skippable by length):
//   - minor bump: additive changes only - new TLV types, new record types (which must be
//     safe to skip), new DemoPacketFlags bits. Old readers skip what they don't know and
//     still play; readers never branch on minor - per-record flags and TLVs carry the
//     semantics (see DEMO_PKT_TEAM_SCOPED for the pattern).
//   - major bump: structural change to the prelude or record framing. Old readers reject
//     with OpenResult::newer_format.
//   - TLV ids are permanent: never reuse an id or change its value width - TlvReader
//     requires the exact width, so a widened value silently reads as 0 on old readers.
//     Allocate a new id instead.
//   - Content an old reader cannot meaningfully play (e.g. a new game mode) must set a bit
//     in the required_features TLV; readers refuse playback when they see unknown bits
//     (OpenResult::missing_features).
//   - Wire-packet layout changes should be additive/length-discriminated where possible
//     (see af_damage_notify's attacker-tagged demo form); otherwise gate the decoder on
//     the recording build's version via demo_playback_recorded_af_version() (demo.h).

enum class DemoRecordType : uint8_t
{
    packet = 0x00,
    marker = 0x01, // reserved for format 2.0 keyframe records (see research/demo-system-prd.md §3)
    server_info = 0x02, // versioned server-config block (display-only metadata; see demo_server_config.h)
    footer = 0xFF,
};

enum DemoPacketFlags : uint8_t
{
    DEMO_PKT_RELIABLE = 0x01,    // captured on the reliable path
    DEMO_PKT_SYNTHESIZED = 0x02, // synthesized during snapshot (join_accept)
    // Packet was team-filtered on the server (team chat, team location ping); playback
    // feeds it only when the followed player is on the scoped team (free cam sees all)
    DEMO_PKT_TEAM_SCOPED = 0x04,
    DEMO_PKT_TEAM1 = 0x08, // scoped team was team 1 (blue); unset with TEAM_SCOPED = team 0 (red)
};

struct DemoHeaderInfo
{
    uint16_t format_major = AFD_FORMAT_MAJOR;
    uint16_t format_minor = AFD_FORMAT_MINOR;
    uint8_t af_version_major = 0;
    uint8_t af_version_minor = 0;
    uint8_t af_version_patch = 0;
    std::string level_filename;
    uint32_t level_checksum = 0;
    int32_t game_type = 0;
    std::string mod_name;
    std::string server_name;
    uint32_t server_netfps = 0;
    uint64_t start_time_unix = 0;
    uint32_t server_max_players = 0;
    uint8_t demo_player_id = 0;
    uint32_t required_features = 0; // absent TLV (older demo) => no requirements
    // afstats identity, populated only for auto-recorded segments reported to FactionFiles
    // under a live session; empty session id / game 0 means "not eligible for upload".
    std::string afstats_session_id;
    uint32_t afstats_game = 0;
};

struct DemoFooterInfo
{
    uint32_t duration_ms = 0;
    uint32_t packet_count = 0;
};

struct DemoRecord
{
    uint32_t t_ms = 0;
    uint8_t type = 0;
    std::vector<uint8_t> payload;

    [[nodiscard]] bool is_packet() const
    {
        return type == static_cast<uint8_t>(DemoRecordType::packet) && payload.size() >= 1 + 3;
    }
    [[nodiscard]] uint8_t flags() const
    {
        return payload.empty() ? 0 : payload[0];
    }
    [[nodiscard]] const uint8_t* packet_data() const
    {
        return payload.size() < 1 + 3 ? payload.data() : payload.data() + 1;
    }
    [[nodiscard]] size_t packet_len() const
    {
        return payload.size() < 1 + 3 ? 0 : payload.size() - 1;
    }
    [[nodiscard]] uint8_t packet_type() const
    {
        return payload.size() < 1 + 3 ? 0 : payload[1];
    }
};

class DemoFileWriter
{
public:
    ~DemoFileWriter();

    bool open(const std::string& path, const DemoHeaderInfo& header);
    void write_packet(uint32_t t_ms, const void* data, size_t len, uint8_t flags);
    // Writes one server_info record carrying an encoded server-config block. No-op if the
    // block is empty or exceeds the u16 record payload cap.
    void write_server_info(const std::vector<uint8_t>& block);
    // Writes the footer record and closes the stream.
    void close(uint32_t duration_ms);
    // Closes without a footer (error path); the file stays parseable.
    void abort();

    [[nodiscard]] bool is_open() const
    {
        return m_file != nullptr;
    }
    [[nodiscard]] uint32_t packet_count() const
    {
        return m_packet_count;
    }
    // True if a write failed (which closes the file). Lets the recorder tear down
    // recording cleanly instead of leaving the virtual player orphaned.
    [[nodiscard]] bool had_error() const
    {
        return m_had_error;
    }
    [[nodiscard]] const std::string& path() const
    {
        return m_path;
    }

private:
    void* m_file = nullptr; // gzFile
    uint32_t m_packet_count = 0;
    bool m_had_error = false;
    std::string m_path;
};

class DemoFileReader
{
public:
    enum class OpenResult
    {
        ok,
        cant_open,
        bad_magic,
        newer_format,
        bad_header,
        // required_features has bits this build doesn't know. Unlike the other failure
        // results the reader stays open with the header parsed, so browser/details code
        // can still show the demo's metadata; playback must refuse.
        missing_features,
    };

    ~DemoFileReader();

    OpenResult open(const std::string& path);
    void close();
    // Reads the next record. Returns false at end of stream (clean or truncated).
    // The footer record is consumed internally (available via footer()) and not returned.
    bool next_record(DemoRecord& out);
    // Restarts record iteration from the first record (re-opens the gz stream).
    bool rewind_to_records();

    [[nodiscard]] bool is_open() const
    {
        return m_file != nullptr;
    }
    [[nodiscard]] const DemoHeaderInfo& header() const
    {
        return m_header;
    }
    [[nodiscard]] bool has_footer() const
    {
        return m_has_footer;
    }
    [[nodiscard]] const DemoFooterInfo& footer() const
    {
        return m_footer;
    }
    // The server-config block decoded from the head-of-stream server_info record, if the
    // demo carried one and it decoded cleanly. Populated by any full record scan.
    [[nodiscard]] const std::optional<server_config::ServerConfigSnapshot>& server_config() const
    {
        return m_server_config;
    }
    [[nodiscard]] const std::string& path() const
    {
        return m_path;
    }

private:
    bool parse_header();

    void* m_file = nullptr; // gzFile
    DemoHeaderInfo m_header;
    DemoFooterInfo m_footer;
    bool m_has_footer = false;
    std::optional<server_config::ServerConfigSnapshot> m_server_config;
    std::string m_path;
};

// Builds "<rf_root>\demos\" (optionally creating it) and returns the full path for a new
// demo file named "<map>_<YYYYMMDD-HHMMSS>.afd". Returns empty on failure.
std::string demo_file_build_new_path(const std::string& map_name);
// Resolves a demo name (as produced by the listing, i.e. with its .afd extension) to a full
// path: prepends <rf_root>\demos\ for relative names and appends .afd only when the name does
// not already end in it (identity for listing names; "x" -> "x.afd"). Relative names may
// contain subfolders ("tourney\match1"); ".." components are rejected (returns empty).
std::string demo_file_resolve_path(const std::string& name);
// Deletes a demo file from disk. Returns false (and logs) on failure.
bool demo_file_delete(const std::string& path);
// "<rf_root>\demos" (optionally created). Empty on failure. The upload feature enumerates
// the demos root and needs the same base path the recorder writes into.
std::string demo_file_demos_dir(bool create);
// "<rf_root>\demos\uploaded" (optionally created). Empty on failure. Where the uploader
// moves a demo after a confirmed upload when delete-after-send is off.
std::string demo_file_uploaded_dir(bool create);
// Lists demo names (with their .afd extension) whose relative path starts with prefix
// (case-insensitive; empty prefix lists the demos root). The prefix may contain subfolders:
// everything up to the last path separator selects the folder to list, the remainder
// filters entries within it. Matching subfolders are returned with a trailing backslash
// ("tourney\"). Sorted ascending.
std::vector<std::string> demo_file_list_names(const std::string& prefix);

// Max folder nesting depth below <rf_root>\demos
constexpr int demo_max_dir_depth = 8;

struct DemoDirListing
{
    bool ok = false;                // false if the directory could not be opened
    std::vector<std::string> dirs;  // subfolder leaf names, sorted ascending
    std::vector<std::string> names; // demo leaf names with .afd, sorted ascending
};

// Lists one folder under <rf_root>\demos. rel_dir is a backslash-separated relative path
// ("" = the demos root, "tourney\finals" = nested). Non-recursive.
DemoDirListing demo_file_list_dir(const std::string& rel_dir);
