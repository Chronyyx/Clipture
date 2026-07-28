#include "clipture/CaptureSession.hpp"
#include "clipture/FrameQueue.hpp"
#include "clipture/MediaClock.hpp"
#include "clipture/Tonemapper.hpp"

#include <iostream>
#include <Windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
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
#include <memory>
#include <mutex>
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

struct OwnedCaptureSlot {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> hdrInputTexture;
    std::atomic<bool> leased = false;
};

struct AcquiredCaptureSlot {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> hdrInputTexture;
    std::shared_ptr<void> lease;
};

}  // namespace

struct CaptureSession::Impl {
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext;
    IDirect3DDevice direct3DDevice { nullptr };
    GraphicsCaptureItem item { nullptr };
    Direct3D11CaptureFramePool framePool { nullptr };
    GraphicsCaptureSession session { nullptr };
    winrt::event_token frameArrivedToken {};
    winrt::event_token itemClosedToken {};
    std::atomic<int> capturedFrames = 0;
    std::atomic<int64_t> lastFramePts100ns = 0;
    std::atomic<int64_t> lastFrameInterval100ns = 0;
    std::atomic<int64_t> maximumFrameInterval100ns = 0;
    std::atomic<int64_t> lastQueuedFramePts100ns = 0;
    std::atomic<int> targetFps = 60;
    std::atomic<uint64_t> captureEpoch = 0;
    std::atomic<uint64_t> frameSequence = 0;
    std::atomic<uint64_t> ownedSlotDrops = 0;
    std::atomic<uint64_t> callbackErrors = 0;
    FrameQueue* frameQueue = nullptr;
    std::string resolution = "Native monitor";
    std::string displayName = "Primary display";
    std::string status = "Capture session has not started.";
    std::atomic<bool> running = false;
    std::atomic<bool> hdrTonemappingActive = false;
    std::atomic<void*> activeMonitor = nullptr;

    DirectXPixelFormat framePoolPixelFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;
    winrt::Windows::Graphics::SizeInt32 framePoolSize {};
    std::mutex callbackMutex;
    std::mutex slotMutex;
    mutable std::mutex stateMutex;
    std::vector<std::shared_ptr<OwnedCaptureSlot>> captureSlots;
    D3D11_TEXTURE2D_DESC captureSlotDesc {};
    std::unique_ptr<Tonemapper> tonemapper;

    void setStatus(std::string nextStatus) {
        std::lock_guard lock(stateMutex);
        status = std::move(nextStatus);
    }

    AcquiredCaptureSlot acquireCaptureSlot(UINT width, UINT height, bool needsUnorderedAccess) {
        if (!d3dDevice || width == 0 || height == 0) return {};

        std::lock_guard lock(slotMutex);
        const bool needsNewGeneration = captureSlots.empty() ||
            captureSlotDesc.Width != width ||
            captureSlotDesc.Height != height ||
            ((captureSlotDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0) != needsUnorderedAccess;
        if (needsNewGeneration) {
            if (tonemapper) tonemapper->ResetViewCache();
            captureSlots.clear();
            captureSlotDesc = {};
            captureSlotDesc.Width = width;
            captureSlotDesc.Height = height;
            captureSlotDesc.MipLevels = 1;
            captureSlotDesc.ArraySize = 1;
            captureSlotDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            captureSlotDesc.SampleDesc.Count = 1;
            captureSlotDesc.Usage = D3D11_USAGE_DEFAULT;
            captureSlotDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            if (needsUnorderedAccess) captureSlotDesc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

            constexpr std::size_t captureSlotCount = 6;
            captureSlots.reserve(captureSlotCount);
            for (std::size_t index = 0; index < captureSlotCount; ++index) {
                auto slot = std::make_shared<OwnedCaptureSlot>();
                const HRESULT hr = d3dDevice->CreateTexture2D(&captureSlotDesc, nullptr, &slot->texture);
                if (FAILED(hr) || !slot->texture) {
                    captureSlots.clear();
                    return {};
                }
                if (needsUnorderedAccess) {
                    D3D11_TEXTURE2D_DESC hdrInputDesc = captureSlotDesc;
                    hdrInputDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                    hdrInputDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                    const HRESULT hdrHr = d3dDevice->CreateTexture2D(
                        &hdrInputDesc,
                        nullptr,
                        &slot->hdrInputTexture);
                    if (FAILED(hdrHr) || !slot->hdrInputTexture) {
                        captureSlots.clear();
                        return {};
                    }
                }
                captureSlots.push_back(std::move(slot));
            }
        }

        for (const auto& slot : captureSlots) {
            bool expected = false;
            if (!slot->leased.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) continue;
            auto lease = std::shared_ptr<void>(slot.get(), [slot](void*) {
                slot->leased.store(false, std::memory_order_release);
            });
            return { slot->texture, slot->hdrInputTexture, std::move(lease) };
        }
        ++ownedSlotDrops;
        return {};
    }

    void discardCaptureSlotGeneration() {
        std::lock_guard lock(slotMutex);
        if (tonemapper) tonemapper->ResetViewCache();
        captureSlots.clear();
        captureSlotDesc = {};
    }
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
    impl_->maximumFrameInterval100ns.store(0);
    impl_->lastQueuedFramePts100ns.store(0);
    impl_->frameQueue = frameQueue;
    if (impl_->frameQueue) impl_->frameQueue->clear();
    ++impl_->captureEpoch;
    impl_->hdrTonemappingActive = false;
    impl_->tonemapper.reset();
    impl_->discardCaptureSlotGeneration();

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        SelectedOutput selectedOutput;
        if (!selectOutput(monitorId, selectedOutput) || !selectedOutput.adapter || !selectedOutput.output) {
            impl_->setStatus("No matching monitor found for WGC capture.");
            return false;
        }
        const auto& outputDesc = selectedOutput.desc;

        const auto width = outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
        const auto height = outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;
        std::ostringstream resolution;
        resolution << width << "x" << height;
        {
            std::lock_guard stateLock(impl_->stateMutex);
            impl_->resolution = resolution.str();
            impl_->displayName = selectedOutput.displayName;
        }
        impl_->activeMonitor.store(selectedOutput.desc.Monitor, std::memory_order_release);

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
            impl_->setStatus(message.str());
            return false;
        }

        Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(impl_->d3dContext.As(&multithread)) && multithread) {
            multithread->SetMultithreadProtected(TRUE);
        }

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        hr = impl_->d3dDevice.As(&dxgiDevice);
        if (FAILED(hr)) {
            impl_->setStatus("Could not query IDXGIDevice from D3D11 device.");
            return false;
        }

        winrt::com_ptr<::IInspectable> inspectableDevice;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectableDevice.put());
        if (FAILED(hr)) {
            std::ostringstream message;
            message << "CreateDirect3D11DeviceFromDXGIDevice failed: HRESULT 0x" << std::hex << hr;
            impl_->setStatus(message.str());
            return false;
        }
        impl_->direct3DDevice = inspectableDevice.as<IDirect3DDevice>();

        auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::com_ptr<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem> abiItem;
        hr = interop->CreateForMonitor(outputDesc.Monitor, __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem), abiItem.put_void());
        if (FAILED(hr)) {
            std::ostringstream message;
            message << "CreateForMonitor failed: HRESULT 0x" << std::hex << hr;
            impl_->setStatus(message.str());
            return false;
        }

        impl_->item = abiItem.as<GraphicsCaptureItem>();
        impl_->itemClosedToken = impl_->item.Closed([this](auto const&, auto const&) {
            impl_->running.store(false, std::memory_order_release);
            impl_->setStatus("Windows.Graphics.Capture item closed; recovery is pending.");
        });
        
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
            std::cerr << "[CaptureSession] HDR tonemapping disabled for SDR monitor: " << selectedOutput.displayName << std::endl;
        }

        impl_->framePoolPixelFormat = pixelFormat;
        impl_->framePoolSize = impl_->item.Size();

        impl_->framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            impl_->direct3DDevice,
            pixelFormat,
            3,
            impl_->framePoolSize);
            
        impl_->frameArrivedToken = impl_->framePool.FrameArrived([this](auto const& sender, auto const&) {
            try {
                std::lock_guard callbackLock(impl_->callbackMutex);
                auto frame = sender.TryGetNextFrame();
                if (!frame || !impl_->running.load(std::memory_order_acquire)) return;
                while (auto newerFrame = sender.TryGetNextFrame()) frame = std::move(newerFrame);

                const auto size = frame.ContentSize();
                if (size.Width <= 0 || size.Height <= 0) return;
                if (size.Width != impl_->framePoolSize.Width || size.Height != impl_->framePoolSize.Height) {
                    frame = nullptr;
                    impl_->framePoolSize = size;
                    impl_->discardCaptureSlotGeneration();
                    if (impl_->frameQueue) impl_->frameQueue->clear();
                    ++impl_->captureEpoch;
                    {
                        std::lock_guard stateLock(impl_->stateMutex);
                        impl_->resolution = std::to_string(size.Width) + "x" + std::to_string(size.Height);
                    }
                    sender.Recreate(impl_->direct3DDevice, impl_->framePoolPixelFormat, 3, size);
                    return;
                }

                const int64_t sourceTime100ns = frame.SystemRelativeTime().count();
                const int64_t timestamp100ns = mediaTimeFromSystemRelative100ns(sourceTime100ns);
                const int64_t previousTimestamp100ns = impl_->lastFramePts100ns.exchange(timestamp100ns);
                if (previousTimestamp100ns > 0 && timestamp100ns > previousTimestamp100ns) {
                    const int64_t interval100ns = timestamp100ns - previousTimestamp100ns;
                    impl_->lastFrameInterval100ns.store(interval100ns);
                    int64_t previousMaximum = impl_->maximumFrameInterval100ns.load();
                    while (interval100ns > previousMaximum &&
                           !impl_->maximumFrameInterval100ns.compare_exchange_weak(previousMaximum, interval100ns)) {
                    }
                }

                const int64_t minimumSpacing100ns = 10'000'000LL / std::max(1, impl_->targetFps.load());
                const int64_t lastQueuedPts100ns = impl_->lastQueuedFramePts100ns.load();
                if (lastQueuedPts100ns > 0 && timestamp100ns - lastQueuedPts100ns < minimumSpacing100ns) return;
                impl_->lastQueuedFramePts100ns.store(timestamp100ns);
                if (!impl_->frameQueue) return;

                auto access = frame.Surface().template as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                Microsoft::WRL::ComPtr<ID3D11Texture2D> sourceTexture;
                if (FAILED(access->GetInterface(IID_PPV_ARGS(&sourceTexture))) || !sourceTexture) return;

                D3D11_TEXTURE2D_DESC sourceDesc {};
                sourceTexture->GetDesc(&sourceDesc);
                const bool needsTonemapping = impl_->tonemapper && sourceDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
                auto owned = impl_->acquireCaptureSlot(
                    static_cast<UINT>(size.Width),
                    static_cast<UINT>(size.Height),
                    needsTonemapping);
                if (!owned.texture || !owned.lease) return;

                if (needsTonemapping) {
                    if (!owned.hdrInputTexture) return;
                    D3D11_BOX sourceBox {
                        0,
                        0,
                        0,
                        static_cast<UINT>(size.Width),
                        static_cast<UINT>(size.Height),
                        1,
                    };
                    impl_->d3dContext->CopySubresourceRegion(
                        owned.hdrInputTexture.Get(), 0, 0, 0, 0, sourceTexture.Get(), 0, &sourceBox);
                    std::string error;
                    if (!impl_->tonemapper->Process(owned.hdrInputTexture, owned.texture, error)) {
                        ++impl_->callbackErrors;
                        impl_->running.store(false, std::memory_order_release);
                        impl_->setStatus("WGC HDR tonemapping failed: " + error);
                        return;
                    }
                } else {
                    D3D11_BOX sourceBox {
                        0,
                        0,
                        0,
                        static_cast<UINT>(size.Width),
                        static_cast<UINT>(size.Height),
                        1,
                    };
                    impl_->d3dContext->CopySubresourceRegion(
                        owned.texture.Get(), 0, 0, 0, 0, sourceTexture.Get(), 0, &sourceBox);
                }

                impl_->capturedFrames.fetch_add(1, std::memory_order_relaxed);
                impl_->frameQueue->push(CapturedFrame {
                    owned.texture,
                    std::move(owned.lease),
                    timestamp100ns,
                    size.Width,
                    size.Height,
                    monotonicNow100ns(),
                    impl_->captureEpoch.load(),
                    ++impl_->frameSequence,
                });
            } catch (const winrt::hresult_error& error) {
                ++impl_->callbackErrors;
                impl_->running.store(false, std::memory_order_release);
                impl_->setStatus("WGC frame callback failed: " + narrow(error.message().c_str()));
            } catch (const std::exception& error) {
                ++impl_->callbackErrors;
                impl_->running.store(false, std::memory_order_release);
                impl_->setStatus("WGC frame callback failed: " + std::string(error.what()));
            } catch (...) {
                ++impl_->callbackErrors;
                impl_->running.store(false, std::memory_order_release);
                impl_->setStatus("WGC frame callback failed with an unknown error.");
            }
        });
        impl_->session = impl_->framePool.CreateCaptureSession(impl_->item);
        impl_->session.IsCursorCaptureEnabled(true);
        try {
            impl_->session.IsBorderRequired(false);
        } catch (...) {
        }
        impl_->running = true;
        impl_->session.StartCapture();
        impl_->setStatus("Windows.Graphics.Capture is running on " + selectedOutput.displayName + ".");
        return true;
    } catch (const winrt::hresult_error& error) {
        std::ostringstream message;
        message << "WGC start failed: " << narrow(error.message().c_str());
        impl_->setStatus(message.str());
        impl_->running = false;
        return false;
    }
}

void CaptureSession::setTargetFps(int fps) {
    impl_->targetFps.store(std::clamp(fps, 1, 240));
}

void CaptureSession::stop() {
    impl_->running = false;
    if (impl_->framePool) {
        try {
            impl_->framePool.FrameArrived(impl_->frameArrivedToken);
        } catch (...) {
        }
    }
    if (impl_->item) {
        try {
            impl_->item.Closed(impl_->itemClosedToken);
        } catch (...) {
        }
    }
    {
        std::lock_guard callbackLock(impl_->callbackMutex);
        if (impl_->session) {
            try {
                impl_->session.Close();
            } catch (...) {
            }
            impl_->session = nullptr;
        }
        if (impl_->framePool) {
            try {
                impl_->framePool.Close();
            } catch (...) {
            }
            impl_->framePool = nullptr;
        }
        impl_->item = nullptr;
        impl_->direct3DDevice = nullptr;
        impl_->frameQueue = nullptr;
        impl_->d3dContext.Reset();
        impl_->d3dDevice.Reset();
        impl_->tonemapper.reset();
        impl_->discardCaptureSlotGeneration();
        impl_->hdrTonemappingActive = false;
        impl_->activeMonitor.store(nullptr, std::memory_order_release);
    }
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

int64_t CaptureSession::maximumFrameInterval100ns() const {
    return impl_->maximumFrameInterval100ns.load(std::memory_order_relaxed);
}

uint64_t CaptureSession::ownedSlotDrops() const {
    return impl_->ownedSlotDrops.load(std::memory_order_relaxed);
}

uint64_t CaptureSession::callbackErrors() const {
    return impl_->callbackErrors.load(std::memory_order_relaxed);
}

uint64_t CaptureSession::captureEpoch() const {
    return impl_->captureEpoch.load(std::memory_order_relaxed);
}

std::string CaptureSession::resolution() const {
    std::lock_guard lock(impl_->stateMutex);
    return impl_->resolution;
}

std::string CaptureSession::displayName() const {
    std::lock_guard lock(impl_->stateMutex);
    return impl_->displayName;
}

bool CaptureSession::hdrTonemappingActive() const {
    return impl_->hdrTonemappingActive;
}

std::string CaptureSession::status() const {
    std::lock_guard lock(impl_->stateMutex);
    return impl_->status;
}

void* CaptureSession::activeMonitor() const {
    return impl_->activeMonitor.load(std::memory_order_acquire);
}

}  // namespace clipture
