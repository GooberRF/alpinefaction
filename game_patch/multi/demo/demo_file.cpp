#include <algorithm>
#include <cstring>
#include <ctime>
#include <format>
#include <utility>
#include <zlib.h>
#include <windows.h>
#include <xlog/xlog.h>
#include "demo_file.h"
#include "../../misc/tlv.h"
#include "../../rf/file/file.h"

namespace
{
    gzFile as_gz(void* file)
    {
        return static_cast<gzFile>(file);
    }

    void tlv_write_str(std::vector<uint8_t>& buf, DemoHeaderTlvType type, const std::string& value)
    {
        const size_t len = std::min(value.size(), size_t{255});
        buf.push_back(std::to_underlying(type));
        buf.push_back(static_cast<uint8_t>(len));
        buf.insert(buf.end(), value.begin(), value.begin() + len);
    }

    bool gz_write_all(gzFile file, const void* data, size_t len)
    {
        return gzwrite(file, data, static_cast<unsigned>(len)) == static_cast<int>(len);
    }

    bool gz_read_all(gzFile file, void* data, size_t len)
    {
        return gzread(file, data, static_cast<unsigned>(len)) == static_cast<int>(len);
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

    template<std::integral T>
    void append_le(std::vector<uint8_t>& buf, T value)
    {
        using U = std::make_unsigned_t<T>;
        const U unsigned_value = static_cast<U>(value);
        for (size_t i = 0; i < sizeof(T); ++i)
            buf.push_back(static_cast<uint8_t>((unsigned_value >> (8 * i)) & 0xFF));
    }

    // "<rf_root>\demos", optionally created. Empty on failure.
    std::string demos_dir(bool create)
    {
        auto dir = std::format("{}\\demos", rf::root_path);
        if (dir.size() + 64 > rf::max_path_len) {
            xlog::error("demo path is too long!");
            return {};
        }
        if (create && !CreateDirectoryA(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            xlog::error("Failed to create demos directory, error {}", GetLastError());
            return {};
        }
        return dir;
    }
}

DemoFileWriter::~DemoFileWriter()
{
    abort();
}

bool DemoFileWriter::open(const std::string& path, const DemoHeaderInfo& header)
{
    abort();
    gzFile file = gzopen(path.c_str(), "wb");
    if (!file) {
        xlog::error("Failed to open demo file for writing: {}", path);
        return false;
    }

    std::vector<uint8_t> tlv_buf;
    TlvWriter<DemoHeaderTlvType> tlv{tlv_buf};
    tlv.write_le(DemoHeaderTlvType::af_version_major, header.af_version_major);
    tlv.write_le(DemoHeaderTlvType::af_version_minor, header.af_version_minor);
    tlv.write_le(DemoHeaderTlvType::af_version_patch, header.af_version_patch);
    tlv_write_str(tlv_buf, DemoHeaderTlvType::level_filename, header.level_filename);
    tlv.write_le(DemoHeaderTlvType::level_checksum, header.level_checksum);
    tlv.write_le(DemoHeaderTlvType::game_type, header.game_type);
    tlv_write_str(tlv_buf, DemoHeaderTlvType::mod_name, header.mod_name);
    tlv_write_str(tlv_buf, DemoHeaderTlvType::server_name, header.server_name);
    tlv.write_le(DemoHeaderTlvType::server_netfps, header.server_netfps);
    tlv.write_le(DemoHeaderTlvType::start_time_unix, header.start_time_unix);
    tlv.write_le(DemoHeaderTlvType::server_max_players, header.server_max_players);
    tlv.write_le(DemoHeaderTlvType::demo_player_id, header.demo_player_id);
    tlv.write_le(DemoHeaderTlvType::required_features, header.required_features);

    std::vector<uint8_t> prelude;
    append_le(prelude, AFD_MAGIC);
    append_le(prelude, AFD_FORMAT_MAJOR);
    append_le(prelude, AFD_FORMAT_MINOR);
    append_le(prelude, static_cast<uint32_t>(tlv_buf.size()));

    if (!gz_write_all(file, prelude.data(), prelude.size()) || !gz_write_all(file, tlv_buf.data(), tlv_buf.size())) {
        xlog::error("Failed to write demo header: {}", path);
        gzclose(file);
        return false;
    }

    m_file = file;
    m_path = path;
    m_packet_count = 0;
    return true;
}

void DemoFileWriter::write_packet(uint32_t t_ms, const void* data, size_t len, uint8_t flags)
{
    if (!m_file || len == 0 || len > 0xFFFF - 1)
        return;
    std::vector<uint8_t> buf;
    buf.reserve(4 + 1 + 2 + 1 + len);
    append_le(buf, t_ms);
    buf.push_back(std::to_underlying(DemoRecordType::packet));
    append_le(buf, static_cast<uint16_t>(len + 1)); // payload = flags byte + packet bytes
    buf.push_back(flags);
    const auto* bytes = static_cast<const uint8_t*>(data);
    buf.insert(buf.end(), bytes, bytes + len);
    if (!gz_write_all(as_gz(m_file), buf.data(), buf.size())) {
        xlog::error("Failed writing to demo file {} - stopping recording", m_path);
        gzclose(as_gz(m_file));
        m_file = nullptr;
        return;
    }
    ++m_packet_count;
}

void DemoFileWriter::close(uint32_t duration_ms)
{
    if (!m_file)
        return;
    std::vector<uint8_t> buf;
    append_le(buf, duration_ms);
    buf.push_back(std::to_underlying(DemoRecordType::footer));
    append_le(buf, static_cast<uint16_t>(8));
    append_le(buf, duration_ms);
    append_le(buf, m_packet_count);
    gz_write_all(as_gz(m_file), buf.data(), buf.size());
    gzclose(as_gz(m_file));
    m_file = nullptr;
}

void DemoFileWriter::abort()
{
    if (m_file) {
        gzclose(as_gz(m_file));
        m_file = nullptr;
    }
}

DemoFileReader::~DemoFileReader()
{
    close();
}

DemoFileReader::OpenResult DemoFileReader::open(const std::string& path)
{
    close();
    gzFile file = gzopen(path.c_str(), "rb");
    if (!file)
        return OpenResult::cant_open;
    m_file = file;
    m_path = path;
    m_header = {}; // don't leak TLV values from a previously opened file (m_footer persists for rewind)

    uint32_t magic = 0;
    uint32_t header_len = 0;
    if (!gz_read_le(file, magic) || magic != AFD_MAGIC) {
        close();
        return OpenResult::bad_magic;
    }
    if (!gz_read_le(file, m_header.format_major) || !gz_read_le(file, m_header.format_minor)
        || !gz_read_le(file, header_len)) {
        close();
        return OpenResult::bad_header;
    }
    if (m_header.format_major > AFD_FORMAT_MAJOR) {
        close();
        return OpenResult::newer_format;
    }
    if (header_len > 0x10000) {
        close();
        return OpenResult::bad_header;
    }

    std::vector<uint8_t> tlv_buf(header_len);
    if (!gz_read_all(file, tlv_buf.data(), tlv_buf.size())) {
        close();
        return OpenResult::bad_header;
    }
    TlvReader<DemoHeaderTlvType> tlv{tlv_buf};
    while (auto entry = tlv.next()) {
        auto read_str = [&] { return std::string{reinterpret_cast<const char*>(entry->value), entry->len}; };
        switch (entry->type) {
        case DemoHeaderTlvType::af_version_major:
            m_header.af_version_major = entry->read_le<uint8_t>().value_or(0);
            break;
        case DemoHeaderTlvType::af_version_minor:
            m_header.af_version_minor = entry->read_le<uint8_t>().value_or(0);
            break;
        case DemoHeaderTlvType::af_version_patch:
            m_header.af_version_patch = entry->read_le<uint8_t>().value_or(0);
            break;
        case DemoHeaderTlvType::level_filename:
            m_header.level_filename = read_str();
            break;
        case DemoHeaderTlvType::level_checksum:
            m_header.level_checksum = entry->read_le<uint32_t>().value_or(0);
            break;
        case DemoHeaderTlvType::game_type:
            m_header.game_type = entry->read_le<int32_t>().value_or(0);
            break;
        case DemoHeaderTlvType::mod_name:
            m_header.mod_name = read_str();
            break;
        case DemoHeaderTlvType::server_name:
            m_header.server_name = read_str();
            break;
        case DemoHeaderTlvType::server_netfps:
            m_header.server_netfps = entry->read_le<uint32_t>().value_or(0);
            break;
        case DemoHeaderTlvType::start_time_unix:
            m_header.start_time_unix = entry->read_le<uint64_t>().value_or(0);
            break;
        case DemoHeaderTlvType::server_max_players:
            m_header.server_max_players = entry->read_le<uint32_t>().value_or(0);
            break;
        case DemoHeaderTlvType::demo_player_id:
            m_header.demo_player_id = entry->read_le<uint8_t>().value_or(0);
            break;
        case DemoHeaderTlvType::required_features:
            m_header.required_features = entry->read_le<uint32_t>().value_or(0);
            break;
        default:
            // Unknown TLV type written by a newer minor version - skip
            break;
        }
    }
    if (m_header.required_features & ~AFD_KNOWN_FEATURES) {
        // Reader intentionally stays open (see OpenResult::missing_features)
        return OpenResult::missing_features;
    }
    return OpenResult::ok;
}

void DemoFileReader::close()
{
    if (m_file) {
        gzclose(as_gz(m_file));
        m_file = nullptr;
    }
}

bool DemoFileReader::next_record(DemoRecord& out)
{
    gzFile file = as_gz(m_file);
    if (!file)
        return false;
    while (true) {
        uint32_t t_ms = 0;
        uint8_t type = 0;
        uint16_t payload_len = 0;
        if (!gz_read_le(file, t_ms) || !gz_read_le(file, type) || !gz_read_le(file, payload_len)) {
            return false; // clean EOF after footer or truncated file - both end iteration
        }
        std::vector<uint8_t> payload(payload_len);
        if (!gz_read_all(file, payload.data(), payload.size())) {
            return false; // truncated record
        }
        if (type == std::to_underlying(DemoRecordType::footer)) {
            if (payload.size() >= 8) {
                std::memcpy(&m_footer.duration_ms, payload.data(), 4);
                std::memcpy(&m_footer.packet_count, payload.data() + 4, 4);
                m_has_footer = true;
            }
            return false;
        }
        if (type != std::to_underlying(DemoRecordType::packet)) {
            xlog::debug("Skipping unknown demo record type 0x{:02x}", type);
            continue; // unknown/reserved record type - skip (forward compat)
        }
        out.t_ms = t_ms;
        out.type = type;
        out.payload = std::move(payload);
        return true;
    }
}

bool DemoFileReader::rewind_to_records()
{
    if (m_path.empty())
        return false;
    const std::string path = m_path;
    const OpenResult result = open(path);
    return result == OpenResult::ok || result == OpenResult::missing_features;
}

std::string demo_file_build_new_path(const std::string& map_name)
{
    auto dir = demos_dir(true);
    if (dir.empty())
        return {};
    std::time_t now = std::time(nullptr);
    std::tm* tm_buf = std::localtime(&now);
    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", tm_buf);
    return std::format("{}\\{}_{}.afd", dir, map_name, timestamp);
}

bool demo_file_delete(const std::string& path)
{
    if (!DeleteFileA(path.c_str())) {
        xlog::warn("Failed to delete demo file {}, error {}", path, GetLastError());
        return false;
    }
    return true;
}

DemoDirListing demo_file_list_dir(const std::string& rel_dir)
{
    DemoDirListing listing;
    auto dir = demos_dir(false);
    if (dir.empty())
        return listing;
    if (!rel_dir.empty())
        dir += "\\" + rel_dir;
    // Same headroom demos_dir() reserves for the filename part
    if (dir.size() + 64 > rf::max_path_len)
        return listing;
    WIN32_FIND_DATAA find_data;
    HANDLE find_handle = FindFirstFileA((dir + "\\*").c_str(), &find_data);
    if (find_handle == INVALID_HANDLE_VALUE)
        return listing;
    listing.ok = true;
    do {
        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Reparse points can form cycles or escape the demos tree
            if (find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                continue;
            if (std::strcmp(find_data.cFileName, ".") == 0 || std::strcmp(find_data.cFileName, "..") == 0)
                continue;
            listing.dirs.emplace_back(find_data.cFileName);
        }
        else {
            std::string name = find_data.cFileName;
            if (name.size() > 4 && _stricmp(name.c_str() + name.size() - 4, ".afd") == 0) {
                name.resize(name.size() - 4);
                listing.names.push_back(std::move(name));
            }
        }
    } while (FindNextFileA(find_handle, &find_data));
    FindClose(find_handle);
    std::sort(listing.dirs.begin(), listing.dirs.end());
    std::sort(listing.names.begin(), listing.names.end());
    return listing;
}

std::vector<std::string> demo_file_list_names(const std::string& prefix)
{
    // Everything up to the last path separator selects the folder to list,
    // the remainder prefix-filters entries within it.
    std::string rel_dir;
    std::string leaf = prefix;
    if (auto sep = prefix.find_last_of("\\/"); sep != std::string::npos) {
        rel_dir = prefix.substr(0, sep);
        leaf = prefix.substr(sep + 1);
    }
    std::replace(rel_dir.begin(), rel_dir.end(), '/', '\\');
    const std::string base = rel_dir.empty() ? std::string{} : rel_dir + "\\";

    std::vector<std::string> names;
    const DemoDirListing listing = demo_file_list_dir(rel_dir);
    for (const auto& dir_name : listing.dirs) {
        if (_strnicmp(dir_name.c_str(), leaf.c_str(), leaf.size()) == 0)
            names.push_back(base + dir_name + "\\");
    }
    for (const auto& demo_name : listing.names) {
        if (_strnicmp(demo_name.c_str(), leaf.c_str(), leaf.size()) == 0)
            names.push_back(base + demo_name);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string demo_file_resolve_path(const std::string& name)
{
    std::string result = name;
    if (result.size() < 4 || _stricmp(result.c_str() + result.size() - 4, ".afd") != 0)
        result += ".afd";
    const bool is_absolute = result.contains(':') || result.starts_with("\\") || result.starts_with("/");
    if (!is_absolute) {
        // Reject ".." components so relative names cannot escape <rf_root>\demos
        for (size_t pos = 0; pos < result.size();) {
            size_t end = result.find_first_of("\\/", pos);
            if (end == std::string::npos)
                end = result.size();
            if (end - pos == 2 && result[pos] == '.' && result[pos + 1] == '.')
                return {};
            pos = end + 1;
        }
        auto dir = demos_dir(false);
        if (dir.empty())
            return result;
        result = std::format("{}\\{}", dir, result);
    }
    return result;
}
