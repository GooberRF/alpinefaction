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
    AttachItem(IDC_LANG_COMBO, m_lang_combo);

    // Populate combo boxes with static content
    m_lang_combo.AddString("Auto");
    m_lang_combo.AddString("English");
    m_lang_combo.AddString("German");
    m_lang_combo.AddString("French");

    InitToolTip();

    CheckDlgButton(IDC_EAX_SOUND_CHECK, m_conf.eax_sound ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_FAST_START_CHECK, m_conf.fast_start ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_KEEP_LAUNCHER_OPEN_CHECK, m_conf.keep_launcher_open ? BST_CHECKED : BST_UNCHECKED);
    m_lang_combo.SetCurSel(m_conf.language + 1);

    return TRUE;
}

void OptionsMiscDlg::InitToolTip()
{
    m_tool_tip.Create(*this);
    m_tool_tip.AddTool(GetDlgItem(IDC_EAX_SOUND_CHECK), "Enable 3D and EAX sounds if supported");
    m_tool_tip.AddTool(GetDlgItem(IDC_FAST_START_CHECK), "Skip game intro videos and go straight to main menu");
    m_tool_tip.AddTool(GetDlgItem(IDC_KEEP_LAUNCHER_OPEN_CHECK), "Keep launcher window open after game or editor launch");
}

void OptionsMiscDlg::OnSave()
{
    m_conf.eax_sound = (IsDlgButtonChecked(IDC_EAX_SOUND_CHECK) == BST_CHECKED);
    m_conf.fast_start = (IsDlgButtonChecked(IDC_FAST_START_CHECK) == BST_CHECKED);
    m_conf.keep_launcher_open = (IsDlgButtonChecked(IDC_KEEP_LAUNCHER_OPEN_CHECK) == BST_CHECKED);
    m_conf.language = m_lang_combo.GetCurSel() - 1;
}
