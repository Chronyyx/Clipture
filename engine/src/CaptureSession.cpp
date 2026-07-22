#include "clipture/CaptureSession.hpp"
#include "clipture/FrameQueue.hpp"
#include "clipture/Tonemapper.hpp"

#include <iostream>
#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <inspectable.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>
#include <wrl/client.h>

#include <sstream>
#include <atomic>
#include <algorithm>
#include <cctype>
#include <vector>

namespace clipture {

constexpr bool ENABLE_HDR_TONEMAPPING = true;

namespace {

float GetMonitorSdrWhiteLevel(HMONITOR hMonitor) {
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMonitor, &mi)) return 4.5f;

    UINT32 pathCount = 0, modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return 4.5f;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS) return 4.5f;

    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = path.sourceInfo.adapterId;
        sourceName.header.id = path.sourceInfo.id;
        
        if (DisplayConfigGetDeviceInfo(&sourceName.header) == ERROR_SUCCESS) {
            if (wcscmp(mi.szDevice, sourceName.viewGdiDeviceName) == 0) {
                DISPLAYCONFIG_SDR_WHITE_LEVEL whiteLevel{};
                whiteLevel.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
                whiteLevel.header.size = sizeof(whiteLevel);
                whiteLevel.header.adapterId = path.targetInfo.adapterId;
                whiteLevel.header.id = path.targetInfo.id;
                
                if (DisplayConfigGetDeviceInfo(&whiteLevel.header) == ERROR_SUCCESS) {
                    return (whiteLevel.SDRWhiteLevel / 1000.0f);
                }
            }
        }
    }
    return 4.5f; // Default fallback (360 nits equivalent)
}

using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;

std::string narrow(const wchar_t* value) {
    if (!value) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string result(static_cast<std::size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), needed, nullptr, nullptr);
    return result;
}

Microsoft::WRL::ComPtr<IDXGIAdapter1> findNvidiaFirstAdapter() {
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;

    Microsoft::WRL::ComPtr<IDXGIAdapter1> firstHardware;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT index = 0; factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND; ++index) {
        DXGI_ADAPTER_DESC1 desc {};
        if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            adapter.Reset();
            continue;
        }
        if (!firstHardware) firstHardware = adapter;
        if (desc.VendorId == 0x10DE) return adapter;
        adapter.Reset();
    }
    return firstHardware;
}

std::string lowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
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

struct SelectedOutput {
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC desc {};
    std::string displayName;
    bool hdrEnabled = false;
};

bool isPrimaryMonitor(HMONITOR monitor) {
    MONITORINFOEXW info {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return false;
    return (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
}

bool monitorIdMatches(const std::string& requestedId, const DXGI_OUTPUT_DESC& desc) {
    if (requestedId.empty() || lowerAscii(requestedId) == "primary") return isPrimaryMonitor(desc.Monitor);
    return lowerAscii(requestedId) == lowerAscii(narrow(desc.DeviceName));
}

bool selectOutput(const std::string& requestedId, SelectedOutput& selected) {
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

    SelectedOutput firstHardwareOutput;
    bool hasFirstHardwareOutput = false;
    SelectedOutput primaryOutput;
    bool hasPrimaryOutput = false;

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

            SelectedOutput candidate;
            candidate.adapter = adapter;
            candidate.output = output;
            candidate.desc = desc;
            candidate.displayName = friendlyMonitorName(desc.DeviceName);
            if (candidate.displayName.empty()) candidate.displayName = narrow(desc.DeviceName);
            candidate.hdrEnabled = monitorHdrEnabled(desc.DeviceName);

            if (!hasFirstHardwareOutput) {
                firstHardwareOutput = candidate;
                hasFirstHardwareOutput = true;
            }
            if (isPrimaryMonitor(desc.Monitor)) {
                primaryOutput = candidate;
                hasPrimaryOutput = true;
            }
            if (monitorIdMatches(requestedId, desc)) {
                selected = candidate;
                return true;
            }
            output.Reset();
        }
        adapter.Reset();
    }

    if (hasPrimaryOutput) {
        selected = primaryOutput;
        return true;
    }
    if (hasFirstHardwareOutput) {
        selected = firstHardwareOutput;
        return true;
    }
    return false;
}

int64_t now100ns() {
    FILETIME fileTime {};
    GetSystemTimePreciseAsFileTime(&fileTime);
    ULARGE_INTEGER value {};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return static_cast<int64_t>(value.QuadPart);
}

}  // namespace

struct CaptureSession::Impl {
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext;
    IDirect3DDevice direct3DDevice { nullptr };
    GraphicsCaptureItem item { nullptr };
    Direct3D11CaptureFramePool framePool { nullptr };
    GraphicsCaptureSession session { nullptr };
    winrt::event_token frameArrivedToken {};
    std::atomic<int> capturedFrames = 0;
    std::atomic<int64_t> lastFramePts100ns = 0;
    std::atomic<int64_t> lastFrameInterval100ns = 0;
    FrameQueue* frameQueue = nullptr;
    std::string resolution = "Native monitor";
    std::string displayName = "Primary display";
    std::string status = "Capture session has not started.";
    bool running = false;
    bool hdrTonemappingActive = false;
    void* activeMonitor = nullptr;
    
    std::unique_ptr<Tonemapper> tonemapper;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> sdrStagingTexture;
};

CaptureSession::CaptureSession()
    : impl_(std::make_unique<Impl>()) {}

CaptureSession::~CaptureSession() {
    stop();
}

bool CaptureSession::startMonitor(FrameQueue* frameQueue, const std::string& monitorId) {
    stop();
    impl_->lastFramePts100ns.store(0);
    impl_->lastFrameInterval100ns.store(0);
    impl_->frameQueue = frameQueue;
    impl_->hdrTonemappingActive = false;
    impl_->tonemapper.reset();
    impl_->sdrStagingTexture.Reset();

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        SelectedOutput selectedOutput;
        if (!selectOutput(monitorId, selectedOutput) || !selectedOutput.adapter || !selectedOutput.output) {
            impl_->status = "No matching monitor found for WGC capture.";
            return false;
        }
        const auto& outputDesc = selectedOutput.desc;

        const auto width = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
        const auto height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;
        std::ostringstream resolution;
        resolution << width << "x" << height;
        impl_->resolution = resolution.str();
        impl_->displayName = selectedOutput.displayName;
        impl_->activeMonitor = selectedOutput.desc.Monitor;

        D3D_FEATURE_LEVEL featureLevels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0
        };
        D3D_FEATURE_LEVEL selectedLevel {};
        HRESULT hr = D3D11CreateDevice(
            selectedOutput.adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels,
            ARRAYSIZE(featureLevels),
            D3D11_SDK_VERSION,
            &impl_->d3dDevice,
            &selectedLevel,
            &impl_->d3dContext);
        if (FAILED(hr)) {
            std::ostringstream message;
            message << "D3D11 device creation for WGC failed: HRESULT 0x" << std::hex << hr;
            impl_->status = message.str();
            return false;
        }

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        hr = impl_->d3dDevice.As(&dxgiDevice);
        if (FAILED(hr)) {
            impl_->status = "Could not query IDXGIDevice from D3D11 device.";
            return false;
        }

        winrt::com_ptr<::IInspectable> inspectableDevice;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectableDevice.put());
        if (FAILED(hr)) {
            std::ostringstream message;
            message << "CreateDirect3D11DeviceFromDXGIDevice failed: HRESULT 0x" << std::hex << hr;
            impl_->status = message.str();
            return false;
        }
        impl_->direct3DDevice = inspectableDevice.as<IDirect3DDevice>();

        auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::com_ptr<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem> abiItem;
        hr = interop->CreateForMonitor(outputDesc.Monitor, __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem), abiItem.put_void());
        if (FAILED(hr)) {
            std::ostringstream message;
            message << "CreateForMonitor failed: HRESULT 0x" << std::hex << hr;
            impl_->status = message.str();
            return false;
        }

        impl_->item = abiItem.as<GraphicsCaptureItem>();
        
        DirectXPixelFormat pixelFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;
        if (ENABLE_HDR_TONEMAPPING && selectedOutput.hdrEnabled) {
            pixelFormat = DirectXPixelFormat::R16G16B16A16Float;
            impl_->tonemapper = std::make_unique<Tonemapper>(impl_->d3dDevice);
            std::string tmError;
            
            float monitorWhiteLevel = GetMonitorSdrWhiteLevel(outputDesc.Monitor);
            std::cerr << "[CaptureSession] Detected HDR Monitor SDR White Level: " << monitorWhiteLevel << " (" << (monitorWhiteLevel * 80.0f) << " nits)" << std::endl;

            if (!impl_->tonemapper->Initialize(tmError, monitorWhiteLevel)) {
                std::cerr << "[CaptureSession] Tonemapper failed to initialize: " << tmError << std::endl;
                pixelFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;
                impl_->tonemapper.reset();
                impl_->hdrTonemappingActive = false;
            } else {
                impl_->hdrTonemappingActive = true;
                std::cerr << "[CaptureSession] Tonemapper initialized successfully!" << std::endl;
            }
        } else {
            impl_->hdrTonemappingActive = false;
            impl_->tonemapper.reset();
            impl_->sdrStagingTexture.Reset();
            std::cerr << "[CaptureSession] HDR tonemapping disabled for SDR monitor: " << selectedOutput.displayName << std::endl;
        }

        impl_->framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            impl_->direct3DDevice,
            pixelFormat,
            3,
            impl_->item.Size());
            
        impl_->frameArrivedToken = impl_->framePool.FrameArrived([this](auto const& sender, auto const&) {
            try {
                auto frame = sender.TryGetNextFrame();
                if (frame) {
                    const auto timestamp100ns = now100ns();
                    const auto previousTimestamp100ns = impl_->lastFramePts100ns.exchange(timestamp100ns);
                    if (previousTimestamp100ns > 0) {
                        impl_->lastFrameInterval100ns.store(timestamp100ns - previousTimestamp100ns);
                    }
                    if (impl_->frameQueue) {
                        auto access = frame.Surface().template as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
                        if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(&texture)))) {
                            const auto size = frame.ContentSize();
                            
                            // HDR Tonemapping
                            if (impl_->tonemapper) {
                                D3D11_TEXTURE2D_DESC desc;
                                texture->GetDesc(&desc);
                                
                                static bool loggedOnce = false;
                                if (!loggedOnce) {
                                    std::cerr << "[CaptureSession] First frame format: " << desc.Format << " (Expected R16_FLOAT = 10, B8G8R8A8 = 87)" << std::endl;
                                    loggedOnce = true;
                                }

                                if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
                                    bool needsNewTexture = true;
                                    if (impl_->sdrStagingTexture) {
                                        D3D11_TEXTURE2D_DESC currentSdrDesc;
                                        impl_->sdrStagingTexture->GetDesc(&currentSdrDesc);
                                        if (currentSdrDesc.Width == desc.Width && currentSdrDesc.Height == desc.Height) {
                                            needsNewTexture = false;
                                        }
                                    }

                                    if (needsNewTexture) {
                                        D3D11_TEXTURE2D_DESC sdrDesc = desc;
                                        sdrDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                                        sdrDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
                                        sdrDesc.MiscFlags = 0; // Remove any shared flags from WGC
                                        impl_->sdrStagingTexture.Reset();
                                        impl_->d3dDevice->CreateTexture2D(&sdrDesc, nullptr, &impl_->sdrStagingTexture);
                                    }
                                    
                                    if (impl_->sdrStagingTexture) {
                                        std::string errorMsg;
                                        if (impl_->tonemapper->Process(texture, impl_->sdrStagingTexture, errorMsg)) {
                                            texture = impl_->sdrStagingTexture; // Pass the SDR texture forward
                                        } else {
                                            std::cerr << "[CaptureSession] Tonemapper process failed: " << errorMsg << std::endl;
                                        }
                                    }
                                }
                            }
                            
                            impl_->capturedFrames.fetch_add(1, std::memory_order_relaxed);
                            impl_->frameQueue->push(CapturedFrame {
                                texture,
                                timestamp100ns,
                                size.Width,
                                size.Height
                            });
                        }
                    }
                }
            } catch (...) {
            }
        });
        impl_->session = impl_->framePool.CreateCaptureSession(impl_->item);
        impl_->session.IsCursorCaptureEnabled(true);
        try {
            impl_->session.IsBorderRequired(false);
        } catch (...) {
        }
        impl_->session.StartCapture();
        impl_->running = true;
        impl_->status = "Windows.Graphics.Capture is running on " + selectedOutput.displayName + ".";
        return true;
    } catch (const winrt::hresult_error& error) {
        std::ostringstream message;
        message << "WGC start failed: " << narrow(error.message().c_str());
        impl_->status = message.str();
        impl_->running = false;
        return false;
    }
}

void CaptureSession::setTargetFps(int /*fps*/) {
    // Deprecated: Frame rate is now strictly controlled by the EncoderWorker loop
}

void CaptureSession::stop() {
    if (impl_->session) {
        impl_->session.Close();
        impl_->session = nullptr;
    }
    if (impl_->framePool) {
        impl_->framePool.FrameArrived(impl_->frameArrivedToken);
        impl_->framePool.Close();
        impl_->framePool = nullptr;
    }
    impl_->item = nullptr;
    impl_->direct3DDevice = nullptr;
    impl_->frameQueue = nullptr;
    impl_->d3dContext.Reset();
    impl_->d3dDevice.Reset();
    impl_->tonemapper.reset();
    impl_->sdrStagingTexture.Reset();
    impl_->hdrTonemappingActive = false;
    impl_->running = false;
}

bool CaptureSession::running() const {
    return impl_->running;
}

int CaptureSession::capturedFrames() const {
    return impl_->capturedFrames.load(std::memory_order_relaxed);
}

int64_t CaptureSession::lastFrameInterval100ns() const {
    return impl_->lastFrameInterval100ns.load(std::memory_order_relaxed);
}

std::string CaptureSession::resolution() const {
    return impl_->resolution;
}

std::string CaptureSession::displayName() const {
    return impl_->displayName;
}

bool CaptureSession::hdrTonemappingActive() const {
    return impl_->hdrTonemappingActive;
}

std::string CaptureSession::status() const {
    return impl_->status;
}

void* CaptureSession::activeMonitor() const {
    return impl_->activeMonitor;
}

}  // namespace clipture
