#include <wxx_wincore.h>
#include "OptionsDisplayDlg.h"
#include "LauncherApp.h"
#include <format>
#include <xlog/xlog.h>
#include <wxx_dialog.h>
#include <wxx_commondlg.h>

OptionsDisplayDlg::OptionsDisplayDlg(GameConfig& conf) :
    CDialog(IDD_OPTIONS_DISPLAY), m_conf(conf)
{
}

BOOL OptionsDisplayDlg::OnInitDialog()
{
    try {
        m_video_info = create_device_info_provider(m_conf.renderer);
    }
    catch (const std::exception& e) {
        MessageBox(e.what(), nullptr, MB_OK);
    }

    // Attach controls
    AttachItem(IDC_RENDERER_COMBO, m_renderer_combo);
    AttachItem(IDC_ADAPTER_COMBO, m_adapter_combo);
    AttachItem(IDC_RESOLUTIONS_COMBO, m_res_combo);
    AttachItem(IDC_COLOR_DEPTH_COMBO, m_color_depth_combo);
    AttachItem(IDC_WND_MODE_COMBO, m_wnd_mode_combo);
    AttachItem(IDC_MSAA_COMBO, m_msaa_combo);

    // Populate combo boxes with static content
    m_renderer_combo.AddString("Direct3D 8");
    m_renderer_combo.AddString("Direct3D 9 (recommended)");
    m_renderer_combo.AddString("Direct3D 11 (testing)");

    m_wnd_mode_combo.AddString("Exclusive Fullscreen");
    m_wnd_mode_combo.AddString("Windowed");
    m_wnd_mode_combo.AddString("Borderless Window");

    // Display
    m_renderer_combo.SetCurSel(static_cast<int>(m_conf.renderer.value()));
    UpdateAdapterCombo();
    UpdateColorDepthCombo(); // should be before resolution
    UpdateResolutionCombo();
    m_wnd_mode_combo.SetCurSel(static_cast<int>(m_conf.wnd_mode));
    SetDlgItemInt(IDC_RENDERING_CACHE_EDIT, m_conf.geometry_cache_size, false);
    UpdateMsaaCombo();
    UpdateAnisotropyCheckbox();
    CheckDlgButton(IDC_HIGH_SCANNER_RES_CHECK, m_conf.high_scanner_res ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(IDC_TRUE_COLOR_TEXTURES_CHECK, m_conf.true_color_textures ? BST_CHECKED : BST_UNCHECKED);

    InitToolTip();

    return TRUE;
}

void OptionsDisplayDlg::UpdateMsaaCombo()
{
    m_msaa_combo.ResetContent();
    m_msaa_combo.AddString("Disabled");

    if (!m_video_info) {
        return;
    }

    int selected_msaa = 0;
    m_multi_sample_types.push_back(0);
    try {
        BOOL windowed = m_conf.wnd_mode != GameConfig::FULLSCREEN;
        auto multi_sample_types = m_video_info->get_multisample_types(m_conf.selected_video_card, m_conf.res_backbuffer_format, windowed);
        for (auto msaa : multi_sample_types) {
            auto s = std::format("MSAAx{}", msaa);
            int idx = m_msaa_combo.AddString(s.c_str());
            if (m_conf.msaa == msaa)
                selected_msaa = idx;
            m_multi_sample_types.push_back(msaa);
        }
    }
    catch (std::exception &e) {
        xlog::error("Cannot check available MSAA modes: {}", e.what());
    }
    m_msaa_combo.SetCurSel(selected_msaa);
}

void OptionsDisplayDlg::UpdateAnisotropyCheckbox()
{
    bool anisotropy_supported = false;
    try {
        if (m_video_info) {
            anisotropy_supported = m_video_info->has_anisotropy_support(m_conf.selected_video_card);
        }
    }
    catch (std::exception &e) {
        xlog::error("Cannot check anisotropy support: {}", e.what());
    }
    if (anisotropy_supported) {
        GetDlgItem(IDC_ANISOTROPIC_CHECK).EnableWindow(TRUE);
        CheckDlgButton(IDC_ANISOTROPIC_CHECK, m_conf.anisotropic_filtering ? BST_CHECKED : BST_UNCHECKED);
    }
    else
        GetDlgItem(IDC_ANISOTROPIC_CHECK).EnableWindow(FALSE);
}

void OptionsDisplayDlg::InitToolTip()
{
    m_tool_tip.Create(*this);
    m_tool_tip.AddTool(GetDlgItem(IDC_RENDERER_COMBO), "Graphics API used for rendering");
    m_tool_tip.AddTool(GetDlgItem(IDC_ADAPTER_COMBO), "Graphics card/adapter used for rendering");
    m_tool_tip.AddTool(GetDlgItem(IDC_RENDERING_CACHE_EDIT), "RAM allocated for level geometry rendering, max 32 MB");
    m_tool_tip.AddTool(GetDlgItem(IDC_RESOLUTIONS_COMBO), "Select resolution from provided dropdown list or type a resolution manually");
    m_tool_tip.AddTool(GetDlgItem(IDC_ANISOTROPIC_CHECK), "Improve render quality of textures at far distances");
    m_tool_tip.AddTool(GetDlgItem(IDC_HIGH_SCANNER_RES_CHECK), "Increase scanner resolution (used by Rail Driver, Rocket Launcher and Fusion Launcher)");
    m_tool_tip.AddTool(GetDlgItem(IDC_TRUE_COLOR_TEXTURES_CHECK), "Increase texture color depth - especially visible for lightmaps and shadows");
}

void OptionsDisplayDlg::UpdateAdapterCombo()
{
    m_adapter_combo.ResetContent();
    if (!m_video_info) {
        return;
    }
    int selected_idx = -1;
    try {
        auto adapters = m_video_info->get_adapters();
        for (const auto &adapter : adapters) {
            int idx = m_adapter_combo.AddString(adapter.c_str());
            if (idx < 0)
                throw std::runtime_error("failed to add string to combo box");
            if (m_conf.selected_video_card == static_cast<unsigned>(idx))
                selected_idx = idx;
        }
    }
    catch (std::exception &e) {
        xlog::error("Cannot get video adapters: {}", e.what());
    }
    if (selected_idx != -1)
        m_adapter_combo.SetCurSel(selected_idx);
}

void OptionsDisplayDlg::UpdateResolutionCombo()
{
    CString buf;
    int selected_res = -1;
    m_res_combo.ResetContent();
    if (!m_video_info) {
        return;
    }
    try {
        auto format = m_video_info->get_format_from_bpp(m_conf.res_bpp);
        auto resolutions = m_video_info->get_resolutions(m_conf.selected_video_card, format);
        for (const auto &res : resolutions) {
            buf.Format("%dx%d", res.width, res.height);
            int pos = m_res_combo.AddString(buf);
            if (m_conf.res_width == res.width && m_conf.res_height == res.height)
                selected_res = pos;
        }
    }
    catch (std::exception &e) {
        // Only 'Disabled' option available. Log error in console.
        xlog::error("Cannot get available screen resolutions: {}", e.what());
    }
    if (selected_res != -1)
        m_res_combo.SetCurSel(selected_res);
    else {
        auto s = std::format("{}x{}", m_conf.res_width.value(), m_conf.res_height.value());
        m_res_combo.SetWindowTextA(s.c_str());
    }
}

void OptionsDisplayDlg::UpdateColorDepthCombo()
{
    m_color_depth_combo.ResetContent();
    if (!m_video_info) {
        return;
    }
    auto format_32 = m_video_info->get_format_from_bpp(32);
    auto format_16 = m_video_info->get_format_from_bpp(16);
    bool has_16bpp_modes = !m_video_info->get_resolutions(m_conf.selected_video_card, format_16).empty();
    bool has_32bpp_modes = !m_video_info->get_resolutions(m_conf.selected_video_card, format_32).empty();

    if (!has_16bpp_modes) {
        m_conf.res_bpp = 32;
        m_conf.res_backbuffer_format = format_32;
    }
    if (!has_32bpp_modes) {
        m_conf.res_bpp = 16;
        m_conf.res_backbuffer_format = format_16;
    }
    int index_32 = -1;
    int index_16 = -1;
    if (has_32bpp_modes) {
        index_32 = m_color_depth_combo.AddString("32 bit");
    }
    if (has_16bpp_modes) {
        index_16 = m_color_depth_combo.AddString("16 bit");
    }
    m_color_depth_combo.SetCurSel(m_conf.res_bpp == 16 ? index_16 : index_32);
}

BOOL OptionsDisplayDlg::OnCommand(WPARAM wparam, [[ maybe_unused ]] LPARAM lparam)
{
    if (HIWORD(wparam) == CBN_SELCHANGE) {
        switch (LOWORD(wparam)) {
            case IDC_RENDERER_COMBO:
                OnRendererChange();
                break;
            case IDC_ADAPTER_COMBO:
                OnAdapterChange();
                break;
            case IDC_WND_MODE_COMBO:
                OnWindowModeChange();
                break;
            case IDC_COLOR_DEPTH_COMBO:
                OnColorDepthChange();
                break;
        }
    }
    return 0;
}

void OptionsDisplayDlg::OnSave()
{
    m_conf.renderer = static_cast<GameConfig::Renderer>(m_renderer_combo.GetCurSel());
    m_conf.selected_video_card = m_adapter_combo.GetCurSel();

    CString resolution_str = GetDlgItemTextA(IDC_RESOLUTIONS_COMBO);
    char *ptr = const_cast<char*>(resolution_str.c_str());
    const char *width_str = strtok(ptr, "x");
    const char *height_str = strtok(nullptr, "x");
    if (width_str && height_str) {
        m_conf.res_width = atoi(width_str);
        m_conf.res_height = atoi(height_str);
    }

    if (m_video_info) {
        m_conf.res_bpp = m_color_depth_combo.GetWindowTextA() == "32 bit" ? 32 : 16;
        m_conf.res_backbuffer_format = m_video_info->get_format_from_bpp(m_conf.res_bpp);
    }
    m_conf.wnd_mode = static_cast<GameConfig::WndMode>(m_wnd_mode_combo.GetCurSel());
    m_conf.geometry_cache_size = GetDlgItemInt(IDC_RENDERING_CACHE_EDIT, false);
    m_conf.msaa = m_multi_sample_types[m_msaa_combo.GetCurSel()];
    m_conf.anisotropic_filtering = (IsDlgButtonChecked(IDC_ANISOTROPIC_CHECK) == BST_CHECKED);
    m_conf.high_scanner_res = (IsDlgButtonChecked(IDC_HIGH_SCANNER_RES_CHECK) == BST_CHECKED);
    m_conf.true_color_textures = (IsDlgButtonChecked(IDC_TRUE_COLOR_TEXTURES_CHECK) == BST_CHECKED);
}

void OptionsDisplayDlg::OnRendererChange()
{
    m_conf.renderer = static_cast<GameConfig::Renderer>(m_renderer_combo.GetCurSel());
    try {
        m_video_info = create_device_info_provider(m_conf.renderer);
    }
    catch (const std::exception& e) {
        MessageBox(e.what(), nullptr, MB_OK);
    }

    if (m_video_info) {
        m_conf.res_backbuffer_format = m_video_info->get_format_from_bpp(m_conf.res_bpp);
    }
    UpdateAdapterCombo();
    UpdateResolutionCombo();
    UpdateColorDepthCombo();
    UpdateMsaaCombo();
    UpdateAnisotropyCheckbox();
}

void OptionsDisplayDlg::OnAdapterChange()
{
    m_conf.selected_video_card = m_adapter_combo.GetCurSel();
    UpdateResolutionCombo();
    UpdateColorDepthCombo();
    UpdateMsaaCombo();
    UpdateAnisotropyCheckbox();
}

void OptionsDisplayDlg::OnColorDepthChange()
{
    if (!m_video_info) {
        return;
    }
    m_conf.res_bpp = m_color_depth_combo.GetWindowTextA() == "32 bit" ? 32 : 16;
    m_conf.res_backbuffer_format = m_video_info->get_format_from_bpp(m_conf.res_bpp);
    UpdateResolutionCombo();
    UpdateMsaaCombo();
}

void OptionsDisplayDlg::OnWindowModeChange()
{
    m_conf.wnd_mode = static_cast<GameConfig::WndMode>(m_wnd_mode_combo.GetCurSel());
    UpdateMsaaCombo();
}
