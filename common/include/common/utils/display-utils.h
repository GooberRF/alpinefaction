#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

struct DisplayInfo
{
    // Monitor device path, the id saved in the config
    std::string device_path;
    std::string friendly_name;
    // \\.\DISPLAYn, as in MONITORINFOEX::szDevice and DXGI_OUTPUT_DESC::DeviceName
    std::wstring gdi_name;
    RECT monitor_rect;
    RECT work_rect;
    bool primary;
};

// Active displays, ordered left to right
std::vector<DisplayInfo> enumerate_displays();
// The saved display, or the primary one when device_path is empty or no longer connected
std::optional<DisplayInfo> resolve_display(std::string_view device_path);
std::optional<unsigned> find_adapter_for_display(
    const DisplayInfo& display, unsigned adapter_count, const std::function<HMONITOR(unsigned)>& get_adapter_monitor);
