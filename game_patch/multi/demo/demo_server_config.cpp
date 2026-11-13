#include "demo_server_config.h"

#include <algorithm>
#include <concepts>
#include <cstring>
#include <type_traits>

#include <xlog/xlog.h>

namespace {

constexpr uint8_t k_block_version = 1;
// u16 record payload cap (0xFFFF) minus one byte of headroom, matching write_packet's limit.
constexpr size_t k_max_block_bytes = 65534;

// Bounds a crafted block cannot exceed without being rejected.
constexpr uint16_t k_max_count = 256;
constexpr uint16_t k_max_str_len = 1024;

// value tags
constexpr uint8_t k_tag_bool = 0;
constexpr uint8_t k_tag_int64 = 1;
constexpr uint8_t k_tag_double = 2;
constexpr uint8_t k_tag_string = 3;

template<std::integral T>
void put_le(std::vector<uint8_t>& buf, T value)
{
    using U = std::make_unsigned_t<T>;
    const U u = static_cast<U>(value);
    for (size_t i = 0; i < sizeof(T); ++i)
        buf.push_back(static_cast<uint8_t>((u >> (8 * i)) & 0xFF));
}

void put_str(std::vector<uint8_t>& buf, const std::string& s)
{
    const uint16_t len = static_cast<uint16_t>(std::min(s.size(), size_t{k_max_str_len}));
    put_le<uint16_t>(buf, len);
    buf.insert(buf.end(), s.begin(), s.begin() + len);
}

void put_value(std::vector<uint8_t>& buf, const server_config::SettingValue& value)
{
    std::visit(
        [&buf](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>) {
                buf.push_back(k_tag_bool);
                buf.push_back(v ? 1 : 0);
            }
            else if constexpr (std::is_same_v<T, int64_t>) {
                buf.push_back(k_tag_int64);
                put_le<int64_t>(buf, v);
            }
            else if constexpr (std::is_same_v<T, double>) {
                buf.push_back(k_tag_double);
                uint64_t bits = 0;
                std::memcpy(&bits, &v, sizeof(bits));
                put_le<uint64_t>(buf, bits);
            }
            else { // std::string
                buf.push_back(k_tag_string);
                put_str(buf, v);
            }
        },
        value);
}

// Bounded read cursor over an untrusted buffer. Every accessor checks against the remaining
// length and refuses (returns false) rather than reading past the end.
struct Cursor
{
    const uint8_t* p;
    size_t remaining;

    bool read_bytes(void* out, size_t n)
    {
        if (n > remaining)
            return false;
        std::memcpy(out, p, n);
        p += n;
        remaining -= n;
        return true;
    }
    bool read_u8(uint8_t& v) { return read_bytes(&v, 1); }
    template<std::integral T>
    bool read_le(T& v)
    {
        uint8_t bytes[sizeof(T)];
        if (!read_bytes(bytes, sizeof(bytes)))
            return false;
        using U = std::make_unsigned_t<T>;
        U u = 0;
        for (size_t i = 0; i < sizeof(T); ++i)
            u |= static_cast<U>(bytes[i]) << (8 * i);
        v = static_cast<T>(u);
        return true;
    }
};

bool read_str(Cursor& c, std::string& out)
{
    uint16_t len = 0;
    if (!c.read_le<uint16_t>(len))
        return false;
    if (len > k_max_str_len)
        return false;
    if (len > c.remaining) // validate against the buffer before allocating
        return false;
    out.resize(len);
    return len == 0 || c.read_bytes(out.data(), len);
}

bool read_value(Cursor& c, server_config::SettingValue& out)
{
    uint8_t tag = 0;
    if (!c.read_u8(tag))
        return false;
    switch (tag) {
        case k_tag_bool: {
            uint8_t b = 0;
            if (!c.read_u8(b))
                return false;
            out = (b != 0);
            return true;
        }
        case k_tag_int64: {
            int64_t v = 0;
            if (!c.read_le<int64_t>(v))
                return false;
            out = v;
            return true;
        }
        case k_tag_double: {
            uint64_t bits = 0;
            if (!c.read_le<uint64_t>(bits))
                return false;
            double d = 0.0;
            std::memcpy(&d, &bits, sizeof(d));
            out = d;
            return true;
        }
        case k_tag_string: {
            std::string s;
            if (!read_str(c, s))
                return false;
            out = std::move(s);
            return true;
        }
        default:
            return false; // unknown value tag => decode fails
    }
}

} // namespace

std::vector<uint8_t> encode_server_config_block(const server_config::ServerConfigSnapshot& snapshot)
{
    std::vector<uint8_t> buf;
    buf.push_back(k_block_version);
    buf.push_back(snapshot.rf_flags);
    put_le<uint32_t>(buf, snapshot.gi_flags);
    buf.push_back(snapshot.match_state);

    const size_t mutator_count = std::min(snapshot.mutators.size(), size_t{k_max_count});
    put_le<uint16_t>(buf, static_cast<uint16_t>(mutator_count));
    for (size_t m = 0; m < mutator_count; ++m) {
        const auto& mutator = snapshot.mutators[m];
        put_str(buf, mutator.name);
        const size_t setting_count = std::min(mutator.settings.size(), size_t{k_max_count});
        put_le<uint16_t>(buf, static_cast<uint16_t>(setting_count));
        for (size_t s = 0; s < setting_count; ++s) {
            const auto& [key, value] = mutator.settings[s];
            put_str(buf, key);
            put_value(buf, value);
        }
    }

    const size_t gametype_setting_count = std::min(snapshot.gametype_settings.size(), size_t{k_max_count});
    put_le<uint16_t>(buf, static_cast<uint16_t>(gametype_setting_count));
    for (size_t g = 0; g < gametype_setting_count; ++g) {
        const auto& [key, value] = snapshot.gametype_settings[g];
        put_str(buf, key);
        put_value(buf, value);
    }

    if (buf.size() > k_max_block_bytes) {
        xlog::warn("Server config block too large ({} bytes); skipping demo server_info record",
                   buf.size());
        return {};
    }
    return buf;
}

std::optional<server_config::ServerConfigSnapshot> decode_server_config_block(const uint8_t* data, size_t len)
{
    if (!data)
        return std::nullopt;
    Cursor c{data, len};

    // Read the version first, then parse the frozen v1 prefix. A future block_version>1 that
    // only appends fields still decodes here (trailing bytes are ignored below).
    uint8_t block_version = 0;
    if (!c.read_u8(block_version))
        return std::nullopt;
    (void)block_version;

    server_config::ServerConfigSnapshot snapshot;
    if (!c.read_u8(snapshot.rf_flags))
        return std::nullopt;
    if (!c.read_le<uint32_t>(snapshot.gi_flags))
        return std::nullopt;
    if (!c.read_u8(snapshot.match_state))
        return std::nullopt;

    uint16_t mutator_count = 0;
    if (!c.read_le<uint16_t>(mutator_count))
        return std::nullopt;
    if (mutator_count > k_max_count)
        return std::nullopt;
    // Each element needs at least one byte, so remaining is a true upper bound on the count;
    // reserve against the smaller of the capped count and what the buffer can actually hold.
    snapshot.mutators.reserve(std::min<size_t>(mutator_count, c.remaining));
    for (uint16_t i = 0; i < mutator_count; ++i) {
        server_config::MutatorRecord record;
        if (!read_str(c, record.name))
            return std::nullopt;
        uint16_t setting_count = 0;
        if (!c.read_le<uint16_t>(setting_count))
            return std::nullopt;
        if (setting_count > k_max_count)
            return std::nullopt;
        record.settings.reserve(std::min<size_t>(setting_count, c.remaining));
        for (uint16_t j = 0; j < setting_count; ++j) {
            std::string key;
            if (!read_str(c, key))
                return std::nullopt;
            server_config::SettingValue value;
            if (!read_value(c, value))
                return std::nullopt;
            record.settings.emplace_back(std::move(key), std::move(value));
        }
        snapshot.mutators.push_back(std::move(record));
    }

    uint16_t gametype_setting_count = 0;
    if (!c.read_le<uint16_t>(gametype_setting_count))
        return std::nullopt;
    if (gametype_setting_count > k_max_count)
        return std::nullopt;
    snapshot.gametype_settings.reserve(std::min<size_t>(gametype_setting_count, c.remaining));
    for (uint16_t i = 0; i < gametype_setting_count; ++i) {
        std::string key;
        if (!read_str(c, key))
            return std::nullopt;
        server_config::SettingValue value;
        if (!read_value(c, value))
            return std::nullopt;
        snapshot.gametype_settings.emplace_back(std::move(key), std::move(value));
    }

    // Any trailing bytes (a future block_version>1's appended fields) are ignored on purpose.
    return snapshot;
}
