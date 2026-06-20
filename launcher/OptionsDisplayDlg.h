#pragma once

#include <launcher_common/VideoDeviceInfoProvider.h>
#include <common/config/GameConfig.h>
#include <wxx_wincore.h>
#include <wxx_dialog.h>
#include <wxx_controls.h>

class OptionsGraphicsDlg;

class OptionsDisplayDlg : public CDialog
{
public:
	OptionsDisplayDlg(GameConfig& conf);
    void OnSave();

protected:
    BOOL OnInitDialog() override;
    BOOL OnCommand(WPARAM wparam, LPARAM lparam) override;

private:
    void InitToolTip();
    void OnRendererChange();
    void OnAdapterChange();
    void OnColorDepthChange();
    void OnWindowModeChange();
    void UpdateAdapterCombo();
    void UpdateResolutionCombo();
    void UpdateColorDepthCombo();
    void UpdateAnisotropyCheckbox();

    GameConfig& m_conf;
    std::unique_ptr<VideoDeviceInfoProvider> m_video_info;
    CToolTip m_tool_tip;
    CComboBox m_renderer_combo;
    CComboBox m_adapter_combo;
    CComboBox m_res_combo;
    CComboBox m_color_depth_combo;
    CComboBox m_wnd_mode_combo;
};
