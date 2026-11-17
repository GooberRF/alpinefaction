#pragma once

#include <windows.h>
#include <commctrl.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

// The editor's file system as a browser shows it: one folder per loose search path, nested under the search path
// that contains it, and one per packfile.
struct VfsBrowserSource
{
    std::string label;   // "red\meshes" for a search path, "meshes.vpp" for a packfile
    std::string path;    // search path as registered; empty for packfiles and the game root
    int parent;          // enclosing search path, -1 for a root or a packfile
    bool packfile;
};

struct VfsBrowserFile
{
    std::string name;    // bare filename, exactly what the property field stores
    int source;
};

// Lists every loose and packed file into the list `classify` picks for its name (an index into `lists`, or -1 to
// leave it out), each list sorted by name.
void vfs_browser_collect(std::vector<VfsBrowserSource>& sources, const std::vector<std::vector<VfsBrowserFile>*>& lists,
                         const std::function<int(const std::string&)>& classify);

// Refills `tree` with the entries that `allow` (when given) and the lowercase substring `filter` let through, and
// selects the one named `select_name`, or the first with `select_first`. `rebuilding` is set while items are deleted
// and reselected, as each notifies the owner. Returns how many entries are shown.
int vfs_browser_rebuild_tree(HWND tree, const std::vector<VfsBrowserSource>& sources,
                             const std::vector<VfsBrowserFile>& entries, const std::vector<char>* allow,
                             const std::string& filter, const std::string& select_name, bool select_first,
                             int* select_index_out, bool& rebuilding);

// The entry a tree item stands for, or -1 for a folder.
int vfs_browser_item_file_index(HWND tree, HTREEITEM item);
// The entry under the double click being handled, or -1 when it was not on an entry's label.
int vfs_browser_dblclk_file_index(HWND tree);

// The search path a file came from, or its packfile's name.
std::string vfs_browser_source_text(const VfsBrowserSource& source);
// The filter box's text, lowercase as vfs_browser_rebuild_tree takes it.
std::string vfs_browser_filter_text(HWND edit);

// Size of a file wherever the engine finds it, loose or packed, or -1.
int vfs_browser_file_size(const std::string& file);
// Reads up to `max_bytes` of a file wherever the engine finds it, loose or packed. False unless all of that was read.
bool vfs_browser_read_file(const std::string& file, std::size_t max_bytes, std::vector<std::uint8_t>& out);

template<typename T>
T vfs_browser_read_le(const std::uint8_t* data, std::size_t offset)
{
    T value;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}
