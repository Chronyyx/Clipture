#include "clipture/Diagnostics.hpp"

#include <Windows.h>
#include <dxgi1_6.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <vector>

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

std::wstring widen(const std::string& value) {
    if (value.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (needed <= 1) return {};
    std::wstring result(static_cast<std::size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), needed);
    return result;
}

std::string jsonEscape(const std::string& value) {
    std::ostringstream out;
    for (const char ch : value) {
        switch (ch) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default: out << ch; break;
        }
    }
    return out.str();
}

struct GpuProbe {
    bool hasNvidia = false;
    std::string adapterName = "No DXGI adapter found";
};

GpuProbe probeGpu() {
    GpuProbe result;
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        result.adapterName = "DXGI factory unavailable";
        return result;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 desc {};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        const auto name = narrow(desc.Description);
        if (result.adapterName == "No DXGI adapter found") {
            result.adapterName = name;
        }
        if (desc.VendorId == 0x10DE) {
            result.hasNvidia = true;
            result.adapterName = name;
            return result;
        }
        adapter.Reset();
    }
    return result;
}

bool nvencApiAvailable() {
    HMODULE module = LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!module) {
        module = LoadLibraryW(L"nvEncodeAPI.dll");
    }
    if (!module) return false;
    const auto createInstance = GetProcAddress(module, "NvEncodeAPICreateInstance");
    FreeLibrary(module);
    return createInstance != nullptr;
}

struct ComApartment {
    bool initialized = false;
    bool usable = false;

    ComApartment() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized = SUCCEEDED(hr);
        usable = initialized || hr == RPC_E_CHANGED_MODE;
    }

    ~ComApartment() {
        if (initialized) CoUninitialize();
    }

    bool available() const {
        return usable;
    }
};

std::string propertyStoreName(IPropertyStore* properties) {
    if (!properties) return "Unknown";
    PROPVARIANT value;
    PropVariantInit(&value);
    const HRESULT hr = properties->GetValue(PKEY_Device_FriendlyName, &value);
    std::string name = "Unknown";
    if (SUCCEEDED(hr) && value.vt == VT_LPWSTR && value.pwszVal) {
        name = narrow(value.pwszVal);
    }
    PropVariantClear(&value);
    return name;
}

std::string propertyStoreString(IPropertyStore* properties, const PROPERTYKEY& key) {
    if (!properties) return {};
    PROPVARIANT value;
    PropVariantInit(&value);
    const HRESULT hr = properties->GetValue(key, &value);
    std::string result;
    if (SUCCEEDED(hr) && value.vt == VT_LPWSTR && value.pwszVal) {
        result = narrow(value.pwszVal);
    }
    PropVariantClear(&value);
    return result;
}

std::string deviceFriendlyName(IMMDevice* device) {
    if (!device) return "Unknown";
    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    const HRESULT hr = device->OpenPropertyStore(STGM_READ, &properties);
    if (FAILED(hr) || !properties) return "Unknown";
    return propertyStoreName(properties.Get());
}

std::string deviceId(IMMDevice* device);

std::string deviceMatchKey(IMMDevice* device) {
    if (!device) return {};
    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    const HRESULT hr = device->OpenPropertyStore(STGM_READ, &properties);
    if (SUCCEEDED(hr) && properties) {
        const auto instanceId = propertyStoreString(properties.Get(), PKEY_Device_InstanceId);
        if (!instanceId.empty()) return instanceId;
    }
    return deviceId(device);
}

std::string deviceId(IMMDevice* device) {
    if (!device) return {};
    LPWSTR id = nullptr;
    const HRESULT hr = device->GetId(&id);
    if (FAILED(hr) || !id) return {};
    const std::string result = narrow(id);
    CoTaskMemFree(id);
    return result;
}

Microsoft::WRL::ComPtr<IMMDevice> defaultEndpoint(EDataFlow dataFlow) {
    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return nullptr;

    Microsoft::WRL::ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(dataFlow, eConsole, &device);
    if (FAILED(hr)) return nullptr;
    return device;
}

std::string defaultEndpointName(EDataFlow dataFlow) {
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coInitialized = SUCCEEDED(coHr);
    if (FAILED(coHr) && coHr != RPC_E_CHANGED_MODE) {
        return "Unavailable";
    }

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) {
        if (coInitialized) CoUninitialize();
        return "Unavailable";
    }

    Microsoft::WRL::ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(dataFlow, eConsole, &device);
    if (FAILED(hr) || !device) {
        if (coInitialized) CoUninitialize();
        return "No default device";
    }

    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    hr = device->OpenPropertyStore(STGM_READ, &properties);
    if (FAILED(hr) || !properties) {
        if (coInitialized) CoUninitialize();
        return "Unknown";
    }

    PROPVARIANT value;
    PropVariantInit(&value);
    hr = properties->GetValue(PKEY_Device_FriendlyName, &value);
    std::string name = "Unknown";
    if (SUCCEEDED(hr) && value.vt == VT_LPWSTR && value.pwszVal) {
        name = narrow(value.pwszVal);
    }
    PropVariantClear(&value);
    if (coInitialized) CoUninitialize();
    return name;
}

bool sourceNameForPath(const DISPLAYCONFIG_PATH_INFO& path, DISPLAYCONFIG_SOURCE_DEVICE_NAME& sourceName) {
    sourceName = {};
    sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    sourceName.header.size = sizeof(sourceName);
    sourceName.header.adapterId = path.sourceInfo.adapterId;
    sourceName.header.id = path.sourceInfo.id;
    return DisplayConfigGetDeviceInfo(&sourceName.header) == ERROR_SUCCESS;
}

bool activeDisplayPathForGdiName(const wchar_t* gdiName, DISPLAYCONFIG_PATH_INFO& matchedPath) {
    if (!gdiName || !*gdiName) return false;

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return false;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) return false;

    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName {};
        if (sourceNameForPath(path, sourceName) && wcscmp(gdiName, sourceName.viewGdiDeviceName) == 0) {
            matchedPath = path;
            return true;
        }
    }
    return false;
}

std::string friendlyMonitorName(const wchar_t* gdiName) {
    DISPLAYCONFIG_PATH_INFO path {};
    if (!activeDisplayPathForGdiName(gdiName, path)) return {};

    DISPLAYCONFIG_TARGET_DEVICE_NAME targetName {};
    targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    targetName.header.size = sizeof(targetName);
    targetName.header.adapterId = path.targetInfo.adapterId;
    targetName.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&targetName.header) != ERROR_SUCCESS) return {};

    const auto friendly = narrow(targetName.monitorFriendlyDeviceName);
    if (!friendly.empty()) return friendly;
    return narrow(targetName.monitorDevicePath);
}

bool monitorHdrEnabled(const wchar_t* gdiName) {
    DISPLAYCONFIG_PATH_INFO path {};
    if (!activeDisplayPathForGdiName(gdiName, path)) return false;

    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO colorInfo {};
    colorInfo.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    colorInfo.header.size = sizeof(colorInfo);
    colorInfo.header.adapterId = path.targetInfo.adapterId;
    colorInfo.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&colorInfo.header) != ERROR_SUCCESS) return false;

    return colorInfo.advancedColorEnabled != 0;
}

}  // namespace

std::string encoderName(EncoderName encoder) {
    switch (encoder) {
        case EncoderName::Nvenc: return "NVENC";
        case EncoderName::MediaFoundationHardware: return "Media Foundation Hardware";
        case EncoderName::Software: return "Software";
        case EncoderName::Unavailable: return "Unavailable";
    }
    return "Unavailable";
}

Diagnostics collectDiagnostics() {
    Diagnostics diagnostics;
    const auto gpu = probeGpu();
    const bool nvenc = gpu.hasNvidia && nvencApiAvailable();

    diagnostics.gpu = gpu.adapterName;
    diagnostics.microphoneDevice = defaultEndpointName(eCapture);
    diagnostics.nvencAvailable = nvenc;
    diagnostics.activeEncoder = nvenc ? EncoderName::Nvenc : EncoderName::Unavailable;
    diagnostics.encoderMode = nvenc ? "NVENC async P3 (4 output buffers)" : "Unavailable";
    diagnostics.hardwareAcceleration = nvenc;
    diagnostics.degraded = !nvenc;
    diagnostics.status = nvenc
        ? "Direct NVENC is available. Capture engine is ready for the NVENC-first path."
        : "Direct NVENC is unavailable. Install NVIDIA drivers with NVENC support or run on supported NVIDIA hardware.";

    return diagnostics;
}

std::string microphoneDeviceName(const std::string& deviceIdValue) {
    ComApartment com;
    if (!com.available()) return "Unavailable";

    if (deviceIdValue.empty()) {
        auto device = defaultEndpoint(eCapture);
        if (!device) return "No default device";
        return deviceFriendlyName(device.Get());
    }

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return "Unavailable";

    Microsoft::WRL::ComPtr<IMMDevice> device;
    const auto wideId = widen(deviceIdValue);
    hr = enumerator->GetDevice(wideId.c_str(), &device);
    if (FAILED(hr) || !device) return "Selected device unavailable";
    return deviceFriendlyName(device.Get());
}

std::string audioEndpointMatchKey(const std::string& deviceIdValue) {
    ComApartment com;
    if (!com.available() || deviceIdValue.empty()) return {};

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return {};

    Microsoft::WRL::ComPtr<IMMDevice> device;
    const auto wideId = widen(deviceIdValue);
    hr = enumerator->GetDevice(wideId.c_str(), &device);
    if (FAILED(hr) || !device) return {};
    return deviceMatchKey(device.Get());
}

std::string audioInputDevicesJson() {
    ComApartment com;
    if (!com.available()) return "[]";

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return "[]";

    auto defaultDevice = defaultEndpoint(eCapture);
    const auto defaultId = deviceId(defaultDevice.Get());

    Microsoft::WRL::ComPtr<IMMDeviceCollection> collection;
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) return "[]";

    UINT count = 0;
    collection->GetCount(&count);

    std::ostringstream out;
    out << "[";
    bool wroteDevice = false;
    for (UINT index = 0; index < count; ++index) {
        Microsoft::WRL::ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(index, &device)) || !device) continue;
        const auto id = deviceId(device.Get());
        const auto name = deviceFriendlyName(device.Get());
        const auto matchKey = deviceMatchKey(device.Get());
        if (wroteDevice) out << ",";
        out << "{"
            << "\"id\":\"" << jsonEscape(id) << "\","
            << "\"name\":\"" << jsonEscape(name) << "\","
            << "\"isDefault\":" << (!id.empty() && id == defaultId ? "true" : "false") << ","
            << "\"state\":\"active\","
            << "\"matchKey\":\"" << jsonEscape(matchKey) << "\""
            << "}";
        wroteDevice = true;
    }
    out << "]";
    return out.str();
}

std::string displayDevicesJson() {
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return "[]";

    std::ostringstream out;
    out << "[";
    bool wroteDisplay = false;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT adapterIndex = 0; factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND; ++adapterIndex) {
        DXGI_ADAPTER_DESC1 adapterDesc {};
        if (FAILED(adapter->GetDesc1(&adapterDesc)) || (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            adapter.Reset();
            continue;
        }

        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        for (UINT outputIndex = 0; adapter->EnumOutputs(outputIndex, &output) != DXGI_ERROR_NOT_FOUND; ++outputIndex) {
            DXGI_OUTPUT_DESC desc {};
            if (FAILED(output->GetDesc(&desc))) {
                output.Reset();
                continue;
            }

            MONITORINFOEXW monitorInfo {};
            monitorInfo.cbSize = sizeof(monitorInfo);
            GetMonitorInfoW(desc.Monitor, &monitorInfo);

            const int width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            const int height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
            const auto id = narrow(desc.DeviceName);
            auto name = friendlyMonitorName(desc.DeviceName);
            if (name.empty()) name = id.empty() ? "Display" : id;

            if (wroteDisplay) out << ",";
            out << "{"
                << "\"id\":\"" << jsonEscape(id) << "\","
                << "\"name\":\"" << jsonEscape(name) << "\","
                << "\"isPrimary\":" << ((monitorInfo.dwFlags & MONITORINFOF_PRIMARY) ? "true" : "false") << ","
                << "\"width\":" << width << ","
                << "\"height\":" << height << ","
                << "\"x\":" << desc.DesktopCoordinates.left << ","
                << "\"y\":" << desc.DesktopCoordinates.top << ","
                << "\"hdr\":" << (monitorHdrEnabled(desc.DeviceName) ? "true" : "false")
                << "}";
            wroteDisplay = true;
            output.Reset();
        }
        adapter.Reset();
    }

    out << "]";
    return out.str();
}

std::pair<int, int> maxDisplayDimensions() {
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return { 0, 0 };

    int maxWidth = 0;
    int maxHeight = 0;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT adapterIndex = 0; factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND; ++adapterIndex) {
        DXGI_ADAPTER_DESC1 adapterDesc {};
        if (FAILED(adapter->GetDesc1(&adapterDesc)) || (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            adapter.Reset();
            continue;
        }

        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        for (UINT outputIndex = 0; adapter->EnumOutputs(outputIndex, &output) != DXGI_ERROR_NOT_FOUND; ++outputIndex) {
            DXGI_OUTPUT_DESC desc {};
            if (SUCCEEDED(output->GetDesc(&desc))) {
                const int width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
                const int height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
                maxWidth = std::max(maxWidth, width);
                maxHeight = std::max(maxHeight, height);
            }
            output.Reset();
        }
        adapter.Reset();
    }

    return { maxWidth, maxHeight };
}


std::string toJson(const Diagnostics& diagnostics) {
    std::ostringstream out;
    out << "{"
        << "\"captureApi\":\"" << jsonEscape(diagnostics.captureApi) << "\","
        << "\"activeEncoder\":\"" << encoderName(diagnostics.activeEncoder) << "\","
        << "\"encoderMode\":\"" << jsonEscape(diagnostics.encoderMode) << "\","
        << "\"gpu\":\"" << jsonEscape(diagnostics.gpu) << "\","
        << "\"microphoneDevice\":\"" << jsonEscape(diagnostics.microphoneDevice) << "\","
        << "\"display\":\"" << jsonEscape(diagnostics.display) << "\","
        << "\"hdrTonemapping\":" << (diagnostics.hdrTonemapping ? "true" : "false") << ","
        << "\"videoSourceResolution\":\"" << jsonEscape(diagnostics.videoSourceResolution) << "\","
        << "\"videoOutputResolution\":\"" << jsonEscape(diagnostics.videoOutputResolution) << "\","
        << "\"videoScaling\":\"" << jsonEscape(diagnostics.videoScaling) << "\","
        << "\"clipTargetResolution\":\"" << jsonEscape(diagnostics.clipTargetResolution) << "\","
        << "\"codec\":\"" << jsonEscape(diagnostics.codec) << "\","
        << "\"resolution\":\"" << jsonEscape(diagnostics.resolution) << "\","
        << "\"fps\":" << diagnostics.fps << ","
        << "\"bitrateMbps\":" << diagnostics.bitrateMbps << ","
        << "\"hardwareAcceleration\":" << (diagnostics.hardwareAcceleration ? "true" : "false") << ","
        << "\"droppedFrames\":" << diagnostics.droppedFrames << ","
        << "\"nvencAvailable\":" << (diagnostics.nvencAvailable ? "true" : "false") << ","
        << "\"engineRunning\":" << (diagnostics.engineRunning ? "true" : "false") << ","
        << "\"d3d11Ready\":" << (diagnostics.d3d11Ready ? "true" : "false") << ","
        << "\"captureReady\":" << (diagnostics.captureReady ? "true" : "false") << ","
        << "\"audioReady\":" << (diagnostics.audioReady ? "true" : "false") << ","
        << "\"muxReady\":" << (diagnostics.muxReady ? "true" : "false") << ","
        << "\"bufferedVideoPackets\":" << diagnostics.bufferedVideoPackets << ","
        << "\"bufferedAudioPackets\":" << diagnostics.bufferedAudioPackets << ","
        << "\"capturedFrames\":" << diagnostics.capturedFrames << ","
        << "\"queuedFrames\":" << diagnostics.queuedFrames << ","
        << "\"encoderAcceptedFrames\":" << diagnostics.encoderAcceptedFrames << ","
        << "\"encoderOutputPackets\":" << diagnostics.encoderOutputPackets << ","
        << "\"audioCapturedPackets\":" << diagnostics.audioCapturedPackets << ","
        << "\"bufferDurationSeconds\":" << diagnostics.bufferDurationSeconds << ","
        << "\"degraded\":" << (diagnostics.degraded ? "true" : "false") << ","
        << "\"status\":\"" << jsonEscape(diagnostics.status) << "\""
        << "}";
    return out.str();
}

}  // namespace clipture
