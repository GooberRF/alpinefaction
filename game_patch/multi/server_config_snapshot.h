#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// One source of truth for the server's base config (mutators + gametype settings + a few
// flags), shared verbatim by the afstats event stream and the demo recorder so both report
// the same data. afstats serializes this to JSON for FactionFiles; the demo recorder encodes
// it into a versioned binary block (see demo/demo_server_config.h).
namespace server_config {

using SettingValue = std::variant<bool, int64_t, double, std::string>;

struct MutatorRecord
{
    std::string name;
    std::vector<std::pair<std::string, SettingValue>> settings;
};

struct ServerConfigSnapshot
{
    uint8_t rf_flags = 0;
    uint32_t gi_flags = 0;
    uint8_t match_state = 0;
    std::vector<MutatorRecord> mutators;
    std::vector<std::pair<std::string, SettingValue>> gametype_settings;
};

// Byte cap for operator-authored strings (mutator names, option keys, string values).
constexpr size_t k_max_string_len = 64;

// Guarantees valid UTF-8 output: every codepoint that reaches the output is a whole,
// well-formed sequence. max_len is a byte cap; truncation stops on a sequence boundary so a
// codepoint is never bisected. C0 controls and DEL are stripped.
std::string sanitize_string(std::string_view in, size_t max_len);

// Fills every field from current server state. Server-side only (reads live netgame/config).
ServerConfigSnapshot capture_server_config_snapshot();

} // namespace server_config
