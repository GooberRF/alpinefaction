#pragma once

#include <common/config/GameConfig.h>
#include <wxx_wincore.h>
#include <wxx_dialog.h>
#include <wxx_controls.h>
#include <memory>
#include <thread>
#include <atomic>
#include "OptionsDisplayDlg.h"
#include "OptionsMiscDlg.h"
#include "OptionsMultiplayerDlg.h"

class OptionsDlg : public CDialog
{
public:
	OptionsDlg();
    OptionsDlg(const OptionsDlg&) = delete;

protected:
    BOOL OnInitDialog() override;
    void OnOK() override;
    BOOL OnCommand(WPARAM wparam, LPARAM lparam) override;
    INT_PTR DialogProc(UINT msg, WPARAM wparam, LPARAM lparam) override;

private:
    void InitToolTip();
    void OnBnClickedOk();
    void OnBnClickedExeBrowse();
    void OnBnClickedFFLinkAction();
    void UpdateFFLinkStatus();
    LRESULT OnFFLinkComplete(WPARAM wparam, LPARAM lparam);
    LRESULT OnFFLinkCancelled(WPARAM wparam, LPARAM lparam);
    void InitNestedDialog(CDialog& dlg, int placeholder_id);

    CToolTip m_tool_tip;
    GameConfig m_conf;
    OptionsDisplayDlg m_display_dlg;
    OptionsMiscDlg m_misc_dlg;
    OptionsMultiplayerDlg m_multiplayer_dlg;

    // FactionFiles Link
    std::string m_fflink_token;
    std::unique_ptr<std::thread> m_fflink_poll_thread;
    std::atomic<bool> m_fflink_polling_active{false};
    std::string m_fflink_result_username;
    std::string m_fflink_result_token;
};
