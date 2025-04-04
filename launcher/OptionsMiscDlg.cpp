#include <wxx_wincore.h>
#include "OptionsMiscDlg.h"
#include "LauncherApp.h"
#include <wxx_commondlg.h>

OptionsMiscDlg::OptionsMiscDlg(GameConfig& conf)
	: CDialog(IDD_OPTIONS_MISC), m_conf(conf)
{
}

BOOL OptionsMiscDlg::OnInitDialog()
{
    InitToolTip();

    CheckDlgButton(IDC_FAST_START_CHECK, m_conf.fast_start ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_ALLOW_OVERWRITE_GAME_CHECK, m_conf.allow_overwrite_game_files ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_REDUCED_SPEED_IN_BG_CHECK, m_conf.reduced_speed_in_background ? BST_CHECKED : BST_UNCHECKED);

    return TRUE;
}

void OptionsMiscDlg::InitToolTip()
{
    m_tool_tip.Create(*this);
    m_tool_tip.AddTool(GetDlgItem(IDC_FAST_START_CHECK), "Skip game intro videos and go straight to main menu");
    m_tool_tip.AddTool(GetDlgItem(IDC_ALLOW_OVERWRITE_GAME_CHECK), "Allows files in custom level folders to override core game files. Recommended to keep disabled (client_mods should be used for this instead)");
    m_tool_tip.AddTool(GetDlgItem(IDC_REDUCED_SPEED_IN_BG_CHECK), "Run the game at reduced speed when it doesn't have focus");
}

void OptionsMiscDlg::OnSave()
{
    m_conf.fast_start = (IsDlgButtonChecked(IDC_FAST_START_CHECK) == BST_CHECKED);
    m_conf.allow_overwrite_game_files = (IsDlgButtonChecked(IDC_ALLOW_OVERWRITE_GAME_CHECK) == BST_CHECKED);
    m_conf.reduced_speed_in_background = (IsDlgButtonChecked(IDC_REDUCED_SPEED_IN_BG_CHECK) == BST_CHECKED);
}
