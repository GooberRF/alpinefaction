#include <cstring>
#include <cctype>
#include <format>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include <windows.h>
#include <commctrl.h>
#include <xlog/xlog.h>
#include <patch_common/MemUtils.h>
#include <patch_common/FunHook.h>
#include <patch_common/CallHook.h>
#include <patch_common/CodeInjection.h>
#include "mfc_types.h"
#include "level.h"
#include "textures.h"
#include "meshes.h"
#include "sounds.h"
#include "event.h"
#include "bitmap_loaders.h"
#include "editor_settings.h"
#include "resources.h"
#include <common/utils/string-utils.h>

// Subdirectory names registered during init, used by VPP packing fix
static std::vector<std::string> custom_texture_subdirs;
// Texture manager pointer, stored at init for reload support
static TextureManager* g_texture_manager = nullptr;

// "Custom" categories list files from disk; "Custom - <dir>" are the subdirectory ones registered here.
static constexpr std::string_view custom_category_prefix = "Custom";
static constexpr std::string_view custom_subdir_category_prefix = "Custom - ";

static bool category_name_starts_with(const char* name, std::string_view prefix = custom_category_prefix)
{
    return std::string_view{name}.starts_with(prefix);
}

static void register_custom_texture_subdirectories(TextureManager* texture_manager)
{
    // Resolve path relative to executable directory
    char exe_dir[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;
    char* last_sep = strrchr(exe_dir, '\\');
    if (last_sep) *(last_sep + 1) = '\0';

    std::string search_pattern = std::string(exe_dir) + "user_maps\\textures\\*";
    xlog::info("Enumerating custom texture subdirectories: {}", search_pattern);

    WIN32_FIND_DATAA find_data;
    HANDLE hFind = FindFirstFileA(search_pattern.c_str(), &find_data);
    if (hFind == INVALID_HANDLE_VALUE) {
        xlog::info("No user_maps\\textures directory found (or it is empty)");
        return;
    }

    std::vector<std::string> subdirs;
    do {
        if (!(find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            continue;
        }
        if (strcmp(find_data.cFileName, ".") == 0 || strcmp(find_data.cFileName, "..") == 0) {
            continue;
        }
        subdirs.emplace_back(find_data.cFileName);
    } while (FindNextFileA(hFind, &find_data));
    FindClose(hFind);

    if (subdirs.empty()) {
        xlog::info("No subdirectories found in user_maps\\textures");
        return;
    }

    std::sort(subdirs.begin(), subdirs.end());

    // Store for later use by VPP packing path fix
    custom_texture_subdirs = subdirs;

    auto* category_array = &texture_manager->categories;

    constexpr size_t texture_dir_max_len = 255;

    for (const auto& dirname : subdirs) {
        std::string display_name = "Custom - " + dirname;
        std::string subdir_path = "user_maps\\textures\\" + dirname;

        if (subdir_path.size() > texture_dir_max_len) {
            xlog::warn("Skipping custom texture directory (path too long): '{}'", subdir_path);
            continue;
        }

        // Allocate and construct category object (matches stock FUN_004776b0 pattern)
        void* mem = rf_alloc(sizeof(TextureCategory));
        if (!mem) {
            continue;
        }
        auto* cat = AddrCaller{0x00478740}.this_call<TextureCategory*>(mem);

        // Set the display name via VString::assign_0
        cat->name.assign_0(display_name.c_str());

        // Register the subdirectory path with the VFS
        cat->path_handle = file_add_path(subdir_path.c_str(), ".tga .vbm .dds .atx .png .jpg .jpeg", false);

        category_array->push_back(cat);

        xlog::info("Registered custom texture category: '{}' (path_handle={})", display_name, cat->path_handle);
    }

    xlog::info("Registered {} custom texture subdirectories (total categories: {})",
               subdirs.size(), category_array->size);
}

void texture_categories_loaded(TextureManager* texture_manager)
{
    g_texture_manager = texture_manager;
    register_custom_texture_subdirectories(texture_manager);
}

// The texture refresh function (FUN_0046fd10) compares category names with "Custom" to decide
// whether to use file enumeration (for custom textures on disk) vs searching loaded texture groups.
// Our subdirectory categories are named "Custom - <dirname>" which fails the exact match.
// Replace the comparison with a prefix check so "Custom - *" categories use the file path too.
static char __cdecl is_custom_category(VString* name, const char* /*cstr*/)
{
    const char* buf = name->c_str();
    return category_name_starts_with(buf) ? 1 : 0;
}

// All call sites in RED.exe where FUN_004b7560 compares a category name against "Custom":
//   0x0046ff05 - texture refresh "All" mode (FUN_0046fd10)
//   0x00470027 - texture refresh specific category (FUN_0046fd10)
//   0x004776e9 - texture category loader at init (FUN_004776b0)
//   0x004774d6 - texture folder serialization for save/VPP (FUN_00477460)
//   0x00445403 - texture browser sidebar category init (FUN_004452f0)
CallHook<char __cdecl(VString*, const char*)> custom_category_check_hook{
    {0x0046ff05, 0x00470027, 0x004776e9, 0x004774d6, 0x00445403},
    is_custom_category
};

// Permissive replacement for the texture browser entry validator
static bool browser_extension_allowed(const char* ext)
{
    if (!ext || !*ext) return false;
    return _stricmp(ext, ".tga") == 0
        || _stricmp(ext, ".vbm") == 0
        || _stricmp(ext, ".dds") == 0
        || _stricmp(ext, ".atx") == 0
        || _stricmp(ext, ".png") == 0
        || _stricmp(ext, ".jpg") == 0
        || _stricmp(ext, ".jpeg") == 0;
}

static int __cdecl permissive_texture_browser_validator(void* entry)
{
    const char* filename = static_cast<const char*>(entry) + 8;
    if (!filename || !*filename) return 0;

    // Skip "-mip" LOD variants
    if (std::strstr(filename, "-mip") != nullptr) return 0;

    // Refuse over-long names
    if (std::strlen(filename) > MAX_TEXTURE_NAME_LEN) {
        xlog::warn("Texture name too long, skipping: {}", filename);
        return 0;
    }

    const char* ext = std::strrchr(filename, '.');
    return browser_extension_allowed(ext) ? 1 : 0;
}

// The Format filter's choices after All; a .jpeg counts as JPG
static constexpr const char* texture_browser_formats[] = {"TGA", "VBM", "DDS", "PNG", "JPG", "ATX"};
// Its selection for the session, 0 for All
static int g_texture_browser_format = 0;

static bool texture_browser_format_matches(const char* filename)
{
    const int sel = g_texture_browser_format;
    if (sel <= 0 || sel > static_cast<int>(std::size(texture_browser_formats))) return true;
    const char* ext = std::strrchr(filename, '.');
    if (!ext) return false;
    const char* format = texture_browser_formats[sel - 1];
    return _stricmp(ext + 1, format) == 0 || (std::strcmp(format, "JPG") == 0 && _stricmp(ext + 1, "jpeg") == 0);
}

// The stock validator's Material filter: one of the material's prefixes starts the name
static bool texture_browser_material_matches(const char* filename)
{
    const int sel = texture_browser_material_index;
    if (sel <= 0 || sel > texture_browser_materials.size) return true;
    const TextureBrowserMaterial* material = texture_browser_materials.data_ptr[sel - 1];
    if (!material) return true;
    for (int i = 0; i < material->bitmap_prefixes.size; i++) {
        const char* prefix = material->bitmap_prefixes.data_ptr[i].c_str();
        if (_strnicmp(filename, prefix, std::strlen(prefix)) == 0) return true;
    }
    return false;
}

static int __cdecl filtered_texture_browser_validator(void* entry)
{
    if (!permissive_texture_browser_validator(entry)) return 0;

    const char* filename = static_cast<const char*>(entry) + 8;
    if (!texture_browser_format_matches(filename) || !texture_browser_material_matches(filename)) return 0;

    if (!texture_browser_filter[0]) return 1;

    return string_icontains(filename, static_cast<const char*>(texture_browser_filter)) ? 1 : 0;
}

FunHook<int __cdecl(void*)> texture_browser_validator_hook{
    0x00470330,
    filtered_texture_browser_validator
};

FunHook<int __cdecl(void*)> texture_sidebar_validator_hook{
    0x004451C0,
    permissive_texture_browser_validator
};

// FUN_0046f880 lays the thumbnail grid out at 32, 64 or 128 px by Display Size; 256 and 512 follow those, and One To
// One moves after them (its dispatch at 0x0046fccc compares with 5 instead of 3). No trampoline: it always resumes past
// the size switch at 0x0046f8c2, so the replaced mov eax, [esi+0x2b8] is never needed.
static constexpr int texture_browser_one_to_one = 5;
CodeInjection texture_browser_thumbnail_size_injection{
    0x0046f89e,
    [](auto& regs) {
        static constexpr int sizes[] = {32, 64, 128, 256, 512};
        const TextureBrowserPanel* panel = regs.esi;
        const int mode = panel->display_size;
        regs.ebp = mode >= 0 && mode < static_cast<int>(std::size(sizes)) ? sizes[mode] : 64;
        regs.eip = 0x0046f8c2;
    },
    false,
};

// A frame narrower than one 512 px cell would give no columns, which the layout divides by
CodeInjection texture_browser_min_columns_injection{
    0x0046f8f0,
    [](auto& regs) {
        if (static_cast<int>(regs.eax) < 1) {
            regs.eax = 1;
        }
    },
};

// The Texture Browser resizes: its view grows with the window and the grid is laid out again for it, the favorites
// strip and the buttons keep to the bottom. Its placement and Display Size are kept in the settings, and the mouse
// wheel scrolls it.
namespace
{
struct TextureBrowserLayout
{
    SIZE min_size{};
    SIZE client{};
    SIZE applied{};
    int wheel = 0;
    RECT view{};
    std::vector<std::pair<int, POINT>> to_bottom;
};
TextureBrowserLayout g_texture_browser_layout;

// the favorites strip, its Change buttons, OK and Cancel
constexpr int texture_browser_bottom_ids[] = {1506, 1507, 1508, 1509, 1510, 1511, 1512, 1513, 1514, IDOK, IDCANCEL};

// As if the scroll bar were hidden: the grid's layout shows and hides it, which must not resize the view again
SIZE texture_browser_client_size(HWND hdlg)
{
    RECT r{};
    GetClientRect(hdlg, &r);
    const bool scroll_bar = (GetWindowLongA(hdlg, GWL_STYLE) & WS_VSCROLL) != 0;
    return {r.right + (scroll_bar ? GetSystemMetrics(SM_CXVSCROLL) : 0), r.bottom};
}

RECT texture_browser_child_rect(HWND hdlg, int id)
{
    RECT r{};
    if (const HWND child = GetDlgItem(hdlg, id)) {
        GetWindowRect(child, &r);
        MapWindowPoints(nullptr, hdlg, reinterpret_cast<POINT*>(&r), 2);
    }
    return r;
}

void texture_browser_layout_init(HWND hdlg)
{
    TextureBrowserLayout& layout = g_texture_browser_layout;
    RECT window{};
    GetWindowRect(hdlg, &window);
    layout.min_size = {window.right - window.left, window.bottom - window.top};
    layout.client = texture_browser_client_size(hdlg);
    layout.applied = layout.client;
    layout.wheel = 0;
    layout.view = texture_browser_child_rect(hdlg, IDC_TEXTURE_BROWSER_VIEW);
    layout.to_bottom.clear();
    for (int id : texture_browser_bottom_ids) {
        const RECT r = texture_browser_child_rect(hdlg, id);
        layout.to_bottom.emplace_back(id, POINT{r.left, r.top});
    }
}

void texture_browser_layout_apply(HWND hdlg, SIZE size)
{
    const TextureBrowserLayout& layout = g_texture_browser_layout;
    const int dx = std::max(0, static_cast<int>(size.cx - layout.client.cx));
    const int dy = std::max(0, static_cast<int>(size.cy - layout.client.cy));
    if (const HWND view = GetDlgItem(hdlg, IDC_TEXTURE_BROWSER_VIEW)) {
        // no bigger than the back buffer it is drawn from
        const int width = std::min(static_cast<int>(layout.view.right - layout.view.left) + dx, gr_get_max_width());
        const int height = std::min(static_cast<int>(layout.view.bottom - layout.view.top) + dy, gr_get_max_height());
        SetWindowPos(view, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    for (const auto& [id, pos] : layout.to_bottom) {
        if (const HWND child = GetDlgItem(hdlg, id)) {
            SetWindowPos(child, nullptr, pos.x, pos.y + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    InvalidateRect(hdlg, nullptr, TRUE);
}

// The restored rect, so a maximized browser comes back to it
void texture_browser_remember_rect(HWND hdlg)
{
    if (!IsIconic(hdlg) && !IsZoomed(hdlg)) {
        GetWindowRect(hdlg, &g_editor_settings.texture_browser_rect);
    }
}

// On the monitor nearest the saved rect, fitted to its work area, which may have changed since
void texture_browser_restore_placement(HWND hdlg)
{
    const RECT saved = g_editor_settings.texture_browser_rect;
    const long long saved_width = static_cast<long long>(saved.right) - saved.left;
    const long long saved_height = static_cast<long long>(saved.bottom) - saved.top;
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (saved_width > 0 && saved_height > 0
        && GetMonitorInfoA(MonitorFromRect(&saved, MONITOR_DEFAULTTONEAREST), &info)) {
        const RECT& work = info.rcWork;
        const SIZE& min_size = g_texture_browser_layout.min_size;
        const long long width =
            std::max<long long>(min_size.cx, std::min<long long>(saved_width, work.right - work.left));
        const long long height =
            std::max<long long>(min_size.cy, std::min<long long>(saved_height, work.bottom - work.top));
        const long long x = std::max<long long>(work.left, std::min<long long>(saved.left, work.right - width));
        const long long y = std::max<long long>(work.top, std::min<long long>(saved.top, work.bottom - height));
        SetWindowPos(hdlg, nullptr, static_cast<int>(x), static_cast<int>(y), static_cast<int>(width),
                     static_cast<int>(height), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g_editor_settings.texture_browser_maximized) {
        ShowWindow(hdlg, SW_MAXIMIZE);
    }
}
} // namespace

static LRESULT CALLBACK texture_browser_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR subclass_id,
                                             DWORD_PTR panel)
{
    if (msg == WM_GETMINMAXINFO) {
        auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
        info->ptMinTrackSize = {g_texture_browser_layout.min_size.cx, g_texture_browser_layout.min_size.cy};
        return 0;
    }
    if (msg == WM_MOUSEWHEEL) {
        // a step per notch, as the scroll bar's arrows go
        int& wheel = g_texture_browser_layout.wheel;
        wheel += GET_WHEEL_DELTA_WPARAM(wparam);
        for (; wheel >= WHEEL_DELTA; wheel -= WHEEL_DELTA) {
            SendMessageA(hwnd, WM_VSCROLL, SB_LINEUP, 0);
        }
        for (; wheel <= -WHEEL_DELTA; wheel += WHEEL_DELTA) {
            SendMessageA(hwnd, WM_VSCROLL, SB_LINEDOWN, 0);
        }
        return 0;
    }
    if (msg == WM_SIZE && wparam != SIZE_MINIMIZED && g_texture_browser_layout.client.cx > 0) {
        const SIZE size = texture_browser_client_size(hwnd);
        SIZE& applied = g_texture_browser_layout.applied;
        if (size.cx != applied.cx || size.cy != applied.cy) {
            applied = size;
            texture_browser_layout_apply(hwnd, size);
            // the scroll range follows the rows, and the view keeps its scroll position
            auto* browser = reinterpret_cast<TextureBrowserPanel*>(panel);
            const int scrolled = GetScrollPos(hwnd, SB_VERT);
            texture_browser_relayout(browser, true, 0, 0);
            if (scrolled > 0) {
                texture_browser_relayout(browser, false, SB_THUMBPOSITION, scrolled);
            }
        }
    }
    if (msg == WM_SIZE || msg == WM_MOVE) {
        texture_browser_remember_rect(hwnd);
    }
    if (msg == WM_DESTROY) {
        texture_browser_remember_rect(hwnd);
        g_editor_settings.texture_browser_maximized = IsZoomed(hwnd) != FALSE;
        g_editor_settings.texture_browser_display_size = reinterpret_cast<TextureBrowserPanel*>(panel)->display_size;
    }
    if (msg == WM_COMMAND && LOWORD(wparam) == IDC_TEXTURE_BROWSER_FORMAT && HIWORD(wparam) == CBN_SELCHANGE) {
        const int sel = static_cast<int>(SendMessageA(reinterpret_cast<HWND>(lparam), CB_GETCURSEL, 0, 0));
        g_texture_browser_format = sel > 0 ? sel : 0;
        texture_browser_refresh(reinterpret_cast<TextureBrowserPanel*>(panel));
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, texture_browser_proc, subclass_id);
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

// FUN_00470a10: the browser's OnInitDialog
static BOOL __fastcall texture_browser_init_dialog_hooked(TextureBrowserPanel* panel);
FunHook<decltype(texture_browser_init_dialog_hooked)> texture_browser_init_dialog_hook{
    0x00470a10,
    texture_browser_init_dialog_hooked
};
static BOOL __fastcall texture_browser_init_dialog_hooked(TextureBrowserPanel* panel)
{
    // Before the stock dialog lays the grid out by it
    panel->display_size = std::clamp(g_editor_settings.texture_browser_display_size, 0, texture_browser_one_to_one);
    const BOOL result = texture_browser_init_dialog_hook.call_target(panel);
    HWND hdlg = WndToHandle(reinterpret_cast<CWnd*>(panel));

    // stock selected the size before these existed
    HWND sizes = GetDlgItem(hdlg, IDC_TEXTURE_BROWSER_SIZES);
    SendMessageA(sizes, CB_INSERTSTRING, 3, reinterpret_cast<LPARAM>("256 x 256"));
    SendMessageA(sizes, CB_INSERTSTRING, 4, reinterpret_cast<LPARAM>("512 x 512"));
    SendMessageA(sizes, CB_SETCURSEL, panel->display_size, 0);

    HWND formats = GetDlgItem(hdlg, IDC_TEXTURE_BROWSER_FORMAT);
    SendMessageA(formats, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>("All"));
    for (const char* format : texture_browser_formats) {
        SendMessageA(formats, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(format));
    }
    SendMessageA(formats, CB_SETCURSEL, g_texture_browser_format, 0);

    // The Name handler copies the text unbounded into its buffer
    SendDlgItemMessageA(hdlg, IDC_TEXTURE_BROWSER_NAME, EM_LIMITTEXT, sizeof(texture_browser_filter) - 1, 0);

    texture_browser_layout_init(hdlg);
    SetWindowSubclass(hdlg, texture_browser_proc, 1, reinterpret_cast<DWORD_PTR>(panel));
    texture_browser_restore_placement(hdlg);
    return result;
}

// FUN_0041b7c0 (startup default-texture folder group build) indexes
// TextureManager::folder_names with each category's path_handle. Custom subdirectory
// categories store a VFS path slot there instead, which reads out of bounds.
// Inject at 0x0041b9fa (EAX = TextureCategory** array element) and jump to the loop
// increment at 0x0041bad7 to skip them.
CodeInjection folder_group_build_skip_custom_subdirs{
    0x0041b9fa,
    [](auto& regs) {
        auto* cat = *reinterpret_cast<TextureCategory**>(static_cast<int>(regs.eax));
        if (category_name_starts_with(cat->name.c_str(), custom_subdir_category_prefix)) {
            regs.eip = 0x0041bad7;
        }
    }
};

// The sidebar's OnInitDialog (FUN_00444eb0) shows the selected category's first texture without checking that the
// list has one, so an empty category (every stock one until startup builds the texture groups) passed the bytes past
// the list head to bm_load as a filename, a fatal error. With an empty list the preview gets the fallback stock's
// Reload uses (0x00445dc0) and the rest is skipped; the init flag stays clear, so the next init builds the list again.
CodeInjection sidebar_init_empty_list_fix{
    0x004450fb,
    [](auto& regs) {
        const TextureModePanel* panel = regs.esi;
        if (panel->textures.head == reinterpret_cast<const TextureListNode*>(&panel->textures)) {
            panel->preview->bm_handle = bm_load("Rck_default.tga", -1, 1);
            regs.eip = 0x0044515d;
        }
    },
};

// FUN_004452f0 (texture mode sidebar) uses a fixed path_handle from [dialog+0x98] for all
// custom categories. This is the root "Custom" directory handle. For our subdirectory categories
// ("Custom - <dir>"), we need to use the selected category's own path_handle instead.
// Inject at 0x0044540f to replace: MOV EDX, [ESI+0x98]
// At this point: ESI = dialog object, [ESI+0x94] = selected category index,
//                [ESI+0xa4] = texture manager ptr
CodeInjection sidebar_custom_texture_path_injection{
    0x0044540f,
    [](auto& regs) {
        auto* panel = reinterpret_cast<TextureModePanel*>(static_cast<uintptr_t>(regs.esi));
        auto* cat_array = &panel->texture_manager->categories;
        int path_handle = (*cat_array)[panel->category_index]->path_handle;
        // A custom subdirectory whose VFS path failed to register (path table full)
        // has path_handle == -1. This EDX value flows into the search's path-handle
        // arg (FUN_004c3ec0 -> FUN_004c33d0), whose stock guard is `if (param_1 != 0)`:
        // a negative handle passes that guard and indexes (&DAT_0156b110)[-3] out of
        // bounds, then strlen's it -> crash. 0 is the stock "no path" sentinel that
        // skips the prefix cleanly, so fall back to it (mirrors the handle>=0 guard in
        // reload_custom_textures) — the category behaves as if it had no custom path.
        regs.edx = (path_handle >= 0) ? path_handle : 0;
        // Original MOV EDX,[ESI+0x98] is 6 bytes; continue at next instruction
        regs.eip = 0x00445415;
    },
    false // skip original instruction
};

// FUN_00445910 (texture→category reverse lookup) is called when the texture browser returns.
// It searches loaded texture groups for the selected texture filename. Custom category textures
// are not in any loaded group, so the lookup falls through to a fallback that calls FUN_004cf9a0
// which searches ALL VFS paths. The function succeeds but the code then uses [panel+0x98] (base
// "Custom" path) for file enumeration, so subcategory textures aren't found → "Missing".
// Fix: inject at 0x445a3f (AFTER file_search succeeds). Read the found VFS path slot from
// search_ctx[0], find which category owns it, and update [panel+0x98] to the correct path.
static int g_custom_lookup_cat_index = -1;
static const char* g_custom_lookup_cat_name = nullptr;

CodeInjection texture_reverse_lookup_fix{
    0x445a3f,
    [](auto& regs) {
        g_custom_lookup_cat_index = -1;
        g_custom_lookup_cat_name = nullptr;

        if (!g_texture_manager) return;

        auto stack = static_cast<uintptr_t>(regs.esp);

        // search_ctx lives at [ESP + 0x1c]. FUN_004cf9a0 stores the found VFS path
        // slot index at [search_ctx + 0] (verified at 0x4cfbc3: MOV [EBP], EDI).
        int found_path = *reinterpret_cast<int*>(stack + 0x1c);

        auto* cat_array = &g_texture_manager->categories;

        for (int i = 0; i < cat_array->get_size(); i++) {
            TextureCategory* cat = (*cat_array)[i];
            // Only match custom categories — stock categories' path_handle values
            // are a different namespace that can numerically overlap with VFS path slots
            if (!category_name_starts_with(cat->name.c_str())) continue;
            if (cat->path_handle == found_path) {
                // Update panel's path_handle so file enumeration at 0x445a92 uses
                // the correct subdirectory
                auto* panel = reinterpret_cast<TextureModePanel*>(static_cast<uintptr_t>(regs.esi));
                panel->custom_path_handle = found_path;
                g_custom_lookup_cat_index = i;
                g_custom_lookup_cat_name = cat->name.c_str();
                return;
            }
        }
    }
};

// At 0x445a4a in FUN_00445910: the stock code does MOV [ESI+0x94], ECX where ECX = path_handle
// (from [ESI+0x98]) and the stack has a pushed "Custom" string for the combo.
// Fix both: set ECX to the correct category array index, and replace the combo text on stack.
CodeInjection texture_reverse_lookup_index_fix{
    0x445a4a,
    [](auto& regs) {
        if (g_custom_lookup_cat_index >= 0) {
            // Fix category index: ECX is about to be stored into [ESI+0x94]
            regs.ecx = g_custom_lookup_cat_index;

            // Fix combo text: [ESP] = "Custom" pointer, replace with actual category name
            auto stack = static_cast<uintptr_t>(regs.esp);
            *reinterpret_cast<const char**>(stack) = g_custom_lookup_cat_name;

            g_custom_lookup_cat_index = -1;
            g_custom_lookup_cat_name = nullptr;
        }
    }
};
// Sorted splice merge of temp into master by filename (strcmp matches stock comparator).
static void merge_sorted_into_master(TextureListSentinel* master, TextureListSentinel* temp)
{
    auto* master_end = reinterpret_cast<TextureListNode*>(master);
    auto* temp_end = reinterpret_cast<TextureListNode*>(temp);
    TextureListNode* mp = master->head;
    TextureListNode* tp = temp->head;

    while (mp != master_end && tp != temp_end) {
        if (std::strcmp(tp->name, mp->name) < 0) {
            TextureListNode* next_tp = tp->next;
            tp->next = mp;
            tp->prev = mp->prev;
            mp->prev->next = tp;
            mp->prev = tp;
            tp = next_tp;
        } else {
            mp = mp->next;
        }
    }

    if (tp != temp_end) {
        TextureListNode* temp_last = temp->tail;
        master->tail->next = tp;
        tp->prev = master->tail;
        temp_last->next = master_end;
        master->tail = temp_last;
    }
}

// Drop any node whose lowercased filename was already seen. Sentinel layout means
// node->next->prev rewrites automatically update master->tail when unlinking the tail.
static void dedup_master_by_name(TextureListSentinel* master)
{
    auto* master_end = reinterpret_cast<TextureListNode*>(master);
    std::unordered_set<std::string> seen;
    TextureListNode* node = master->head;
    while (node != master_end) {
        TextureListNode* next_node = node->next;
        std::string key = node->name;
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (!seen.insert(std::move(key)).second) {
            node->prev->next = node->next;
            node->next->prev = node->prev;
            editor_free(node);
        }
        node = next_node;
    }
}

CodeInjection texture_refresh_all_iterate_custom_injection{
    0x0046feee,
    [](auto& regs) {
        auto* panel = reinterpret_cast<TextureBrowserPanel*>(
            static_cast<uintptr_t>(regs.ebx));

        // Skip the stock single-category scan/merge block (0x0046feee … 0x0046fffd)
        regs.eip = 0x00470134;

        uint8_t flags = texture_browser_get_scan_flags(panel);
        auto* cat_array = &panel->texture_manager->categories;

        for (int i = 0; i < cat_array->get_size(); i++) {
            TextureCategory* cat = (*cat_array)[i];
            if (!category_name_starts_with(cat->name.c_str())) continue;
            // A subdirectory whose VFS path failed to register has path_handle == -1;
            // texture_browser_scan_path (0x004c3ec0) would index the path table out of
            // bounds on a negative handle. Skip it (mirrors reload_custom_textures).
            if (cat->path_handle < 0) continue;

            TextureListSentinel temp;
            temp.head = reinterpret_cast<TextureListNode*>(&temp);
            temp.tail = reinterpret_cast<TextureListNode*>(&temp);

            texture_browser_scan_path(&temp, cat->path_handle, "*.*",
                                      flags, texture_browser_validator);

            if (temp.head != reinterpret_cast<TextureListNode*>(&temp)) {
                merge_sorted_into_master(&panel->master_list, &temp);
            }
        }
    }
};

CodeInjection texture_refresh_dedup_master_injection{
    0x00470134,
    [](auto& regs) {
        auto* panel = reinterpret_cast<TextureBrowserPanel*>(
            static_cast<uintptr_t>(regs.ebx));
        dedup_master_by_name(&panel->master_list);
    }
};

// ─── Texture browser invocation ─────────────────────────────────────────────

// CDedLevel holds one shared browser instance in dialog_panels[] (+0x444); the browser is
// the entry at +0x4a0, which is what every stock browse handler reaches for.
static_assert(offsetof(CDedLevel, dialog_panels) == 0x444);
constexpr int texture_browser_panel_index = (0x4a0 - 0x444) / 4;

// SetFolderByName takes the string by value and destroys it (MSVC callee-destroys), so the
// buffer handed over must not be freed here.
struct VStringByValue {
    int max_len;
    char* buf;
};

int texture_browser_pick(const char* folder, int current_bm)
{
    auto* level = CDedLevel::Get();
    if (!level) return -1;

    auto* panel = static_cast<TextureBrowserPanel*>(
        level->dialog_panels[texture_browser_panel_index]);
    if (!panel || !panel->preview) return -1;

    if (folder && folder[0]) {
        VString folder_name;
        folder_name.assign_0(folder);
        AddrCaller{0x004711c0}.this_call<int>(
            panel, VStringByValue{folder_name.max_len, folder_name.buf});
    }

    panel->preview->bm_handle = current_bm;

    // DoModal is CDialog vtable slot 46
    auto** vtbl = *reinterpret_cast<void***>(panel);
    auto do_modal = reinterpret_cast<int(__thiscall*)(void*)>(vtbl[0xb8 / 4]);
    if (do_modal(panel) != IDOK) return -1;

    return panel->preview->bm_handle;
}

static bool same_texture_stem(std::string_view a, std::string_view b)
{
    a = a.substr(0, a.find_last_of('.'));
    b = b.substr(0, b.find_last_of('.'));
    return a.size() == b.size() && _strnicmp(a.data(), b.data(), a.size()) == 0;
}

// Stock categories list their textures in the startup groups, as texture mode's reverse lookup
// (0x00445910) searches them; a custom category's search path is matched otherwise, as its files may
// postdate the groups and its subdirectories have none.
const char* texture_category_of(const char* filename)
{
    CDedLevel* level = CDedLevel::Get();
    if (!level || !filename || !filename[0]) return nullptr;
    const auto& groups = level->texture_groups;
    for (int g = 0; g < groups.size; g++) {
        const TextureGroup* group = groups.data_ptr[g];
        if (!group) continue;
        for (int i = 0; i < group->textures.size; i++) {
            if (same_texture_stem(group->textures.data_ptr[i].c_str(), filename)) return group->name.c_str();
        }
    }
    if (!g_texture_manager) return nullptr;
    const auto& categories = g_texture_manager->categories;
    for (EditorVfsFile* node : vfs_file_buckets) {
        for (; node; node = node->next) {
            if (!node->name || !same_texture_stem(node->name, filename)) continue;
            for (int c = 0; c < categories.size; c++) {
                const TextureCategory* cat = categories.data_ptr[c];
                if (cat && cat->path_handle == node->path_index
                    && category_name_starts_with(cat->name.c_str())) {
                    return cat->name.c_str();
                }
            }
        }
    }
    return nullptr;
}

// VPP packfile creation (FUN_004482c0) constructs custom texture paths by combining a
// fixed base directory ("user_maps\textures\") with the bare filename via FUN_004b6ee0.
// For textures in subdirectories, this produces the wrong path. Inject at 0x004485a2
// (after path construction at 0x0044859a, before adding to the file list at 0x004485a7)
// to check if the file exists and search through registered subdirectories if not.
// At this point ESP points to the 8-byte output VString containing the constructed path.
CodeInjection vpp_texture_path_fix{
    0x004485a2,
    [](auto& regs) {
        auto* path_vstr = reinterpret_cast<VString*>(static_cast<int>(regs.esp));
        const char* path = path_vstr->c_str();

        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            return; // File exists at default path, nothing to fix
        }

        // Find the "user_maps\textures\" segment in the constructed path
        const char* marker = strstr(path, "user_maps\\textures\\");
        if (!marker) {
            return;
        }

        // Split into prefix (everything up to and including "user_maps\textures\")
        // and the bare filename after it
        constexpr size_t marker_len = 19; // strlen("user_maps\\textures\\")
        std::string prefix(path, (marker - path) + marker_len);
        std::string filename(marker + marker_len);

        for (const auto& subdir : custom_texture_subdirs) {
            std::string try_path = prefix + subdir + "\\" + filename;
            if (GetFileAttributesA(try_path.c_str()) != INVALID_FILE_ATTRIBUTES) {
                path_vstr->assign_0(try_path.c_str());
                xlog::info("VPP: Resolved texture '{}' in subdirectory '{}'", filename, subdir);
                return;
            }
        }
    }
};

// File names come out of level and mesh files, which are shared content.
// Only bare filenames should be packable, not paths.
static bool has_packable_path(const char* filename)
{
    if (!filename || !filename[0]) return false;
    if (strpbrk(filename, "\\/:") != nullptr) {
        xlog::warn("Refusing to pack file with a path in its name: '{}'", filename);
        return false;
    }
    return true;
}

static bool has_extension_from(const char* filename, std::initializer_list<const char*> extensions)
{
    if (!has_packable_path(filename)) return false;
    const char* ext = strrchr(filename, '.');
    if (!ext) return false;
    for (const char* known : extensions) {
        if (_stricmp(ext, known) == 0) return true;
    }
    return false;
}

// What RED itself can preview — the gate for textures named by level and mesh data.
static bool has_texture_extension(const char* filename)
{
    return has_extension_from(filename, {".tga", ".vbm", ".dds", ".atx", ".png", ".jpg", ".jpeg"});
}

// Everything the game's loader resolves (mirrors g_texture_extensions in bmpman.cpp).
static bool has_loadable_texture_extension(const char* filename)
{
    return has_extension_from(filename, {".tga", ".vbm", ".dds", ".atx", ".png", ".jpg", ".jpeg",
                                         ".pcx", ".vaf", ".m2v"});
}

static void push_to_pack_list(void* temp_list, const char* filename)
{
    VString str;
    str.assign_0(filename);
    // FUN_00438640 is __thiscall(list, VString by value) where VString = {int, char*}.
    // VString's copy constructor is deleted, so pass the two fields directly — the 8 bytes
    // on the stack match the VString layout. Transfer buffer ownership to the list.
    int ml = str.max_len;
    char* b = str.buf;
    str.max_len = 0;
    str.buf = nullptr;
    AddrCaller{0x00438640}.this_call<int>(temp_list, ml, b);
}

// Add a texture filename to the VPP temp file list if it has a valid texture extension.
// .atx frame dependencies are expanded by expand_atx_deps_in_pack_list, which sweeps the whole
// list after every caller here has run.
static void add_texture_to_pack_list(void* temp_list, const char* filename)
{
    if (!has_texture_extension(filename)) return;
    push_to_pack_list(temp_list, filename);
}

struct VppFileList {
    int count;
    int field_4;
    VString* data;
};

static bool push_new_to_pack_list(void* temp_list, const char* filename)
{
    auto* list = static_cast<VppFileList*>(temp_list);
    const int before = list->count;
    push_to_pack_list(temp_list, filename);
    return list->count != before;
}

static void expand_atx_deps_in_pack_list(void* temp_list)
{
    auto* list = static_cast<VppFileList*>(temp_list);
    const int initial_count = list->count;
    if (initial_count <= 0 || !list->data) return;

    for (int i = 0; i < initial_count; i++) {
        // Copy the name out: the pushes below can reallocate the element array.
        const std::string name = list->data[i].c_str();
        // The bare-name rule the rest of the pack flow applies, before reading anything by name.
        if (name.empty() || !has_packable_path(name.c_str())) continue;

        std::string atx = name;
        if (!string_iends_with(atx, ".atx")) {
            // A face or decal can name a legacy texture that the supercede chain resolves to a
            // sibling .atx. The packer resolves entries by their literal name, so that .atx is
            // not packed by itself and has to be pushed here.
            atx = find_atx_sibling(name.c_str());
            if (atx.empty()) continue;
            if (push_new_to_pack_list(temp_list, atx.c_str())) {
                xlog::info("VPP: Added superceding ATX '{}' for texture '{}'", atx, name);
            }
        }

        for (const auto& dep : parse_atx_dependencies(atx.c_str())) {
            if (!has_loadable_texture_extension(dep.c_str()) || string_iends_with(dep, ".atx")) continue;
            if (push_new_to_pack_list(temp_list, dep.c_str())) {
                xlog::info("VPP: Added ATX frame '{}' from '{}'", dep, atx);
            }
        }
    }
}

// Clear the RED console log at the start of Create Level Packfile (FUN_004482c0), so the warnings
// its file gathering reports stay above "Creating Packfile...", which the writer FUN_0044cb10 prints.
// FUN_00444940 is __thiscall LogDlg_Clear.
CodeInjection vpp_clear_log_injection{
    0x004482c0,
    [](auto& regs) {
        log_dlg_clear(GetLogDlg());
    }
};

// Ships with the game: inside a packfile the editor mounted (the stock ones, alpinefaction.vpp). A file only loose in
// an editor folder (red\textures, say) does not count, since the game will not have it either. FUN_004caae0 looks in
// the packfiles alone, where File::open would find a loose copy first; it copies the name into 256 bytes.
static bool shipped_in_packfile(const char* name)
{
    return name && std::strlen(name) < 256 && AddrCaller{0x004caae0}.c_call<void*>(name) != nullptr;
}

// FUN_0044c7a0 (process single VPP file) fails fatally when fopen returns NULL, which
// causes two bugs: a blank VPP is left on disk, and the file list is never cleaned up
// (FUN_004386f0 only runs on the success path) so packfiles can't be created again.
// Inject at 0x0044c8bf (the fopen-failure error path) to log a warning and skip the
// file instead of failing. The fopen check is before any counting/writing, so all 3
// passes (count, directory, data) consistently skip the same missing files.
CodeInjection vpp_skip_missing_file_injection{
    0x0044c8bf,
    [](auto& regs) {
        auto* vstr = reinterpret_cast<VString*>(static_cast<int>(regs.esp) + 0x20);
        const char* filename = vstr->c_str();

        // Only log during the data pass (param_4 != 0, param_5 == 0) to avoid
        // duplicate warnings from count and directory passes
        int file_handle = *reinterpret_cast<int*>(static_cast<int>(regs.esp) + 0x2C);
        int mode = *reinterpret_cast<int*>(static_cast<int>(regs.esp) + 0x30);
        if (file_handle != 0 && mode == 0) {
            // The stock texture filter misses some textures that ship with the game (rock02.tga in
            // ui.vpp, alpinefaction.vpp's), which then get pushed as missing user_maps\textures files
            const char* bare_name = std::strrchr(filename, '\\');
            bare_name = bare_name ? bare_name + 1 : filename;
            if (!shipped_in_packfile(bare_name)) {
                editor_report(EditorReportLevel::warn, "Packfile",
                              std::format("Warning: Skipping missing file {}", filename), true);
            }
        }

        // Redirect to success return (return 1) instead of error return (return 0)
        regs.eip = 0x0044c898;
    }
};

// ─── Mesh file VPP helpers ──────────────────────────────────────────────────

// Add a mesh file to the global VPP file list if it exists on disk.
static void add_mesh_to_vpp_list(const char* filename)
{
    std::string full_path = find_mesh_on_disk(filename);
    if (full_path.empty()) return;

    push_to_pack_list(&vpp_file_list, full_path.c_str());
    xlog::info("VPP: Added mesh file '{}'", full_path);
}

// Find a mesh file on disk and add its referenced textures to the VPP pack list.
static void add_mesh_textures_to_pack_list(void* temp_list, const char* filename)
{
    std::string path = find_mesh_on_disk(filename);
    if (path.empty()) return;

    auto tex_names = string_iends_with(path, ".vfx") ? extract_vfx_texture_names(path.c_str())
                                                     : extract_v3d_texture_names(path.c_str());
    for (const auto& name : tex_names) {
        add_texture_to_pack_list(temp_list, name.c_str());
        xlog::info("VPP: Added texture '{}' from mesh '{}'", name, filename);
    }
}

// Stock VPP packing (FUN_004482c0) gathers textures from geometry, particle emitters,
// and decals, but misses bolt emitters, room effect liquid surfaces,
// Display_Fullscreen_Image events, and the geomod default crater texture.
// Inject at 0x004484FE (before Loop 5's stock-texture filtering) to add the missing
// textures to the temp list so they get the same filtering and path resolution.
// At this point: EBX = CDedLevel*, [ESP+0x30] = temp file list.

CodeInjection vpp_extra_textures_injection{
    0x004484FE,
    [](auto& regs) {
        auto* level = reinterpret_cast<CDedLevel*>(static_cast<int>(regs.ebx));
        auto* temp_list = reinterpret_cast<void*>(static_cast<int>(regs.esp) + 0x30);

        // Iterate master_objects to find assets to add to the packfile
        for (int i = 0; i < level->master_objects.get_size(); i++) {
            auto* obj = level->master_objects[i];

            if (obj->type == DedObjectType::DED_BOLT_EMITTER) {
                auto* bolt = static_cast<DedBoltEmitter*>(obj);
                add_texture_to_pack_list(temp_list, bolt->bitmap.c_str());
            }
            else if (obj->type == DedObjectType::DED_ROOM_EFFECT) {
                auto* room = static_cast<DedRoomEffect*>(obj);
                if (room->effect_type == static_cast<int>(DedRoomEffectType::Liquid)) {
                    add_texture_to_pack_list(temp_list, room->liquid_bitmap.c_str());
                }
            }
            else if (obj->type == DedObjectType::DED_EVENT) {
                auto* evt = static_cast<DedEvent*>(obj);
                if (evt->event_type == af_ded_event_to_int(AlpineDedEventID::Display_Fullscreen_Image) ||
                    evt->event_type == af_ded_event_to_int(AlpineDedEventID::AF_Fullscreen_Image)) {
                    add_texture_to_pack_list(temp_list, evt->str1.c_str());
                }
                else if (evt->event_type == af_ded_event_to_int(AlpineDedEventID::Swap_Textures)) {
                    add_texture_to_pack_list(temp_list, evt->str1.c_str());
                    add_texture_to_pack_list(temp_list, evt->str2.c_str());
                }
                else if (evt->event_type == af_ded_event_to_int(AlpineDedEventID::Switch_Model)
                         || evt->event_type == af_ded_event_to_int(AlpineDedEventID::Set_Debris)) {
                    add_mesh_textures_to_pack_list(temp_list, evt->str1.c_str());
                }
                else if (evt->event_type == af_ded_event_to_int(AlpineDedEventID::Mesh_Set_Texture)) {
                    add_texture_to_pack_list(temp_list, evt->str1.c_str());
                }
                else if (evt->event_type == af_ded_event_to_int(AlpineDedEventID::World_HUD_Sprite)) {
                    add_texture_to_pack_list(temp_list, evt->str1.c_str());
                    add_texture_to_pack_list(temp_list, evt->str2.c_str());
                }
            }
        }

        // Geomod default crater texture (VString at CDedLevel+0x24)
        add_texture_to_pack_list(temp_list, level->geomod_texture.c_str());

        // Textures referenced by custom mesh files (.v3m/.v3c), and per-object texture overrides
        for (auto* mesh : level->GetAlpineLevelProperties().mesh_objects) {
            add_mesh_textures_to_pack_list(temp_list, mesh->mesh_filename.c_str());
            add_mesh_textures_to_pack_list(temp_list, mesh->clutter_props.debris_filename.c_str());
            add_mesh_textures_to_pack_list(temp_list, mesh->clutter_props.corpse_filename.c_str());
            if (mesh->brush_geo_source == 2 && !mesh->collision_mesh_filename.empty()) {
                add_mesh_textures_to_pack_list(temp_list, mesh->collision_mesh_filename.c_str());
            }
            for (const auto& ovr : mesh->texture_overrides) {
                add_texture_to_pack_list(temp_list, ovr.filename.c_str());
            }
        }

        for (auto* region : level->GetAlpineLevelProperties().weather_region_objects) {
            if (region->weather_type == WeatherRegionType::snow) {
                add_texture_to_pack_list(temp_list, region->snow_bitmap.c_str());
            }
        }

        // Corona bitmaps (corona sprite + optional volumetric bitmap)
        for (auto* corona : level->GetAlpineLevelProperties().corona_objects) {
            add_texture_to_pack_list(temp_list, corona->corona_bitmap.c_str());
            add_texture_to_pack_list(temp_list, corona->volumetric_bitmap.c_str());
        }

        // Rope emitter bitmaps, the textures on their decoration meshes, and the corona bitmaps of
        // any decoration slot carrying a glare
        for (auto* rope : level->GetAlpineLevelProperties().rope_emitter_objects) {
            add_texture_to_pack_list(temp_list, rope->bitmap.c_str());
            for (const auto& deco_mesh : rope->deco_meshes) {
                add_mesh_textures_to_pack_list(temp_list, deco_mesh.c_str());
            }
            for (const auto& fx : rope->deco_fx) {
                if (!fx.has_glare()) continue;
                add_texture_to_pack_list(temp_list, fx.glare_bitmap.c_str());
                add_texture_to_pack_list(temp_list, fx.volumetric_bitmap.c_str());
            }
        }

        const auto& props = level->GetAlpineLevelProperties();
        if (props.minimap_enabled) {
            add_texture_to_pack_list(temp_list, props.minimap_bitmap.c_str());
        }

        // Terrain layer, overlay, underside and crater textures, and the textures on decoration meshes
        for (auto* terrain : level->GetAlpineLevelProperties().terrain_objects) {
            for (const auto& layer : terrain->data.layers) {
                add_texture_to_pack_list(temp_list, layer.texture.c_str());
            }
            for (const auto& overlay : terrain->data.overlays) {
                add_texture_to_pack_list(temp_list, overlay.texture.c_str());
            }
            for (const auto& deco : terrain->data.decorations) {
                add_mesh_textures_to_pack_list(temp_list, deco.mesh.c_str());
            }
            add_texture_to_pack_list(temp_list, terrain->data.underside_texture.c_str());
            add_texture_to_pack_list(temp_list, terrain->data.crater_texture.c_str());
        }

        // Last, so it also covers the stock loops' entries and everything added above
        expand_atx_deps_in_pack_list(temp_list);
    }
};

// ─── Sound file VPP helpers ─────────────────────────────────────────────────

// A sound is custom, and packed, when a file of its name sits loose under user_maps\sounds. A name in a mounted
// packfile (audio.vpp, music.vpp, alpinefaction.vpp) ships with the game; any other is reported as not found, with
// `describe_owner()` naming what references it.
template<typename DescribeOwner>
static void add_sound_to_vpp_list(const char* filename, DescribeOwner describe_owner,
                                  std::unordered_set<std::string>& seen)
{
    if (!filename || !filename[0]) return;
    if (!seen.insert(string_to_lower(filename)).second) return;
    if (!has_packable_path(filename)) return;

    std::string full_path = find_sound_on_disk(filename);
    if (!full_path.empty()) {
        push_to_pack_list(&vpp_file_list, full_path.c_str());
        xlog::info("VPP: Added sound file '{}'", full_path);
        return;
    }

    if (shipped_in_packfile(filename)) return;
    editor_report(EditorReportLevel::warn, "Packfile",
                  std::format("Warning: Sound file {} not found ({})", filename, describe_owner()), true);
}

// Every sound file name a level holds: ambient sounds, Play_Sound, Switch and Music_Start events (str1, which
// the game's event loader, RF.exe 0x00462150, hands on as the file name), and each moving group's start, loop,
// stop and close sounds.
static void add_level_sounds_to_vpp_list(CDedLevel& level)
{
    // Picks up sounds and subdirectories added since startup
    reload_custom_sounds();

    std::unordered_set<std::string> seen;
    for (int i = 0; i < level.master_objects.get_size(); i++) {
        auto* obj = level.master_objects[i];
        if (obj->type == DedObjectType::DED_AMBIENT_SOUND) {
            add_sound_to_vpp_list(static_cast<DedAmbientSound*>(obj)->sound_filename.c_str(),
                                  [obj] { return std::format("ambient sound uid {}", obj->uid); }, seen);
        }
        else if (obj->type == DedObjectType::DED_EVENT) {
            auto* evt = static_cast<DedEvent*>(obj);
            if (evt->event_type == af_ded_event_to_int(AlpineDedEventID::Play_Sound)
                || evt->event_type == af_ded_event_to_int(AlpineDedEventID::Switch)
                || evt->event_type == af_ded_event_to_int(AlpineDedEventID::Music_Start)) {
                add_sound_to_vpp_list(evt->str1.c_str(), [obj] { return std::format("event uid {}", obj->uid); },
                                      seen);
            }
        }
    }

    for (int i = 0; i < level.moving_groups.get_size(); i++) {
        const GroupEntry* group = level.moving_groups[i];
        if (!group || !group->is_moving_group()) continue;
        for (const auto& sound : group->keyframes->sounds) {
            add_sound_to_vpp_list(sound.filename.c_str(),
                                  [group] { return std::format("moving group '{}'", group->name.c_str()); }, seen);
        }
    }
}

CodeInjection vpp_extra_files_injection{
    0x004485e2,
    [](auto& regs) {
        auto* level = CDedLevel::Get();
        if (!level) return;

        // Mesh objects: mesh_filename, state_anim, debris, corpse mesh, corpse state anim
        for (auto* mesh : level->GetAlpineLevelProperties().mesh_objects) {
            add_mesh_to_vpp_list(mesh->mesh_filename.c_str());
            add_mesh_to_vpp_list(mesh->state_anim.c_str());
            add_mesh_to_vpp_list(mesh->clutter_props.debris_filename.c_str());
            add_mesh_to_vpp_list(mesh->clutter_props.corpse_filename.c_str());
            add_mesh_to_vpp_list(mesh->clutter_props.corpse_state_anim.c_str());
            if (mesh->brush_geo_source == 2 && !mesh->collision_mesh_filename.empty()) {
                add_mesh_to_vpp_list(mesh->collision_mesh_filename.c_str());
            }
        }

        // Rope emitter decoration meshes
        for (auto* rope : level->GetAlpineLevelProperties().rope_emitter_objects) {
            for (const auto& deco_mesh : rope->deco_meshes) {
                add_mesh_to_vpp_list(deco_mesh.c_str());
            }
        }

        // Terrain decoration meshes
        for (auto* terrain : level->GetAlpineLevelProperties().terrain_objects) {
            for (const auto& deco : terrain->data.decorations) {
                add_mesh_to_vpp_list(deco.mesh.c_str());
            }
        }

        // Events: Switch_Model and Set_Debris (str1=mesh), Play_Animation (str1=anim),
        // Mesh_Animate (str1=anim)
        for (int i = 0; i < level->master_objects.get_size(); i++) {
            auto* obj = level->master_objects[i];
            if (obj->type != DedObjectType::DED_EVENT) continue;
            auto* evt = static_cast<DedEvent*>(obj);
            if (evt->event_type == af_ded_event_to_int(AlpineDedEventID::Switch_Model)
                || evt->event_type == af_ded_event_to_int(AlpineDedEventID::Set_Debris)) {
                add_mesh_to_vpp_list(evt->str1.c_str());
            }
            else if (evt->event_type == af_ded_event_to_int(AlpineDedEventID::Play_Animation)) {
                add_mesh_to_vpp_list(evt->str1.c_str());
            }
            else if (evt->event_type == af_ded_event_to_int(AlpineDedEventID::Mesh_Animate)) {
                add_mesh_to_vpp_list(evt->str1.c_str());
            }
        }

        add_level_sounds_to_vpp_list(*level);
    }
};

// ─── Texture reload ─────────────────────────────────────────────────────────

// Reloads `name` into `entry`, whose checksum the caller has inverted so bm_load's name lookup
// skips it (it stays in its hash slot, preserving the linear probe chain). bm_load builds a real
// entry, and its data is copied into `entry` so the handle stays valid. False, with `entry`
// visible again, when the file still can't be read.
static bool reload_entry_in_place(BitmapEntry* entry, int original_checksum, const char* name)
{
    const int new_handle = BitmapEntry::load(name, -1);
    const int new_index = new_handle >= 0 ? rf::bm::get_cache_slot(new_handle) : -1;
    BitmapEntry* new_entry = new_index >= 0 ? &BitmapEntry::entries[new_index] : nullptr;
    if (!new_entry || new_entry == entry) {
        entry->name_checksum = original_checksum;
        return false;
    }

    // bm_load never fails: an unreadable file gets another TYPE_USER placeholder.
    if (new_entry->bm_type == BitmapEntry::TYPE_USER) {
        entry->name_checksum = original_checksum;
        new_entry->name_checksum = ~original_checksum;
        return false;
    }

    // Preserve the old entry's handle and linked list pointers
    const int old_handle = entry->handle;
    BitmapEntry* old_next = entry->next;
    BitmapEntry* old_prev = entry->prev;
    // Name and checksum come across from the new entry: same filename, same values.
    memcpy(entry, new_entry, sizeof(BitmapEntry));
    entry->handle = old_handle;
    entry->next = old_next;
    entry->prev = old_prev;

    // Invalidate the new entry's checksum so hash lookups find the old entry, not this one
    new_entry->name_checksum = ~original_checksum;

    // Invalidate the cached D3D texture so the renderer recreates it from the real data
    gr_d3d_mark_texture_dirty(old_handle);
    return true;
}

// Reload bitmap manager placeholder entries in-place.
// bm_load creates a 32x32 TYPE_USER placeholder with the texture's name on failure
// (FUN_004bc9c0). Subsequent loads find this cached entry and never retry from disk.
// We reload each placeholder by: temporarily hiding it from name lookup, calling bm_load
// to create a real entry, then copying the real metadata into the original entry so the
// handle stays valid (faces referencing it don't break on save/load).
static void reload_bm_placeholders()
{
    int table_size = rf::bm::num_cache_slots + 1;

    struct Placeholder {
        BitmapEntry* entry;
        int original_checksum;
        char name[32];
    };
    std::vector<Placeholder> placeholders;

    // Phase 1: Find placeholders and temporarily invalidate their checksums
    // so bm_load won't match them during the reload pass
    for (int i = 0; i < table_size; i++) {
        BitmapEntry* entry = BitmapEntry::hash_table[i];
        if (!entry) continue;

        // Placeholder signature: TYPE_USER + FORMAT_888_RGB + 32x32
        // bm_create sets width/height (not orig_width/orig_height)
        if (entry->bm_type == BitmapEntry::TYPE_USER &&
            entry->format == BitmapEntry::FORMAT_888_RGB &&
            entry->width == 32 && entry->height == 32) {
            if (entry->name[0] == '\0') continue; // skip already-cleared entries

            Placeholder ph;
            ph.entry = entry;
            ph.original_checksum = entry->name_checksum;
            memcpy(ph.name, entry->name, 31);
            ph.name[31] = '\0';

            // Invalidate checksum so bm_load's name lookup skips this entry.
            // Entry stays in its hash slot (preserving linear probe chain).
            entry->name_checksum = ~ph.original_checksum;

            placeholders.push_back(ph);
        }
    }

    // Phase 2: Reload each placeholder in-place
    int reloaded = 0;
    for (auto& ph : placeholders) {
        if (!reload_entry_in_place(ph.entry, ph.original_checksum, ph.name)) {
            continue;
        }
        xlog::info("Reloaded bmpman placeholder '{}' in-place (handle=0x{:x})", ph.name, ph.entry->handle);
        reloaded++;
    }

    if (!placeholders.empty()) {
        xlog::info("Reloaded {}/{} bmpman placeholder(s)", reloaded, placeholders.size());
    }
}

bool reload_bitmap_in_place(const char* filename)
{
    const int handle = BitmapEntry::find(filename);
    if (handle < 0) {
        // Not resident, so a plain load already reads the file from disk.
        BitmapEntry::load(filename, -1);
        return true;
    }
    const int index = rf::bm::get_cache_slot(handle);
    if (index < 0) return false;
    BitmapEntry* entry = &BitmapEntry::entries[index];
    const int checksum = entry->name_checksum;
    entry->name_checksum = ~checksum;
    return reload_entry_in_place(entry, checksum, filename);
}

void reload_custom_textures()
{
    if (!g_texture_manager) return;

    auto* category_array = &g_texture_manager->categories;

    for (int i = 0; i < category_array->get_size(); i++) {
        const char* name = (*category_array)[i]->name.c_str();
        if (category_name_starts_with(name)) {
            int handle = (*category_array)[i]->path_handle;
            if (handle >= 0) {
                file_scan_path(handle);
            }
        }
    }

    // Drop cached redirects so edits to .atx files (or new sibling files dropped on disk
    // mid-session) are re-resolved on the next read_header.
    clear_editor_bitmap_redirects();

    // Reload placeholder bitmap entries so previously-failed textures load from disk.
    // This updates entries in-place (same handle) so faces referencing them stay valid.
    reload_bm_placeholders();
}

void ApplyTexturesPatches() {
    texture_browser_validator_hook.install();
    texture_sidebar_validator_hook.install();
    texture_browser_thumbnail_size_injection.install();
    texture_browser_min_columns_injection.install();
    write_mem<int8_t>(0x0046fcce, texture_browser_one_to_one);
    texture_browser_init_dialog_hook.install();
    custom_category_check_hook.install();
    sidebar_custom_texture_path_injection.install();
    sidebar_init_empty_list_fix.install();
    texture_reverse_lookup_fix.install();
    texture_reverse_lookup_index_fix.install();
    vpp_texture_path_fix.install();
    vpp_extra_textures_injection.install();
    vpp_skip_missing_file_injection.install();
    vpp_clear_log_injection.install();
    vpp_extra_files_injection.install();
    folder_group_build_skip_custom_subdirs.install();
    texture_refresh_all_iterate_custom_injection.install();
    texture_refresh_dedup_master_injection.install();
}
