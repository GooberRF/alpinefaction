#include <wxx_wincore.h>
#include "OptionsAdvancedDlg.h"
#include "LauncherApp.h"

OptionsAdvancedDlg::OptionsAdvancedDlg(GameConfig& conf, CDialog& display_dlg)
    : CDialog(IDD_OPTIONS_ADVANCED), m_conf(conf), m_display_dlg(display_dlg)
{
}

BOOL OptionsAdvancedDlg::OnInitDialog()
{
    InitToolTip();

    SetDlgItemInt(IDC_RENDERING_CACHE_EDIT, m_conf.geometry_cache_size, false);
    CheckDlgButton(IDC_ALLOW_OVERWRITE_GAME_CHECK, m_conf.allow_overwrite_game_files ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_REDUCED_SPEED_IN_BG_CHECK, m_conf.reduced_speed_in_background ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_EDITOR_LAA_CHECK, m_conf.editor_large_address_aware ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_AUTOPLAY_AF_DEMOS_CHECK, m_conf.autoplay_af_demos ? BST_CHECKED : BST_UNCHECKED);

    return TRUE;
}

void OptionsAdvancedDlg::InitToolTip()
{
    m_tool_tip.Create(*this);
    m_tool_tip.SetMaxTipWidth(512);
    m_tool_tip.AddTool(GetDlgItem(IDC_RENDERER_COMBO), "Graphics API used for rendering.\nWarning: Direct3D 8 and 9 do not support certain features used by Alpine Faction");
    m_tool_tip.AddTool(GetDlgItem(IDC_RENDERING_CACHE_EDIT), "RAM allocated for level geometry rendering, which must be 32 MB or less");
    m_tool_tip.AddTool(GetDlgItem(IDC_ALLOW_OVERWRITE_GAME_CHECK), "Allows files in custom level folders to override core game files. Recommended to keep disabled (client_mods should be used for this instead)");
    m_tool_tip.AddTool(GetDlgItem(IDC_REDUCED_SPEED_IN_BG_CHECK), "Run the game at reduced speed when it doesn't have focus");
    m_tool_tip.AddTool(GetDlgItem(IDC_EDITOR_LAA_CHECK), "Start the level editor from a large address aware copy of RED.exe (RED_laa.exe), so it can use up to 4 GB of memory instead of 2 GB");
    m_tool_tip.AddTool(GetDlgItem(IDC_AUTOPLAY_AF_DEMOS_CHECK), "Skip the confirmation window when opening af://demo links and start playback immediately");
}

BOOL OptionsAdvancedDlg::OnCommand(WPARAM wparam, LPARAM lparam)
{
    UINT id = LOWORD(wparam);
    if (id == IDC_RENDERER_COMBO || id == IDC_COLOR_DEPTH_COMBO) {
        m_display_dlg.SendMessage(WM_COMMAND, wparam, lparam);
        return TRUE;
    }
    return FALSE;
}

void OptionsAdvancedDlg::OnSave()
{
    m_conf.geometry_cache_size = GetDlgItemInt(IDC_RENDERING_CACHE_EDIT, false);
    m_conf.allow_overwrite_game_files = (IsDlgButtonChecked(IDC_ALLOW_OVERWRITE_GAME_CHECK) == BST_CHECKED);
    m_conf.reduced_speed_in_background = (IsDlgButtonChecked(IDC_REDUCED_SPEED_IN_BG_CHECK) == BST_CHECKED);
    m_conf.editor_large_address_aware = (IsDlgButtonChecked(IDC_EDITOR_LAA_CHECK) == BST_CHECKED);
    m_conf.autoplay_af_demos = (IsDlgButtonChecked(IDC_AUTOPLAY_AF_DEMOS_CHECK) == BST_CHECKED);
}
