#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <string>
#include <vector>
#include <patch_common/MemUtils.h>
#include <xlog/xlog.h>
#include "file_dialogs.h"

namespace
{

// RED.exe IAT slots for COMDLG32!GetOpenFileNameA / GetSaveFileNameA. MFC's CFileDialog::DoModal
// (0x0052CFD9) is the only stock caller, so shimming the imports upgrades every editor dialog.
constexpr unsigned get_open_file_name_iat = 0x005546E4;
constexpr unsigned get_save_file_name_iat = 0x005546E8;

// The struct MFC passes predates FlagsEx; never read past what the caller declared.
constexpr DWORD min_struct_size = 0x4C;

constexpr DWORD unsupported_flags = OFN_ALLOWMULTISELECT | OFN_ENABLETEMPLATE |
                                    OFN_ENABLETEMPLATEHANDLE;

using GetFileNameFn = BOOL(WINAPI*)(LPOPENFILENAMEA);

GetFileNameFn g_orig_get_open_file_name = nullptr;
GetFileNameFn g_orig_get_save_file_name = nullptr;

enum class DialogOutcome
{
    ok,
    cancelled,
    unsupported, // hand the call back to COMDLG32
};

template<typename T>
class ComPtr
{
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr()
    {
        if (ptr_) {
            ptr_->Release();
        }
    }
    T** put() { return &ptr_; }
    T* get() const { return ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

// RED only initialises COM lazily in its sound paths, so the shim owns the apartment for the
// duration of the dialog unless one already exists.
class ComInit
{
public:
    ComInit()
    {
        hr_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    }
    ComInit(const ComInit&) = delete;
    ComInit& operator=(const ComInit&) = delete;
    ~ComInit()
    {
        if (SUCCEEDED(hr_)) {
            CoUninitialize();
        }
    }
    bool usable() const { return SUCCEEDED(hr_); }

private:
    HRESULT hr_ = E_FAIL;
};

std::wstring widen(const char* text)
{
    if (!text || !*text) {
        return {};
    }
    const int len = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (len <= 1) {
        return {};
    }
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, out.data(), len);
    return out;
}

// Empty on anything the editor's ANSI file APIs could not reopen anyway.
std::string narrow(const wchar_t* text)
{
    if (!text || !*text) {
        return {};
    }
    const int len = WideCharToMultiByte(CP_ACP, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) {
        return {};
    }
    std::string out(static_cast<size_t>(len - 1), '\0');
    BOOL used_default = FALSE;
    WideCharToMultiByte(CP_ACP, 0, text, -1, out.data(), len, nullptr, &used_default);
    if (used_default) {
        return {};
    }
    return out;
}

void set_folder(IFileDialog* dialog, const char* path)
{
    const std::wstring wide = widen(path);
    if (wide.empty()) {
        return;
    }
    ComPtr<IShellItem> item;
    // Skipped silently when the path is gone: Windows then falls back to its own folder memory.
    if (SUCCEEDED(SHCreateItemFromParsingName(wide.c_str(), nullptr, IID_IShellItem,
                                              reinterpret_cast<void**>(item.put())))) {
        dialog->SetFolder(item.get());
    }
}

// text outlives the call: IFileDialog is not documented to copy the strings it is handed.
void apply_filters(IFileDialog* dialog, const OPENFILENAMEA* ofn, std::vector<std::wstring>& text)
{
    const std::vector<AlpineFileFilter> pairs = alpine_file_dialog_split_filter(ofn->lpstrFilter);
    if (pairs.empty()) {
        return;
    }
    text.reserve(pairs.size() * 2);
    for (const AlpineFileFilter& pair : pairs) {
        text.push_back(widen(pair.label.c_str()));
        text.push_back(widen(pair.spec.c_str()));
    }
    std::vector<COMDLG_FILTERSPEC> specs;
    specs.reserve(pairs.size());
    for (size_t i = 0; i < pairs.size(); ++i) {
        specs.push_back({text[i * 2].c_str(), text[i * 2 + 1].c_str()});
    }
    if (FAILED(dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data()))) {
        return;
    }
    UINT index = ofn->nFilterIndex ? ofn->nFilterIndex : 1;
    if (index > specs.size()) {
        index = 1;
    }
    dialog->SetFileTypeIndex(index);
}

void apply_initial_name(IFileDialog* dialog, const OPENFILENAMEA* ofn, std::wstring& name_text)
{
    bool have_folder = false;
    if (ofn->lpstrFile && ofn->lpstrFile[0]) {
        const char* name = PathFindFileNameA(ofn->lpstrFile);
        if (name != ofn->lpstrFile) {
            // a path in lpstrFile outranks lpstrInitialDir, as it does in COMDLG32
            set_folder(dialog, std::string(ofn->lpstrFile, static_cast<size_t>(name - ofn->lpstrFile)).c_str());
            have_folder = true;
        }
        if (*name) {
            name_text = widen(name);
            dialog->SetFileName(name_text.c_str());
        }
    }
    if (!have_folder && ofn->lpstrInitialDir) {
        set_folder(dialog, ofn->lpstrInitialDir);
    }
}

DialogOutcome write_back(IFileDialog* dialog, OPENFILENAMEA* ofn, const std::string& path)
{
    if (!alpine_file_dialog_copy_bounded(path.c_str(), ofn->lpstrFile, ofn->nMaxFile)) {
        xlog::warn("file dialog: '{}' does not fit the caller's {} byte buffer, treating as cancel",
                   path, ofn->nMaxFile);
        return DialogOutcome::cancelled;
    }
    if (ofn->lpstrFileTitle && ofn->nMaxFileTitle) {
        const char* name = PathFindFileNameA(ofn->lpstrFile);
        if (!alpine_file_dialog_copy_bounded(name, ofn->lpstrFileTitle, ofn->nMaxFileTitle)) {
            xlog::warn("file dialog: '{}' does not fit the caller's {} byte name buffer, treating as cancel",
                       name, ofn->nMaxFileTitle);
            return DialogOutcome::cancelled;
        }
    }
    alpine_file_dialog_name_offsets(ofn->lpstrFile, ofn->nFileOffset, ofn->nFileExtension);
    UINT index = 0;
    if (SUCCEEDED(dialog->GetFileTypeIndex(&index)) && index) {
        ofn->nFilterIndex = index;
    }
    return DialogOutcome::ok;
}

class CoTaskMemString
{
public:
    CoTaskMemString() = default;
    CoTaskMemString(const CoTaskMemString&) = delete;
    CoTaskMemString& operator=(const CoTaskMemString&) = delete;
    ~CoTaskMemString()
    {
        if (ptr_) {
            CoTaskMemFree(ptr_);
        }
    }
    PWSTR* put() { return &ptr_; }
    PWSTR get() const { return ptr_; }

private:
    PWSTR ptr_ = nullptr;
};

DialogOutcome collect_result(IFileDialog* dialog, OPENFILENAMEA* ofn)
{
    ComPtr<IShellItem> item;
    HRESULT hr = dialog->GetResult(item.put());
    if (FAILED(hr) || !item) {
        xlog::warn("file dialog: GetResult failed ({:#x}), treating as cancel", static_cast<unsigned>(hr));
        return DialogOutcome::cancelled;
    }
    CoTaskMemString wide_path;
    hr = item->GetDisplayName(SIGDN_FILESYSPATH, wide_path.put());
    if (FAILED(hr) || !wide_path.get()) {
        xlog::warn("file dialog: the chosen item has no file system path ({:#x}), treating as cancel", static_cast<unsigned>(hr));
        return DialogOutcome::cancelled;
    }
    const std::string path = narrow(wide_path.get());
    if (path.empty()) {
        xlog::warn("file dialog: the chosen path cannot be represented in the editor's code page");
        return DialogOutcome::cancelled;
    }
    return write_back(dialog, ofn, path);
}

DialogOutcome show_dialog(OPENFILENAMEA* ofn, bool save)
{
    if (!ofn || ofn->lStructSize < min_struct_size || !ofn->lpstrFile || !ofn->nMaxFile) {
        return DialogOutcome::unsupported;
    }
    if ((ofn->Flags & unsupported_flags) || ofn->lpstrCustomFilter) {
        xlog::warn("file dialog: flags {:#x} need the stock dialog", ofn->Flags);
        return DialogOutcome::unsupported;
    }

    ComInit com;
    if (!com.usable()) {
        return DialogOutcome::unsupported;
    }

    // Every string handed to the dialog outlives it: IFileDialog is not documented to copy them.
    std::vector<std::wstring> filter_text;
    std::wstring initial_name;
    std::wstring title;
    std::wstring defext;
    ComPtr<IFileDialog> dialog;
    HRESULT hr = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_IFileDialog,
                                  reinterpret_cast<void**>(dialog.put()));
    if (FAILED(hr) || !dialog) {
        xlog::warn("file dialog: CoCreateInstance failed ({:#x})", static_cast<unsigned>(hr));
        return DialogOutcome::unsupported;
    }

    DWORD options = 0;
    // Without the shell's own defaults there is nothing to add to, so they are left alone.
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        options |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
        if (ofn->Flags & OFN_OVERWRITEPROMPT) {
            options |= FOS_OVERWRITEPROMPT;
        }
        if (ofn->Flags & OFN_FILEMUSTEXIST) {
            options |= FOS_FILEMUSTEXIST;
        }
        if (ofn->Flags & OFN_PATHMUSTEXIST) {
            options |= FOS_PATHMUSTEXIST;
        }
        dialog->SetOptions(options);
    }

    apply_filters(dialog.get(), ofn, filter_text);
    apply_initial_name(dialog.get(), ofn, initial_name);
    title = widen(ofn->lpstrTitle);
    if (!title.empty()) {
        dialog->SetTitle(title.c_str());
    }
    defext = widen(alpine_file_dialog_normalize_defext(ofn->lpstrDefExt).c_str());
    if (!defext.empty()) {
        dialog->SetDefaultExtension(defext.c_str());
    }

    hr = dialog->Show(ofn->hwndOwner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return DialogOutcome::cancelled;
    }
    if (FAILED(hr)) {
        xlog::warn("file dialog: Show failed ({:#x})", static_cast<unsigned>(hr));
        return DialogOutcome::unsupported;
    }

    // The dialog has been shown, so a failure past this point is reported as a cancel rather than
    // handed on to COMDLG32, which would put a second dialog in front of the user. A throw counts
    // as a failure, so it stops here rather than reaching run_dialog's fallback.
    try {
        return collect_result(dialog.get(), ofn);
    }
    catch (...) {
        xlog::error("file dialog: reading back the chosen path failed, treating as cancel");
        return DialogOutcome::cancelled;
    }
}

// A C++ exception unwinding out of here would have to pass RED's SEH frames, which corrupts, so
// anything that escapes the dialog leaves the call to the stock one.
BOOL run_dialog(LPOPENFILENAMEA ofn, bool save, GetFileNameFn fallback)
{
    try {
        switch (show_dialog(ofn, save)) {
        case DialogOutcome::ok:
            return TRUE;
        case DialogOutcome::cancelled:
            return FALSE;
        default:
            break;
        }
    }
    catch (...) {
        xlog::error("file dialog: the modern dialog failed, falling back to the stock one");
    }
    return fallback ? fallback(ofn) : FALSE;
}

BOOL WINAPI GetOpenFileNameA_alpine(LPOPENFILENAMEA ofn)
{
    return run_dialog(ofn, false, g_orig_get_open_file_name);
}

BOOL WINAPI GetSaveFileNameA_alpine(LPOPENFILENAMEA ofn)
{
    return run_dialog(ofn, true, g_orig_get_save_file_name);
}

} // namespace

BOOL alpine_get_save_file_name(OPENFILENAMEA* ofn)
{
    return run_dialog(ofn, true, &GetSaveFileNameA);
}

BOOL alpine_get_open_file_name(OPENFILENAMEA* ofn)
{
    return run_dialog(ofn, false, &GetOpenFileNameA);
}

void ApplyFileDialogPatches()
{
    static bool installed = false;
    if (installed) {
        return;
    }
    installed = true;
    g_orig_get_open_file_name = addr_as_ref<GetFileNameFn>(get_open_file_name_iat);
    g_orig_get_save_file_name = addr_as_ref<GetFileNameFn>(get_save_file_name_iat);
    write_mem_ptr(get_open_file_name_iat, &GetOpenFileNameA_alpine);
    write_mem_ptr(get_save_file_name_iat, &GetSaveFileNameA_alpine);
}
