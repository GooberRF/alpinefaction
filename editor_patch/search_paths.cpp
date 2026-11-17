#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>
#include <xlog/xlog.h>
#include <common/utils/os-utils.h>
#include <common/utils/string-utils.h>
#include "search_paths.h"
#include "vtypes.h"

namespace
{

constexpr int subdir_max_depth = 8;
constexpr size_t dir_max_len = 255;

// Append every subdirectory of `rel_dir` (exe-relative, no trailing backslash) to `out` as
// exe-relative paths without trailing backslash, recursing depth-first in sorted order.
void collect_subdirs_recursive(const std::string& exe_dir, const std::string& rel_dir, int depth,
                               const char* kind, std::vector<std::string>& out)
{
    if (depth >= subdir_max_depth) return;

    std::string search_pattern = exe_dir + rel_dir + "\\*";
    WIN32_FIND_DATAA find_data;
    HANDLE find_handle = FindFirstFileA(search_pattern.c_str(), &find_data);
    if (find_handle == INVALID_HANDLE_VALUE) return;

    std::vector<std::string> names;
    do {
        if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            continue;
        }
        // Reparse points can form cycles that would make this recursion unbounded
        if (find_data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            continue;
        }
        if (std::strcmp(find_data.cFileName, ".") == 0 || std::strcmp(find_data.cFileName, "..") == 0) {
            continue;
        }
        names.emplace_back(find_data.cFileName);
    } while (FindNextFileA(find_handle, &find_data));
    FindClose(find_handle);

    // Sorted so registration order (and therefore VFS lookup order) is deterministic
    std::sort(names.begin(), names.end());

    for (const auto& name : names) {
        std::string sub_rel = rel_dir + "\\" + name;
        // Skipping the subtree too: anything below it is longer still.
        if (sub_rel.size() > dir_max_len) {
            xlog::warn("Skipping {} directory (path too long): '{}'", kind, sub_rel);
            continue;
        }
        out.push_back(sub_rel);
        collect_subdirs_recursive(exe_dir, sub_rel, depth + 1, kind, out);
    }
}

// Every subdirectory (any depth) of the exe-relative root `root`
std::vector<std::string> enumerate_subdirs(const char* root, const char* kind)
{
    std::vector<std::string> subdirs;
    std::string exe_dir = get_module_dir(nullptr);
    if (exe_dir.empty()) return subdirs;
    collect_subdirs_recursive(exe_dir, root, 0, kind, subdirs);
    return subdirs;
}

} // namespace

// Register `rel_dir` with the VFS unless it is already registered, recording it for reload.
void SearchPathTree::register_dir(const std::string& rel_dir)
{
    // Record on first sight (whether or not registration then succeeds) so this dir is
    // skipped on every later reload. This is also what dedups the search_dirs_
    // push at each call site: a dir left out of this set would have its search entry
    // re-appended on every reload.
    if (!registered_dirs_.insert(rel_dir).second) return;

    // Every caller is already bounded, but this is the last point before the unchecked
    // strcpy inside file_add_path.
    if (rel_dir.size() > dir_max_len) {
        xlog::warn("Refusing to register {} path (too long): '{}'", kind_, rel_dir);
        return;
    }

    int slot = file_add_path(rel_dir.c_str(), extensions_, false);
    if (slot >= 0) {
        path_slots_.push_back(slot);
    }
    else {
        xlog::warn("Failed to register {} path '{}' (VFS path table full)", kind_, rel_dir);
    }
}

void SearchPathTree::init(std::initializer_list<const char*> register_roots,
                          std::initializer_list<const char*> search_roots)
{
    for (const char* root : register_roots) {
        register_dir(root);
    }

    std::vector<std::string> subdirs;
    std::string root_list;
    for (const char* root : search_roots) {
        search_roots_.emplace_back(root);
        search_dirs_.push_back(std::string{root} + "\\");
        std::vector<std::string> root_subdirs = enumerate_subdirs(root, kind_);
        subdirs.insert(subdirs.end(), root_subdirs.begin(), root_subdirs.end());
        root_list += root_list.empty() ? root : std::string{" and "} + root;
    }

    for (const auto& subdir : subdirs) {
        register_dir(subdir);
        search_dirs_.push_back(subdir + "\\");
    }

    xlog::info("Registered {} {} subdirectories under {}", subdirs.size(), kind_, root_list);
}

void SearchPathTree::reload()
{
    // Pick up subdirectories created since init; already registered ones must be skipped
    for (const auto& root : search_roots_) {
        for (const auto& subdir : enumerate_subdirs(root.c_str(), kind_)) {
            if (registered_dirs_.count(subdir)) continue;
            register_dir(subdir);
            search_dirs_.push_back(subdir + "\\");
            xlog::info("Registered new {} subdirectory '{}'", kind_, subdir);
        }
    }

    for (int slot : path_slots_) {
        file_scan_path(slot);
    }
}

bool SearchPathTree::has_extension(const char* filename) const
{
    const char* ext = filename ? std::strrchr(filename, '.') : nullptr;
    if (!ext) return false;
    for (std::string_view exts{extensions_}; !exts.empty();) {
        const std::size_t end = std::min(exts.find(' '), exts.size());
        if (string_iequals(exts.substr(0, end), ext)) return true;
        exts.remove_prefix(std::min(end + 1, exts.size()));
    }
    return false;
}

std::string SearchPathTree::find_on_disk(const char* filename) const
{
    if (!has_extension(filename)) return {};

    // Strip path prefix to get bare filename
    const char* bare = std::strrchr(filename, '\\');
    if (!bare) bare = std::strrchr(filename, '/');
    bare = bare ? bare + 1 : filename;
    if (!bare[0]) return {};

    std::string exe_dir = get_module_dir(nullptr);
    if (exe_dir.empty()) return {};

    for (const auto& search_dir : search_dirs_) {
        std::string full_path = exe_dir + search_dir + bare;
        if (GetFileAttributesA(full_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return full_path;
        }
    }
    return {};
}
