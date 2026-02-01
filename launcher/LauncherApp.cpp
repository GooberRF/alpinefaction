#include "LauncherApp.h"
#include "faction_files.h"
#include "DownloadProgressDlg.h"
#include "MainDlg.h"
#include "LauncherCommandLineInfo.h"
#include <common/config/GameConfig.h>
#include <common/version/version.h>
#include <common/error/error-utils.h>
#include <launcher_common/PatchedAppLauncher.h>
#include <xlog/xlog.h>
#include <thread>

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

static bool fflink_token_is_invalid = false;

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
                    "Your FactionFiles account was unlinked."
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
            "Size: " + std::to_string(file_info->size_in_bytes / 1024) + " KB\n\n" +
            "Are you sure you want to install this " + file_type_string;

        // Show confirmation dialog
        int userChoice =
            MessageBoxA(nullptr, message.c_str(), "Confirm Install - Alpine Faction", MB_OKCANCEL | MB_ICONQUESTION);

        if (userChoice == IDCANCEL) {
            xlog::info("User canceled download.");
            return 0;
        }

        // Show the progress dialog
        DownloadProgressDlg progressDlg(file_id, file_info->name, file_info->size_in_bytes / 1024);

        bool download_success = false; // Track download result

        // Start the download in a separate thread
        std::thread downloadThread([&]() {
            try {
                download_success = downloader.download_and_extract(
                    file_id, file_info->file_type, [&](unsigned bytes_received, std::chrono::milliseconds duration) {
                        PostMessage(progressDlg.GetHwnd(), WM_UPDATE_PROGRESS, bytes_received, 0);
                        return true; // Continue downloading
                    });

                // Notify the progress dialog that the download is complete
                PostMessage(progressDlg.GetHwnd(), WM_DOWNLOAD_COMPLETE, download_success, 0);
            }
            catch (const std::exception& e) { // catch errors
                xlog::error("Please try again. An error occured when downloading:: {}", e.what());
                PostMessage(progressDlg.GetHwnd(), WM_DOWNLOAD_COMPLETE, FALSE, 0);
            }
        });

        // Start the progress dialog
        progressDlg.DoModal(nullptr);

        // Wait for the thread to finish
        downloadThread.join();

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

    if (m_cmd_line_info.HasHelpFlag()) {
        // Note: we can't use stdio console API in win32 application
        Message(nullptr,
            "Usage: AlpineFactionLauncher [-game] [-level name] [-editor] args...\n"
            "-game        Starts game immediately\n"
            "-level name  Starts game immediately and loads specified level\n"
            "-levelm name  Starts game immediately and loads specified level in multiplayer\n"
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
        if (config.load() && config.alpine_faction_version.value() != VERSION_STR) {
            config.alpine_faction_version = VERSION_STR;
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
