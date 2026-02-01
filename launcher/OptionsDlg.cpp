#include <wxx_wincore.h>
#include "OptionsDlg.h"
#include "LauncherApp.h"
#include "FFLinkProgressDlg.h"
#include "FFLinkHelper.h"
#include <xlog/xlog.h>
#include <wxx_dialog.h>
#include <wxx_commondlg.h>
#include <common/HttpRequest.h>
#include <common/version/version.h>
#include <chrono>

#define WM_FFLINK_COMPLETE_OPTIONS (WM_USER + 51)
#define WM_FFLINK_CANCELLED_OPTIONS (WM_USER + 52)

OptionsDlg::OptionsDlg() :
    CDialog(IDD_OPTIONS),
    m_display_dlg(m_conf),
    m_misc_dlg(m_conf),
    m_multiplayer_dlg(m_conf)
{
}

BOOL OptionsDlg::OnInitDialog()
{
    try {
        m_conf.load();
    }
    catch (std::exception &e) {
        MessageBoxA(e.what(), nullptr, MB_ICONERROR | MB_OK);
    }

    SetDlgItemTextA(IDC_EXE_PATH_EDIT, m_conf.game_executable_path->c_str());

    // Init nested dialogs
    InitNestedDialog(m_display_dlg, IDC_DISPLAY_OPTIONS_BOX);
    InitNestedDialog(m_misc_dlg, IDC_MISC_OPTIONS_BOX);
    InitNestedDialog(m_multiplayer_dlg, IDC_MULTIPLAYER_OPTIONS_BOX);

    // Update FactionFiles link status
    UpdateFFLinkStatus();

    InitToolTip();

    return TRUE;
}

void OptionsDlg::InitToolTip()
{
    m_tool_tip.Create(*this);
    m_tool_tip.AddTool(GetDlgItem(IDC_EXE_PATH_EDIT), "Path to RF.exe file in Red Faction directory");
    m_tool_tip.AddTool(GetDlgItem(IDC_FFLINK_ACTION_BTN), "Link or unlink your FactionFiles account for achievements and map ranking");
}

void OptionsDlg::OnOK()
{
    OnBnClickedOk();
    CDialog::OnOK();
}

BOOL OptionsDlg::OnCommand(WPARAM wparam, LPARAM lparam)
{
    UNREFERENCED_PARAMETER(lparam);

    UINT id = LOWORD(wparam);
    switch (id) {
    case IDC_EXE_BROWSE:
        OnBnClickedExeBrowse();
        return TRUE;
    case IDC_FFLINK_ACTION_BTN:
        OnBnClickedFFLinkAction();
        return TRUE;
    }

    return FALSE;
}

void OptionsDlg::OnBnClickedOk()
{
    // fix for strange bug that would blank exe path field if changing force port or maxfps
    if (!GetDlgItemTextA(IDC_EXE_PATH_EDIT).GetString().empty()) {
        m_conf.game_executable_path = GetDlgItemTextA(IDC_EXE_PATH_EDIT).GetString();
    }
    
    // Nested dialogs
    m_display_dlg.OnSave();
    m_multiplayer_dlg.OnSave();
    m_misc_dlg.OnSave();

    try {
        m_conf.save();
    }
    catch (std::exception &e) {
        MessageBoxA(e.what(), nullptr, MB_ICONERROR | MB_OK);
    }
}

void OptionsDlg::OnBnClickedExeBrowse()
{
    LPCTSTR filter = "Executable Files (*.exe)|*.exe|All Files (*.*)|*.*||";
    auto exe_path = GetDlgItemTextA(IDC_EXE_PATH_EDIT);
    CFileDialog dlg(TRUE, ".exe", exe_path, OFN_HIDEREADONLY, filter);
    dlg.SetTitle("Select game executable (RF.exe)");

    if (dlg.DoModal(*this) == IDOK)
        SetDlgItemText(IDC_EXE_PATH_EDIT, dlg.GetPathName());
}

void OptionsDlg::InitNestedDialog(CDialog& dlg, int placeholder_id)
{
    dlg.DoModeless(GetHwnd());
    CWnd placeholder = GetDlgItem(placeholder_id);
    RECT rc = placeholder.GetClientRect();
    RECT border = { 6, 12, 6, 6 };
    MapDialogRect(border);
    rc.left += border.left;
    rc.top += border.top;
    placeholder.MapWindowPoints(GetHwnd(), rc);
    dlg.SetWindowPos(placeholder, rc.left, rc.top, -1, -1, SWP_NOSIZE | SWP_NOACTIVATE);
}

void OptionsDlg::UpdateFFLinkStatus()
{
    std::string username = m_conf.fflink_username.value();

    if (username.empty()) {
        SetDlgItemTextA(IDC_FFLINK_STATUS_LABEL, "Status: Not Linked");
        SetDlgItemTextA(IDC_FFLINK_ACTION_BTN, "Link Account");
    } else {
        std::string status = "Linked as: " + username;
        SetDlgItemTextA(IDC_FFLINK_STATUS_LABEL, status.c_str());
        SetDlgItemTextA(IDC_FFLINK_ACTION_BTN, "Unlink");
    }
}

void OptionsDlg::OnBnClickedFFLinkAction()
{
    std::string username = m_conf.fflink_username.value();

    if (username.empty()) {
        // Link account
        xlog::info("FactionFiles Link button clicked (from Options)");

        // Generate token
        m_fflink_token = GenerateLinkToken();
        xlog::info("Generated link token: {}", m_fflink_token);

        // Open browser
        std::string url = "https://link.factionfiles.com/aflauncher/v1/link_request.php?token=" + m_fflink_token;
        HINSTANCE result = ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        auto result_int = reinterpret_cast<INT_PTR>(result);

        if (result_int <= 32) {
            xlog::error("Failed to open browser. Error code: {}", result_int);
            MessageBoxA("Failed to open browser. Please visit FactionFiles.com manually to link your account.", "Error", MB_OK | MB_ICONERROR);
            return;
        }

        // Show progress dialog and start polling
        FFLinkProgressDlg progressDlg;

        // Start polling thread
        m_fflink_polling_active = true;
        m_fflink_poll_thread = std::make_unique<std::thread>([this]() {
            try {
                HWND optionsHwnd = GetHwnd();
                int attempts = 0;
                const int max_attempts = 60; // 5 minutes (60 * 5 seconds)

                while (m_fflink_polling_active && attempts < max_attempts) {
                    // Sleep in 100ms increments to check cancellation flag frequently
                    for (int i = 0; i < 50 && m_fflink_polling_active; ++i) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }

                    if (!m_fflink_polling_active) {
                        break;
                    }

                    attempts++;
                    xlog::info("Polling FactionFiles API (attempt {})", attempts);

                    try {
                        std::string response = PollFFLinkAPI(m_fflink_token);

                        if (ParseFFLinkResponse(response, m_fflink_result_username, m_fflink_result_token)) {
                            xlog::info("Link found! Username: {}", m_fflink_result_username);
                            PostMessage(optionsHwnd, WM_FFLINK_COMPLETE_OPTIONS, 0, 0);
                            return;
                        }
                    }
                    catch (const std::exception& e) {
                        xlog::warn("Polling error (will retry): {}", e.what());
                    }
                }

                if (attempts >= max_attempts) {
                    xlog::warn("Polling timed out after {} attempts", max_attempts);
                }

                PostMessage(optionsHwnd, WM_FFLINK_CANCELLED_OPTIONS, 0, 0);
            }
            catch (const std::exception& e) {
                xlog::error("FFLink polling thread error: {}", e.what());
                PostMessage(GetHwnd(), WM_FFLINK_CANCELLED_OPTIONS, 0, 0);
            }
        });

        // Show modal dialog
        progressDlg.DoModal(GetHwnd());

        // Clean up thread
        if (m_fflink_poll_thread && m_fflink_poll_thread->joinable()) {
            m_fflink_polling_active = false;
            m_fflink_poll_thread->join();
        }
    } else {
        // Unlink account
        int result = MessageBoxA("Are you sure you want to unlink your FactionFiles account?\n\nFeatures such as achievement tracking and map ranking will be unavailable until you re-link.",
                                 "Unlink Account", MB_YESNO | MB_ICONQUESTION);

        if (result == IDYES) {
            m_conf.fflink_username = "";
            m_conf.fflink_token = "";
            m_conf.save();
            UpdateFFLinkStatus();

            // Update main window title
            HWND mainWindow = GetParent();
            if (mainWindow) {
                ::SetWindowTextA(mainWindow, "Alpine Faction Launcher - Not Linked to a FactionFiles Account");
            }

            MessageBoxA("Your FactionFiles account has been unlinked.", "Account Unlinked", MB_OK | MB_ICONINFORMATION);
        }
    }
}

LRESULT OptionsDlg::OnFFLinkComplete(WPARAM wparam, LPARAM lparam)
{
    xlog::info("FFLink complete - Username: {}", m_fflink_result_username);

    // Close progress dialog
    HWND progressDlg = FindWindow(nullptr, "Linking FactionFiles Account...");
    if (progressDlg) {
        SendMessage(progressDlg, WM_FFLINK_CLOSE_DIALOG, 0, 0);
    }

    // Save to config
    m_conf.fflink_username = m_fflink_result_username;
    m_conf.fflink_token = m_fflink_result_token;
    m_conf.save();

    // Update status display
    UpdateFFLinkStatus();

    // Update main window title
    HWND mainWindow = GetParent();
    if (mainWindow) {
        std::string window_title = "Alpine Faction Launcher - Linked to FactionFiles as " + m_fflink_result_username;
        ::SetWindowTextA(mainWindow, window_title.c_str());
    }

    // Show success message
    MessageBoxA("Alpine Faction has successfully been linked to your FactionFiles account!\n\n"
                "Features that depend on FF account linking such as achievements and map ranking are now available.",
                "Success! FactionFiles Account Linked", MB_OK | MB_ICONINFORMATION);

    // Clean up
    m_fflink_result_username.clear();
    m_fflink_result_token.clear();
    m_fflink_token.clear();

    return 0;
}

LRESULT OptionsDlg::OnFFLinkCancelled(WPARAM wparam, LPARAM lparam)
{
    xlog::info("FFLink cancelled or timed out");

    // Close progress dialog
    HWND progressDlg = FindWindow(nullptr, "Linking FactionFiles Account...");
    if (progressDlg) {
        SendMessage(progressDlg, WM_FFLINK_CLOSE_DIALOG, 0, 0);
    }

    // Clean up
    m_fflink_result_username.clear();
    m_fflink_result_token.clear();
    m_fflink_token.clear();

    return 0;
}

INT_PTR OptionsDlg::DialogProc(UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_FFLINK_COMPLETE_OPTIONS) {
        return OnFFLinkComplete(wparam, lparam);
    }

    if (msg == WM_FFLINK_CANCELLED_OPTIONS) {
        return OnFFLinkCancelled(wparam, lparam);
    }

    return CDialog::DialogProc(msg, wparam, lparam);
}
