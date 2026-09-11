#include <windows.h>
#include <shellapi.h>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <format>
#include <patch_common/MemUtils.h>
#include <patch_common/FunHook.h>
#include <xlog/xlog.h>
#include "headless_bake.h"
#include "level.h"
#include "vtypes.h"

namespace
{

constexpr int max_suppressed_dialogs = 50;

// Painting the perspective viewport re-orthonormalises its camera matrix, and the level info
// section stores all four cameras, so how many frames a bake happened to paint changed the saved
// file by an ULP. Captured from the freshly loaded level and put back before the save.
constexpr int viewport_count = 4;

struct ViewCamera {
    Matrix3 orient;
    Vector3 pos;
    bool valid;
};
ViewCamera g_view_cameras[viewport_count];

bool g_active = false;
bool g_bake_started = false;
// CDedDoc keeps its document object whether or not the level behind it parsed, so "the doc is
// there" is not evidence that anything loaded; the load's own return value is.
bool g_load_reported = false;
bool g_load_ok = false;
std::string g_input_path;
std::string g_output_path;
std::string g_log_path;
std::string g_init_error;
DWORD g_start_ticks = 0;
DWORD g_first_idle_ticks = 0;
int g_dialogs_suppressed = 0;

std::string narrow(const wchar_t* wide)
{
    int len = WideCharToMultiByte(CP_ACP, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) {
        return {};
    }
    std::string result(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_ACP, 0, wide, -1, result.data(), len, nullptr, nullptr);
    return result;
}

void bake_log(std::string_view line)
{
    xlog::info("[bake] {}", line);
    if (g_log_path.empty()) {
        return;
    }
    FILE* f = std::fopen(g_log_path.c_str(), "a");
    if (!f) {
        return;
    }
    std::fprintf(f, "[%9.3f] %.*s\n", (GetTickCount() - g_start_ticks) / 1000.0,
                 static_cast<int>(line.size()), line.data());
    std::fclose(f);
}

EditorViewData* view_camera(int index)
{
    auto* main_frame = static_cast<CMainFrame*>(GetMainFrame());
    if (!main_frame) {
        return nullptr;
    }
    auto* viewport = static_cast<EditorViewport*>(main_frame->views[index]);
    return viewport ? viewport->view_data : nullptr;
}

void capture_view_cameras()
{
    for (int i = 0; i < viewport_count; ++i) {
        const EditorViewData* view = view_camera(i);
        g_view_cameras[i].valid = view != nullptr;
        if (view) {
            g_view_cameras[i].orient = view->camera_orient;
            g_view_cameras[i].pos = view->camera_pos;
        }
    }
}

void restore_view_cameras()
{
    for (int i = 0; i < viewport_count; ++i) {
        EditorViewData* view = g_view_cameras[i].valid ? view_camera(i) : nullptr;
        if (view) {
            view->camera_orient = g_view_cameras[i].orient;
            view->camera_pos = g_view_cameras[i].pos;
        }
    }
}

void bake_finish(int code)
{
    bake_log(std::format("done rc={}", code));
    ExitProcess(static_cast<UINT>(code));
}

int WINAPI MessageBoxA_headless(HWND, LPCSTR text, LPCSTR caption, UINT type)
{
    bake_log(std::format("dialog suppressed: [{}] {}", caption ? caption : "", text ? text : ""));
    if (++g_dialogs_suppressed > max_suppressed_dialogs) {
        bake_log("too many dialogs, giving up");
        bake_finish(3);
    }
    switch (type & MB_TYPEMASK) {
        case MB_YESNO:
        case MB_YESNOCANCEL:
            return IDYES;
        case MB_OKCANCEL:
            return IDOK;
        case MB_RETRYCANCEL:
        case MB_ABORTRETRYIGNORE:
            return IDCANCEL;
        default:
            return IDOK;
    }
}

void run_bake()
{
    if (!g_init_error.empty()) {
        bake_log(std::format("error: {}", g_init_error));
        bake_finish(1);
    }

    auto* main_frame = static_cast<CMainFrame*>(GetMainFrame());
    CDedDoc* doc = main_frame ? main_frame->doc : nullptr;
    if (!doc) {
        bake_log("error: level did not load");
        bake_finish(2);
    }
    if (!g_load_reported) {
        bake_log(std::format("error: {} was never opened", g_input_path));
        bake_finish(2);
    }
    if (!g_load_ok) {
        bake_log(std::format("error: {} failed to load", g_input_path));
        bake_finish(2);
    }
    bake_log(std::format("loaded {}", g_input_path));

    DWORD bake_begin = GetTickCount();
    bake_log("baking");
    main_frame->OnCalculateLighting();
    bake_log(std::format("baked in {:.1f}s", (GetTickCount() - bake_begin) / 1000.0));

    DWORD save_begin = GetTickCount();
    restore_view_cameras();
    if (!doc->LoadSaveLevel(g_output_path.c_str(), 0, 0)) {
        bake_log(std::format("error: failed to save {}", g_output_path));
        bake_finish(4);
    }

    WIN32_FILE_ATTRIBUTE_DATA attrs{};
    if (!GetFileAttributesExA(g_output_path.c_str(), GetFileExInfoStandard, &attrs)) {
        bake_log(std::format("error: {} missing after save", g_output_path));
        bake_finish(4);
    }
    bake_log(std::format("saved {} ({} bytes) in {:.1f}s", g_output_path, attrs.nFileSizeLow,
                         (GetTickCount() - save_begin) / 1000.0));
    bake_finish(0);
}

// the launcher escapes its arguments by doubling every backslash and Win32 collapses those runs, so
// the path RED opens is not the string the command line carried
bool path_is_bake_input(const char* path)
{
    auto take = [](const char*& p) {
        const auto c = static_cast<unsigned char>(*p++);
        if (c == '\\' || c == '/') {
            while (*p == '\\' || *p == '/') {
                ++p;
            }
            return static_cast<int>('\\');
        }
        return std::tolower(c);
    };
    const char* a = path;
    const char* b = g_input_path.c_str();
    while (*a && *b) {
        if (take(a) != take(b)) {
            return false;
        }
    }
    return !*a && !*b;
}

// spelling is not identity, do not save over input
std::string canonical_path(const std::string& path)
{
    char buf[MAX_PATH];
    const DWORD len = GetFullPathNameA(path.c_str(), static_cast<DWORD>(sizeof(buf)), buf, nullptr);
    if (len == 0 || len >= sizeof(buf)) {
        return path;
    }
    return buf;
}

char __fastcall CDedDoc_LoadSaveLevel_new(void* self, int edx, const char* path, int is_load,
                                          int is_autosave);
FunHook<char __fastcall(void*, int, const char*, int, int)> CDedDoc_LoadSaveLevel_hook{
    0x0041CCE0, CDedDoc_LoadSaveLevel_new}; // CDedDoc::LoadSaveLevel

char __fastcall CDedDoc_LoadSaveLevel_new(void* self, int edx, const char* path, int is_load,
                                          int is_autosave)
{
    char result = CDedDoc_LoadSaveLevel_hook.call_target(self, edx, path, is_load, is_autosave);
    // only the load of the level named on the command line decides the bake's fate
    if (is_load && !is_autosave && path && path_is_bake_input(path)) {
        g_load_reported = true;
        g_load_ok = result != 0;
        if (result) {
            capture_view_cameras();
        }
    }
    return result;
}

int __fastcall CEditorApp_OnIdle_new(void* self, int edx, int count);
FunHook<int __fastcall(void*, int, int)> CEditorApp_OnIdle_hook{0x00482F00, CEditorApp_OnIdle_new};

int __fastcall CEditorApp_OnIdle_new(void* self, int edx, int count)
{
    if (!g_bake_started) {
        if (!g_first_idle_ticks) {
            g_first_idle_ticks = GetTickCount() | 1;
        }
        // The document is opened during InitInstance, so it is already there by the first idle;
        // the short delay only lets a failed open finish reporting itself.
        if (GetTickCount() - g_first_idle_ticks >= 500) {
            g_bake_started = true;
            run_bake();
        }
    }
    return CEditorApp_OnIdle_hook.call_target(self, edx, count);
}

void parse_args()
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return;
    }
    for (int i = 1; i < argc; ++i) {
        std::wstring_view arg = argv[i];
        if (arg == L"-bake" && i + 1 < argc) {
            g_input_path = narrow(argv[++i]);
        }
        else if (arg == L"-bakeout" && i + 1 < argc) {
            g_output_path = narrow(argv[++i]);
        }
    }
    LocalFree(argv);
    g_active = !g_input_path.empty();
}

} // namespace

bool headless_bake_active()
{
    return g_active;
}

const char* headless_bake_input_path()
{
    return g_input_path.c_str();
}

void ApplyHeadlessBakePatches()
{
    parse_args();
    if (!g_active) {
        return;
    }

    g_start_ticks = GetTickCount();
    if (!g_output_path.empty()) {
        g_log_path = g_output_path + ".log";
        DeleteFileA(g_log_path.c_str());
    }

    if (g_output_path.empty()) {
        g_init_error = "-bake requires -bakeout <output.rfl>";
    }
    else if (_stricmp(canonical_path(g_input_path).c_str(),
                      canonical_path(g_output_path).c_str()) == 0) {
        g_init_error = "-bakeout must differ from the -bake input";
    }
    else if (GetFileAttributesA(g_input_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        g_init_error = std::format("input {} not found", g_input_path);
    }

    bake_log(std::format("started in={} out={}", g_input_path, g_output_path));

    // RED.exe IAT slot for USER32!MessageBoxA (call sites 0x0041CD58, 0x0041CD9B)
    write_mem_ptr(0x005545F4, &MessageBoxA_headless);
    CDedDoc_LoadSaveLevel_hook.install();
    CEditorApp_OnIdle_hook.install();
}
