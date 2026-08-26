#pragma once

#include <cstdint>
#include <string>

// Display metadata read from an .afd demo file's TLV header. Only the fields the launcher
// shows are populated; everything else in the header is skipped.
struct AfdHeaderInfo
{
    uint16_t format_major = 0;
    uint16_t format_minor = 0;
    std::string level_filename;
    int32_t game_type = -1;
    std::string mod_name;
    std::string server_name;
    uint64_t start_time_unix = 0;
    uint32_t required_features = 0;
};

enum class AfdReadStatus
{
    ok,
    cant_open,
    bad_magic,
    newer_format,
    bad_header,
};

AfdReadStatus afd_read_header(const std::string& path, AfdHeaderInfo& out);
