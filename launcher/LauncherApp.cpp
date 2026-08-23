#include "LauncherApp.h"
#include "faction_files.h"
#include "afd_header_reader.h"
#include "demo_download.h"
#include "DownloadProgressDlg.h"
#include "MainDlg.h"
#include "LauncherCommandLineInfo.h"
#include <common/afd_format.h>
#include <common/config/GameConfig.h>
#include <common/version/version.h>
#include <common/error/error-utils.h>
#include <launcher_common/PatchedAppLauncher.h>
#include <launcher_common/VideoDeviceInfoProvider.h>
#include <xlog/xlog.h>
#include <thread>
#include <filesystem>
#include <algorithm>
#include <ctime>
#include <cstdio>
#include <commctrl.h>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

static bool fflink_token_is_invalid = false;

// ---------------------------------------------------------------------------
// Demo confirmation helpers (af://demo/<game_id> and local .afd files)
// ---------------------------------------------------------------------------
namespace {

std::wstring widen(const std::string& s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0)
        return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// Replace characters not valid in a Windows filename component; trim and cap length.
std::string sanitize_filename_part(const std::string& s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
        case '\\': case '/': case ':': case '*': case '?':
        case '"': case '<': case '>': case '|':
            out += '_';
            break;
        default:
            out += (static_cast<unsigned char>(c) < 0x20) ? '_' : c;
            break;
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();
    if (out.size() > 64)
        out.resize(64);
    return out;
}

// A demo we already downloaded carries the game id in its name; find it if present.
std::string find_cached_demo(const std::string& dir, const std::string& game_id)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec))
        return {};
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec)
            break;
        if (!entry.is_regular_file(ec))
            continue;
        const std::string name = entry.path().filename().string();
        if (name.size() >= 4 && name.substr(name.size() - 4) == ".afd"
            && name.find(game_id) != std::string::npos) {
            return entry.path().string();
        }
    }
    return {};
}

std::string format_hms(int64_t duration_ms)
{
    if (duration_ms <= 0)
        return {};
    const int64_t total_s = duration_ms / 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld:%02lld", static_cast<long long>(total_s / 60),
                  static_cast<long long>(total_s % 60));
    return buf;
}

std::string format_unix_date(int64_t unix_seconds)
{
    if (unix_seconds <= 0)
        return {};
    const std::time_t t = static_cast<std::time_t>(unix_seconds);
    const std::tm* tm = std::localtime(&t);
    if (!tm)
        return {};
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tm);
    return buf;
}

// What the confirmation dialog shows, whichever source the details came from
// (a FactionFiles resolve for af://demo, or the .afd header for a local file).
struct DemoDisplayInfo
{
    std::string map;
    std::string gametype_name;
    std::string server_name;
    std::string mod_name;
    int64_t duration_ms = 0;
    int64_t recorded_unix = 0;
    uint64_t bytes = 0;
};

std::wstring build_demo_confirm_text(const DemoDisplayInfo& info)
{
    std::string text;
    if (info.map.empty() && info.server_name.empty() && info.mod_name.empty() && info.duration_ms == 0
        && info.recorded_unix == 0) {
        text += "No details are available for this demo.\n";
    }
    else {
        text += "Map: " + info.map;
        if (!info.gametype_name.empty())
            text += " (" + info.gametype_name + ")";
        text += "\n";
        if (!info.server_name.empty())
            text += "Server: " + info.server_name + "\n";
        if (!info.mod_name.empty())
            text += "Mod: " + info.mod_name + "\n";
        const std::string len = format_hms(info.duration_ms);
        if (!len.empty())
            text += "Length: " + len + "\n";
        const std::string when = format_unix_date(info.recorded_unix);
        if (!when.empty())
            text += "Recorded: " + when + "\n";
    }
    if (info.bytes > 0) {
        char sizebuf[32];
        const double mb = static_cast<double>(info.bytes) / (1000.0 * 1000.0);
        if (mb < 0.05)
            std::snprintf(sizebuf, sizeof(sizebuf), "%.2f MB", std::max(mb, 0.01));
        else
            std::snprintf(sizebuf, sizeof(sizebuf), "%.1f MB", mb);
        text += std::string("Size: ") + sizebuf;
    }
    return widen(text);
}

// rf::NetGameType index; kept in sync with multi_game_type_name (game_patch/multi/multi.cpp).
std::string demo_game_type_name(int32_t game_type)
{
    static const char* const names[] = {
        "Deathmatch", "Capture the Flag", "Team Deathmatch", "King of the Hill", "Damage Control",
        "Revolt", "Run", "Escalation", "Bagman", "Team Bagman", "Pit", "Wipeout", "Gun Game", "Salvage",
    };
    if (game_type < 0 || game_type >= static_cast<int32_t>(ARRAYSIZE(names)))
        return {};
    return names[game_type];
}

// Returns 1 = watch, 2 = download only, 0 = cancel.
int show_demo_confirm_dialog(const std::wstring& content, bool offer_download_only)
{
    const TASKDIALOG_BUTTON download_buttons[] = {
        {1001, L"Download and watch\nSave the demo and start playing it now."},
        {1002, L"Download only\nSave the demo so you can watch it later."},
    };
    const TASKDIALOG_BUTTON play_buttons[] = {
        {1001, L"Watch\nStart the game and play this demo."},
    };
    TASKDIALOGCONFIG cfg = {};
    cfg.cbSize = sizeof(cfg);
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_COMMAND_LINKS;
    cfg.pszWindowTitle = L"Watch Demo - Alpine Faction";
    cfg.pszMainIcon = TD_INFORMATION_ICON;
    cfg.pszMainInstruction = L"Are you sure you want to watch this demo?";
    cfg.pszContent = content.c_str();
    cfg.pButtons = offer_download_only ? download_buttons : play_buttons;
    cfg.cButtons = offer_download_only ? static_cast<UINT>(ARRAYSIZE(download_buttons))
                                       : static_cast<UINT>(ARRAYSIZE(play_buttons));
    cfg.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    cfg.nDefaultButton = 1001;
    int pressed = 0;
    if (FAILED(TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr)))
        return 0;
    if (pressed == 1001)
        return 1;
    if (pressed == 1002)
        return 2;
    return 0;
}

} // namespace

// LauncherApp initialization
int LauncherApp::Run()
{
	// InitCommonControlsEx() is required on Windows XP if an application
	// manifest specifies use of ComCtl32.dll version 6 or later to enable
	// visual styles.  Otherwise, any window creation will fail.
    Win32xx::LoadCommonControls();

    // Command line parsing
    xlog::info("Parsing command line");
    m_cmd_line_info.Parse();

    if (m_cmd_line_info.GetAFLinkArg().has_value()) {
        std::string fflink_token = m_cmd_line_info.GetAFLinkArg().value();
        bool fflink_successful = ValidateAFLinkToken(fflink_token); // Validate and update if necessary
        if (fflink_successful) {
            Message(nullptr,
                    "Alpine Faction has successfully been linked to your FactionFiles account!\n\n"
                    "Features that depend on FF account linking such as achievements and map ranking are now available.",
                    "Success! FactionFiles Account Linked", MB_OK | MB_ICONINFORMATION);
        }
        else {
            Message(nullptr,
                    "Alpine Faction was unable to establish a link to your FactionFiles account. "
                    "You can still play the game, but features that depend on FF account linking will be unavailable.\n\n"
                    "Visit https://alpinefaction.com/help for help resources to assist with resolving this issue.\n",
                    "FactionFiles Account Link Failed", MB_OK | MB_ICONINFORMATION);
        }

    }
    else {
        // Validate stored token on every launch
        GameConfig game_config;
        bool fflink_successful = false;
        if (game_config.load() && !game_config.fflink_token.value().empty()) {
            fflink_successful = ValidateAFLinkToken(game_config.fflink_token.value());
        }
        if (!fflink_successful && fflink_token_is_invalid) {
            Message(nullptr,
                    "Your FactionFiles account was unlinked.\n\n"
                    "Features such as achievement tracking and map ranking will be unavailable"
                    " until you link your account again.",
                    "FactionFiles Account Unlinked", MB_OK | MB_ICONINFORMATION);
        }
    }

    if (m_cmd_line_info.GetAFDownloadArg().has_value()) {
        int file_id = std::stoi(m_cmd_line_info.GetAFDownloadArg().value());
        xlog::info("Processing af://download/{}", file_id);

        FactionFilesAFLink downloader;

        // Fetch file info before downloading
        auto file_info = downloader.get_file_info(file_id);
        if (!file_info) {
            MessageBoxA(nullptr, "Error: File not found on FactionFiles!", "Error", MB_OK | MB_ICONERROR);
            return 0;
        }

        // Create confirmation message
        std::string file_type_string =
            file_info->file_type == "mod_tc" || file_info->file_type == "mod_clientside" ? "mod?" : "level?";

        std::string message = "File Name: " + file_info->name + "\n" + "Author: " + file_info->author + "\n" +
            "Size: " + std::to_string(file_info->size_in_bytes / 1000) + " KB\n\n" +
            "Are you sure you want to install this " + file_type_string;

        // Show confirmation dialog
        int userChoice =
            MessageBoxA(nullptr, message.c_str(), "Confirm Install - Alpine Faction", MB_OKCANCEL | MB_ICONQUESTION);

        if (userChoice == IDCANCEL) {
            xlog::info("User canceled download.");
            return 0;
        }

        // Show the progress dialog
        DownloadProgressDlg progressDlg(file_id, file_info->name, file_info->size_in_bytes / 1000);

        bool download_success = false; // Track download result

        // Start the download in a separate thread
        std::thread downloadThread([&]() {
            try {
                download_success = downloader.download_and_extract(
                    file_id, file_info->file_type, [&](unsigned bytes_received, std::chrono::milliseconds duration) {
                        progressDlg.SetProgress(bytes_received);
                        return !progressDlg.IsCancelRequested();
                    });

                progressDlg.SetFinished(download_success);
            }
            catch (const std::exception& e) { // catch errors
                xlog::error("Please try again. An error occured when downloading:: {}", e.what());
                progressDlg.SetFinished(false);
            }
        });

        // Start the progress dialog
        progressDlg.DoModal(nullptr);

        // Wait for the thread to finish
        downloadThread.join();

        if (progressDlg.IsCancelRequested()) {
            xlog::info("User canceled download.");
            return 0;
        }

        // download succeeded?
        if (download_success) {
            std::string success_message = "Successfully downloaded and installed " + file_info->name + "!" +
                                          "\n\nWould you like to open the Alpine Faction launcher now?";
            int result = MessageBoxA(nullptr, success_message.c_str(), "Success!", MB_YESNO | MB_ICONINFORMATION);

            if (result == IDNO) {
                return 0; // Exit if the user chooses not to open the launcher
            }
        }
        else {
            MessageBoxA(nullptr, "Download or install failed!", "Error", MB_OK | MB_ICONERROR);
        }
    }

    if (m_cmd_line_info.GetAFDemoArg().has_value()) {
        std::string game_id = m_cmd_line_info.GetAFDemoArg().value();
        xlog::info("Processing af://demo/{}", game_id);

        if (!DemoDownloader::is_valid_game_id(game_id)) {
            MessageBoxA(nullptr, "That demo link is not valid.", "Alpine Faction", MB_OK | MB_ICONERROR);
            return 0;
        }

        // Locate <game>\demos\downloaded from the configured game executable.
        std::string demos_dir;
        {
            GameConfig cfg;
            if (cfg.load() && !cfg.game_executable_path.value().empty()) {
                std::filesystem::path exe(cfg.game_executable_path.value());
                demos_dir = (exe.parent_path() / "demos" / "downloaded").string();
            }
        }
        if (demos_dir.empty()) {
            MessageBoxA(nullptr,
                        "Could not find your Red Faction installation. Open the launcher and set the game path first.",
                        "Alpine Faction", MB_OK | MB_ICONERROR);
            return 0;
        }

        // Already downloaded? Dedup is keyed on the game id embedded in the filename.
        std::string cached_path = find_cached_demo(demos_dir, game_id);
        const bool already_have = !cached_path.empty();

        // Resolve for the download URL and display info. Required when not cached; best
        // effort when cached (we can still play what we already have if resolve fails).
        DemoDownloader downloader;
        DemoDownloader::ResolveResult info;
        const auto rstatus = downloader.resolve(game_id, info);
        if (!already_have && rstatus != DemoDownloader::ResolveStatus::ok) {
            const char* msg = "That demo is not available.";
            switch (rstatus) {
            case DemoDownloader::ResolveStatus::disabled:
                msg = "Demo downloads are temporarily unavailable. Please try again later.";
                break;
            case DemoDownloader::ResolveStatus::server_error:
            case DemoDownloader::ResolveStatus::network_error:
                msg = "Could not reach FactionFiles to fetch that demo. Please try again later.";
                break;
            case DemoDownloader::ResolveStatus::invalid_id:
                msg = "That demo link is not valid.";
                break;
            default:
                break; // not_found
            }
            MessageBoxA(nullptr, msg, "Alpine Faction", MB_OK | MB_ICONINFORMATION);
            return 0;
        }

        constexpr uint64_t k_max_demo_bytes = 100ull * 1024 * 1024;
        if (!already_have && info.bytes > k_max_demo_bytes) {
            MessageBoxA(nullptr, "That demo is too large to download.", "Alpine Faction", MB_OK | MB_ICONERROR);
            return 0;
        }

        // Destination path (keyed on game id so a future click dedups).
        std::string dest_path = cached_path;
        std::string display_name;
        if (already_have) {
            display_name = std::filesystem::path(cached_path).filename().string();
        }
        else {
            std::filesystem::create_directories(demos_dir);
            std::string friendly = "demo";
            if (info.game) {
                std::string base = !info.game->level_name.empty() ? info.game->level_name : info.game->level_file;
                if (base.size() > 4 && base.substr(base.size() - 4) == ".rfl")
                    base = base.substr(0, base.size() - 4);
                const std::string s = sanitize_filename_part(base);
                if (!s.empty())
                    friendly = s;
            }
            display_name = friendly + "_" + game_id + ".afd";
            dest_path = (std::filesystem::path(demos_dir) / display_name).string();
        }

        // Confirm: Watch when already cached, otherwise Download and watch / Download only / Cancel.
        DemoDisplayInfo display;
        if (info.game) {
            const auto& g = *info.game;
            display.map = !g.level_name.empty() ? g.level_name : g.level_file;
            if (display.map.empty())
                display.map = "Unknown map";
            display.gametype_name = g.gametype_name;
            display.server_name = g.server_name;
            display.mod_name = g.tc_mod;
            display.duration_ms = g.duration_ms;
            display.recorded_unix = g.started_at ? g.started_at : info.uploaded_at;
        }
        else if (already_have) {
            AfdHeaderInfo hdr;
            if (afd_read_header(cached_path, hdr) == AfdReadStatus::ok) {
                display.map = hdr.level_filename;
                if (display.map.size() > 4 && display.map.substr(display.map.size() - 4) == ".rfl")
                    display.map = display.map.substr(0, display.map.size() - 4);
                display.gametype_name = demo_game_type_name(hdr.game_type);
                display.server_name = hdr.server_name;
                display.mod_name = hdr.mod_name;
                display.recorded_unix = static_cast<int64_t>(hdr.start_time_unix);
            }
        }
        if (already_have) {
            std::error_code ec;
            display.bytes = std::filesystem::file_size(cached_path, ec);
            if (ec)
                display.bytes = 0;
        }
        else {
            display.bytes = info.bytes;
        }
        const int choice = show_demo_confirm_dialog(build_demo_confirm_text(display), !already_have);
        if (choice == 0)
            return 0;

        if (!already_have) {
            DownloadProgressDlg progressDlg(0, display_name, static_cast<size_t>(info.bytes / 1000));
            DemoDownloader::DownloadStatus dstatus = DemoDownloader::DownloadStatus::network_error;
            const std::string url = info.download_url;
            const uint64_t expected = info.bytes;

            std::thread dlThread([&]() {
                auto post_progress = [&](uint64_t received) {
                    progressDlg.SetProgress(static_cast<unsigned>(received));
                    return !progressDlg.IsCancelRequested();
                };
                try {
                    dstatus = downloader.download(url, dest_path, k_max_demo_bytes, expected, post_progress);
                    if (dstatus == DemoDownloader::DownloadStatus::link_expired) {
                        // Signed URL expired between resolve and download - resolve once more.
                        DemoDownloader::ResolveResult info2;
                        if (downloader.resolve(game_id, info2) == DemoDownloader::ResolveStatus::ok) {
                            dstatus = downloader.download(info2.download_url, dest_path, k_max_demo_bytes,
                                                          info2.bytes, post_progress);
                        }
                    }
                }
                catch (const std::exception& e) {
                    xlog::error("Demo download error: {}", e.what());
                }
                progressDlg.SetFinished(dstatus == DemoDownloader::DownloadStatus::ok);
            });

            progressDlg.DoModal(nullptr);
            dlThread.join();

            if (progressDlg.IsCancelRequested()) {
                return 0;
            }

            if (dstatus != DemoDownloader::DownloadStatus::ok) {
                MessageBoxA(nullptr, "The demo could not be downloaded. Please try again later.", "Alpine Faction",
                            MB_OK | MB_ICONERROR);
                return 0;
            }
        }

        if (choice == 1) {
            // Download and watch: launch the game straight into playback (falls through to LaunchGame).
            m_cmd_line_info.PlayDemoAfterLaunch(dest_path);
        }
        else {
            // Download only: keep it saved, do not play.
            MessageBoxA(nullptr, "Demo saved. You can watch it any time from the Demos menu in-game.",
                        "Alpine Faction", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
    }

    if (m_cmd_line_info.GetPlayDemoArg().has_value()) {
        const std::string demo_path = m_cmd_line_info.GetPlayDemoArg().value();
        xlog::info("Processing -play-demo {}", demo_path);

        std::error_code ec;
        if (!std::filesystem::exists(demo_path, ec)) {
            MessageBoxA(nullptr, "That demo file could not be found.", "Alpine Faction", MB_OK | MB_ICONERROR);
            return 0;
        }

        static const char newer_version_msg[] =
            "This demo was recorded by a newer version of Alpine Faction. Update Alpine Faction to watch it.";

        AfdHeaderInfo hdr;
        const AfdReadStatus rstatus = afd_read_header(demo_path, hdr);
        if (rstatus != AfdReadStatus::ok) {
            const char* msg = "This file is not a valid Alpine Faction demo.";
            switch (rstatus) {
            case AfdReadStatus::cant_open:
                msg = "That demo file could not be opened.";
                break;
            case AfdReadStatus::newer_format:
                msg = newer_version_msg;
                break;
            default:
                break; // bad_magic, bad_header
            }
            MessageBoxA(nullptr, msg, "Alpine Faction", MB_OK | MB_ICONERROR);
            return 0;
        }
        if ((hdr.required_features & ~AFD_KNOWN_FEATURES) != 0) {
            MessageBoxA(nullptr, newer_version_msg, "Alpine Faction", MB_OK | MB_ICONERROR);
            return 0;
        }

        DemoDisplayInfo display;
        display.map = hdr.level_filename;
        if (display.map.size() > 4 && display.map.substr(display.map.size() - 4) == ".rfl")
            display.map = display.map.substr(0, display.map.size() - 4);
        display.gametype_name = demo_game_type_name(hdr.game_type);
        display.server_name = hdr.server_name;
        display.mod_name = hdr.mod_name;
        display.recorded_unix = static_cast<int64_t>(hdr.start_time_unix);
        display.bytes = std::filesystem::file_size(demo_path, ec);
        if (ec)
            display.bytes = 0;

        if (show_demo_confirm_dialog(build_demo_confirm_text(display), false) == 0)
            return 0;

        // Falls through to LaunchGame.
        m_cmd_line_info.PlayDemoAfterLaunch(demo_path);
    }

    if (m_cmd_line_info.HasHelpFlag()) {
        // Note: we can't use stdio console API in win32 application
        Message(nullptr,
            "Usage: AlpineFactionLauncher [-game] [-level name] [-editor] args...\n"
            "-game        Starts game immediately\n"
            "-level name  Starts game immediately and loads specified level\n"
            "-levelm name  Starts game immediately and loads specified level in multiplayer\n"
            "-demo file   Starts game immediately and plays specified demo file\n"
            "-play-demo file  Shows demo info and asks before playing it\n"
            "-editor      Starts level editor immediately\n"
            "-exe-path     Override patched executable file location\n"
            "args...      Additional arguments passed to game or editor\n",
            "Alpine Faction Launcher Help", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    // Migrate config from old version
    MigrateConfig();

    // Disable elevation (UAC)
    SetEnvironmentVariableA("__COMPAT_LAYER", "RunAsInvoker");


    // Launch game or editor based on command line flag
    if (m_cmd_line_info.HasGameFlag()) {
        LaunchGame(nullptr, nullptr);
        return 0;
    }

    if (m_cmd_line_info.HasEditorFlag()) {
        LaunchEditor(nullptr, nullptr);
        return 0;
    }

    // Check for updates
    if (UpdateChecker::CheckForUpdates()) {
        return 0;
    }

    // Show main dialog
    xlog::info("Showing main dialog");
	MainDlg dlg;
	dlg.DoModal();

    xlog::info("Closing the launcher");
	return 0;
}

bool LauncherApp::ValidateAFLinkToken(const std::string& fflink_token)
{
    xlog::info("Attempting to validate FactionFiles token: {}...", fflink_token);

    std::string verify_url = "https://link.factionfiles.com/aflauncher/v1/link_check.php?token=" + fflink_token;
    //xlog::info("AFLink validity check URL: {}", verify_url);

    HttpSession session(AF_USER_AGENT_SUFFIX("Link"));

    try {
        session.set_connect_timeout(3000);
        session.set_receive_timeout(3000);
        HttpRequest req(verify_url, "GET", session);
        req.send();

        std::string response;
        char buf[256];
        while (true) {
            size_t bytesRead = req.read(buf, sizeof(buf) - 1);
            if (bytesRead == 0)
                break;
            buf[bytesRead] = '\0';
            response += buf;
        }

        response.erase(response.find_last_not_of(" \n\r\t") + 1);
        //xlog::info("AFLink verification response: {}", response);

        GameConfig game_config;
        game_config.load();

        if (response.empty()) {
            xlog::warn("FactionFiles link check failed: No response received.");
            return 0;
        }
        else if (response == "notfound") {
            xlog::warn("Invalid FactionFiles link token detected.");
            game_config.fflink_token = "";
            game_config.fflink_username = "";
            // The token is no longer valid (account unlinked or token revoked FactionFiles-side),
            // so reset the stats identity as well: the next stats-server join mints a fresh,
            // unlinked PSK rather than reusing the one tied to the now-detached account.
            game_config.afstats_psk = "";
            game_config.save();
            fflink_token_is_invalid = true;
            return 0;
        }
        else if (response.rfind("found", 0) == 0) {
            std::string username = response.substr(6); // Extract username
            xlog::info("Validated FactionFiles link for username: {}", username);
            game_config.fflink_token = fflink_token;
            game_config.fflink_username = username;
            game_config.save();
            return 1;
        }
        else {
            xlog::warn("Unexpected response from FactionFiles link check: {}.", response);
            return 0;
        }
    }
    catch (const std::exception& e) {
        xlog::warn("FactionFiles link check failed: {}.", e.what());
        return 0;
    }
}

void LauncherApp::MigrateConfig()
{
    try {
        GameConfig config;
        config.load();

        bool is_new_install = config.alpine_faction_version.value().empty();

        if (config.alpine_faction_version.value() != VERSION_STR) {
            config.alpine_faction_version = VERSION_STR;

            // On a new install, if D3D11 hardware device creation fails,
            // fall back to D3D9 instead of using the default
            if (is_new_install && config.renderer.value() == GameConfig::Renderer::d3d11
                && !is_d3d11_device_available()) {
                xlog::warn("D3D11 is not available on this machine, falling back to D3D9");
                config.renderer = GameConfig::Renderer::d3d9;
            }

            config.save();
        }
    }
    catch (std::exception&) {
        // ignore
    }
}

bool LauncherApp::LaunchGame(HWND hwnd, const char* mod_name)
{
    WatchDogTimer::ScopedStartStop wdt_start{m_watch_dog_timer};
    GameLauncher launcher;
    auto exe_path_opt = m_cmd_line_info.GetExePath();
    if (exe_path_opt) {
        launcher.set_app_exe_path(exe_path_opt.value());
    }
    if (mod_name) {
        launcher.set_mod(mod_name);
    }
    launcher.set_args(m_cmd_line_info.GetPassThroughArgs());

    try {
        xlog::info("Checking installation");
        launcher.check_installation();
        xlog::info("Installation is okay");
    }
    catch (FileNotFoundException &e) {
        std::stringstream ss;
        std::string download_url;

        ss << "Game directory validation has failed! File is missing:\n" << e.get_file_name() << "\n"
            << "Please make sure game executable specified in options is located inside a valid Red Faction installation "
            << "root directory.";
        std::string str = ss.str();
        Message(hwnd, str.c_str(), nullptr, MB_OK | MB_ICONWARNING);
        return false;
    }
    catch (FileHashVerificationException &e) {
        std::stringstream ss;
        std::string download_url;

        ss << "Game directory validation has failed! File " << e.get_file_name() << " has unrecognized hash sum.\n\n"
            << "SHA1:\n" << e.get_sha1();
        if (e.get_file_name() == "tables.vpp") {
            ss << "\n\nThis will prevent the game from functioning properly.\n"
                << "If your game has not been updated to 1.20, please do that first. If this warning still shows up, "
                << "replace your tables.vpp file with the original 1.20 NA " << e.get_file_name() << " available on FactionFiles.com.\n"
                << "Do you want to open the download page?";
            std::string str = ss.str();
            download_url = "https://www.factionfiles.com/ff.php?action=file&id=4729";
            int result = Message(hwnd, str.c_str(), nullptr, MB_YESNOCANCEL | MB_ICONWARNING);
            if (result == IDYES) {
                ShellExecuteA(hwnd, "open", download_url.c_str(), nullptr, nullptr, SW_SHOW);
                return false;
            }
            if (result == IDCANCEL) {
                return false;
            }
        }
        else {
            std::string str = ss.str();
            if (Message(hwnd, str.c_str(), nullptr, MB_OKCANCEL | MB_ICONWARNING) == IDCANCEL) {
                return false;
            }
        }
    }

    try {
        xlog::info("Launching the game...");
        launcher.launch();
        xlog::info("Game launched!");
        return true;
    }
    catch (PrivilegeElevationRequiredException&){
        Message(hwnd,
            "Privilege elevation is required. Please change RF.exe file properties and disable all "
            "compatibility settings (Run as administrator, Compatibility mode for Windows XX, etc.) or run "
            "the Alpine Faction Launcher as administrator.",
            nullptr, MB_OK | MB_ICONERROR);
    }
    catch (FileNotFoundException&) {
        Message(hwnd, "Game executable has not been found. Please set the correct path in Options.",
                nullptr, MB_OK | MB_ICONERROR);
    }
    catch (FileHashVerificationException &e) {
        std::stringstream ss;
        ss << "Unsupported game executable has been detected!\n\n"
            << "SHA1:\n" << e.get_sha1() << "\n\n"
            << "Alpine Faction requires an unmodified Red Faction 1.20 NA executable.\n"
            << "If your game has not been updated to 1.20, please do that first. If this error still shows up, "
            << "replace your RF.exe file with the original 1.20 NA RF.exe available on FactionFiles.com.\n"
            << "Click OK to open the download page.";
        std::string str = ss.str();
        if (Message(hwnd, str.c_str(), nullptr, MB_OKCANCEL | MB_ICONERROR) == IDOK) {
            ShellExecuteA(hwnd, "open", "https://www.factionfiles.com/ff.php?action=file&id=517545", nullptr, nullptr,
                SW_SHOW);
        }
    }
    catch (std::exception &e) {
        std::string msg = generate_message_for_exception(e);
        Message(hwnd, msg.c_str(), nullptr, MB_ICONERROR | MB_OK);
    }
    return false;
}

bool LauncherApp::LaunchEditor(HWND hwnd, const char* mod_name)
{
    WatchDogTimer::ScopedStartStop wdt_start{m_watch_dog_timer};
    EditorLauncher launcher;
    auto exe_path_opt = m_cmd_line_info.GetExePath();
    if (exe_path_opt) {
        launcher.set_app_exe_path(exe_path_opt.value());
    }
    if (mod_name) {
        launcher.set_mod(mod_name);
    }
    launcher.set_args(m_cmd_line_info.GetPassThroughArgs());

    try {
        xlog::info("Launching editor...");
        launcher.launch();
        xlog::info("Editor launched!");
        return true;
    }
    catch (std::exception &e) {
        std::string msg = generate_message_for_exception(e);
        Message(hwnd, msg.c_str(), nullptr, MB_ICONERROR | MB_OK);
        return false;
    }
}

int LauncherApp::Message(HWND hwnd, const char *text, const char *title, int flags)
{
    WatchDogTimer::ScopedPause wdt_pause{m_watch_dog_timer};
    xlog::info("{}: {}", title ? title : "Error", text);
    bool no_gui = GetSystemMetrics(SM_CMONITORS) == 0;
    if (no_gui) {
        std::fprintf(stderr, "%s: %s", title ? title : "Error", text);
        return -1;
    }
    return MessageBoxA(hwnd, text, title, flags);
}
