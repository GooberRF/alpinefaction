#include <common/cache_pack.h>
#include <windows.h>
#include <algorithm>
#include <bit>
#include <format>
#include <fstream>
#include <random>
#include <string_view>
#include <unordered_set>
#include <zlib.h>

namespace cache_pack
{

namespace
{
    static_assert(std::endian::native == std::endian::little);

    constexpr uint64_t header_size = 20;
    constexpr std::streamoff write_id_offset = 8;
    // An index entry without its name: u16 name length, u64 tag, u32 data size, u32 data CRC
    constexpr uint64_t entry_fixed_size = 2 + 8 + 4 + 4;

    uint32_t crc_of(std::span<const unsigned char> data)
    {
        return static_cast<uint32_t>(crc32(crc32(0, Z_NULL, 0), data.data(), static_cast<uInt>(data.size())));
    }

    template<typename T>
    bool read_value(std::istream& in, T& value)
    {
        return static_cast<bool>(in.read(reinterpret_cast<char*>(&value), sizeof(value)));
    }

    template<typename T>
    void write_value(std::ostream& out, const T& value)
    {
        out.write(reinterpret_cast<const char*>(&value), sizeof(value));
    }

    std::optional<Index> read_index(std::istream& in)
    {
        in.seekg(0, std::ios::end);
        const auto end = static_cast<std::streamoff>(in.tellg());
        in.seekg(0);
        uint32_t file_signature = 0;
        uint32_t file_version = 0;
        Index index;
        uint32_t count = 0;
        if (end < static_cast<std::streamoff>(header_size) || !read_value(in, file_signature)
            || !read_value(in, file_version) || !read_value(in, index.write_id) || !read_value(in, count)
            || file_signature != signature || file_version != version) {
            return std::nullopt;
        }
        const auto file_size = static_cast<uint64_t>(end);
        // Every entry has a name of at least one byte
        if (count > (file_size - header_size) / (entry_fixed_size + 1)) {
            return std::nullopt;
        }
        uint64_t data_size = 0;
        for (uint32_t i = 0; i < count; ++i) {
            uint16_t name_len = 0;
            if (!read_value(in, name_len) || name_len == 0) {
                return std::nullopt;
            }
            // Grown as entries are read, so a header claiming many costs nothing until they exist
            Entry& entry = index.entries.emplace_back();
            entry.name.resize(name_len);
            if (!in.read(entry.name.data(), name_len) || !read_value(in, entry.tag) || !read_value(in, entry.size)
                || !read_value(in, entry.crc)) {
                return std::nullopt;
            }
            entry.offset = data_size;
            data_size += entry.size;
        }
        const auto data_start = static_cast<uint64_t>(in.tellg());
        if (data_start + data_size != file_size) {
            return std::nullopt;
        }
        std::unordered_set<std::string_view> names;
        for (Entry& entry : index.entries) {
            if (!names.insert(entry.name).second) {
                return std::nullopt;
            }
            entry.offset += data_start;
        }
        return index;
    }

    std::filesystem::path temp_path(const std::filesystem::path& path)
    {
        std::filesystem::path tmp = path;
        tmp += std::format(".{}.{}.tmp", GetCurrentProcessId(), GetCurrentThreadId());
        return tmp;
    }
}

std::optional<Index> read_index(const std::filesystem::path& path)
{
    std::ifstream in{path, std::ios::binary};
    if (!in) {
        return std::nullopt;
    }
    return read_index(in);
}

bool read_data(const std::filesystem::path& path, uint64_t write_id, const Entry& entry,
               std::vector<unsigned char>& out)
{
    std::ifstream in{path, std::ios::binary};
    uint64_t file_write_id = 0;
    out.resize(entry.size);
    if (!in || !in.seekg(write_id_offset) || !read_value(in, file_write_id) || file_write_id != write_id
        || !in.seekg(static_cast<std::streamoff>(entry.offset))
        || !in.read(reinterpret_cast<char*>(out.data()), entry.size) || crc_of(out) != entry.crc) {
        out.clear();
        return false;
    }
    return true;
}

std::optional<Index> update(const std::filesystem::path& path, std::span<const std::string> removed,
                            std::span<const NewEntry> added)
{
    std::unordered_set<std::string_view> added_names;
    for (const NewEntry& entry : added) {
        if (entry.name.empty() || entry.name.size() > max_name_len || entry.data.size() > UINT32_MAX
            || !added_names.insert(entry.name).second) {
            return std::nullopt;
        }
    }
    const std::unordered_set<std::string_view> removed_names{removed.begin(), removed.end()};

    const std::filesystem::path tmp = temp_path(path);
    Index index;
    bool written = false;
    {
        // Open for the whole copy: Windows refuses to replace a file that is open, so the index
        // read here keeps describing the data copied below
        std::ifstream src{path, std::ios::binary};
        Index current;
        if (src) {
            current = read_index(src).value_or(Index{});
            src.clear();
        }
        else {
            // A pack that exists but can't be opened (another process replacing it, a backup tool
            // holding it) would lose its entries
            std::error_code ec;
            if (std::filesystem::exists(path, ec) || ec) {
                return std::nullopt;
            }
        }
        std::erase_if(current.entries, [&](const Entry& entry) {
            return removed_names.contains(entry.name) || added_names.contains(entry.name);
        });
        if (current.entries.size() + added.size() > UINT32_MAX) {
            return std::nullopt;
        }

        std::random_device random;
        do {
            index.write_id = (static_cast<uint64_t>(random()) << 32) | random();
        } while (index.write_id == current.write_id);

        uint64_t data_start = header_size;
        for (const Entry& entry : current.entries) {
            data_start += entry_fixed_size + entry.name.size();
        }
        for (const NewEntry& entry : added) {
            data_start += entry_fixed_size + entry.name.size();
        }
        index.entries.reserve(current.entries.size() + added.size());
        uint64_t offset = data_start;
        for (const Entry& entry : current.entries) {
            index.entries.push_back({entry.name, entry.tag, offset, entry.size, entry.crc});
            offset += entry.size;
        }
        for (const NewEntry& entry : added) {
            index.entries.push_back(
                {entry.name, entry.tag, offset, static_cast<uint32_t>(entry.data.size()), crc_of(entry.data)});
            offset += entry.data.size();
        }

        std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
        write_value(out, signature);
        write_value(out, version);
        write_value(out, index.write_id);
        write_value(out, static_cast<uint32_t>(index.entries.size()));
        for (const Entry& entry : index.entries) {
            write_value(out, static_cast<uint16_t>(entry.name.size()));
            out.write(entry.name.data(), static_cast<std::streamsize>(entry.name.size()));
            write_value(out, entry.tag);
            write_value(out, entry.size);
            write_value(out, entry.crc);
        }
        bool copied = true;
        std::vector<char> buf(64 * 1024);
        for (const Entry& entry : current.entries) {
            src.seekg(static_cast<std::streamoff>(entry.offset));
            for (uint32_t left = entry.size; left > 0 && copied;) {
                const uint32_t chunk = std::min<uint32_t>(left, static_cast<uint32_t>(buf.size()));
                copied = static_cast<bool>(src.read(buf.data(), chunk));
                out.write(buf.data(), chunk);
                left -= chunk;
            }
        }
        for (const NewEntry& entry : added) {
            out.write(reinterpret_cast<const char*>(entry.data.data()), static_cast<std::streamsize>(entry.data.size()));
        }
        out.close();
        written = copied && out;
    }
    if (!written || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        return std::nullopt;
    }
    return index;
}

void remove_stale_temps(const std::filesystem::path& path)
{
    // Named <pack file name>.<process id>.<thread id>.tmp by temp_path
    const std::wstring prefix = path.filename().wstring() + L".";
    std::error_code ec;
    for (std::filesystem::directory_iterator it{path.parent_path(), ec}, end; !ec && it != end; it.increment(ec)) {
        const std::wstring name = it->path().filename().wstring();
        if (name.starts_with(prefix) && name.ends_with(L".tmp")) {
            std::error_code remove_ec;
            std::filesystem::remove(it->path(), remove_ec);
        }
    }
}

}
