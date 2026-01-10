#pragma once

#include <wxx_wincore.h>
#include <wxx_dialog.h>
#include <wxx_controls.h>
#include <wxx_stdcontrols.h>
#include <memory>
#include <launcher_common/UpdateChecker.h>
#include "ImageButton.h"

class MainDlg : public CDialog
{
public:
    MainDlg();

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    BOOL OnCommand(WPARAM wparam, LPARAM lparam) override;
    LRESULT OnNotify(WPARAM wparam, LPARAM lparam) override;
    LRESULT OnShowFFLinkReminder(WPARAM wparam, LPARAM lparam);
    LRESULT OnShowWhatsNew(WPARAM wparam, LPARAM lparam);
    INT_PTR DialogProc(UINT msg, WPARAM wparam, LPARAM lparam) override;

private:
    LRESULT OnUpdateCheck(WPARAM wParam, LPARAM lParam);
    void OnBnClickedOptionsBtn();
    void OnBnClickedOk();
    void OnBnClickedEditorBtn();
    void OnSupportLinkClick(int link_id);
    void OnOpenGameFolder(int folder_id);
    void OnAboutLinkClick();
    void RefreshModSelector();
    CString GetSelectedMod();
    void AfterLaunch();
    CEdit m_news_box;
    void FetchNews();
    bool ShouldShowWhatsNew();
    std::string FetchWhatsNewContent();
    void ClearWhatsNewFlag();
    CEdit m_about_link;
    bool m_about_link_hover = false;

protected:
    // Controls
    CStatic m_picture;
    CStatic m_update_status;
    CComboBox m_mod_selector;

    ImageButton m_play_button;
    ImageButton m_editor_button;
    ImageButton m_options_button;

    //ImageButton m_sb1_button;
    ImageButton m_sb2_button;
    ImageButton m_sb3_button;
    ImageButton m_sb4_button;
    ImageButton m_sm1_button;
    ImageButton m_sm2_button;
    ImageButton m_sm3_button;
    ImageButton m_sm4_button;

    CToolTip m_tool_tip;
};
