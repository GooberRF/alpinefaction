#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Alpine cache packs (.cache): named blobs in one file, always rewritten whole (a temporary file
// beside it, renamed over it) so a reader never sees a partly written pack. Little-endian:
//   header: u32 signature, u32 version, u64 write id (new on every write), u32 entry count
//   index:  per entry: u16 name length, name bytes, u64 tag, u32 data size
//   data:   each entry's bytes in index order, ending at the end of the file
// Names are non-empty and unique. A file that breaks any of this, or has another version, is invalid.

namespace cache_pack
{

inline constexpr uint32_t signature = 0x50434641; // "AFCP"
inline constexpr uint32_t version = 1;
inline constexpr size_t max_name_len = 0xFFFF;

struct Entry
{
    std::string name;
    uint64_t tag = 0;    // the writer's own, e.g. a hash or timestamp of what the data was made from
    uint64_t offset = 0; // of the data, from the start of the file
    uint32_t size = 0;
};

struct Index
{
    // New on every write: the entries are only valid while the file still has it
    uint64_t write_id = 0;
    std::vector<Entry> entries;
};

struct NewEntry
{
    std::string name;
    uint64_t tag = 0;
    std::span<const unsigned char> data;
};

// The pack's index, or nullopt if the file is missing or not a valid pack of this version.
std::optional<Index> read_index(const std::filesystem::path& path);

// An entry's data, given its index's write id. False if the read fails or the pack is no longer the
// one the index came from (another process saved it since); read the index again then.
bool read_data(const std::filesystem::path& path, uint64_t write_id, const Entry& entry,
               std::vector<unsigned char>& out);

// Rewrites the pack at path (created if missing, replaced if invalid) with its current entries,
// less the removed names and the names an added entry replaces, followed by the added entries.
// Entries another process saved in the meantime are kept too. The new pack is written beside path
// and renamed over it, so the old one stays intact on failure. Returns the new index, or nullopt on
// failure (including a pack that exists but can't be opened) or when an added name is empty, too
// long or given twice.
std::optional<Index> update(const std::filesystem::path& path, std::span<const std::string> removed,
                            std::span<const NewEntry> added);

// Deletes temporary files an interrupted update left beside path.
void remove_stale_temps(const std::filesystem::path& path);

}
