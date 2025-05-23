#include <wxx_wincore.h>
#include "OptionsMultiplayerDlg.h"
#include "LauncherApp.h"
#include <wxx_commondlg.h>

OptionsMultiplayerDlg::OptionsMultiplayerDlg(GameConfig& conf)
	: CDialog(IDD_OPTIONS_MULTIPLAYER), m_conf(conf)
{
}

BOOL OptionsMultiplayerDlg::OnInitDialog()
{
    InitToolTip();

    CheckDlgButton(IDC_FORCE_PORT_CHECK, m_conf.force_port != 0);
    if (m_conf.force_port)
        SetDlgItemInt(IDC_PORT_EDIT, m_conf.force_port, false);
    else
        GetDlgItem(IDC_PORT_EDIT).EnableWindow(FALSE);
    SetDlgItemInt(IDC_RATE_EDIT, m_conf.update_rate, false);

    return TRUE;
}

void OptionsMultiplayerDlg::InitToolTip()
{
    m_tool_tip.Create(*this);
    m_tool_tip.AddTool(GetDlgItem(IDC_RATE_EDIT), "Internet connection speed in bytes/s (recommended to leave the default value of 200000)");
    m_tool_tip.AddTool(GetDlgItem(IDC_FORCE_PORT_CHECK), "If not checked, port is randomly selected");
}

void OptionsMultiplayerDlg::OnSave()
{
    bool force_port = IsDlgButtonChecked(IDC_FORCE_PORT_CHECK) == BST_CHECKED;
    m_conf.force_port = force_port ? GetDlgItemInt(IDC_PORT_EDIT, false) : 0;
    m_conf.update_rate = GetDlgItemInt(IDC_RATE_EDIT, false);
}

BOOL OptionsMultiplayerDlg::OnCommand(WPARAM wparam, LPARAM lparam)
{
    UNREFERENCED_PARAMETER(lparam);

    UINT id = LOWORD(wparam);
    switch (id) {
    case IDC_FORCE_PORT_CHECK:
        OnForcePortClick();
        return TRUE;
    }

    return FALSE;
}

void OptionsMultiplayerDlg::OnForcePortClick()
{
    bool force_port = IsDlgButtonChecked(IDC_FORCE_PORT_CHECK) == BST_CHECKED;
    GetDlgItem(IDC_PORT_EDIT).EnableWindow(force_port);
}
