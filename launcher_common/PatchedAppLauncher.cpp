#include "PatchedAppLauncher.h"
#include "sha1.h"
#include "Win32Handle.h"
#include "Process.h"
#include "Thread.h"
#include "DllInjector.h"
#include "InjectingProcessLauncher.h"
#include <common/config/GameConfig.h>
#include <common/error/Exception.h>
#include <common/error/Win32Error.h>
#include <common/utils/os-utils.h>
#include <xlog/xlog.h>
#include <windows.h>
#include <shlwapi.h>
#include <fstream>
#include <iterator>

// Needed by MinGW
#ifndef ERROR_ELEVATION_REQUIRED
#define ERROR_ELEVATION_REQUIRED 740
#endif

// Process start-up can stall for well over 10 s on a busy system, before any of the game's code runs. Also bounds
// each of inject_dll's remote-thread waits.
#define INIT_TIMEOUT 60000
#define RF_120_NA_SHA1  "f94f2e3f565d18f75ab6066e77f73a62a593fe03"
#define RF_120_NA_4GB_SHA1  "4140f7619b6427c170542c66178b47bd48795a99"
#define RED_120_NA_SHA1 "b4f421bfa9343362d7cc565e9f7ab8c6cc36f3a2"
// Large address aware RED.exe as NTCore's 4GB Patch writes it (PE checksum recomputed), and with the flag alone
#define RED_120_NA_4GB_SHA1 "29db946cd24df4f053c4083bb8e260769649be66"
#define RED_120_NA_4GB_FLAG_ONLY_SHA1 "179dbdf2a3ce4983aa0d82cb11acece5b5d88a15"
#define RED_120_NA_4GB_PE_CHECKSUM 0x001FBBA3
#define TABLES_VPP_SHA1 "ded5e1b5932f47044ba760699d5931fda6bdc8ba"

inline bool is_no_gui_mode()
{
    return GetSystemMetrics(SM_CMONITORS) == 0;
}

std::string PatchedAppLauncher::get_patch_dll_path()
{
    auto buf = get_module_pathname(nullptr);

    // Get GetFinalPathNameByHandleA function address dynamically in order to support Windows XP
    using GetFinalPathNameByHandleA_Type = decltype(GetFinalPathNameByHandleA);
    HMODULE kernel32_module = GetModuleHandleA("kernel32");
    auto* GetFinalPathNameByHandleA_ptr = reinterpret_cast<GetFinalPathNameByHandleA_Type*>(reinterpret_cast<void(*)()>(
        GetProcAddress(kernel32_module, "GetFinalPathNameByHandleA")));
    // Make sure path is pointing to an actual module and not a symlink
    if (GetFinalPathNameByHandleA_ptr) {
        HANDLE file_handle = CreateFileA(buf.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file_handle != INVALID_HANDLE_VALUE) {
            buf.resize(MAX_PATH);

            DWORD result = GetFinalPathNameByHandleA(file_handle, buf.data(), buf.size(), FILE_NAME_NORMALIZED);
            if (result > buf.size()) {
                buf.resize(result);
                result = GetFinalPathNameByHandleA(file_handle, buf.data(), buf.size(), FILE_NAME_NORMALIZED);
            }
            if (result == 0 || result > buf.size()) {
                THROW_WIN32_ERROR();
            }
            // Windows Vista returns number of characters including the terminating null character
            if (buf[result - 1] == '\0') {
                --result;
            }
            buf.resize(result);
            CloseHandle(file_handle);
        }
    }

    std::string dir = get_dir_from_path(buf);
    xlog::info("Determined Alpine Faction directory: {}", dir);
    return dir + "\\" + m_patch_dll_name;
}

std::string PatchedAppLauncher::get_app_path()
{
    if (m_forced_app_exe_path) {
        return m_forced_app_exe_path.value();
    }
    return get_default_app_path();
}

void PatchedAppLauncher::launch()
{
    std::string app_path = get_app_path();
    std::string work_dir = get_dir_from_path(app_path);
    std::string launch_path = get_launch_path(app_path, verify_before_launch());

    STARTUPINFO si;
    setup_startup_info(si);

    if (m_mod_name.find(' ') != std::string::npos) {
        MessageBoxA(nullptr, "Mods with space in the name are not supported", nullptr, MB_OK | MB_ICONWARNING);
    }

    try {
        std::string patch_dll_path = get_patch_dll_path();
        std::optional<InjectingProcessLauncher> proc_launcher;
        auto start = [&](const std::string& exe_path) {
            std::string cmd_line = build_cmd_line(exe_path);
            xlog::info("Starting the process: {}", exe_path);
            proc_launcher.emplace(exe_path.c_str(), work_dir.c_str(), cmd_line.c_str(), si, INIT_TIMEOUT);
            xlog::info("Injecting {}", patch_dll_path);
            proc_launcher->inject_dll(patch_dll_path.c_str(), "Init", INIT_TIMEOUT);
        };
        if (launch_path != app_path) {
            // The system or an antivirus may block or end a copy it does not know: the verified executable still runs
            try {
                start(launch_path);
            }
            catch (const std::exception& e) {
                const auto* win32_error = dynamic_cast<const Win32Error*>(&e);
                if (win32_error && win32_error->error() == ERROR_ELEVATION_REQUIRED) {
                    throw;
                }
                proc_launcher.reset();
                xlog::warn("Starting {} failed, starting {} instead: {}", launch_path, app_path, e.what());
            }
        }
        if (!proc_launcher) {
            start(app_path);
        }

        xlog::info("Resuming app main thread");
        proc_launcher->resume_main_thread();
        xlog::info("Process launched successfully");

        // Wait for child process in Wine No GUI mode
        if (is_no_gui_mode()) {
            xlog::info("Waiting for app to close");
            proc_launcher->wait(INFINITE);
        }
    }
    catch (const Win32Error& e)
    {
        if (e.error() == ERROR_ELEVATION_REQUIRED)
            throw PrivilegeElevationRequiredException();
        throw;
    }
    catch (const ProcessTerminatedError&)
    {
        throw LauncherError(
            "Game process has terminated before injection!\n"
            "Check your Red Faction installation.");
    }
}

std::string PatchedAppLauncher::verify_before_launch()
{
    std::string app_path = get_app_path();
    xlog::info("Verifying {} SHA1", app_path);
    std::ifstream file(app_path, std::fstream::in | std::fstream::binary);
    if (!file.is_open()) {
        throw FileNotFoundException(app_path);
    }
    SHA1 sha1;
    sha1.update(file);
    auto hash = sha1.final();

    // Error reporting for headless env
    if (!check_app_hash(hash)) {
        throw FileHashVerificationException(app_path, hash);
    }
    xlog::info("SHA1 is valid");

    std::string work_dir = get_dir_from_path(app_path);

    std::string tables_vpp_path = work_dir + "\\tables.vpp";
    if (!PathFileExistsA(tables_vpp_path.c_str())) {
        throw LauncherError(
            "Game directory validation has failed.\nPlease make sure game executable specified in options is located "
            "inside a valid Red Faction installation root directory.");
    }
    return hash;
}

void PatchedAppLauncher::setup_startup_info(_STARTUPINFOA& startup_info)
{
    ZeroMemory(&startup_info, sizeof(startup_info));
    startup_info.cb = sizeof(startup_info);

    if (GetFileType(GetStdHandle(STD_OUTPUT_HANDLE))) {
        // Redirect std handles - fixes nohup logging
        startup_info.dwFlags |= STARTF_USESTDHANDLES;
        startup_info.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup_info.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup_info.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
}

static std::string escape_cmd_line_arg(const std::string& arg)
{
    std::string result;
    bool enclose_in_quotes = arg.find(' ') != std::string::npos;
    result.reserve(arg.size());
    if (enclose_in_quotes) {
        result += '"';
    }
    for (char c : arg) {
        if (c == '\\' || c == '"') {
            result += '\\';
        }
        result += c;
    }
    if (enclose_in_quotes) {
        result += '"';
    }
    return result;
}

std::string PatchedAppLauncher::build_cmd_line(const std::string& app_path)
{
    std::vector<std::string> all_args;
    all_args.push_back(app_path);
    if (!m_mod_name.empty()) {
        all_args.emplace_back("-mod");
        all_args.push_back(m_mod_name);
    }
    all_args.insert(all_args.end(), m_args.begin(), m_args.end());

    std::string cmd_line;
    for (const auto& arg : all_args) {
        if (!cmd_line.empty()) {
            cmd_line += ' ';
        }
        cmd_line += escape_cmd_line_arg(arg);
    }
    return cmd_line;
}

void PatchedAppLauncher::check_installation()
{
    std::string app_path = get_app_path();
    std::string root_dir = get_dir_from_path(app_path);

    auto tables_vpp_path = root_dir + "\\tables.vpp";
    std::ifstream file(tables_vpp_path, std::fstream::in | std::fstream::binary);
    if (!file.is_open()) {
        throw FileNotFoundException("tables.vpp");
    }

    SHA1 sha1;
    sha1.update(file);
    auto checksum = sha1.final();

    if (checksum != TABLES_VPP_SHA1) {
        throw FileHashVerificationException("tables.vpp", checksum);
    }
}

GameLauncher::GameLauncher() : PatchedAppLauncher("AlpineFaction.dll")
{
    xlog::info("Checking if config exists");
    if (!m_conf.load()) {
        // Failed to load config - save defaults
        xlog::info("Saving default config");
        m_conf.save();
    }
}

std::string GameLauncher::get_default_app_path()
{
    return m_conf.game_executable_path;
}

bool GameLauncher::check_app_hash(const std::string& sha1)
{
    return sha1 == RF_120_NA_SHA1 || sha1 == RF_120_NA_4GB_SHA1;
}

namespace
{

// The launcher's manifest has no requestedExecutionLevel, so UAC virtualizes its file system: a write the game
// directory denies would land in the user's VirtualStore and look like a success.
class FileVirtualizationOff
{
public:
    FileVirtualizationOff()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_DEFAULT, &token)) {
            THROW_WIN32_ERROR("OpenProcessToken failed");
        }
        m_token = token;
        DWORD enabled = 0;
        DWORD size = 0;
        if (!GetTokenInformation(m_token, TokenVirtualizationEnabled, &enabled, sizeof(enabled), &size) || !enabled) {
            return;
        }
        DWORD disabled = 0;
        if (!SetTokenInformation(m_token, TokenVirtualizationEnabled, &disabled, sizeof(disabled))) {
            THROW_WIN32_ERROR("SetTokenInformation failed");
        }
        m_restore = true;
    }

    ~FileVirtualizationOff()
    {
        if (m_restore) {
            DWORD enabled = 1;
            SetTokenInformation(m_token, TokenVirtualizationEnabled, &enabled, sizeof(enabled));
        }
    }

    FileVirtualizationOff(const FileVirtualizationOff&) = delete;
    FileVirtualizationOff& operator=(const FileVirtualizationOff&) = delete;

private:
    Win32Handle m_token;
    bool m_restore = false;
};

std::optional<std::string> read_file(const std::string& path)
{
    std::ifstream file(path, std::fstream::in | std::fstream::binary);
    if (!file.is_open()) {
        return {};
    }
    return std::string{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

// Through a temporary file: the target is replaced whole or not at all, and is written from memory so no alternate
// data stream (mark of the web) comes along.
void write_file_replacing(const std::string& path, const std::string& data)
{
    std::string temp_path = path + ".tmp";
    HANDLE handle =
        CreateFileA(temp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        THROW_WIN32_ERROR("CreateFileA failed");
    }
    bool written = false;
    {
        Win32Handle file{handle};
        DWORD bytes_written = 0;
        written = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &bytes_written, nullptr)
            && bytes_written == data.size()
            && FlushFileBuffers(file);
    }
    if (!written || !MoveFileExA(temp_path.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DWORD error = GetLastError();
        DeleteFileA(temp_path.c_str());
        throw Win32Error(error, ("writing " + path + " failed").c_str());
    }
}

// Stock RED.exe made large address aware the way RED_120_NA_4GB_SHA1 was: false for anything else.
bool make_large_address_aware(std::string& image)
{
    SHA1 stock_sha1;
    stock_sha1.update(image);
    if (stock_sha1.final() != RED_120_NA_SHA1) {
        return false;
    }
    const auto& dos_header = *reinterpret_cast<const IMAGE_DOS_HEADER*>(image.data());
    auto& nt_headers = *reinterpret_cast<IMAGE_NT_HEADERS32*>(image.data() + dos_header.e_lfanew);
    nt_headers.FileHeader.Characteristics |= IMAGE_FILE_LARGE_ADDRESS_AWARE;
    nt_headers.OptionalHeader.CheckSum = RED_120_NA_4GB_PE_CHECKSUM;
    SHA1 sha1;
    sha1.update(image);
    return sha1.final() == RED_120_NA_4GB_SHA1;
}

// RED.exe has no embedded manifest; the external one setup can install (visual styles) applies to that file name only.
void mirror_manifest(const std::string& app_path, const std::string& copy_path)
{
    std::string manifest_path = app_path + ".manifest";
    std::string copy_manifest_path = copy_path + ".manifest";
    if (GetFileAttributesA(manifest_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            xlog::warn("Cannot check {} (error {}), leaving {} as it is", manifest_path, error, copy_manifest_path);
            return;
        }
        if (!DeleteFileA(copy_manifest_path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
            THROW_WIN32_ERROR("DeleteFileA failed");
        }
        return;
    }
    auto manifest = read_file(manifest_path);
    if (!manifest) {
        xlog::warn("Cannot read {}, leaving {} as it is", manifest_path, copy_manifest_path);
        return;
    }
    if (read_file(copy_manifest_path) != manifest) {
        write_file_replacing(copy_manifest_path, manifest.value());
    }
}

} // namespace

EditorLauncher::EditorLauncher() : PatchedAppLauncher("AlpineEditor.dll")
{
    if (!m_conf.load()) {
        // Failed to load config - save defaults
        m_conf.save();
    }
}

std::string EditorLauncher::get_default_app_path()
{
    std::string workDir = get_dir_from_path(m_conf.game_executable_path);
    return workDir + "\\RED.exe";
}

bool EditorLauncher::check_app_hash(const std::string& sha1)
{
    return sha1 == RED_120_NA_SHA1 || sha1 == RED_120_NA_4GB_SHA1 || sha1 == RED_120_NA_4GB_FLAG_ONLY_SHA1;
}

// RED.exe is not large address aware, and only its file header can make it so. The editor starts from a copy that
// is, kept beside RED.exe because RED takes its data root from its own path. Any failure starts RED.exe instead.
std::string EditorLauncher::get_launch_path(const std::string& app_path, const std::string& app_sha1)
{
    // Other accepted hashes are large address aware already
    if (!m_conf.editor_large_address_aware || app_sha1 != RED_120_NA_SHA1) {
        return app_path;
    }
    std::string laa_path = get_dir_from_path(app_path) + "\\RED_laa.exe";
    try {
        FileVirtualizationOff virtualization_off;
        if (SHA1::from_file(laa_path) != RED_120_NA_4GB_SHA1) {
            auto image = read_file(app_path);
            if (!image || !make_large_address_aware(image.value())) {
                throw std::runtime_error(app_path + " changed or could not be read");
            }
            xlog::info("Writing {}", laa_path);
            write_file_replacing(laa_path, image.value());
        }
        mirror_manifest(app_path, laa_path);
    }
    catch (const std::exception& e) {
        xlog::warn("Starting the editor with 2 GB of address space, {} is not usable: {}", laa_path, e.what());
        return app_path;
    }
    // Again as CreateProcess sees it, with virtualization back on: a VirtualStore copy would take precedence
    if (SHA1::from_file(laa_path) != RED_120_NA_4GB_SHA1) {
        xlog::warn("Starting the editor with 2 GB of address space, {} changed after it was checked", laa_path);
        return app_path;
    }
    return laa_path;
}
