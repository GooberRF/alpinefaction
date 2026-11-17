#include <common/utils/display-utils.h>
#include <algorithm>
#include <format>
#include <utility>

namespace
{

std::string narrow(std::wstring_view str)
{
    const int len = WideCharToMultiByte(
        CP_ACP, 0, str.data(), static_cast<int>(str.size()), nullptr, 0, nullptr, nullptr);
    std::string result(std::max(len, 0), '\0');
    WideCharToMultiByte(
        CP_ACP, 0, str.data(), static_cast<int>(str.size()), result.data(), len, nullptr, nullptr);
    return result;
}

BOOL CALLBACK add_display_monitor(HMONITOR hmon, HDC, LPRECT, LPARAM data)
{
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(hmon, &info)) {
        auto& displays = *reinterpret_cast<std::vector<DisplayInfo>*>(data);
        displays.push_back({
            .device_path = narrow(info.szDevice),
            .friendly_name = {},
            .gdi_name = info.szDevice,
            .monitor_rect = info.rcMonitor,
            .work_rect = info.rcWork,
            .primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0,
        });
    }
    return TRUE;
}

// Under Wine it can fail, and the GDI names stay the ids
void name_displays(std::vector<DisplayInfo>& displays)
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG result;
    do {
        UINT32 num_paths = 0;
        UINT32 num_modes = 0;
        result = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &num_paths, &num_modes);
        if (result != ERROR_SUCCESS) {
            return;
        }
        paths.resize(num_paths);
        modes.resize(num_modes);
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &num_paths, paths.data(), &num_modes, modes.data(), nullptr);
        paths.resize(num_paths);
    } while (result == ERROR_INSUFFICIENT_BUFFER);
    if (result != ERROR_SUCCESS) {
        return;
    }

    std::vector<bool> named(displays.size());
    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) {
            continue;
        }
        auto it = std::ranges::find(displays, std::wstring_view{source.viewGdiDeviceName}, &DisplayInfo::gdi_name);
        if (it == displays.end()) {
            continue;
        }
        const auto idx = static_cast<size_t>(it - displays.begin());
        // A cloned source has several targets: keep the first
        if (named[idx]) {
            continue;
        }

        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size = sizeof(target);
        target.header.adapterId = path.targetInfo.adapterId;
        target.header.id = path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&target.header) != ERROR_SUCCESS || !target.monitorDevicePath[0]) {
            continue;
        }
        named[idx] = true;
        it->device_path = narrow(target.monitorDevicePath);
        it->friendly_name = narrow(target.monitorFriendlyDeviceName);
    }
}

bool display_matches_monitor(const DisplayInfo& display, HMONITOR hmon)
{
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    return hmon && GetMonitorInfoW(hmon, &info) && display.gdi_name == info.szDevice;
}

} // namespace

std::vector<DisplayInfo> enumerate_displays()
{
    std::vector<DisplayInfo> displays;
    EnumDisplayMonitors(nullptr, nullptr, add_display_monitor, reinterpret_cast<LPARAM>(&displays));
    std::ranges::sort(displays, [](const DisplayInfo& a, const DisplayInfo& b) {
        return std::pair{a.monitor_rect.left, a.monitor_rect.top} < std::pair{b.monitor_rect.left, b.monitor_rect.top};
    });
    name_displays(displays);
    for (size_t i = 0; i < displays.size(); ++i) {
        if (displays[i].friendly_name.empty()) {
            displays[i].friendly_name = std::format("Display {}", i + 1);
        }
    }
    return displays;
}

std::optional<DisplayInfo> resolve_display(std::string_view device_path)
{
    auto displays = enumerate_displays();
    auto it = displays.end();
    if (!device_path.empty()) {
        it = std::ranges::find(displays, device_path, &DisplayInfo::device_path);
    }
    if (it == displays.end()) {
        it = std::ranges::find(displays, true, &DisplayInfo::primary);
    }
    if (it == displays.end()) {
        return std::nullopt;
    }
    return std::move(*it);
}

std::optional<unsigned> find_adapter_for_display(
    const DisplayInfo& display, unsigned adapter_count, const std::function<HMONITOR(unsigned)>& get_adapter_monitor)
{
    for (unsigned i = 0; i < adapter_count; ++i) {
        if (display_matches_monitor(display, get_adapter_monitor(i))) {
            return i;
        }
    }
    return std::nullopt;
}
