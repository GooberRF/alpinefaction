#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <string>
#include <vector>
#include <common/utils/string-utils.h>
#include "vfs_browser.h"
#include "vtypes.h"

namespace
{

constexpr std::size_t packfile_name_max = 60;

// Packfile directory names come from a fixed 60 byte record and search path strings are engine
// owned, so every read out of the file system tables is length bounded.
std::string bounded_string(const char* str, std::size_t max_len)
{
    if (!str) {
        return {};
    }
    std::size_t len = 0;
    while (len < max_len && str[len]) {
        ++len;
    }
    return std::string{str, len};
}

bool path_contains(const std::string& parent, const std::string& child)
{
    return child.size() > parent.size() + 1 && child[parent.size()] == '\\'
           && string_istarts_with(child, parent);
}

void sort_entries(std::vector<VfsBrowserFile>& entries)
{
    std::sort(entries.begin(), entries.end(), [](const VfsBrowserFile& a, const VfsBrowserFile& b) {
        const int order = _stricmp(a.name.c_str(), b.name.c_str());
        return order != 0 ? order < 0 : a.source < b.source;
    });
}

HTREEITEM insert_node(HWND tree, HTREEITEM parent, const char* label, LPARAM data)
{
    TVINSERTSTRUCT ins = {};
    ins.hParent = parent;
    ins.hInsertAfter = TVI_LAST;
    ins.item.mask = TVIF_TEXT | TVIF_PARAM;
    ins.item.pszText = const_cast<char*>(label);
    ins.item.lParam = data;
    return TreeView_InsertItem(tree, &ins);
}

void insert_source(HWND tree, HTREEITEM parent, const std::vector<VfsBrowserSource>& sources,
                   const std::vector<VfsBrowserFile>& entries,
                   const std::vector<std::vector<int>>& by_source,
                   const std::vector<std::vector<int>>& children,
                   const std::vector<bool>& visible_source, int source, bool expand,
                   const std::string& select_name, bool select_first, HTREEITEM& select_item,
                   int& select_index)
{
    if (!visible_source[source]) {
        return;
    }
    HTREEITEM node = insert_node(tree, parent, sources[source].label.c_str(), 0);
    if (!node) {
        return;
    }
    for (int child : children[source]) {
        insert_source(tree, node, sources, entries, by_source, children, visible_source, child, expand,
                      select_name, select_first, select_item, select_index);
    }
    for (int file : by_source[source]) {
        HTREEITEM leaf = insert_node(tree, node, entries[file].name.c_str(), file + 1);
        const bool wanted = select_name.empty()
                                ? select_first
                                : string_iequals(entries[file].name, select_name);
        if (leaf && !select_item && wanted) {
            select_item = leaf;
            select_index = file;
        }
    }
    if (expand) {
        TreeView_Expand(tree, node, TVE_EXPAND);
    }
}

} // namespace

void vfs_browser_collect(std::vector<VfsBrowserSource>& sources, const std::vector<std::vector<VfsBrowserFile>*>& lists,
                         const std::function<int(const std::string&)>& classify)
{
    const auto list_for = [&](const std::string& name) -> std::vector<VfsBrowserFile>* {
        const int index = classify(name);
        return (index >= 0 && index < static_cast<int>(lists.size())) ? lists[index] : nullptr;
    };

    std::vector<int> path_source(editor_vfs_path_count, -1);

    for (EditorVfsFile* node : vfs_file_buckets) {
        for (; node; node = node->next) {
            const int slot = node->path_index;
            if (slot < 0 || slot >= editor_vfs_path_count) {
                continue;
            }
            std::string name = bounded_string(node->name, MAX_PATH);
            std::vector<VfsBrowserFile>* into = list_for(name);
            if (!into) {
                continue;
            }
            int& source = path_source[slot];
            if (source < 0) {
                std::string path = bounded_string(vfs_paths[slot].path, MAX_PATH);
                source = static_cast<int>(sources.size());
                sources.push_back({path.empty() ? "(game folder)" : path, path, -1, false});
            }
            into->push_back({std::move(name), source});
        }
    }

    const int packfile_count = std::clamp(num_packfiles, 0, editor_packfile_max);
    for (int i = 0; i < packfile_count; ++i) {
        const EditorPackfile& pack = packfiles[i];
        if (!pack.entries) {
            continue;
        }
        const int entry_count = std::clamp(pack.num_entries, 0, editor_packfile_entry_max);
        int source = -1;
        for (int e = 0; e < entry_count; ++e) {
            std::string name = bounded_string(pack.entries[e].name, packfile_name_max);
            std::vector<VfsBrowserFile>* into = list_for(name);
            if (!into) {
                continue;
            }
            if (source < 0) {
                source = static_cast<int>(sources.size());
                sources.push_back({bounded_string(pack.name, sizeof(pack.name)), {}, -1, true});
            }
            into->push_back({std::move(name), source});
        }
    }

    // Search paths nest ("red\meshes" and "red\meshes\props" are separate slots), so the tree
    // parents each one under the longest other path it sits inside.
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (sources[i].packfile || sources[i].path.empty()) {
            continue;
        }
        std::size_t best_len = 0;
        for (std::size_t j = 0; j < sources.size(); ++j) {
            if (i == j || sources[j].packfile || sources[j].path.empty()) {
                continue;
            }
            if (path_contains(sources[j].path, sources[i].path) && sources[j].path.size() > best_len) {
                best_len = sources[j].path.size();
                sources[i].parent = static_cast<int>(j);
            }
        }
        if (sources[i].parent >= 0) {
            sources[i].label = sources[i].path.substr(best_len + 1);
        }
    }

    for (std::vector<VfsBrowserFile>* list : lists) {
        sort_entries(*list);
    }
}

int vfs_browser_rebuild_tree(HWND tree, const std::vector<VfsBrowserSource>& sources,
                             const std::vector<VfsBrowserFile>& entries, const std::vector<char>* allow,
                             const std::string& filter, const std::string& select_name, bool select_first,
                             int* select_index_out, bool& rebuilding)
{
    int visible_count = 0;
    std::vector<std::vector<int>> by_source(sources.size());
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (allow && !(*allow)[i]) {
            continue;
        }
        if (!filter.empty() && string_to_lower(entries[i].name).find(filter) == std::string::npos) {
            continue;
        }
        ++visible_count;
        by_source[entries[i].source].push_back(static_cast<int>(i));
    }

    std::vector<std::vector<int>> children(sources.size());
    std::vector<bool> visible_source(sources.size(), false);
    for (std::size_t i = 0; i < sources.size(); ++i) {
        visible_source[i] = !by_source[i].empty();
        if (sources[i].parent >= 0) {
            children[sources[i].parent].push_back(static_cast<int>(i));
        }
    }
    // A folder stays in the tree when anything below it survived the filter. Parents are
    // always earlier in path order than their children only by accident, so iterate until
    // the flag stops spreading.
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t i = 0; i < sources.size(); ++i) {
            const int parent = sources[i].parent;
            if (visible_source[i] && parent >= 0 && !visible_source[parent]) {
                visible_source[parent] = true;
                changed = true;
            }
        }
    }

    auto by_label = [&sources](int a, int b) {
        return _stricmp(sources[a].label.c_str(), sources[b].label.c_str()) < 0;
    };
    std::vector<int> roots;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        std::sort(children[i].begin(), children[i].end(), by_label);
        if (sources[i].parent < 0) {
            roots.push_back(static_cast<int>(i));
        }
    }
    std::sort(roots.begin(), roots.end(), [&](int a, int b) {
        if (sources[a].packfile != sources[b].packfile) {
            return !sources[a].packfile;
        }
        return by_label(a, b);
    });

    rebuilding = true;
    SendMessageA(tree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(tree);
    HTREEITEM select_item = nullptr;
    int select_index = -1;
    for (int root : roots) {
        insert_source(tree, nullptr, sources, entries, by_source, children, visible_source, root,
                      !filter.empty(), select_name, select_first, select_item, select_index);
    }
    SendMessageA(tree, WM_SETREDRAW, TRUE, 0);

    if (select_item) {
        TreeView_SelectItem(tree, select_item);
        TreeView_EnsureVisible(tree, select_item);
    }
    rebuilding = false;
    if (select_index_out) {
        *select_index_out = select_index;
    }
    return visible_count;
}

// Item data is the file index plus one so that zero marks a folder.
int vfs_browser_item_file_index(HWND tree, HTREEITEM item)
{
    if (!item) {
        return -1;
    }
    TVITEM tvi = {};
    tvi.mask = TVIF_PARAM;
    tvi.hItem = item;
    if (!TreeView_GetItem(tree, &tvi)) {
        return -1;
    }
    return static_cast<int>(tvi.lParam) - 1;
}

int vfs_browser_dblclk_file_index(HWND tree)
{
    TVHITTESTINFO hit{};
    if (!GetCursorPos(&hit.pt) || !ScreenToClient(tree, &hit.pt)) {
        return -1;
    }
    const HTREEITEM item = TreeView_HitTest(tree, &hit);
    return (hit.flags & TVHT_ONITEM) ? vfs_browser_item_file_index(tree, item) : -1;
}

std::string vfs_browser_source_text(const VfsBrowserSource& source)
{
    return source.path.empty() ? source.label : source.path;
}

std::string vfs_browser_filter_text(HWND edit)
{
    char filter_buf[128] = {};
    GetWindowTextA(edit, filter_buf, sizeof(filter_buf));
    return string_to_lower(filter_buf);
}

int vfs_browser_file_size(const std::string& file)
{
    rf::File handle;
    if (handle.open_mode(file.c_str()) != 0) {
        return -1;
    }
    const int size = handle.get_size();
    handle.close();
    return size;
}

bool vfs_browser_read_file(const std::string& file, std::size_t max_bytes, std::vector<std::uint8_t>& out)
{
    rf::File handle;
    if (handle.open_mode(file.c_str()) != 0) {
        return false;
    }
    const int size = handle.get_size();
    if (size <= 0) {
        handle.close();
        return false;
    }
    const std::size_t want = std::min(static_cast<std::size_t>(size), max_bytes);
    // rf::File has no destructor, so a throw here would strand an engine file slot
    try {
        out.resize(want);
    }
    catch (...) {
        handle.close();
        throw;
    }
    const int got = handle.read(out.data(), out.size());
    handle.close();
    return got == static_cast<int>(want);
}
