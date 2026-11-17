#pragma once

#include <initializer_list>
#include <string>
#include <unordered_set>
#include <vector>

// Exe-relative directory trees registered as VFS search paths for one extension list: each root and
// every subdirectory under it (any depth), each registered exactly once, rescanned by reload().
class SearchPathTree
{
public:
    // `kind` names the files in log lines ("mesh" logs "Registered new mesh subdirectory").
    SearchPathTree(const char* extensions, const char* kind) : extensions_{extensions}, kind_{kind} {}

    // Registers `register_roots` in that order, then the subdirectories of `search_roots`.
    // find_on_disk searches the `search_roots` themselves, in order, ahead of every subdirectory.
    void init(std::initializer_list<const char*> register_roots, std::initializer_list<const char*> search_roots);
    // Registers subdirectories created since init, then rescans every registered path.
    void reload();
    // Full path of the first search directory's file named like `filename` (any path prefix dropped),
    // or empty, also for a name without one of the tree's extensions.
    std::string find_on_disk(const char* filename) const;

private:
    void register_dir(const std::string& rel_dir);
    bool has_extension(const char* filename) const;

    const char* extensions_;
    const char* kind_;
    std::vector<std::string> search_roots_;
    // VFS slots returned by file_add_path, rescanned on reload
    std::vector<int> path_slots_;
    // Exe-relative directories (with trailing backslash) probed by find_on_disk, in priority order
    std::vector<std::string> search_dirs_;
    // Exe-relative directories (no trailing backslash) already passed to file_add_path.
    // file_add_path does not deduplicate: calling it again with an already registered path appends
    // the extension list to that slot's extensions instead of creating a new slot, growing it without
    // bound. Every path must therefore be registered exactly once.
    std::unordered_set<std::string> registered_dirs_;
};
