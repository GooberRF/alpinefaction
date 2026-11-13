#include <windows.h>
#include <d3d11.h>
#include "VideoDeviceInfoProvider.h"
#include <common/error/Exception.h>
#include <common/error/Win32Error.h>
#include <common/DynamicLinkLibrary.h>
#include <common/ComPtr.h>
#include <xlog/xlog.h>
#include <dxgi.h>

class VideoDeviceInfoProviderD3D11 : public VideoDeviceInfoProvider
{
public:
    VideoDeviceInfoProviderD3D11();
    ~VideoDeviceInfoProviderD3D11() override = default;
    std::set<Resolution> get_resolutions(const DisplayInfo& display, unsigned format) override;
    std::set<uint32_t> get_multisample_types(const DisplayInfo& display, uint32_t format, bool windowed) override;

    unsigned get_format_from_bpp(unsigned bpp) override
    {
        switch (bpp) {
            case 16: return static_cast<unsigned>(DXGI_FORMAT_B5G6R5_UNORM);
            case 32: return static_cast<unsigned>(DXGI_FORMAT_B8G8R8A8_UNORM);
            default: return static_cast<unsigned>(DXGI_FORMAT_UNKNOWN);
        }
    }

private:
    // Searches every adapter: a hybrid laptop's panel hangs off the integrated GPU
    bool find_output(const DisplayInfo& display, ComPtr<IDXGIAdapter>& adapter, ComPtr<IDXGIOutput>& output);

    DynamicLinkLibrary m_dxgi_lib;
    DynamicLinkLibrary m_d3d11_lib;
    ComPtr<IDXGIFactory> m_factory;
    decltype(D3D11CreateDevice)* m_D3D11CreateDevice;
};

VideoDeviceInfoProviderD3D11::VideoDeviceInfoProviderD3D11() :
    m_dxgi_lib{L"dxgi.dll"}, m_d3d11_lib{L"d3d11.dll"}
{
    if (!m_dxgi_lib)
        THROW_WIN32_ERROR("Failed to load dxgi.dll");

    auto pCreateDXGIFactory = m_dxgi_lib.get_proc_address<decltype(CreateDXGIFactory)*>("CreateDXGIFactory");
    if (!pCreateDXGIFactory)
        THROW_WIN32_ERROR("Failed to load get CreateDXGIFactory function address");

    HRESULT hr = pCreateDXGIFactory(IID_IDXGIFactory, reinterpret_cast<void**>(&m_factory));
    if (FAILED(hr))
        THROW_EXCEPTION("CreateDXGIFactory failed");

    if (!m_d3d11_lib)
        THROW_WIN32_ERROR("Failed to load d3d11.dll");

    m_D3D11CreateDevice = m_d3d11_lib.get_proc_address<decltype(D3D11CreateDevice)*>("D3D11CreateDevice");
    if (!m_D3D11CreateDevice)
        THROW_WIN32_ERROR("Failed to load get D3D11CreateDevice function address");
}

bool VideoDeviceInfoProviderD3D11::find_output(
    const DisplayInfo& display,
    ComPtr<IDXGIAdapter>& adapter,
    ComPtr<IDXGIOutput>& output
) {
    for (unsigned i = 0; SUCCEEDED(m_factory->EnumAdapters(i, &adapter)); ++i) {
        for (unsigned j = 0; SUCCEEDED(adapter->EnumOutputs(j, &output)); ++j) {
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(output->GetDesc(&desc)) && display.gdi_name == desc.DeviceName) {
                return true;
            }
        }
    }
    xlog::warn("[Display] No DXGI output shows display {}, using the first one", display.friendly_name);
    return SUCCEEDED(m_factory->EnumAdapters(0, &adapter)) && SUCCEEDED(adapter->EnumOutputs(0, &output));
}

std::set<VideoDeviceInfoProvider::Resolution> VideoDeviceInfoProviderD3D11::get_resolutions(
    const DisplayInfo& display,
    unsigned format
) {
    ComPtr<IDXGIAdapter> dxgi_adapter;
    ComPtr<IDXGIOutput> dxgi_output;
    if (!find_output(display, dxgi_adapter, dxgi_output)) {
        return {};
    }

    std::set<Resolution> result;

    unsigned mode_count = 0;
    HRESULT hr = dxgi_output->GetDisplayModeList(static_cast<DXGI_FORMAT>(format), 0, &mode_count, nullptr);
    if (FAILED(hr)) {
        xlog::error("GetDisplayModeList failed: {:x}", hr);
    }

    std::vector<DXGI_MODE_DESC> desc_vec;
    desc_vec.resize(mode_count);
    hr = dxgi_output->GetDisplayModeList(static_cast<DXGI_FORMAT>(format), 0, &mode_count, desc_vec.data());
    if (FAILED(hr)) {
        xlog::error("GetDisplayModeList failed: {:x}", hr);
    }

    for (auto& desc : desc_vec) {
        result.insert({desc.Width, desc.Height});
    }

    return result;
}

std::set<uint32_t> VideoDeviceInfoProviderD3D11::get_multisample_types(
    const DisplayInfo& display,
    const uint32_t format,
    [[maybe_unused]] const bool windowed
) {
    ComPtr<IDXGIAdapter> dxgi_adapter;
    ComPtr<IDXGIOutput> dxgi_output;
    if (!find_output(display, dxgi_adapter, dxgi_output)) {
        return {};
    }

    ComPtr<ID3D11Device> d3d11_device{};
    HRESULT hr = m_D3D11CreateDevice(
        dxgi_adapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        0,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &d3d11_device,
        nullptr,
        nullptr
    );
    if (FAILED(hr)) {
        xlog::error("D3D11CreateDevice failed: {:x}", hr);
        return {};
    }

    std::set<uint32_t> result{};
    for (uint32_t i = 2; i <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT; i <<= 1) {
        UINT quality_levels = 0;
        const HRESULT hr = d3d11_device->CheckMultisampleQualityLevels(
            static_cast<DXGI_FORMAT>(format),
            i,
            &quality_levels
        );
        if (SUCCEEDED(hr) && quality_levels > 0) {
            result.insert(i);
        }
    }
    return result;
}

std::unique_ptr<VideoDeviceInfoProvider> create_d3d11_device_info_provider()
{
    return std::make_unique<VideoDeviceInfoProviderD3D11>();
}

bool is_d3d11_device_available()
{
    try {
        DynamicLinkLibrary d3d11_lib{L"d3d11.dll"};
        if (!d3d11_lib)
            return false;

        auto pD3D11CreateDevice = d3d11_lib.get_proc_address<decltype(D3D11CreateDevice)*>("D3D11CreateDevice");
        if (!pD3D11CreateDevice)
            return false;

        ComPtr<ID3D11Device> device;
        HRESULT hr = pD3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            0,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            &device,
            nullptr,
            nullptr
        );
        return SUCCEEDED(hr);
    }
    catch (...) {
        return false;
    }
}
