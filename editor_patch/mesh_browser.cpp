#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <xlog/xlog.h>
#include <patch_common/MemUtils.h>
#include <common/utils/string-utils.h>
#include "mesh_browser.h"
#include "mesh.h"
#include "mfc_types.h"
#include "resources.h"
#include "vtypes.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{

constexpr UINT_PTR preview_timer_id = 1;
constexpr UINT preview_timer_ms = 33;
constexpr float preview_fov = 60.0f;
constexpr float preview_spin_per_tick = 0.9f;
constexpr float preview_zoom_min = 0.15f;
constexpr float preview_zoom_max = 24.0f;
constexpr float preview_pitch_limit = 89.0f;
constexpr float pi = 3.14159265f;
// Packfile directory records store the name in a fixed 60 byte field.
constexpr std::size_t packfile_name_max = 60;
constexpr int preview_max_actions = 96;
constexpr int preview_owned_max_actions = 160;
static_assert(preview_max_actions < editor_character_max_actions);
static_assert(preview_owned_max_actions < editor_character_max_actions);
constexpr unsigned preview_max_anim_skeletons = 790;
static_assert(preview_max_anim_skeletons < editor_max_anim_skeletons);
// 0x004FFF90 copies both the name it is given and each name already in the table into 60 byte stack
// buffers that sit directly below its return address.
constexpr std::size_t anim_name_max = 59;
// An .rfa stores no bone names, only a per bone table indexed by the character's own bone index.
// 0x005002DB reads that table without a bounds check, so an animation with fewer bones than the
// character walks off it into keyframe data and dereferences the result.
constexpr std::uint32_t rfa_signature = 0x46564D56; // "VMVF"
constexpr int rfa_max_bones = 50;
constexpr std::size_t rfa_header_size = 0x50;
// Ceiling guard on how much of an animation is read for validation. This is larger than any
// animation is realistically going to ever be.
constexpr std::size_t rfa_read_max = 16u * 1024 * 1024;

struct BrowserSource
{
    std::string label;   // "red\meshes" for a search path, "meshes.vpp" for a packfile
    std::string path;    // search path as registered; empty for packfiles and the game root
    int parent;          // enclosing search path, -1 for a root or a packfile
    bool packfile;
};

struct BrowserFile
{
    std::string name;    // bare filename, exactly what the property field stores
    int source;
};

enum PreviewBackground
{
    preview_bg_editor,
    preview_bg_white,
    preview_bg_black,
};

// Kept across openings for the session only, as the dialog holds no settings of its own.
PreviewBackground g_preview_background = preview_bg_editor;

// Animations already proven playable on a mesh file, so a second look at one does not re-read it.
// Action indices are deliberately not cached: character_mesh_load_action returns the index of an
// action the character already carries and appends only otherwise, so the engine is the one safe
// source for one, where a cached index outlives the character a level unload frees.
std::map<std::string, std::set<std::string>> g_validated_actions;

// Bone count per animation file, 0 when unreadable. Session wide: the files do not change.
std::map<std::string, int> g_anim_bone_cache;

struct BrowserState
{
    std::vector<BrowserSource> sources;
    std::vector<BrowserFile> files;
    std::vector<BrowserFile> anims;
    unsigned kinds;
    bool anims_enabled;
    std::string initial;
    std::string initial_anim;
    std::string selected;
    std::string selected_anim;
    std::string result;
    std::string anim_result;
    bool anim_chosen;

    std::vector<char> anim_mask; // per animation: listed for the current character
    bool anim_filtered;
    int char_bones;
    int shown_meshes;
    int shown_anims;

    EditorVMesh* vmesh;
    void* owned_character;      // base character slot this preview brought in, if any
    bool load_failed;
    int anim_index;
    Vector3 bound_center;
    float bound_radius;
    float yaw;
    float pitch;
    float zoom;
    bool spinning;
    bool dragging;
    bool rebuilding;
    bool anim_pane_shown;
    POINT drag_pos;
    RECT tree_split;      // mesh tree as laid out beside the animation pane
    int tree_full_bottom; // mesh tree bottom with the animation pane hidden
};

// The preview draws in its own space rather than beside the editor camera: with the light list
// emptied and a fixed ambient, nothing about the result depends on where the level is.
const Vector3 preview_origin{0.0f, 0.0f, 0.0f};
const Color preview_ambient{180, 180, 180, 255};

// The subclassed preview keeps its own original proc in its window data rather than in a global.
WNDPROC preview_orig_wndproc(HWND ctl)
{
    return reinterpret_cast<WNDPROC>(GetWindowLongPtrA(ctl, GWLP_USERDATA));
}

LRESULT preview_default(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    WNDPROC orig = preview_orig_wndproc(hwnd);
    return orig ? CallWindowProcA(orig, hwnd, msg, wparam, lparam)
                : DefWindowProcA(hwnd, msg, wparam, lparam);
}

BrowserState* get_state(HWND hdlg)
{
    return reinterpret_cast<BrowserState*>(GetWindowLongPtrA(hdlg, GWLP_USERDATA));
}

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

enum EntryKind
{
    entry_none,
    entry_mesh,
    entry_anim,
};

EntryKind classify_name(const std::string& name, unsigned kinds, bool want_anims)
{
    const auto ext = get_ext_from_filename(name);
    if ((kinds & ALPINE_MESH_V3M) && string_iequals(ext, "v3m")) return entry_mesh;
    if ((kinds & ALPINE_MESH_V3C) && string_iequals(ext, "v3c")) return entry_mesh;
    if ((kinds & ALPINE_MESH_VFX) && string_iequals(ext, "vfx")) return entry_mesh;
    if (want_anims && (string_iequals(ext, "rfa") || string_iequals(ext, "mvf"))) return entry_anim;
    return entry_none;
}

const char* mesh_type_label(const std::string& name)
{
    const auto ext = get_ext_from_filename(name);
    if (string_iequals(ext, "v3m")) return "Static mesh (.v3m)";
    if (string_iequals(ext, "v3c")) return "Skeletal mesh (.v3c)";
    if (string_iequals(ext, "vfx")) return "Animated mesh (.vfx)";
    return "";
}

bool is_v3c(const std::string& name)
{
    return !name.empty() && string_iequals(get_ext_from_filename(name), "v3c");
}

// The animation loader (0x00500D30) cuts the name at its FIRST dot (strchr) and appends ".rfa", so
// the name validated here has to be built the same way or a multi dot name validates one file and
// loads another.
std::string anim_load_name(const std::string& name)
{
    return name.substr(0, name.find('.')) + ".rfa";
}

bool anim_name_fits(const std::string& name)
{
    return name.size() <= anim_name_max && anim_load_name(name).size() <= anim_name_max;
}

template<typename T>
T read_le(const std::uint8_t* data, std::size_t offset)
{
    T value;
    std::memcpy(&value, data + offset, sizeof(value));
    return value;
}

bool read_anim_file(const std::string& file, std::size_t max_bytes, std::vector<std::uint8_t>& out)
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

int rfa_header_bone_count(const std::string& file)
{
    std::vector<std::uint8_t> buf;
    if (!read_anim_file(file, rfa_header_size, buf) || buf.size() < rfa_header_size) {
        return 0;
    }
    if (read_le<std::uint32_t>(buf.data(), 0) != rfa_signature) {
        return 0;
    }
    const int version = read_le<std::int32_t>(buf.data(), 4);
    if (version != 7 && version != 8) {
        return 0;
    }
    const int num_bones = read_le<std::int32_t>(buf.data(), 0x18);
    return (num_bones >= 1 && num_bones <= rfa_max_bones) ? num_bones : 0;
}

int anim_bone_count(const std::string& anim_name)
{
    const std::string file = anim_load_name(anim_name);
    const std::string key = string_to_lower(file);
    const auto it = g_anim_bone_cache.find(key);
    if (it != g_anim_bone_cache.end()) {
        return it->second;
    }
    const int bones = rfa_header_bone_count(file);
    g_anim_bone_cache.emplace(key, bones);
    return bones;
}

struct CharacterLimits
{
    bool character;     // the vmesh is a loaded .v3c
    int bones;          // 0 when the character's own bone count is outside what the engine poses
    int morph_capacity; // exclusive upper bound on an animation's morph vertex indices
};

// The capacity LOD0 offers: morph indices are applied by 0x00500720 as `chunk->orig_map[index]`
// when the LOD carries a map, and straight into the per chunk vertex scratch buffer at 0x017202E0
// when it does not. 0 means indeterminable.
int lod_morph_capacity(const EditorVifMesh* mesh)
{
    if (mesh->flags & VIF_LOD_MORPH_VERTICES_MAP) {
        return std::max(mesh->num_original_vecs, 0);
    }
    if (mesh->chunks && mesh->num_chunks > 0) {
        int smallest = mesh->chunks[0].num_vecs;
        for (unsigned i = 1; i < mesh->num_chunks; ++i) {
            smallest = std::min<int>(smallest, mesh->chunks[i].num_vecs);
        }
        return smallest;
    }
    return 0;
}

// Morph apply only ever runs on LOD0.
CharacterLimits character_limits(EditorVMesh* vmesh)
{
    CharacterLimits limits{};
    if (!vmesh || vmesh_get_type(vmesh) != VMESH_TYPE_CHARACTER || !vmesh->mesh) {
        return limits;
    }
    limits.character = true;
    const auto* character = static_cast<const EditorCharacter*>(vmesh->mesh);
    const int bones = character->num_bones;
    limits.bones = (bones >= 1 && bones <= rfa_max_bones) ? bones : 0;

    const int mesh_count =
        std::clamp<int>(character->num_character_meshes, 0,
                        static_cast<int>(std::size(character->character_meshes)));
    bool any_mesh = false;
    int capacity = 0;
    for (int m = 0; m < mesh_count; ++m) {
        const EditorV3dMesh* v3d_mesh = character->character_meshes[m].mesh;
        const EditorVifLodMesh* lod_mesh = v3d_mesh ? v3d_mesh->lod_mesh : nullptr;
        if (!lod_mesh || lod_mesh->num_levels < 1 || !lod_mesh->meshes[0]) {
            continue;
        }
        const int mesh_capacity = lod_morph_capacity(lod_mesh->meshes[0]);
        capacity = any_mesh ? std::min(capacity, mesh_capacity) : mesh_capacity;
        any_mesh = true;
    }
    limits.morph_capacity = any_mesh ? capacity : 0;
    return limits;
}

bool rfa_is_playable(const std::string& file, const CharacterLimits& limits)
{
    // The pose path indexes an animation's per bone table with the character's own bone index and
    // its capacity is exactly 50, which 0x004C1F90 does not check the loaded count against, so a
    // character whose count falls outside that range can be given no animation at all.
    if (limits.character && limits.bones <= 0) {
        return false;
    }
    std::vector<std::uint8_t> buf;
    if (!read_anim_file(file, rfa_read_max, buf) || buf.size() < rfa_header_size) {
        return false;
    }
    const std::uint8_t* data = buf.data();
    const std::uint64_t size = buf.size();
    const auto fits = [size](std::int32_t offset, std::uint64_t len) {
        return offset >= 0 && static_cast<std::uint64_t>(offset) <= size &&
               len <= size - static_cast<std::uint64_t>(offset);
    };

    if (read_le<std::uint32_t>(data, 0) != rfa_signature) {
        return false;
    }
    const int version = read_le<std::int32_t>(data, 4);
    if (version != 7 && version != 8) {
        return false;
    }
    const int num_bones = read_le<std::int32_t>(data, 0x18);
    const int num_morph_verts = read_le<std::int32_t>(data, 0x1C);
    const int num_morph_keys = read_le<std::int32_t>(data, 0x20);
    if (num_bones < 1 || num_bones > rfa_max_bones) {
        return false;
    }
    if (limits.bones > 0 && num_bones < limits.bones) {
        return false;
    }
    if (!fits(static_cast<std::int32_t>(rfa_header_size), 4ull * num_bones)) {
        return false;
    }
    for (int i = 0; i < num_bones; ++i) {
        const auto offset = read_le<std::int32_t>(data, rfa_header_size + 4 * i);
        if (!fits(offset, 8)) {
            return false;
        }
        const int rot_keys = read_le<std::int16_t>(data, static_cast<std::size_t>(offset) + 4);
        const int pos_keys = read_le<std::int16_t>(data, static_cast<std::size_t>(offset) + 6);
        if (rot_keys < 0 || pos_keys < 0) {
            return false;
        }
        if (!fits(offset, 8ull + 16ull * rot_keys + 40ull * pos_keys)) {
            return false;
        }
    }
    if (num_morph_verts > 0) {
        // Counts are capped before they are multiplied out, and the morph index is a 16 bit field.
        if (num_morph_verts > 0xffff || num_morph_keys < 1 || num_morph_keys > 0xffff) {
            return false;
        }
        const auto map_offset = read_le<std::int32_t>(data, 0x48);
        const auto data_offset = read_le<std::int32_t>(data, 0x4C);
        const std::uint64_t frames =
            (version >= 8) ? 4ull * num_morph_keys + 0x18 + 3ull * num_morph_keys * num_morph_verts
                           : 12ull * num_morph_keys * num_morph_verts;
        if (!fits(map_offset, 2ull * num_morph_verts) || !fits(data_offset, frames)) {
            return false;
        }
        // 0x005007E8 zero extends the stored index, so the sign bit is no escape from the bound.
        int highest = -1;
        for (int i = 0; i < num_morph_verts; ++i) {
            const int index =
                read_le<std::uint16_t>(data, static_cast<std::size_t>(map_offset) + 2 * i);
            highest = std::max(highest, index);
        }
        if (highest >= limits.morph_capacity) {
            return false;
        }
    }
    return true;
}

bool path_contains(const std::string& parent, const std::string& child)
{
    return child.size() > parent.size() + 1 && child[parent.size()] == '\\' &&
           string_istarts_with(child, parent);
}

void sort_entries(std::vector<BrowserFile>& entries)
{
    std::sort(entries.begin(), entries.end(), [](const BrowserFile& a, const BrowserFile& b) {
        const int order = _stricmp(a.name.c_str(), b.name.c_str());
        return order != 0 ? order < 0 : a.source < b.source;
    });
}

void collect_entries(BrowserState& st)
{
    std::vector<int> path_source(editor_vfs_path_count, -1);

    for (EditorVfsFile* node : vfs_file_buckets) {
        for (; node; node = node->next) {
            const int slot = node->path_index;
            if (slot < 0 || slot >= editor_vfs_path_count) {
                continue;
            }
            std::string name = bounded_string(node->name, MAX_PATH);
            const EntryKind kind = classify_name(name, st.kinds, st.anims_enabled);
            if (kind == entry_none) {
                continue;
            }
            int& source = path_source[slot];
            if (source < 0) {
                std::string path = bounded_string(vfs_paths[slot].path, MAX_PATH);
                source = static_cast<int>(st.sources.size());
                st.sources.push_back({path.empty() ? "(game folder)" : path, path, -1, false});
            }
            auto& into = (kind == entry_mesh) ? st.files : st.anims;
            into.push_back({std::move(name), source});
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
            const EntryKind kind = classify_name(name, st.kinds, st.anims_enabled);
            if (kind == entry_none) {
                continue;
            }
            if (source < 0) {
                source = static_cast<int>(st.sources.size());
                st.sources.push_back({bounded_string(pack.name, sizeof(pack.name)), {}, -1, true});
            }
            auto& into = (kind == entry_mesh) ? st.files : st.anims;
            into.push_back({std::move(name), source});
        }
    }

    // Search paths nest ("red\meshes" and "red\meshes\props" are separate slots), so the tree
    // parents each one under the longest other path it sits inside.
    for (std::size_t i = 0; i < st.sources.size(); ++i) {
        if (st.sources[i].packfile || st.sources[i].path.empty()) {
            continue;
        }
        std::size_t best_len = 0;
        for (std::size_t j = 0; j < st.sources.size(); ++j) {
            if (i == j || st.sources[j].packfile || st.sources[j].path.empty()) {
                continue;
            }
            if (path_contains(st.sources[j].path, st.sources[i].path) &&
                st.sources[j].path.size() > best_len) {
                best_len = st.sources[j].path.size();
                st.sources[i].parent = static_cast<int>(j);
            }
        }
        if (st.sources[i].parent >= 0) {
            st.sources[i].label = st.sources[i].path.substr(best_len + 1);
        }
    }

    sort_entries(st.files);
    sort_entries(st.anims);
}

// ─── Tree ────────────────────────────────────────────────────────────────────

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

// Item data is the file index plus one so that zero marks a folder.
int item_file_index(HWND tree, HTREEITEM item)
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

void insert_source(HWND tree, HTREEITEM parent, const BrowserState& st,
                   const std::vector<BrowserFile>& entries,
                   const std::vector<std::vector<int>>& by_source,
                   const std::vector<std::vector<int>>& children,
                   const std::vector<bool>& visible_source, int source, bool expand,
                   const std::string& select_name, bool select_first, HTREEITEM& select_item,
                   int& select_index)
{
    if (!visible_source[source]) {
        return;
    }
    HTREEITEM node = insert_node(tree, parent, st.sources[source].label.c_str(), 0);
    if (!node) {
        return;
    }
    for (int child : children[source]) {
        insert_source(tree, node, st, entries, by_source, children, visible_source, child, expand,
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

int rebuild_tree(HWND hdlg, BrowserState& st, int tree_id, const std::vector<BrowserFile>& entries,
                 const std::vector<char>* allow, const std::string& filter,
                 const std::string& select_name, bool select_first = false,
                 int* select_index_out = nullptr)
{
    HWND tree = GetDlgItem(hdlg, tree_id);
    if (!tree) {
        return 0;
    }

    int visible_count = 0;
    std::vector<std::vector<int>> by_source(st.sources.size());
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

    std::vector<std::vector<int>> children(st.sources.size());
    std::vector<bool> visible_source(st.sources.size(), false);
    for (std::size_t i = 0; i < st.sources.size(); ++i) {
        visible_source[i] = !by_source[i].empty();
        if (st.sources[i].parent >= 0) {
            children[st.sources[i].parent].push_back(static_cast<int>(i));
        }
    }
    // A folder stays in the tree when anything below it survived the filter. Parents are
    // always earlier in path order than their children only by accident, so iterate until
    // the flag stops spreading.
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t i = 0; i < st.sources.size(); ++i) {
            const int parent = st.sources[i].parent;
            if (visible_source[i] && parent >= 0 && !visible_source[parent]) {
                visible_source[parent] = true;
                changed = true;
            }
        }
    }

    auto by_label = [&st](int a, int b) {
        return _stricmp(st.sources[a].label.c_str(), st.sources[b].label.c_str()) < 0;
    };
    std::vector<int> roots;
    for (std::size_t i = 0; i < st.sources.size(); ++i) {
        std::sort(children[i].begin(), children[i].end(), by_label);
        if (st.sources[i].parent < 0) {
            roots.push_back(static_cast<int>(i));
        }
    }
    std::sort(roots.begin(), roots.end(), [&](int a, int b) {
        if (st.sources[a].packfile != st.sources[b].packfile) {
            return !st.sources[a].packfile;
        }
        return by_label(a, b);
    });

    // Deleting and reselecting items notifies the dialog; the selection handler must not act
    // on those intermediate states.
    st.rebuilding = true;
    SendMessageA(tree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(tree);
    HTREEITEM select_item = nullptr;
    int select_index = -1;
    for (int root : roots) {
        insert_source(tree, nullptr, st, entries, by_source, children, visible_source, root,
                      !filter.empty(), select_name, select_first, select_item, select_index);
    }
    SendMessageA(tree, WM_SETREDRAW, TRUE, 0);

    if (select_item) {
        TreeView_SelectItem(tree, select_item);
        TreeView_EnsureVisible(tree, select_item);
    }
    st.rebuilding = false;
    if (select_index_out) {
        *select_index_out = select_index;
    }
    return visible_count;
}

std::string current_filter(HWND hdlg)
{
    char filter_buf[128] = {};
    GetDlgItemTextA(hdlg, IDC_MESH_BROWSER_FILTER, filter_buf, sizeof(filter_buf));
    return string_to_lower(filter_buf);
}

// The filter box spans the mesh tree only, so the animation count is a plain total.
void update_counts(HWND hdlg, const BrowserState& st)
{
    char count_buf[128];
    if (st.anims_enabled) {
        std::snprintf(count_buf, sizeof(count_buf), "%d of %d meshes, %d %sanimations",
                      st.shown_meshes, static_cast<int>(st.files.size()), st.shown_anims,
                      st.anim_filtered ? "compatible " : "");
    }
    else {
        std::snprintf(count_buf, sizeof(count_buf), "%d of %d meshes", st.shown_meshes,
                      static_cast<int>(st.files.size()));
    }
    SetDlgItemTextA(hdlg, IDC_MESH_BROWSER_COUNT, count_buf);
}

// Animations whose bone count differs from the character's are left out of the tree. When nothing
// at all matches, the list stays whole so an unusual mesh is still browsable; the play guard is
// what keeps the incompatible ones from reaching the engine.
void build_anim_mask(BrowserState& st)
{
    st.anim_mask.assign(st.anims.size(), 1);
    st.anim_filtered = false;
    if (st.char_bones <= 0) {
        return;
    }
    int matches = 0;
    for (std::size_t i = 0; i < st.anims.size(); ++i) {
        const bool ok = anim_bone_count(st.anims[i].name) == st.char_bones;
        st.anim_mask[i] = ok ? 1 : 0;
        matches += ok ? 1 : 0;
    }
    if (matches == 0) {
        st.anim_mask.assign(st.anims.size(), 1);
        return;
    }
    st.anim_filtered = true;
}

bool anim_allowed(const BrowserState& st, const std::string& name)
{
    return !st.anim_filtered || anim_bone_count(name) == st.char_bones;
}

// With nothing selected and a compatible list to draw from, the first entry is taken so the
// preview starts animated; that counts as the user's pick unless they choose another.
void rebuild_anim_tree(HWND hdlg, BrowserState& st, const std::string& select_anim,
                       bool select_first = false)
{
    if (!st.anims_enabled) {
        return;
    }
    int selected = -1;
    st.shown_anims = rebuild_tree(hdlg, st, IDC_MESH_BROWSER_ANIM_TREE, st.anims, &st.anim_mask, {},
                                  select_anim, select_first, &selected);
    if (select_first && selected >= 0) {
        st.selected_anim = st.anims[selected].name;
        st.anim_chosen = true;
    }
    update_counts(hdlg, st);
}

void rebuild_mesh_tree(HWND hdlg, BrowserState& st, const std::string& select_mesh)
{
    st.shown_meshes = rebuild_tree(hdlg, st, IDC_MESH_BROWSER_TREE, st.files, nullptr,
                                   current_filter(hdlg), select_mesh);
    update_counts(hdlg, st);
}

void update_anim_pane(HWND hdlg, BrowserState& st)
{
    const bool show = st.anims_enabled && is_v3c(st.selected);
    if (show == st.anim_pane_shown) {
        return;
    }
    HWND tree = GetDlgItem(hdlg, IDC_MESH_BROWSER_TREE);
    HWND label = GetDlgItem(hdlg, IDC_MESH_BROWSER_ANIM_LABEL);
    HWND anim_tree = GetDlgItem(hdlg, IDC_MESH_BROWSER_ANIM_TREE);
    if (!tree || !label || !anim_tree) {
        return;
    }
    st.anim_pane_shown = show;
    HWND focus = GetFocus();
    if (!show && (focus == anim_tree || IsChild(anim_tree, focus))) {
        SetFocus(tree);
    }
    ShowWindow(label, show ? SW_SHOW : SW_HIDE);
    ShowWindow(anim_tree, show ? SW_SHOW : SW_HIDE);
    MoveWindow(tree, st.tree_split.left, st.tree_split.top,
               st.tree_split.right - st.tree_split.left,
               (show ? st.tree_split.bottom : st.tree_full_bottom) - st.tree_split.top, TRUE);
}

// ─── Preview ─────────────────────────────────────────────────────────────────

Vector3 vec_scale(const Vector3& v, float s)
{
    return {v.x * s, v.y * s, v.z * s};
}

void preview_free(BrowserState& st)
{
    if (st.vmesh) {
        vmesh_free(st.vmesh);
        st.vmesh = nullptr;
    }
    if (st.owned_character) {
        character_free(st.owned_character);
        st.owned_character = nullptr;
    }
    st.anim_index = -1;
}

// A .v3c's base character stays in a table of 64 slots that nothing frees before the editor's own
// atexit handler runs, so browsing characters ends in the fatal "No more base character room" at
// the 64th distinct one. Take ownership of the slot this load brings in and release it on the next
// swap; one that was already resident belongs to whatever loaded it.
EditorVMesh* preview_load_vmesh(BrowserState& st, const std::string& name)
{
    const std::uint64_t before = editor_base_characters_in_use();
    EditorVMesh* vmesh = mesh_load_vmesh_file(name.c_str());
    const std::uint64_t claimed = editor_base_characters_in_use() & ~before;
    if (!claimed) {
        return vmesh;
    }
    if (vmesh && vmesh->mesh && vmesh_get_type(vmesh) == VMESH_TYPE_CHARACTER) {
        st.owned_character = vmesh->mesh;
        return vmesh;
    }
    // The load claimed a slot and then failed, so nothing is left holding it.
    for (unsigned i = 0; i < editor_max_base_characters; ++i) {
        if (claimed & (1ull << i)) {
            character_free(&editor_base_characters[i]);
        }
    }
    return vmesh;
}

// Nothing removes an entry from a character's action list, so browsing distinct animations on one
// mesh eventually fills it.
bool preview_recycle_character(BrowserState& st)
{
    if (!st.owned_character || st.selected.empty()) {
        return false;
    }
    preview_free(st);
    st.vmesh = preview_load_vmesh(st, st.selected);
    st.load_failed = (st.vmesh == nullptr);
    // The caller carries on against the reloaded character, so it has to be as usable as the one
    // the entry check accepted.
    return st.vmesh && st.vmesh->mesh && st.vmesh->instance &&
           vmesh_get_type(st.vmesh) == VMESH_TYPE_CHARACTER;
}

// character_mesh_load_action keys its dedup on the skeleton a name resolves to, so an action the
// character already carries is recognised the way the engine recognises it: through the same name
// keyed table 0x004FFF90 searches, with the extension stripped from both sides.
const void* find_anim_skeleton(const std::string& file)
{
    const auto stem = [](const char* text, std::size_t len) {
        for (std::size_t i = len; i > 0; --i) {
            if (text[i - 1] == '.') {
                return i - 1;
            }
        }
        return len;
    };
    const std::size_t key_len = stem(file.c_str(), file.size());
    for (const EditorAnimSkeleton& skeleton : editor_anim_skeletons) {
        const std::size_t name_len = strnlen(skeleton.name, sizeof(skeleton.name));
        if (name_len == 0) {
            continue;
        }
        const std::size_t len = stem(skeleton.name, name_len);
        if (len == key_len && _strnicmp(skeleton.name, file.c_str(), len) == 0) {
            return &skeleton;
        }
    }
    return nullptr;
}

unsigned anim_skeletons_in_use()
{
    unsigned used = 0;
    for (const EditorAnimSkeleton& skeleton : editor_anim_skeletons) {
        if (skeleton.name[0] != '\0') {
            ++used;
        }
    }
    return used;
}

bool character_carries_action(const EditorCharacter* character, const void* skeleton,
                              std::uint8_t is_state)
{
    if (!skeleton) {
        return false;
    }
    const int count = std::clamp(character->num_actions, 0, editor_character_max_actions);
    for (int i = 0; i < count; ++i) {
        if (character->actions[i] == skeleton && character->action_is_state[i] == is_state) {
            return true;
        }
    }
    return false;
}

enum ActionLoadResult
{
    action_loaded,
    action_incompatible, // the file cannot be played on this mesh
    action_no_room,      // the mesh has no room for another animation
};

ActionLoadResult preview_load_action(BrowserState& st, const std::string& anim_name, int& index_out)
{
    index_out = -1;
    if (!st.vmesh || vmesh_get_type(st.vmesh) != VMESH_TYPE_CHARACTER || !st.vmesh->mesh ||
        !st.vmesh->instance) {
        return action_incompatible;
    }
    if (!anim_name_fits(anim_name)) {
        return action_incompatible;
    }
    const std::string file = anim_load_name(anim_name);
    auto& validated = g_validated_actions[string_to_lower(st.selected)];
    const std::string file_key = string_to_lower(file);
    if (validated.find(file_key) == validated.end()) {
        // The verdict is about the two files and survives a recycle, which changes neither.
        if (!rfa_is_playable(file, character_limits(st.vmesh))) {
            return action_incompatible;
        }
        validated.insert(file_key);
    }
    // An action the character already carries appends nothing and claims no skeleton, so both
    // ceilings bind only on a load that would grow something. The second pass runs on the character
    // a recycle brought in, whose list and skeletons are both fresh.
    for (int pass = 0; pass < 2; ++pass) {
        const auto* character = static_cast<const EditorCharacter*>(st.vmesh->mesh);
        const void* skeleton = find_anim_skeleton(file);
        if (!character_carries_action(character, skeleton, 0)) {
            if (!skeleton && anim_skeletons_in_use() >= preview_max_anim_skeletons) {
                return action_no_room;
            }
            const int ceiling =
                st.owned_character ? preview_owned_max_actions : preview_max_actions;
            if (character->num_actions >= ceiling) {
                if (pass == 0 && preview_recycle_character(st)) {
                    continue;
                }
                return action_no_room;
            }
        }
        const int index = character_mesh_load_action(st.vmesh->mesh, file.c_str(), 0, 0);
        if (index < 0) {
            return action_incompatible;
        }
        index_out = index;
        return action_loaded;
    }
    return action_no_room;
}

void preview_play_anim(HWND hdlg, BrowserState& st, const std::string& anim_name)
{
    st.anim_index = -1;
    // Only a loaded character can answer for an animation; against anything else the selection is
    // just recorded and replayed once a .v3c is previewed.
    if (st.vmesh && vmesh_get_type(st.vmesh) == VMESH_TYPE_CHARACTER) {
        vmesh_stop_all_actions(st.vmesh);
        ActionLoadResult result = action_loaded;
        if (!anim_name.empty()) {
            int index = -1;
            result = preview_load_action(st, anim_name, index);
            st.anim_index = index;
        }
        if (st.anim_index >= 0) {
            mesh_play_v3c_action_looping(st.vmesh, st.anim_index);
            vmesh_process(st.vmesh, 0.01f, 0, &preview_origin, &identity_orient, 1);
            SetDlgItemTextA(hdlg, IDC_MESH_BROWSER_STATUS, "");
        }
        else if (!anim_name.empty()) {
            st.anim_chosen = false;
            const char* status = "Animation is not compatible with this mesh";
            if (st.load_failed) {
                status = "Preview unavailable (mesh failed to load)";
            }
            else if (result == action_no_room) {
                status = "Animation limit reached for this mesh";
                xlog::warn("Mesh browser: '{}' carries as many animations as it can hold; nothing releases them before the editor exits", st.selected);
            }
            SetDlgItemTextA(hdlg, IDC_MESH_BROWSER_STATUS, status);
        }
    }
    SetDlgItemTextA(hdlg, IDC_MESH_BROWSER_ANIM,
                    (st.anim_index >= 0) ? anim_name.c_str() : "");
    if (HWND preview = GetDlgItem(hdlg, IDC_MESH_BROWSER_PREVIEW)) {
        InvalidateRect(preview, nullptr, FALSE);
    }
}

void preview_load(HWND hdlg, BrowserState& st, const std::string& name)
{
    preview_free(st);
    st.load_failed = false;
    st.bound_center = {};
    st.bound_radius = 1.0f;
    st.yaw = 30.0f;
    st.pitch = 15.0f;
    st.zoom = 2.6f;
    st.spinning = true;
    st.dragging = false;

    if (!name.empty()) {
        st.vmesh = preview_load_vmesh(st, name);
        st.load_failed = (st.vmesh == nullptr);
    }
    if (st.vmesh) {
        vmesh_get_bound_sphere(st.vmesh, &st.bound_center, &st.bound_radius);
        if (!(st.bound_radius > 0.01f)) {
            st.bound_radius = 0.5f;
        }
    }

    if (st.anims_enabled) {
        const int bones = character_limits(st.vmesh).bones;
        if (bones != st.char_bones) {
            st.char_bones = bones;
            build_anim_mask(st);
            if (!st.selected_anim.empty() && !anim_allowed(st, st.selected_anim)) {
                st.selected_anim.clear();
                st.anim_chosen = false;
            }
            rebuild_anim_tree(hdlg, st, st.selected_anim,
                              st.anim_filtered && st.selected_anim.empty());
        }
    }

    SetDlgItemTextA(hdlg, IDC_MESH_BROWSER_NAME, name.c_str());
    SetDlgItemTextA(hdlg, IDC_MESH_BROWSER_TYPE, name.empty() ? "" : mesh_type_label(name));
    SetDlgItemTextA(hdlg, IDC_MESH_BROWSER_STATUS,
                    st.load_failed ? "Preview unavailable (mesh failed to load)" : "");
    SetDlgItemTextA(hdlg, IDC_MESH_BROWSER_ANIM, "");

    if (st.anims_enabled && is_v3c(name) && !st.selected_anim.empty()) {
        preview_play_anim(hdlg, st, st.selected_anim);
    }
    else if (HWND preview = GetDlgItem(hdlg, IDC_MESH_BROWSER_PREVIEW)) {
        InvalidateRect(preview, nullptr, FALSE);
    }
}

void preview_draw_mesh(BrowserState& st)
{
    const float yaw = st.yaw * pi / 180.0f;
    const float pitch = st.pitch * pi / 180.0f;
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);

    const Vector3 to_camera{cp * sy, sp, cp * cy};
    const Vector3 camera_pos = st.bound_center + vec_scale(to_camera, st.bound_radius * st.zoom);
    // Basis taken straight from the two angles: right stays level and unit length for every
    // pitch, where a look-at built against a fixed world up rolls over as forward nears vertical.
    const Matrix3 camera_orient{{-cy, 0.0f, sy},
                                {-sp * sy, cp, -sp * cy},
                                {-cp * sy, -sp, -cp * cy}};
    gr_setup_3d(&camera_orient, &camera_pos, preview_fov, true, true);

    set_draw_color(0xff, 0xff, 0xff, 0xff);
    EditorRenderParams params = editor_mesh_render_params();
    params.flags |= ERF_CUSTOM_AMBIENT;
    params.ambient_color = preview_ambient;

    const bool anim_fx = vmesh_get_type(st.vmesh) == VMESH_TYPE_ANIM_FX;
    if (anim_fx) {
        vfx_render_transparent = 1;
    }
    // Empties the light list the viewport left behind, so the one dynamic light 0x00505920 would
    // otherwise pick up cannot reach the preview.
    room_cleanup();
    vmesh_render(st.vmesh, &preview_origin, &identity_orient, &params);
    if (anim_fx) {
        vfx_render_transparent = 0;
    }
}

Color preview_background_color()
{
    if (g_preview_background == preview_bg_white) {
        return {0xff, 0xff, 0xff, 0xff};
    }
    if (g_preview_background == preview_bg_black) {
        return {0x00, 0x00, 0x00, 0xff};
    }
    // Same source the viewport painter uses.
    if (const EditorColorPrefs* prefs = editor_color_prefs()) {
        const COLORREF color = prefs->background;
        return {GetRValue(color), GetGValue(color), GetBValue(color), 0xff};
    }
    return {0x00, 0x00, 0x00, 0xff};
}

void preview_draw(HWND ctrl, BrowserState& st)
{
    RECT rc;
    GetClientRect(ctrl, &rc);
    const int w = std::min<int>(rc.right - rc.left, gr_get_max_width());
    const int h = std::min<int>(rc.bottom - rc.top, gr_get_max_height());
    if (w <= 0 || h <= 0) {
        return;
    }

    const Color background = preview_background_color();
    // The viewport painter clears before it sets its own colour, so leave the global as found.
    const auto saved_color = static_cast<unsigned>(red::gr_screen.current_color);
    // A level with a short far clip would otherwise cut the preview and squeeze its depth range.
    const float saved_far = gr_far_clip_dist;
    gr_set_far_clip(0.0f);

    gr_set_viewport_wnd(ctrl);
    // Two draw passes then one flip, as CBitmapPreviewDialog::OnPaint (0x0044C1B0) does.
    for (int pass = 0; pass < 2; ++pass) {
        gr_set_clip(0, 0, w, h);
        set_draw_color(background.r, background.g, background.b, background.a);
        gr_clear();
        if (st.vmesh) {
            preview_draw_mesh(st);
        }
    }
    gr_flip();
    gr_set_far_clip(saved_far);
    set_draw_color(saved_color & 0xff, (saved_color >> 8) & 0xff, (saved_color >> 16) & 0xff,
                   (saved_color >> 24) & 0xff);
}

LRESULT preview_surface_message(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    HWND hdlg = GetParent(hwnd);
    BrowserState* st = hdlg ? get_state(hdlg) : nullptr;
    if (!st) {
        return preview_default(hwnd, msg, wparam, lparam);
    }

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_SETCURSOR:
        SetCursor(LoadCursorA(nullptr, IDC_SIZEALL));
        return TRUE;
    case WM_LBUTTONDOWN:
        st->dragging = true;
        st->spinning = false;
        st->drag_pos = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        SetCapture(hwnd);
        // A tree keeps WM_MOUSEWHEEL for its own scrolling, so the wheel only reaches the dialog
        // while the focus is somewhere that passes it on. Hovering must not steal focus from the
        // trees, hence only on a drag.
        SetFocus(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        if (st->dragging) {
            const POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            st->yaw += static_cast<float>(pt.x - st->drag_pos.x) * 0.5f;
            st->pitch = std::clamp(st->pitch + static_cast<float>(pt.y - st->drag_pos.y) * 0.5f,
                                   -preview_pitch_limit, preview_pitch_limit);
            st->drag_pos = pt;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (st->dragging) {
            st->dragging = false;
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        st->dragging = false;
        break;
    case WM_NCDESTROY:
        if (WNDPROC orig = preview_orig_wndproc(hwnd)) {
            SetWindowLongPtrA(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig));
        }
        break;
    default:
        break;
    }
    return preview_default(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK PreviewSurfaceProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    try {
        return preview_surface_message(hwnd, msg, wparam, lparam);
    }
    catch (...) {
        xlog::error("Mesh browser: preview surface message {:#x} failed", msg);
        return 0;
    }
}

void subclass_preview(HWND hdlg)
{
    HWND ctrl = GetDlgItem(hdlg, IDC_MESH_BROWSER_PREVIEW);
    if (!ctrl) {
        return;
    }
    WNDPROC prev = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(ctrl, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(PreviewSurfaceProc)));
    if (prev != PreviewSurfaceProc) {
        SetWindowLongPtrA(ctrl, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(prev));
    }
}

// ─── Dialog ──────────────────────────────────────────────────────────────────

void accept_selection(HWND hdlg, BrowserState& st)
{
    if (st.selected.empty()) {
        return;
    }
    st.result = st.selected;
    if (st.anims_enabled && st.anim_chosen && is_v3c(st.selected)) {
        st.anim_result = anim_load_name(st.selected_anim);
    }
    EndDialog(hdlg, IDOK);
}

void on_selection_changed(HWND hdlg, BrowserState& st)
{
    HWND tree = GetDlgItem(hdlg, IDC_MESH_BROWSER_TREE);
    const int index = item_file_index(tree, TreeView_GetSelection(tree));
    const bool have_file = index >= 0 && index < static_cast<int>(st.files.size());
    const std::string name = have_file ? st.files[index].name : std::string{};
    EnableWindow(GetDlgItem(hdlg, IDOK), !name.empty());
    if (name == st.selected) {
        return;
    }
    st.selected = name;
    std::string source;
    if (have_file) {
        const BrowserSource& src = st.sources[st.files[index].source];
        source = src.path.empty() ? src.label : src.path;
    }
    SetDlgItemTextA(hdlg, IDC_MESH_BROWSER_SOURCE, source.c_str());
    update_anim_pane(hdlg, st);
    preview_load(hdlg, st, name);
}

void on_anim_selection_changed(HWND hdlg, BrowserState& st)
{
    HWND tree = GetDlgItem(hdlg, IDC_MESH_BROWSER_ANIM_TREE);
    const int index = item_file_index(tree, TreeView_GetSelection(tree));
    const std::string name = (index >= 0 && index < static_cast<int>(st.anims.size()))
                                 ? st.anims[index].name
                                 : std::string{};
    if (name == st.selected_anim) {
        return;
    }
    st.selected_anim = name;
    st.anim_chosen = !name.empty();
    preview_play_anim(hdlg, st, name);
}

void capture_tree_rects(HWND hdlg, BrowserState& st)
{
    HWND tree = GetDlgItem(hdlg, IDC_MESH_BROWSER_TREE);
    HWND anim_tree = GetDlgItem(hdlg, IDC_MESH_BROWSER_ANIM_TREE);
    if (!tree || !anim_tree) {
        return;
    }
    RECT anim_rc;
    GetWindowRect(tree, &st.tree_split);
    GetWindowRect(anim_tree, &anim_rc);
    MapWindowPoints(nullptr, hdlg, reinterpret_cast<POINT*>(&st.tree_split), 2);
    MapWindowPoints(nullptr, hdlg, reinterpret_cast<POINT*>(&anim_rc), 2);
    st.tree_full_bottom = anim_rc.bottom;
}

INT_PTR mesh_browser_message(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
    BrowserState* st = get_state(hdlg);

    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowLongPtrA(hdlg, GWLP_USERDATA, lparam);
        auto& state = *reinterpret_cast<BrowserState*>(lparam);
        alpine_center_dialog_on_owner(hdlg);
        subclass_preview(hdlg);
        capture_tree_rects(hdlg, state);
        SendDlgItemMessage(hdlg, IDC_MESH_BROWSER_FILTER, EM_SETLIMITTEXT, 64, 0);
        CheckRadioButton(hdlg, IDC_MESH_BROWSER_BG_EDITOR, IDC_MESH_BROWSER_BG_BLACK,
                         IDC_MESH_BROWSER_BG_EDITOR + g_preview_background);
        bool listing_failed = false;
        try {
            collect_entries(state);
            build_anim_mask(state);
        }
        catch (...) {
            state.sources.clear();
            state.files.clear();
            state.anims.clear();
            state.anim_mask.clear();
            listing_failed = true;
        }
        // The template is laid out with the animation pane in place, so this is its start state.
        state.anim_pane_shown = true;
        rebuild_mesh_tree(hdlg, state, state.initial);
        rebuild_anim_tree(hdlg, state, state.initial_anim);
        on_anim_selection_changed(hdlg, state);
        on_selection_changed(hdlg, state);
        update_anim_pane(hdlg, state);
        if (listing_failed) {
            xlog::error("Mesh browser: out of memory listing the file system");
            SetDlgItemTextA(hdlg, IDC_MESH_BROWSER_STATUS, "Out of memory listing meshes");
        }
        SetTimer(hdlg, preview_timer_id, preview_timer_ms, nullptr);
        SetFocus(GetDlgItem(hdlg, IDC_MESH_BROWSER_TREE));
        return FALSE;
    }
    case WM_TIMER:
        if (st && wparam == preview_timer_id) {
            bool redraw = false;
            if (st->vmesh && st->spinning) {
                st->yaw += preview_spin_per_tick;
                redraw = true;
            }
            const float dt = preview_timer_ms / 1000.0f;
            const auto type = st->vmesh ? vmesh_get_type(st->vmesh) : VMESH_TYPE_STATIC;
            if (type == VMESH_TYPE_ANIM_FX) {
                vmesh_process(st->vmesh, dt, 0, &preview_origin, &identity_orient, 1);
                redraw = true;
            }
            else if (type == VMESH_TYPE_CHARACTER && st->anim_index >= 0) {
                // Painting happens in WM_DRAWITEM after this returns, so the tick that finishes
                // the action restarts and steps past the seam here: the endpoint frames, where
                // the animation's ramp envelope is zero and the pose falls back to the bind
                // transform, are never the ones on screen.
                vmesh_process(st->vmesh, dt, 0, &preview_origin, &identity_orient, 1);
                if (mesh_v3c_action_finished(st->vmesh)) {
                    mesh_play_v3c_action_looping(st->vmesh, st->anim_index);
                    vmesh_process(st->vmesh, dt, 0, &preview_origin, &identity_orient, 1);
                }
                redraw = true;
            }
            if (redraw) {
                if (HWND preview = GetDlgItem(hdlg, IDC_MESH_BROWSER_PREVIEW)) {
                    InvalidateRect(preview, nullptr, FALSE);
                }
            }
            return TRUE;
        }
        break;
    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (st && dis && dis->CtlID == IDC_MESH_BROWSER_PREVIEW) {
            preview_draw(dis->hwndItem, *st);
            return TRUE;
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        if (!st) {
            break;
        }
        HWND preview = GetDlgItem(hdlg, IDC_MESH_BROWSER_PREVIEW);
        POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        RECT rc;
        if (preview && GetWindowRect(preview, &rc) && PtInRect(&rc, pt)) {
            const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
            st->zoom = std::clamp(st->zoom * std::pow(0.85f, steps), preview_zoom_min,
                                  preview_zoom_max);
            InvalidateRect(preview, nullptr, FALSE);
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lparam);
        if (!st || !hdr) {
            break;
        }
        // Both notifications fall through to the default handler: NM_DBLCLK must keep its
        // folder expand/collapse behaviour when the click was not on a mesh.
        if (hdr->idFrom == IDC_MESH_BROWSER_TREE) {
            if (hdr->code == TVN_SELCHANGEDA || hdr->code == TVN_SELCHANGEDW) {
                if (!st->rebuilding) {
                    on_selection_changed(hdlg, *st);
                }
            }
            else if (hdr->code == NM_DBLCLK) {
                accept_selection(hdlg, *st);
            }
        }
        else if (hdr->idFrom == IDC_MESH_BROWSER_ANIM_TREE) {
            // Only selection matters here; a double click previews rather than accepting.
            if (hdr->code == TVN_SELCHANGEDA || hdr->code == TVN_SELCHANGEDW) {
                if (!st->rebuilding) {
                    on_anim_selection_changed(hdlg, *st);
                }
            }
        }
        break;
    }
    case WM_COMMAND: {
        if (!st) {
            break;
        }
        const int id = LOWORD(wparam);
        const int code = HIWORD(wparam);
        if (id == IDC_MESH_BROWSER_FILTER) {
            if (code == EN_CHANGE) {
                rebuild_mesh_tree(hdlg, *st, st->selected);
                on_selection_changed(hdlg, *st);
                return TRUE;
            }
            break;
        }
        if (id >= IDC_MESH_BROWSER_BG_EDITOR && id <= IDC_MESH_BROWSER_BG_BLACK) {
            g_preview_background = static_cast<PreviewBackground>(id - IDC_MESH_BROWSER_BG_EDITOR);
            if (HWND preview = GetDlgItem(hdlg, IDC_MESH_BROWSER_PREVIEW)) {
                InvalidateRect(preview, nullptr, FALSE);
            }
            return TRUE;
        }
        if (id == IDOK) {
            accept_selection(hdlg, *st);
            return TRUE;
        }
        if (id == IDCANCEL) {
            EndDialog(hdlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    case WM_DESTROY:
        KillTimer(hdlg, preview_timer_id);
        if (st) {
            preview_free(*st);
        }
        break;
    default:
        break;
    }
    return FALSE;
}

// Nothing the dialog allocates may unwind into USER32's frames, so it is all contained here.
INT_PTR CALLBACK MeshBrowserDialogProc(HWND hdlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
    try {
        return mesh_browser_message(hdlg, msg, wparam, lparam);
    }
    catch (...) {
        xlog::error("Mesh browser: out of memory handling message {:#x}", msg);
        return FALSE;
    }
}

} // namespace

bool alpine_anim_playable_on(EditorVMesh* vmesh, const char* anim_name)
{
    if (!vmesh || !anim_name || anim_name[0] == '\0') {
        return false;
    }
    // Called from hooks that RED frames sit below, so the validator's own allocation stops here.
    try {
        return anim_name_fits(anim_name) &&
               rfa_is_playable(anim_load_name(anim_name), character_limits(vmesh));
    }
    catch (...) {
        xlog::error("Mesh browser: out of memory validating animation '{}'", anim_name);
        return false;
    }
}

namespace
{

bool browse_mesh(HWND parent, std::string& filename, unsigned kinds, std::string* anim)
{
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(&__ImageBase);
    if (!FindResourceA(instance, MAKEINTRESOURCEA(IDD_ALPINE_MESH_BROWSER),
                       reinterpret_cast<LPCSTR>(RT_DIALOG))) {
        return false;
    }

    // SysTreeView32 is already registered: RED builds its own tree controls (CTreeCtrl at
    // 0x004422B0), so MFC's common control init has run before any dialog can open.
    BrowserState state = {};
    state.kinds = kinds ? kinds : ALPINE_MESH_ANY;
    state.anims_enabled = (anim != nullptr) && (state.kinds & ALPINE_MESH_V3C) != 0;
    state.initial = filename;
    if (anim) {
        state.initial_anim = *anim;
    }
    state.bound_radius = 1.0f;
    state.zoom = 2.6f;
    state.anim_index = -1;

    const INT_PTR result = DialogBoxParamA(instance, MAKEINTRESOURCEA(IDD_ALPINE_MESH_BROWSER),
                                           parent, MeshBrowserDialogProc,
                                           reinterpret_cast<LPARAM>(&state));
    preview_free(state);

    if (result == IDOK && !state.result.empty()) {
        filename = state.result;
        if (anim && !state.anim_result.empty()) {
            *anim = state.anim_result;
        }
        return true;
    }
    return false;
}

} // namespace

bool alpine_browse_mesh(HWND parent, std::string& filename, unsigned kinds, std::string* anim)
{
    // The caller is a RED dialog handler, so the browser's own allocation stops here too.
    try {
        return browse_mesh(parent, filename, kinds, anim);
    }
    catch (...) {
        xlog::error("Mesh browser: out of memory opening the browser");
        return false;
    }
}
