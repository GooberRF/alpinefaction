#include "afd_header_reader.h"
#include <concepts>
#include <type_traits>
#include <vector>
#include <zlib.h>
#include <common/afd_format.h>
#include <common/tlv.h>

namespace
{

bool gz_read_all(gzFile file, void* data, size_t len)
{
    auto* bytes = static_cast<uint8_t*>(data);
    size_t total = 0;
    while (total < len) {
        const int n = gzread(file, bytes + total, static_cast<unsigned>(len - total));
        if (n <= 0)
            return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

template<std::integral T>
bool gz_read_le(gzFile file, T& out)
{
    uint8_t bytes[sizeof(T)];
    if (!gz_read_all(file, bytes, sizeof(bytes)))
        return false;
    using U = std::make_unsigned_t<T>;
    U res = 0;
    for (size_t i = 0; i < sizeof(T); ++i)
        res |= static_cast<U>(bytes[i]) << (8 * i);
    out = static_cast<T>(res);
    return true;
}

} // namespace

AfdReadStatus afd_read_header(const std::string& path, AfdHeaderInfo& out)
{
    gzFile file = gzopen(path.c_str(), "rb");
    if (!file)
        return AfdReadStatus::cant_open;

    uint32_t magic = 0;
    uint32_t header_len = 0;
    if (!gz_read_le(file, magic) || magic != AFD_MAGIC) {
        gzclose(file);
        return AfdReadStatus::bad_magic;
    }
    if (!gz_read_le(file, out.format_major) || !gz_read_le(file, out.format_minor)
        || !gz_read_le(file, header_len)) {
        gzclose(file);
        return AfdReadStatus::bad_header;
    }
    if (out.format_major > AFD_FORMAT_MAJOR) {
        gzclose(file);
        return AfdReadStatus::newer_format;
    }
    if (header_len > 0x10000) {
        gzclose(file);
        return AfdReadStatus::bad_header;
    }

    std::vector<uint8_t> tlv_buf(header_len);
    const bool read_ok = gz_read_all(file, tlv_buf.data(), tlv_buf.size());
    gzclose(file);
    if (!read_ok)
        return AfdReadStatus::bad_header;

    TlvReader<DemoHeaderTlvType> tlv{tlv_buf};
    while (auto entry = tlv.next()) {
        auto read_str = [&] { return std::string{reinterpret_cast<const char*>(entry->value), entry->len}; };
        switch (entry->type) {
        case DemoHeaderTlvType::level_filename:
            out.level_filename = read_str();
            break;
        case DemoHeaderTlvType::game_type:
            out.game_type = entry->read_le<int32_t>().value_or(-1);
            break;
        case DemoHeaderTlvType::mod_name:
            out.mod_name = read_str();
            break;
        case DemoHeaderTlvType::server_name:
            out.server_name = read_str();
            break;
        case DemoHeaderTlvType::start_time_unix:
            out.start_time_unix = entry->read_le<uint64_t>().value_or(0);
            break;
        case DemoHeaderTlvType::required_features:
            out.required_features = entry->read_le<uint32_t>().value_or(0);
            break;
        default:
            break;
        }
    }
    return AfdReadStatus::ok;
}
