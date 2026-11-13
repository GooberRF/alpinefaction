#include <windows.h>
#include <d3d8.h>
#include "VideoDeviceInfoProvider.h"
#include <common/error/Exception.h>
#include <common/error/Win32Error.h>
#include <common/DynamicLinkLibrary.h>
#include <common/ComPtr.h>
#include <xlog/xlog.h>

class VideoDeviceInfoProviderD3D8 : public VideoDeviceInfoProvider
{
public:
    VideoDeviceInfoProviderD3D8();
    ~VideoDeviceInfoProviderD3D8() override = default;
    std::set<Resolution> get_resolutions(const DisplayInfo& display, unsigned format) override;
    std::set<uint32_t> get_multisample_types(const DisplayInfo& display, uint32_t format, bool windowed) override;

    unsigned get_format_from_bpp(unsigned bpp) override
    {
        switch (bpp) {
            case 16: return static_cast<unsigned>(D3DFMT_R5G6B5);
            default: return static_cast<unsigned>(D3DFMT_X8R8G8B8);
        }
    }

private:
    unsigned get_adapter(const DisplayInfo& display);

    DynamicLinkLibrary m_lib;
    ComPtr<IDirect3D8> m_d3d;
};

VideoDeviceInfoProviderD3D8::VideoDeviceInfoProviderD3D8() :
    m_lib{L"d3d8.dll"}
{
    if (!m_lib)
        THROW_WIN32_ERROR("Failed to load d3d8.dll");

    auto pDirect3DCreate8 = m_lib.get_proc_address<decltype(Direct3DCreate8)*>("Direct3DCreate8");
    if (!pDirect3DCreate8)
        THROW_WIN32_ERROR("Failed to load get Direct3DCreate8 function address");

    m_d3d = pDirect3DCreate8(D3D_SDK_VERSION);
    if (!m_d3d)
        THROW_EXCEPTION("Direct3DCreate8 failed");
}

unsigned VideoDeviceInfoProviderD3D8::get_adapter(const DisplayInfo& display)
{
    auto adapter = find_adapter_for_display(display, m_d3d->GetAdapterCount(), [this](unsigned i) {
        return m_d3d->GetAdapterMonitor(i);
    });
    return adapter.value_or(D3DADAPTER_DEFAULT);
}

std::set<VideoDeviceInfoProvider::Resolution> VideoDeviceInfoProviderD3D8::get_resolutions(const DisplayInfo& display, unsigned format)
{
    const unsigned adapter = get_adapter(display);
    std::set<Resolution> result;
    unsigned mode_idx = 0;
    while (true) {
        D3DDISPLAYMODE d3d_display_mode;
        HRESULT hr = m_d3d->EnumAdapterModes(adapter, mode_idx++, &d3d_display_mode);
        if (hr == D3DERR_INVALIDCALL)
            break;
        if (FAILED(hr))
            THROW_EXCEPTION("EnumAdapterModes failed: {:x}", hr);

        if (d3d_display_mode.Format != format)
            continue;

        result.insert({d3d_display_mode.Width, d3d_display_mode.Height});
    }

    return result;
}

std::set<uint32_t> VideoDeviceInfoProviderD3D8::get_multisample_types(
    const DisplayInfo& display,
    const uint32_t format,
    const bool windowed)
{
    const unsigned adapter = get_adapter(display);
    std::set<uint32_t> result{};
    for (uint32_t i = 2; i <= 16; ++i) {
        const HRESULT hr = m_d3d->CheckDeviceMultiSampleType(
            adapter,
            D3DDEVTYPE_HAL,
            static_cast<D3DFORMAT>(format),
            windowed,
            static_cast<D3DMULTISAMPLE_TYPE>(i)
        );
        if (SUCCEEDED(hr)) {
            result.insert(i);
        }
    }
    return result;
}

std::unique_ptr<VideoDeviceInfoProvider> create_d3d8_device_info_provider()
{
    return std::make_unique<VideoDeviceInfoProviderD3D8>();
}
