#include <cstddef>
#include <cstring>
#include <functional>
#include <filesystem>
#include <string_view>
#include <windows.h>
#include <shellapi.h>
#include <vector>
#include <memory>
#include <iomanip>
#include <sstream>
#include <string>
#include <common/version/version.h>
#include <common/config/BuildConfig.h>
#include <common/utils/os-utils.h>
#include <xlog/xlog.h>
#include <xlog/ConsoleAppender.h>
#include <xlog/FileAppender.h>
#include <xlog/Win32Appender.h>
#include <patch_common/MemUtils.h>
#include <patch_common/CallHook.h>
#include <patch_common/FunHook.h>
#include <patch_common/AsmWriter.h>
#include <patch_common/CodeInjection.h>
#include <crash_handler_stub.h>
#include "../game_patch/rf/os/array.h"
#include "exports.h"
#include "resources.h"
#include "mfc_types.h"
#include "vtypes.h"
#include "event.h"

#define LAUNCHER_FILENAME "AlpineFactionLauncher.exe"
HMODULE g_module;
static std::string g_launcher_pathname;

constexpr size_t max_texture_name_len = 31;

bool g_skip_wnd_set_text = false;

static bool g_is_saving_af_version = true;
static bool g_skip_legacy_level_warning = false;

static const auto g_editor_app = reinterpret_cast<std::byte*>(0x006F9DA0);

static auto& LogDlg_Append = addr_as_ref<int(void* self, const char* format, ...)>(0x00444980);


void *GetMainFrame()
{
    return struct_field_ref<void*>(g_editor_app, 0xC8);
}

void *GetLogDlg()
{
    return struct_field_ref<void*>(GetMainFrame(), 692);
}

HWND GetMainFrameHandle()
{
    auto* main_frame = struct_field_ref<CWnd*>(g_editor_app, 0xC8);
    return WndToHandle(main_frame);
}

bool get_is_saving_af_version()
{
    return g_is_saving_af_version;
}

void OpenLevel(const char* level_path)
{
    void* doc_manager = *reinterpret_cast<void**>(g_editor_app + 0x80);
    void** doc_manager_vtbl = *reinterpret_cast<void***>(doc_manager);
    using CDocManager_OpenDocumentFile_Ptr = int(__thiscall*)(void* this_, LPCSTR path);
    auto DocManager_OpenDocumentFile = reinterpret_cast<CDocManager_OpenDocumentFile_Ptr>(doc_manager_vtbl[7]);
    DocManager_OpenDocumentFile(doc_manager, level_path);
}

CodeInjection CMainFrame_PreCreateWindow_injection{
    0x0044713C,
    [](auto& regs) {
        auto& cs = addr_as_ref<CREATESTRUCTA>(regs.eax);
        cs.style |= WS_MAXIMIZEBOX|WS_THICKFRAME;
        cs.dwExStyle |= WS_EX_ACCEPTFILES;
    },
};

CodeInjection CEditorApp_InitInstance_additional_file_paths_injection{
    0x0048290D,
    []() {
        // Load v3m files from more localizations instead of only VPP packfiles
        auto file_add_path = addr_as_ref<int(const char *path, const char *exts, bool cd)>(0x004C3950);
        file_add_path("red\\meshes", ".v3m .vfx", false);
        file_add_path("user_maps\\meshes", ".v3m .vfx", false);
    },
};

CodeInjection CEditorApp_InitInstance_open_level_injection{
    0x00482BF1,
    []() {
        static auto& argv = addr_as_ref<char**>(0x01DBF8E4);
        static auto& argc = addr_as_ref<int>(0x01DBF8E0);
        const char* level_param = nullptr;
        for (int i = 1; i < argc; ++i) {
            xlog::trace("argv[{}] = {}", i, argv[i]);
            std::string_view arg = argv[i];
            if (arg == "-level") {
                ++i;
                if (i < argc) {
                    level_param = argv[i];
                }
            }
        }
        if (level_param) {
            OpenLevel(level_param);
        }
    },
};

CodeInjection CWnd_CreateDlg_injection{
    0x0052F112,
    [](auto& regs) {
        auto& hCurrentResourceHandle = regs.esi;
        auto lpszTemplateName = addr_as_ref<LPCSTR>(regs.esp);
        // Dialog resource customizations:
        // - 136: main window top bar (added tool buttons)
        if (lpszTemplateName == MAKEINTRESOURCE(IDD_MAIN_FRAME_TOP_BAR)) {
            hCurrentResourceHandle = reinterpret_cast<int>(g_module);
        }
    },
};

CodeInjection CDialog_DoModal_injection{
    0x0052F461,
    [](auto& regs) {
        auto& hCurrentResourceHandle = regs.ebx;
        auto lpszTemplateName = addr_as_ref<LPCSTR>(regs.esp);
        // Customize:
        // - 148: trigger properties dialog
        if (lpszTemplateName == MAKEINTRESOURCE(IDD_TRIGGER_PROPERTIES) ||
            lpszTemplateName == MAKEINTRESOURCE(IDD_LEVEL_PROPERTIES)
        ) {
            hCurrentResourceHandle = reinterpret_cast<int>(g_module);
        }
    },
};

HMENU WINAPI LoadMenuA_new(HINSTANCE hInstance, LPCSTR lpMenuName) {
    HMENU result = LoadMenuA(g_module, lpMenuName);
    if (!result) {
        result = LoadMenuA(hInstance, lpMenuName);
    }
    return result;
}

HICON WINAPI LoadIconA_new(HINSTANCE hInstance, LPCSTR lpIconName) {
    HICON result = LoadIconA(g_module, lpIconName);
    if (!result) {
        result = LoadIconA(hInstance, lpIconName);
    }
    return result;
}

HACCEL WINAPI LoadAcceleratorsA_new(HINSTANCE hInstance, LPCSTR lpTableName) {
    HACCEL result = LoadAcceleratorsA(g_module, lpTableName);
    if (!result) {
        result = LoadAcceleratorsA(hInstance, lpTableName);
    }
    return result;
}

CodeInjection CDedLevel_OpenRespawnPointProperties_injection{
    0x00404CB8,
    [](auto& regs) {
        // Fix wrong respawn point reference being used when copying booleans from checkboxes
        regs.esi = regs.ebx;
    },
};

using wnd_set_text_type = int __fastcall(void*, void*, const char*);
extern CallHook<wnd_set_text_type> log_append_wnd_set_text_hook;
int __fastcall log_append_wnd_set_text_new(void* self, void* edx, const char* str)
{
    if (g_skip_wnd_set_text) {
        return std::strlen(str);
    }
    return log_append_wnd_set_text_hook.call_target(self, edx, str);
}
CallHook<wnd_set_text_type> log_append_wnd_set_text_hook{0x004449C6, log_append_wnd_set_text_new};

using group_mode_handle_selection_type = void __fastcall(void* self);
extern FunHook<group_mode_handle_selection_type> group_mode_handle_selection_hook;
void __fastcall group_mode_handle_selection_new(void* self)
{
    g_skip_wnd_set_text = true;
    group_mode_handle_selection_hook.call_target(self);
    g_skip_wnd_set_text = false;
    // TODO: print
    LogDlg_Append(GetLogDlg(), "");
}
FunHook<group_mode_handle_selection_type> group_mode_handle_selection_hook{0x00423460, group_mode_handle_selection_new};

using brush_mode_handle_selection_type = void __fastcall(void* self);
extern FunHook<brush_mode_handle_selection_type> brush_mode_handle_selection_hook;
void __fastcall brush_mode_handle_selection_new(void* self)
{
    g_skip_wnd_set_text = true;
    brush_mode_handle_selection_hook.call_target(self);
    g_skip_wnd_set_text = false;
    // TODO: print
    LogDlg_Append(GetLogDlg(), "");

}
FunHook<brush_mode_handle_selection_type> brush_mode_handle_selection_hook{0x0043F430, brush_mode_handle_selection_new};

void __fastcall DedLight_UpdateLevelLight(void *this_);
FunHook DedLight_UpdateLevelLight_hook{
    0x00453200,
    DedLight_UpdateLevelLight,
};
void __fastcall DedLight_UpdateLevelLight(void *this_)
{
    auto& this_is_enabled = struct_field_ref<bool>(this_, 0xD5);
    auto& level_light = struct_field_ref<void*>(this_, 0xD8);
    auto& level_light_is_enabled = struct_field_ref<bool>(level_light, 0x91);
    level_light_is_enabled = this_is_enabled;
    DedLight_UpdateLevelLight_hook.call_target(this_);
}

struct CTextureBrowserDialog;
rf::String * __fastcall CTextureBrowserDialog_GetFolderName(CTextureBrowserDialog *this_, int edx, rf::String *folder_name);
FunHook CTextureBrowserDialog_GetFolderName_hook{
    0x00471260,
    CTextureBrowserDialog_GetFolderName,
};
rf::String * __fastcall CTextureBrowserDialog_GetFolderName(CTextureBrowserDialog *this_, int edx, rf::String *folder_name)
{
    auto& texture_browser_folder_index = addr_as_ref<int>(0x006CA404);
    if (texture_browser_folder_index > 0) {
        return CTextureBrowserDialog_GetFolderName_hook.call_target(this_, edx, folder_name);
    }
    folder_name->buf = nullptr;
    folder_name->max_len = 0;
    return folder_name;
}

CodeInjection CCutscenePropertiesDialog_ct_crash_fix{
    0x00458A84,
    [](auto& regs) {
        void* this_ = regs.esi;
        auto& this_num_shots = struct_field_ref<int>(this_, 0x60);
        this_num_shots = 0;
    },
};

struct CFrameWnd
{
    char padding[0xBC];
};
static_assert(sizeof(CFrameWnd) == 0xBC);

struct CMainFrame : CFrameWnd
{
    void* views[4];
    void* unk_view;
    CDocument* doc;
    VString field_D4;
    char dialog_bar[0x88]; // CDialogBar
    char status_bar[0x7C]; // CStatusBar
    char splitter[0x26C]; // CDedSplitterWnd
    float grid_size_available_values[12];
    float rotate_by_available_vals[8];
    int texture_grid_size_available_values[8];
    int grid_size_index;
    int rotate_by_index;
    int texture_grid_size_index;
    float camera_speed_allowed_values[6];
    int camera_speed_index;
    float grid_brightness;
    int custom_colors[16];
    int favorite_textures[8];
    bool play_no_tnl;
    char padding[3];
    void* preferences_dlg;
};
static_assert(sizeof(CMainFrame) == 0x550);

static auto RedrawEditorAfterModification = addr_as_ref<int __cdecl()>(0x00483560);

void* GetLevelFromMainFrame(CWnd* main_frame)
{
    auto* doc = struct_field_ref<void*>(main_frame, 0xD0);
    return &struct_field_ref<int>(doc, 0x60);
}

void CMainFrame_OpenHelp(CWnd* this_)
{
    ShellExecuteA(WndToHandle(this_), "open", "https://redfactionwiki.com/wiki/RF1_Editing_Main_Page", nullptr, nullptr, SW_SHOW);
}

void CMainFrame_OpenHotkeysHelp(CWnd* this_)
{
    ShellExecuteA(WndToHandle(this_), "open", "https://redfactionwiki.com/wiki/RED_Hotkey_Reference", nullptr, nullptr, SW_SHOW);
}

void CMainFrame_OpenAlpineHelp(CWnd* this_)
{
    ShellExecuteA(WndToHandle(this_), "open", "https://redfactionwiki.com/wiki/Alpine_Level_Design", nullptr, nullptr, SW_SHOW);
}

void CMainFrame_HideAllObjects(CWnd* this_)
{
    AddrCaller{0x0042DCA0}.this_call(GetLevelFromMainFrame(this_));
    RedrawEditorAfterModification();
}

void CMainFrame_ShowAllObjects(CWnd* this_)
{
    AddrCaller{0x0042DDA0}.this_call(GetLevelFromMainFrame(this_));
    RedrawEditorAfterModification();
}

void CMainFrame_HideSelected(CWnd* this_)
{
    AddrCaller{0x0042DBF0}.this_call(GetLevelFromMainFrame(this_));
    RedrawEditorAfterModification();
}

void CMainFrame_SelectObjectByUid(CWnd* this_)
{
    AddrCaller{0x0042E720}.this_call(GetLevelFromMainFrame(this_));
    RedrawEditorAfterModification();
}

void CMainFrame_InvertSelection(CWnd* this_)
{
    AddrCaller{0x0042E890}.this_call(GetLevelFromMainFrame(this_));
    RedrawEditorAfterModification();
}

void CMainFrame_PlayMulti(CWnd* this_)
{
    xlog::warn("Full Launcher Path: {}", g_launcher_pathname);
    CMainFrame* mainframe = reinterpret_cast<CMainFrame*>(GetMainFrame());
    if (!mainframe || !mainframe->doc) {
        xlog::error("Failed to get CMainFrame or document!");
        return;
    }

    std::string filename = std::filesystem::path(mainframe->doc->_d.m_strPathName.c_str()).filename().generic_string();
    if (filename.empty()) {
        xlog::error("No valid filename found!");
        return;
    }

    xlog::warn("Launching game with filename: {}", filename);

    std::string commandLine = g_launcher_pathname + " -game -levelm \"" + filename + "\"";
    xlog::warn("Running: {}", commandLine);

    STARTUPINFOA startupInfo{};
    PROCESS_INFORMATION processInfo{};
    startupInfo.cb = sizeof(startupInfo);

    if (!CreateProcessA(g_launcher_pathname.c_str(),
                        commandLine.data(),
                        nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &startupInfo, &processInfo)) {
        xlog::error("Failed to launch game! Error Code: {}", GetLastError());
    }
}

BOOL __fastcall CMainFrame_OnCmdMsg(CWnd* this_, int, UINT nID, int nCode, void* pExtra, void* pHandlerInfo)
{
    constexpr int CN_COMMAND = 0;

    if (nCode == CN_COMMAND) {
        std::function<void()> handler;
        switch (nID) {
            case ID_WIKI_EDITING_MAIN_PAGE:
                handler = std::bind(CMainFrame_OpenHelp, this_);
                break;
            case ID_WIKI_HOTKEYS:
                handler = std::bind(CMainFrame_OpenHotkeysHelp, this_);
                break;
            case ID_WIKI_ALPINE_HELP:
                handler = std::bind(CMainFrame_OpenAlpineHelp, this_);
                break;
            case ID_HIDE_ALL_OBJECTS:
                handler = std::bind(CMainFrame_HideAllObjects, this_);
                break;
            case ID_SHOW_ALL_OBJECTS:
                handler = std::bind(CMainFrame_ShowAllObjects, this_);
                break;
            case ID_HIDE_SELECTED:
                handler = std::bind(CMainFrame_HideSelected, this_);
                break;
            case ID_SELECT_OBJECT_BY_UID:
                handler = std::bind(CMainFrame_SelectObjectByUid, this_);
                break;
            case ID_INVERT_SELECTION:
                handler = std::bind(CMainFrame_InvertSelection, this_);
                break;
            case ID_PLAY_MULTI:
                handler = std::bind(CMainFrame_PlayMulti, this_);
                break;
        }

        if (handler) {
            // Tell MFC that this command has a handler so it does not disable menu item
            if (pHandlerInfo) {
                // It seems handler info is not used but it's better to initialize it just in case
                ZeroMemory(pHandlerInfo, 8);
            }
            else {
                // Run the handler
                handler();
            }
            return TRUE;
        }
    }
    return AddrCaller{0x00540C5B}.this_call<BOOL>(this_, nID, nCode, pExtra, pHandlerInfo);
}

void InitLogging()
{
    CreateDirectoryA("logs", nullptr);
    xlog::LoggerConfig::get()
        .add_appender<xlog::FileAppender>("logs/AlpineEditor.log", false)
        .add_appender<xlog::ConsoleAppender>()
        .add_appender<xlog::Win32Appender>();
    xlog::info("Alpine Faction Editor log started.");
}

void InitCrashHandler()
{
    char current_dir[MAX_PATH] = ".";
    GetCurrentDirectoryA(std::size(current_dir), current_dir);

    CrashHandlerConfig config;
    config.this_module_handle = g_module;
    std::snprintf(config.log_file, std::size(config.log_file), "%s\\logs\\AlpineEditor.log", current_dir);
    std::snprintf(config.output_dir, std::size(config.output_dir), "%s\\logs", current_dir);
    std::snprintf(config.app_name, std::size(config.app_name), "AlpineEditor");
    config.add_known_module("RED");
    config.add_known_module("AlpineEditor");

    CrashHandlerStubInstall(config);
}

void ApplyGraphicsPatches();
void ApplyTriggerPatches();
void ApplyLevelPatches();
void ApplyEventsPatches();
void ApplyTexturesPatches();

void LoadAlpineEditorPackfile()
{
    static auto& vpackfile_add = addr_as_ref<int __cdecl(const char *name, const char *dir)>(0x004CA930);
    static auto& root_path = addr_as_ref<char[256]>(0x0158CA10);

    auto df_dir = get_module_dir(g_module);
    std::string old_root_path = root_path;
    std::strncpy(root_path, df_dir.c_str(), sizeof(root_path) - 1);
    if (!vpackfile_add("alpinefaction.vpp", nullptr)) {
        xlog::error("Failed to load alpinefaction.vpp from {}", df_dir);
    }
    std::strncpy(root_path, old_root_path.c_str(), sizeof(root_path) - 1);
}

CodeInjection vpackfile_init_injection{
    0x004CA533,
    []() {
        LoadAlpineEditorPackfile();
    },
};

CodeInjection CMainFrame_OnPlayLevelCmd_skip_level_dir_injection{
    0x004479AD,
    [](auto& regs) {
        char* level_pathname = regs.eax;
        regs.eax = std::strrchr(level_pathname, '\\') + 1;
    },
};

CodeInjection CMainFrame_OnPlayLevelFromCameraCmd_skip_level_dir_injection{
    0x00447CF2,
    [](auto& regs) {
        char* level_pathname = regs.eax;
        regs.eax = std::strrchr(level_pathname, '\\') + 1;
    },
};

CodeInjection CDedLevel_CloneObject_injection{
    0x004135B9,
    [](auto& regs) {
        void* that = regs.ecx;
        int type = regs.eax;
        void* obj = regs.edi;
        if (type == 0xC) {
            // clone cutscene path node
            regs.eax = AddrCaller{0x00413C70}.this_call<void*>(that, obj);
            regs.eip = 0x004135CF;
        }
    },
};

CodeInjection CDedEvent_Copy_injection{
    0x0045146D,
    [](auto& regs) {
        void* src_event = regs.edi;
        void* dst_event = regs.esi;
        // copy bool2 field
        struct_field_ref<bool>(dst_event, 0xB1) = struct_field_ref<bool>(src_event, 0xB1);
    },
};

CodeInjection texture_name_buffer_overflow_injection1{
    0x00445297,
    [](auto &regs) {
        const char *filename = regs.esi;
        if (std::strlen(filename) > max_texture_name_len) {
            LogDlg_Append(GetLogDlg(), "Texture name too long: %s\n", filename);
            regs.eip = 0x00445273;
        }
    },
};

CodeInjection texture_name_buffer_overflow_injection2{
    0x004703EC,
    [](auto &regs) {
        const char *filename = regs.ebp;
        if (std::strlen(filename) > max_texture_name_len) {
            LogDlg_Append(GetLogDlg(), "Texture name too long: %s\n", filename);
            regs.eip = 0x0047047F;
        }
    },
};

CodeInjection LoadSaveLevel_patch{
    0x0041CD20, [](auto& regs) {
        int* version = reinterpret_cast<int*>(regs.esi + 0x54);
        *version = MAXIMUM_RFL_VERSION; // set rfl version saved to file
        regs.eip = 0x0041CD27; // skip version being set to 200 by original code
    }
};

INT_PTR CALLBACK DialogLegacyLevel(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_INITDIALOG:
        SetDlgItemTextA(hwndDlg, IDC_LEVEL_PROMPT_MESSAGE, (LPCSTR)lParam);
        return TRUE;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDLEVELLINK: // If the link button is clicked
            ShellExecuteA(NULL, "open", "https://www.redfactionwiki.com/wiki/Alpine_Level_Design", NULL, NULL,
                          SW_SHOWNORMAL);
            return TRUE;

        case IDOK: // OK button
        case IDCANCEL:
            EndDialog(hwndDlg, LOWORD(wParam));
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void ShowLegacyLevelDialog(int current_version, int new_version) {
    std::string message =
        std::format("This level file was constructed using an older version of RED.\n\n"
                    "The version of this level file is {}.\n\n"
                    "Due to new Alpine Faction features and capabilities that are not available in older client versions, "
                    "Alpine RED saves levels with version {}.\n\n"
                    "If you resave this file, it will be converted to an Alpine Level with version {}, "
                    "and it will no longer be playable on non-Alpine legacy client versions.\n\n"
                    "To learn more about Alpine Levels, click below to visit the Red Faction Wiki.",
                    current_version, new_version, new_version);

    DialogBoxParamA(
        ((HINSTANCE) & __ImageBase),
        MAKEINTRESOURCEA(IDD_ALPINE_LEVEL_POPUP),
        GetActiveWindow(),
        DialogLegacyLevel,
        reinterpret_cast<LPARAM>(message.c_str())
    );
}

CodeInjection LoadSaveLevel_patch2{
    0x0041CDAA, [](auto& regs) {
        int* version = regs.edi;

        if (*version < 300 && !g_skip_legacy_level_warning) {
            xlog::info("Displaying legacy level warning dialog");
            ShowLegacyLevelDialog(*version, MAXIMUM_RFL_VERSION);
        }
    }
};

// disable splash screen if editor is launched with -level switch, fixes splash screen issue with version popup
CodeInjection disable_splash_screen_on_load_level {
    0x00482672, [](auto& regs) {
        static auto& argv = addr_as_ref<char**>(0x01DBF8E4);
        static auto& argc = addr_as_ref<int>(0x01DBF8E0);
        for (int i = 1; i < argc; ++i) {
            std::string_view arg = argv[i];

            // skip legacy level warning dialog
            if (arg == "-skiplegacywarning") {
                g_skip_legacy_level_warning = true;
            }

            // skip past the splash screen if loading a level directly
            // this fixes unintuitive behaviour when the splash screen and legacy level warning both show up at the same time
            if (arg == "-level") {
                ++i;
                if (i < argc) {
                    regs.eip = 0x0048268E;
                }
            }
        }
    }
};

void apply_af_level_editor_changes()
{
    // Use new version for saved rfls
    LoadSaveLevel_patch.install();
    LoadSaveLevel_patch2.install();
    disable_splash_screen_on_load_level.install();

    // Raise maximum apply map pixels/m value to 8192 (default 256)
    AsmWriter(0x00476FCD).push(0x2000);

    // Write version 300 to saved group files, instead of 200 (default)
    // Fixes issue loading events with orientation values (in event.cpp DedEvent__exchange_patch)
    // Note: Doesn't need to be updated when MAXIMUM_RFL_VERSION is incremented
    AsmWriter(0x0041D0AF).push(0x12C);
    AsmWriter(0x0041D0BC).push(0x12C);

    // Allow linking to multiplayer respawn points
    AsmWriter(0x004698C7).nop(2);

    // Remove legacy geometry maximums from build output window
    static char new_faces_string[] = "Faces: %d\n";                                  // Replace "Faces: %d/%d\n"
    static char new_face_vertices_string[] = "Face Vertices: %d\n";                  // Replace "Face Vertices: %d/%d\n"
    static char new_vertices_string[] = "Vertices: %d\n";                            // Replace "Vertices: %d/%d\n"
    AsmWriter(0x0043A4A3).push(reinterpret_cast<int32_t>(new_faces_string));         // faces
    AsmWriter(0x0043A4C7).push(reinterpret_cast<int32_t>(new_face_vertices_string)); // face verts
    AsmWriter(0x0043A4EB).push(reinterpret_cast<int32_t>(new_vertices_string));      // verts

    // Remove "You must rebuild geometry before leaving group mode"
    AsmWriter(0x0042645E).jmp(0x00426486);
    AsmWriter(0x004263D1).jmp(0x004263F9);
    AsmWriter(0x0042637A).jmp(0x004263A2);
    AsmWriter(0x0042631C).jmp(0x00426344);
    AsmWriter(0x004262E2).jmp(0x0042630A);

    // Remove "You must rebuild geometry before texturing brushes"
    AsmWriter{0x0042642E}.jmp(0x00426456);

    // Stop adding faces to "fix ps2 tiling" when the surface UVs tile a lot
    AsmWriter{0x0043A081}.jmp(0x0043A0E9);
    AsmWriter{0x0043A0EF}.nop(3);
}

extern "C" DWORD DF_DLL_EXPORT Init([[maybe_unused]] void* unused)
{
    InitLogging();
    InitCrashHandler();

    // stops pink lightmaps, but also crashes with too many lights todo: can this be fixed?
    //AsmWriter{0x004AC60B}.jmp(0x004AC611);

    // Apply AF-specific changes only if legacy mode isn't active
    if (get_is_saving_af_version()) {
        apply_af_level_editor_changes();
    }

    // Change command for Play Level action to use AF launcher
    g_launcher_pathname = get_module_dir(g_module) + LAUNCHER_FILENAME;
    using namespace asm_regs;
    AsmWriter(0x00447B32, 0x00447B39).mov(eax, g_launcher_pathname.c_str());
    AsmWriter(0x00448024, 0x0044802B).mov(eax, g_launcher_pathname.c_str());
    CMainFrame_OnPlayLevelCmd_skip_level_dir_injection.install();
    CMainFrame_OnPlayLevelFromCameraCmd_skip_level_dir_injection.install();

    // Add additional file paths for V3M loading
    CEditorApp_InitInstance_additional_file_paths_injection.install();

    // Add handling for "-level" command line argument
    CEditorApp_InitInstance_open_level_injection.install();

    // Change main frame style flags to allow dropping files
    CMainFrame_PreCreateWindow_injection.install();

    // Increase memory size of log view buffer (500 lines * 64 characters)
    write_mem<int32_t>(0x0044489C + 1, 32000);

    // Optimize object selection logging
    log_append_wnd_set_text_hook.install();
    brush_mode_handle_selection_hook.install();
    group_mode_handle_selection_hook.install();

    // Replace some editor resources
    CWnd_CreateDlg_injection.install();
    CDialog_DoModal_injection.install();
    write_mem_ptr(0x0055456C, &LoadMenuA_new);
    write_mem_ptr(0x005544FC, &LoadIconA_new);
    write_mem_ptr(0x005543AC, &LoadAcceleratorsA_new);

    // Remove "Packfile saved successfully!" message
    AsmWriter{0x0044CCA3, 0x0044CCB3}.nop();

    // Fix changing properties of multiple respawn points
    CDedLevel_OpenRespawnPointProperties_injection.install();

    // Apply patches defined in other files
    ApplyGraphicsPatches();
    ApplyTriggerPatches();
    ApplyLevelPatches();
    ApplyEventsPatches();
    ApplyTexturesPatches();

    // Browse for .v3m files instead of .v3d
    static char mesh_ext_filter[] = "Mesh (*.v3m)|*.v3m|All Files (*.*)|*.*||";
    write_mem_ptr(0x0044243E + 1, mesh_ext_filter);
    write_mem_ptr(0x00462490 + 1, mesh_ext_filter);
    write_mem_ptr(0x0044244F + 1, ".v3m");
    write_mem_ptr(0x004624A9 + 1, ".v3m");
    write_mem_ptr(0x0044244A + 1, "RFBrush.v3m");

    // Fix rendering of VBM textures from user_maps/textures
    write_mem_ptr(0x004828C2 + 1, ".tga .vbm");

    // Fix lights sometimes not working
    DedLight_UpdateLevelLight_hook.install();

    // Fix crash when selecting decal texture from 'All' folder
    CTextureBrowserDialog_GetFolderName_hook.install();

    // No longer require "-sound" argument to enable sound support
    AsmWriter{0x00482BC4}.nop(2);

    // Fix random crash when opening cutscene properties
    CCutscenePropertiesDialog_ct_crash_fix.install();

    // Load DashEditor.vpp
    vpackfile_init_injection.install();

    // Add maps_af.txt to the collection of files scanned for default textures in order to add more textures from the
    // base game to the texture browser
    // Especially add Rck_DefaultP.tga to default textures to fix error when packing a level containing a particle
    // emitter with default properties
    static const char* maps_files_names[] = {
        "maps.txt",
        "maps1.txt",
        "maps2.txt",
        "maps3.txt",
        "maps4.txt",
        "maps_af.txt",
        nullptr,
    };
    write_mem_ptr(0x0041B813 + 1, maps_files_names);
    write_mem_ptr(0x0041B824 + 1, maps_files_names);

    // Fix path node connections sometimes being rendered incorrectly
    // For some reason editor code uses copies of radius and position fields in some situation and those copies
    // are sometimes uninitialized
    write_mem<u8>(0x004190EB, asm_opcodes::jmp_rel_short);
    write_mem<u8>(0x0041924A, asm_opcodes::jmp_rel_short);

    // Support additional commands
    write_mem_ptr(0x00556574, &CMainFrame_OnCmdMsg);

    // Fix F4 key (Maximize active viewport) for screens larger than 1024x768
    constexpr int max_size = 0x7FFF;
    write_mem<int>(0x0044770D + 1, max_size); // 1 cx
    write_mem<int>(0x0044771D + 1, max_size); // 1 cy
    write_mem<int>(0x00447750 + 1, -max_size); // 2 add cx
    write_mem<int>(0x004477E1 + 1, -max_size); // 4 add cx crash
    write_mem<int>(0x00447797 + 1, max_size); // 3 cx
    write_mem<int>(0x00447761 + 1, max_size); // 3 cy
    write_mem<int>(0x004477A0 + 2, -max_size); // 3 lea
    write_mem<int>(0x004477EE + 2, -max_size); // 4 lea

    // Fix editor crash when building geometry after lightmap resolution for a face was set to Undefined
    write_mem<i8>(0x00402DFA + 1, 0);

    // Allow more decals before displaying a warning message about too many decals in the level
    write_mem<i8>(0x0041E2A9 + 2, 127);
    write_mem<i8>(0x0041E2BA + 2, 127);
    write_mem_ptr(0x0041E2C6 + 1, "There are more than 127 decals in the level! It can result in a crash for older game clients.");

    // Fix copying cutscene path node
    CDedLevel_CloneObject_injection.install();

    // Fix copying bool2 field in events
    CDedEvent_Copy_injection.install();

    // Remove uid limit (50k) by removing cmp and jge instructions in FindBiggestUid function
    AsmWriter{0x004844AC, 0x004844B3}.nop();

    // Ignore textures with filename longer than 31 characters to avoid buffer overflow errors
    texture_name_buffer_overflow_injection1.install();
    texture_name_buffer_overflow_injection2.install();

    // Increase face limit in g_boolean_find_all_pairs
    static void *found_faces_a[0x10000];
    static void *found_faces_b[0x10000];
    write_mem_ptr(0x004A7290+3, found_faces_a);
    write_mem_ptr(0x004A7158+1, found_faces_a);
    write_mem_ptr(0x004A72E5+4, found_faces_a);
    write_mem_ptr(0x004A717D+1, found_faces_b);
    write_mem_ptr(0x004A71A6+1, found_faces_b);
    write_mem_ptr(0x004A72A2+3, found_faces_b);
    write_mem_ptr(0x004A72F9+1, found_faces_b);

    // Display geometry limits according to pools sizes
    //write_mem<std::uint32_t>(0x0043A49D + 1, 30000);
    //write_mem<std::uint32_t>(0x0043A4C1 + 1, 150000);
    //write_mem<std::uint32_t>(0x0043A4E5 + 1, 50000);

    // Disable red background if geometry limits are crossed
    AsmWriter{0x0043A528, 0x0043A546}.nop();

    return 1; // success
}

extern "C" void subhook_unk_opcode_handler(uint8_t* opcode)
{
    xlog::error("SubHook unknown opcode 0x{:x} at {}", *opcode, static_cast<void*>(opcode));
}

BOOL WINAPI DllMain(HINSTANCE instance, [[maybe_unused]] DWORD reason, [[maybe_unused]] LPVOID reserved)
{
    g_module = (HMODULE)instance;
    DisableThreadLibraryCalls(instance);
    return TRUE;
}
