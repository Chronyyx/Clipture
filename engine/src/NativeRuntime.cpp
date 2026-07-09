#include "clipture/NativeRuntime.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <sstream>

namespace clipture {
namespace {

std::string narrow(const wchar_t* value) {
    if (!value) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string result(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), needed, nullptr, nullptr);
    return result;
}

Microsoft::WRL::ComPtr<IDXGIAdapter1> findPreferredAdapter(std::string& adapterName) {
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        adapterName = "DXGI factory unavailable";
        return nullptr;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> firstHardware;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 desc {};
        if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            adapter.Reset();
            continue;
        }

        if (!firstHardware) {
            firstHardware = adapter;
            adapterName = narrow(desc.Description);
        }

        if (desc.VendorId == 0x10DE) {
            adapterName = narrow(desc.Description);
            return adapter;
        }
        adapter.Reset();
    }

    return firstHardware;
}

std::string primaryOutputResolution(IDXGIAdapter1* adapter) {
    if (!adapter) return "Native monitor";

    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(0, &output))) {
        return "Native monitor";
    }

    DXGI_OUTPUT_DESC desc {};
    if (FAILED(output->GetDesc(&desc))) {
        return "Native monitor";
    }

    const auto width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
    const auto height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
    std::ostringstream resolution;
    resolution << width << "x" << height;
    return resolution.str();
}

}  // namespace

NativeRuntimeState initializeNativeRuntime(bool requireNvenc) {
    NativeRuntimeState state;
    std::string adapterName;
    const auto adapter = findPreferredAdapter(adapterName);
    state.adapterName = adapterName.empty() ? "No hardware adapter found" : adapterName;

    if (!adapter) {
        state.status = "No hardware GPU adapter is available for D3D11 capture.";
        return state;
    }

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };
    D3D_FEATURE_LEVEL selectedLevel {};
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;

    const HRESULT hr = D3D11CreateDevice(
        adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &device,
        &selectedLevel,
        &context);

    if (FAILED(hr)) {
        std::ostringstream message;
        message << "D3D11 device creation failed: HRESULT 0x" << std::hex << hr;
        state.status = message.str();
        return state;
    }

    state.d3d11Ready = true;
    state.captureReady = false;
    state.audioReady = false;
    state.muxReady = false;
    state.resolution = primaryOutputResolution(adapter.Get());
    state.status = requireNvenc
        ? "Native runtime armed on D3D11/NVENC path. WGC frame pool, NVENC session, WASAPI clients, and MP4 muxer are next."
        : "Native runtime armed on D3D11 fallback path.";
    return state;
}

}  // namespace clipture
