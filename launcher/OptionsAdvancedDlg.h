#pragma once

#include <common/config/GameConfig.h>
#include <wxx_wincore.h>
#include <wxx_dialog.h>
#include <wxx_controls.h>

class OptionsAdvancedDlg : public CDialog
{
public:
    OptionsAdvancedDlg(GameConfig& conf, CDialog& display_dlg);
    void OnSave();

protected:
    BOOL OnInitDialog() override;
    BOOL OnCommand(WPARAM wparam, LPARAM lparam) override;

private:
    void InitToolTip();

    GameConfig& m_conf;
    CDialog& m_display_dlg;
    CToolTip m_tool_tip;
};
