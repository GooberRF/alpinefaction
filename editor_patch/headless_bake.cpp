#include <windows.h>
#include <shellapi.h>
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

void* GetMainFrame();

namespace
{

// RED.exe IAT slot for USER32!MessageBoxA (call sites 0x0041CD58, 0x0041CD9B)
constexpr uintptr_t messageboxa_iat = 0x005545F4;

// CDedDoc::LoadSaveLevel(this, path, is_load, is_autosave), __thiscall, RET 0xC
constexpr uintptr_t doc_load_save_level = 0x0041CCE0;

// CMainFrame::OnCalculateLightingCmd (shadowed variant), __thiscall. This is the menu handler,
// which rebuilds the lightmap surfaces (0x00448CA0) before baking (0x00448F20); calling the bake
// alone leaves the surfaces from the last build, so mover solids never get any.
constexpr uintptr_t calculate_lighting_cmd = 0x00449680;

constexpr int max_suppressed_dialogs = 50;

// CMainFrame holds its four viewports at +0xbc (FUN_004835b0) and each keeps its camera in the
// EditorViewData at +0x54: orientation at +0x04, position at +0x28. Painting the perspective one
// re-orthonormalises that matrix, and the level info section stores all four, so how many frames a
// bake happened to paint changed the saved file by an ULP. Captured from the freshly loaded level
// and put back before the save.
constexpr uintptr_t editor_app = 0x006F9DA0;
constexpr int viewport_count = 4;

struct ViewCamera {
    float orient[9];
    float pos[3];
    bool valid;
};
ViewCamera g_view_cameras[viewport_count];

bool g_active = false;
bool g_bake_started = false;
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

float* view_camera(int index)
{
    auto* main_frame = struct_field_ref<std::byte*>(reinterpret_cast<void*>(editor_app), 0xC8);
    if (!main_frame) {
        return nullptr;
    }
    auto* viewport = struct_field_ref<std::byte*>(main_frame, 0xbc + index * 4);
    if (!viewport) {
        return nullptr;
    }
    auto* view_data = struct_field_ref<std::byte*>(viewport, 0x54);
    return view_data ? reinterpret_cast<float*>(view_data + 4) : nullptr;
}

void capture_view_cameras()
{
    for (int i = 0; i < viewport_count; ++i) {
        const float* camera = view_camera(i);
        g_view_cameras[i].valid = camera != nullptr;
        if (camera) {
            std::memcpy(g_view_cameras[i].orient, camera, sizeof(g_view_cameras[i].orient));
            std::memcpy(g_view_cameras[i].pos, camera + 9, sizeof(g_view_cameras[i].pos));
        }
    }
}

void restore_view_cameras()
{
    for (int i = 0; i < viewport_count; ++i) {
        float* camera = g_view_cameras[i].valid ? view_camera(i) : nullptr;
        if (camera) {
            std::memcpy(camera, g_view_cameras[i].orient, sizeof(g_view_cameras[i].orient));
            std::memcpy(camera + 9, g_view_cameras[i].pos, sizeof(g_view_cameras[i].pos));
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

    void* main_frame = GetMainFrame();
    void* doc = main_frame ? struct_field_ref<void*>(main_frame, 0xD0) : nullptr;
    if (!doc) {
        bake_log("error: level did not load");
        bake_finish(2);
    }
    bake_log(std::format("loaded {}", g_input_path));

    DWORD bake_begin = GetTickCount();
    bake_log("baking");
    AddrCaller{calculate_lighting_cmd}.this_call(main_frame);
    bake_log(std::format("baked in {:.1f}s", (GetTickCount() - bake_begin) / 1000.0));

    DWORD save_begin = GetTickCount();
    restore_view_cameras();
    if (!AddrCaller{doc_load_save_level}.this_call<char>(doc, g_output_path.c_str(), 0, 0)) {
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

char __fastcall CDedDoc_LoadSaveLevel_new(void* self, int edx, const char* path, int is_load,
                                          int is_autosave);
FunHook<char __fastcall(void*, int, const char*, int, int)> CDedDoc_LoadSaveLevel_hook{
    doc_load_save_level, CDedDoc_LoadSaveLevel_new};

char __fastcall CDedDoc_LoadSaveLevel_new(void* self, int edx, const char* path, int is_load,
                                          int is_autosave)
{
    char result = CDedDoc_LoadSaveLevel_hook.call_target(self, edx, path, is_load, is_autosave);
    if (is_load && result) {
        capture_view_cameras();
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
    else if (_stricmp(g_input_path.c_str(), g_output_path.c_str()) == 0) {
        g_init_error = "-bakeout must differ from the -bake input";
    }
    else if (GetFileAttributesA(g_input_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        g_init_error = std::format("input {} not found", g_input_path);
    }

    bake_log(std::format("started in={} out={}", g_input_path, g_output_path));

    write_mem_ptr(messageboxa_iat, &MessageBoxA_headless);
    CDedDoc_LoadSaveLevel_hook.install();
    CEditorApp_OnIdle_hook.install();
}
