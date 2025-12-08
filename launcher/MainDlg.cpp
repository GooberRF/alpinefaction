#include "MainDlg.h"
#include "LauncherApp.h"
#include "OptionsDlg.h"
#include "AboutDlg.h"
#include "FFLinkReminderDlg.h"
#include <wxx_wincore.h>
#include <xlog/xlog.h>
#include <common/version/version.h>
#include <common/error/error-utils.h>
#include <common/utils/string-utils.h>
#include <common/HttpRequest.h>
#include <launcher_common/PatchedAppLauncher.h>
#include <launcher_common/UpdateChecker.h>
#include "ImageButton.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

#define WM_UPDATE_CHECK (WM_USER + 10)
#define WM_SHOW_FFLINK_REMINDER (WM_USER + 20)

MainDlg::MainDlg() : CDialog(IDD_MAIN)
{
}

BOOL MainDlg::OnInitDialog()
{
    CDialog::OnInitDialog();

    // Load game config
    GameConfig game_config;
    game_config.load();

    // Get fflink_username value
    std::string username = game_config.fflink_username.value();

    // Determine window title
    std::string window_title = username.empty() ? "Alpine Faction Launcher - Not Linked to a FactionFiles Account"
                                                : "Alpine Faction Launcher - Linked as " + username;

    // Set window title
    SetWindowText(window_title.c_str());

    // Set the icon for this dialog
    SetIconLarge(IDR_ICON);
    SetIconSmall(IDR_ICON);

    // Attach controls
    AttachItem(IDC_HEADER_PIC, m_picture);
    AttachItem(IDC_MOD_COMBO, m_mod_selector);
    AttachItem(IDC_UPDATE_STATUS, m_update_status);

    // Set header bitmap
    auto* hbm = static_cast<HBITMAP>(GetApp()->LoadImage(MAKEINTRESOURCE(IDB_HEADER), IMAGE_BITMAP, 0, 0, 0));
    m_picture.SetBitmap(hbm);

    // Fill mod selector
    RefreshModSelector();

    // Setup image buttons
    m_play_button.AttachDlgItem(IDC_PLAY_BTN, *this);
    m_options_button.AttachDlgItem(IDC_OPTIONS_BTN, *this);
    m_sb2_button.AttachDlgItem(IDC_SB2_BTN, *this);
    m_sb3_button.AttachDlgItem(IDC_SB3_BTN, *this);
    m_sb4_button.AttachDlgItem(IDC_SB4_BTN, *this);
    m_sm1_button.AttachDlgItem(IDC_SM1_BTN, *this);
    m_sm2_button.AttachDlgItem(IDC_SM2_BTN, *this);
    m_sm3_button.AttachDlgItem(IDC_SM3_BTN, *this);
    m_sm4_button.AttachDlgItem(IDC_SM4_BTN, *this);

    // Load button images
    m_play_button.LoadImages(IDB_PLAY_NORMAL, IDB_PLAY_HOVER, IDB_PLAY_PRESSED);
    m_options_button.LoadImages(IDB_OPTIONS_NORMAL, IDB_OPTIONS_HOVER, IDB_OPTIONS_PRESSED);
    m_sb2_button.LoadImages(IDB_SB2_NORMAL, IDB_SB2_HOVER, IDB_SB2_PRESSED);
    m_sb3_button.LoadImages(IDB_SB3_NORMAL, IDB_SB3_HOVER, IDB_SB3_PRESSED);
    m_sb4_button.LoadImages(IDB_SB4_NORMAL, IDB_SB4_HOVER, IDB_SB4_PRESSED);
    m_sm1_button.LoadImages(IDB_SM1_NORMAL, IDB_SM1_HOVER, IDB_SM1_PRESSED);
    m_sm2_button.LoadImages(IDB_SM2_NORMAL, IDB_SM2_HOVER, IDB_SM2_PRESSED);
    m_sm3_button.LoadImages(IDB_SM3_NORMAL, IDB_SM3_HOVER, IDB_SM3_PRESSED);
    m_sm4_button.LoadImages(IDB_SM4_NORMAL, IDB_SM4_HOVER, IDB_SM4_PRESSED);

    // get news feed
    AttachItem(IDC_NEWS_BOX, m_news_box);
    FetchNews();

    AttachItem(IDC_ABOUT_LINK, m_about_link);

    // Force buttons to redraw
    m_play_button.Invalidate();
    m_options_button.Invalidate();
    m_sb2_button.Invalidate();
    m_sb3_button.Invalidate();
    m_sb4_button.Invalidate();
    m_sm1_button.Invalidate();
    m_sm2_button.Invalidate();
    m_sm3_button.Invalidate();
    m_sm4_button.Invalidate();

    // Setup tooltips
    m_tool_tip.Create(*this);
    m_tool_tip.AddTool(m_mod_selector, "To find more mods, visit FactionFiles.com");
    m_tool_tip.AddTool(m_options_button, "Adjust settings");
    m_tool_tip.AddTool(m_play_button, "Launch Alpine Faction");
    m_tool_tip.AddTool(m_sm1_button, "Open the Alpine Faction level editor");
    m_tool_tip.AddTool(m_sb2_button, "Join the active Red Faction Community Discord");
    m_tool_tip.AddTool(m_sb3_button, "Visit FactionFiles.com to find community-made mods and levels or to link your account");
    m_tool_tip.AddTool(m_sb4_button, "Visit the Red Faction Wiki to access documentation and information about Red Faction");
    m_tool_tip.AddTool(m_about_link, "Current version, click to learn more about Alpine Faction");
    m_tool_tip.AddTool(m_sm2_button, "Open your mods directory");
    m_tool_tip.AddTool(m_sm3_button, "Open your clientside mods directory");
    m_tool_tip.AddTool(m_sm4_button, "Open your user_maps directory for custom levels");

    // Set placeholder text for mod box when no selection
    SendMessage(m_mod_selector.GetHwnd(), CB_SETCUEBANNER, 0, (LPARAM)L"No mod selected...");

    // show reminder if first launch
    if (!game_config.suppress_first_launch_window) {
        HWND hwnd = GetHwnd();
        PostMessageA(hwnd, WM_SHOW_FFLINK_REMINDER, 0, 0);

        game_config.suppress_first_launch_window = true;
        game_config.save();
    }

    return TRUE; // return TRUE  unless you set the focus to a control
}

LRESULT MainDlg::OnShowFFLinkReminder(WPARAM wparam, LPARAM lparam)
{
    FFLinkReminderDlg reminderDlg;
    reminderDlg.DoModal(GetHwnd());

    return 0;
}

void MainDlg::OnOK()
{
    OnBnClickedOk();
}

BOOL MainDlg::OnCommand(WPARAM wparam, LPARAM lparam)
{
    UNREFERENCED_PARAMETER(lparam);

    UINT id = LOWORD(wparam);
    switch (id) {
    case IDC_PLAY_BTN:
        OnBnClickedOk();
        return TRUE;
    case IDC_OPTIONS_BTN:
        OnBnClickedOptionsBtn();
        return TRUE;
    case IDC_SM1_BTN:
        OnBnClickedEditorBtn();
        return TRUE;
    case IDC_SB2_BTN:
        OnSupportLinkClick(0);
        return TRUE;
    case IDC_SB3_BTN:
        OnSupportLinkClick(1);
        return TRUE;
    case IDC_SB4_BTN:
        OnSupportLinkClick(2);
        return TRUE;
    case IDC_SM2_BTN:
        OnOpenGameFolder(0);
        return TRUE;
    case IDC_SM3_BTN:
        OnOpenGameFolder(1);
        return TRUE;
    case IDC_SM4_BTN:
        OnOpenGameFolder(2);
        return TRUE;
    case IDC_ABOUT_LINK:
        OnAboutLinkClick();
        return TRUE;
    case IDC_MOD_COMBO: // display "Select a mod..." when the mod selector is empty
        if (HIWORD(wparam) == CBN_EDITCHANGE) 
        {
            CString text;
            m_mod_selector.GetWindowText();

            if (text.IsEmpty())
            {
                SendMessage(m_mod_selector.GetHwnd(), CB_SETCUEBANNER, 0, (LPARAM)L"No mod selected...");
            }
        }
        break;
    }

    return FALSE;
}

LRESULT MainDlg::OnNotify([[ maybe_unused ]] WPARAM wparam, LPARAM lparam)
{
    auto& nmhdr = *reinterpret_cast<LPNMHDR>(lparam);
    switch (nmhdr.code) {
    case NM_CLICK:
        // fall through
    case NM_RETURN:
        if (nmhdr.idFrom == IDC_ABOUT_LINK) {
            OnAboutLinkClick();
        }
        else if (nmhdr.idFrom == IDC_SUPPORT_LINK) {
            OnSupportLinkClick(0);
        }
        break;
    }
    return 0;
}

INT_PTR MainDlg::DialogProc(UINT msg, WPARAM wparam, LPARAM lparam)
{
    if (msg == WM_DRAWITEM) {
        LPDRAWITEMSTRUCT lpDrawItem = (LPDRAWITEMSTRUCT)lparam;

        if (lpDrawItem->CtlID == IDC_PLAY_BTN ||
            lpDrawItem->CtlID == IDC_EDITOR_BTN ||
            lpDrawItem->CtlID == IDC_OPTIONS_BTN ||
            lpDrawItem->CtlID == IDC_SB2_BTN ||
            lpDrawItem->CtlID == IDC_SB3_BTN ||
            lpDrawItem->CtlID == IDC_SB4_BTN ||
            lpDrawItem->CtlID == IDC_SM1_BTN ||
            lpDrawItem->CtlID == IDC_SM2_BTN ||
            lpDrawItem->CtlID == IDC_SM3_BTN ||
            lpDrawItem->CtlID == IDC_SM4_BTN) {

            // Retrieve HWND of the control
            HWND hwndCtrl = GetDlgItem(lpDrawItem->CtlID).GetHwnd();
            if (!hwndCtrl)
                return FALSE; // Safety check

            // Use GetCWndPtr() to retrieve a pointer to the control
            CWnd* pWnd = GetCWndPtr(hwndCtrl);
            if (!pWnd)
                return FALSE;

            // Attempt to cast to ImageButton
            ImageButton* pButton = dynamic_cast<ImageButton*>(pWnd);
            if (pButton) {
                pButton->DrawItem(lpDrawItem);
                return TRUE; // Indicate that WM_DRAWITEM was handled
            }
        }
    }

    if (msg == WM_CTLCOLORSTATIC) {
        HDC hdcStatic = (HDC)wparam;
        if ((HWND)lparam == m_news_box.GetHwnd()) {
            SetBkMode(hdcStatic, TRANSPARENT);
            SetTextColor(hdcStatic, RGB(255, 255, 255)); // White text
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }

        if ((HWND)lparam == m_about_link.GetHwnd()) {
            SetBkMode(hdcStatic, TRANSPARENT);
            SetTextColor(hdcStatic, RGB(255, 255, 255)); // White text
            return (LRESULT)GetStockObject(NULL_BRUSH);
        }
    }

    if (msg == WM_DRAWITEM) {
        LPDRAWITEMSTRUCT lpDrawItem = (LPDRAWITEMSTRUCT)lparam;
        if (lpDrawItem->CtlID == IDC_ABOUT_LINK) {
            HDC hdc = lpDrawItem->hDC;
            SetBkMode(hdc, TRANSPARENT);           // Transparent background
            SetTextColor(hdc, RGB(255, 255, 255)); // Always white text

            std::string version_text = std::format("AF v{} ({})", VERSION_STR, VERSION_CODE);


            DrawText(hdc, version_text.c_str(), -1, &lpDrawItem->rcItem, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE);

            return TRUE;
        }
    }

    if (msg == WM_CTLCOLORBTN) {
        HDC hdcButton = (HDC)wparam;
        SetBkMode(hdcButton, TRANSPARENT);
        return (LRESULT)GetStockObject(NULL_BRUSH); // Transparent background
    }

    if (msg == WM_SHOW_FFLINK_REMINDER) {
        return OnShowFFLinkReminder(wparam, lparam);
    }

    return CDialog::DialogProc(msg, wparam, lparam);
}

void MainDlg::RefreshModSelector()
{
    xlog::info("Refreshing mods list");
    CString selected_mod;
    selected_mod = m_mod_selector.GetWindowText();
    m_mod_selector.ResetContent();
    m_mod_selector.AddString("");

    GameConfig game_config;
    try {
        game_config.load();
    }
    catch (...) {
        return;
    }
    std::string game_dir = get_dir_from_path(game_config.game_executable_path);
    std::string mods_dir = game_dir + "\\mods\\*";
    WIN32_FIND_DATA fi;

    HANDLE hfind = FindFirstFileExA(mods_dir.c_str(), FindExInfoBasic, &fi, FindExSearchLimitToDirectories, nullptr, 0);
    if (hfind != INVALID_HANDLE_VALUE) {
        do {
            bool is_dir = fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY;
            std::string_view file_name{fi.cFileName};
            if (is_dir && file_name != "." && file_name != "..") {
                m_mod_selector.AddString(fi.cFileName);
            }
        } while (FindNextFileA(hfind, &fi));
        FindClose(hfind);
    }

    m_mod_selector.SetWindowTextA(selected_mod);
}

std::string replace_html_breaks(const std::string& input)
{
    std::string output = input;
    size_t pos = 0;

    // Replace all occurrences of <br> with Windows-style newlines \r\n
    while ((pos = output.find("<br>", pos)) != std::string::npos) {
        output.replace(pos, 4, "\r\n"); // Replace "<br>" with "\r\n"
        pos += 2;                       // Move past the replacement
    }

    return output;
}

void MainDlg::FetchNews()
{
    try {
        HttpSession session{"Alpine Faction v1.2.1 News"};
        HttpRequest req{"https://update.alpinefaction.com/news.php", "GET", session};
        req.send();

        char buf[8192]; // Large enough buffer to store the response
        size_t bytes_read = req.read(buf, sizeof(buf) - 1);
        buf[bytes_read] = '\0'; // Ensure null termination

        std::string news_content(buf, bytes_read);

        // Strip HTML tags
        news_content = replace_html_breaks(news_content);

        // Convert to wide string for CEdit control
        CString news_text = news_content.c_str();

        // Update news box content
        m_news_box.SetWindowTextA(news_text);
    }
    catch (const std::exception& e) {
        xlog::error("Failed to fetch news: {}", e.what());
        m_news_box.SetWindowTextA("Failed to load news.");
    }
}

void MainDlg::OnBnClickedOptionsBtn()
{
    try {
        OptionsDlg dlg;
        INT_PTR nResponse = dlg.DoModal(*this);
        if (nResponse == IDC_PLAY_BTN) {
            RefreshModSelector();
        }
    }
    catch (std::exception& e) {
        std::string msg = generate_message_for_exception(e);
        MessageBoxA(msg.c_str(), nullptr, MB_ICONERROR | MB_OK);
    }
}

void MainDlg::OnBnClickedOk()
{
    CString selected_mod = GetSelectedMod();
    if (GetLauncherApp()->LaunchGame(*this, selected_mod))
        AfterLaunch();
}

void MainDlg::OnBnClickedEditorBtn()
{
    CStringA selected_mod = GetSelectedMod();
    if (GetLauncherApp()->LaunchEditor(*this, selected_mod))
        AfterLaunch();
}

void MainDlg::OnSupportLinkClick(int link_id)
{
    xlog::info("Opening support channel");
    std::string url = "";
    switch (link_id) {
    case 0:
        url = "https://discord.gg/factionfiles";
        break;
    case 1:
        url = "https://factionfiles.com";
        break;
    case 2:
        url = "https://redfactionwiki.com";
        break;
    }
    HINSTANCE result = ShellExecuteA(*this, "open", url.c_str(), nullptr, nullptr, SW_SHOW);
    auto result_int = reinterpret_cast<INT_PTR>(result);
    if (result_int <= 32) {
        xlog::error("ShellExecuteA failed {}", result_int);
    }
}

void MainDlg::OnOpenGameFolder(int folder_id)
{
    // Load the game executable path from GameConfig
    GameConfig gameConfig;
    if (!gameConfig.load()) {
        xlog::error("Failed to load game configuration.");
        return;
    }

    std::string game_exe_path = gameConfig.game_executable_path.value();
    if (game_exe_path.empty()) {
        xlog::error("Game executable path is empty.");
        return;
    }

    // Extract game directory from RF.exe path
    std::string game_dir = game_exe_path.substr(0, game_exe_path.find_last_of("\\/"));

    // Define folder paths relative to the game directory
    std::string folder_path;
    switch (folder_id) {
        case 0:
            folder_path = game_dir + "\\mods\\";
            break;
        case 1:
            folder_path = game_dir + "\\client_mods\\";
            break;
        case 2:
            folder_path = game_dir + "\\user_maps\\";
            break;
        default:
            xlog::error("Invalid folder ID: {}", folder_id);
            return;
    }

    xlog::info("Opening folder: {}", folder_path);

    // Open the folder in Windows Explorer
    HINSTANCE result = ShellExecuteA(nullptr, "open", folder_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    auto result_int = reinterpret_cast<INT_PTR>(result);

    if (result_int <= 32) {
        xlog::error("ShellExecuteA failed with error code: {}", result_int);
    }
}

void MainDlg::OnAboutLinkClick()
{
    AboutDlg dlg;
    dlg.DoModal(*this);
}

CString MainDlg::GetSelectedMod()
{
    return m_mod_selector.GetWindowText();
}

void MainDlg::AfterLaunch()
{
    xlog::info("Checking if launcher should be closed");
    GameConfig config;
    try {
        config.load();
    }
    catch (...) {
        // ignore
    }

    if (!config.keep_launcher_open) {
        xlog::info("Closing launcher after launch");
        CDialog::OnOK();
    }
}
