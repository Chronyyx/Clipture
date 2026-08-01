#include "CaptureBackend.hpp"

#include "clipture/MediaClock.hpp"
#include "clipture/DesktopDuplicationHelpers.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>

namespace clipture::capture {
namespace {

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
    if (QueryDisplayConfig(
            QDC_ONLY_ACTIVE_PATHS,
            &pathCount,
            paths.data(),
            &modeCount,
            modes.data(),
            nullptr) != ERROR_SUCCESS) {
        return false;
    }

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

void monitorRefreshRate(const wchar_t* gdiName, uint32_t& numerator, uint32_t& denominator) {
    if (!gdiName || !*gdiName) return;
    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (QueryDisplayConfig(
            QDC_ONLY_ACTIVE_PATHS,
            &pathCount,
            paths.data(),
            &modeCount,
            modes.data(),
            nullptr) != ERROR_SUCCESS) {
        return;
    }
    for (const auto& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName {};
        if (!sourceNameForPath(path, sourceName) || wcscmp(gdiName, sourceName.viewGdiDeviceName) != 0) continue;
        auto refresh = path.targetInfo.refreshRate;
        if ((refresh.Numerator == 0 || refresh.Denominator == 0) &&
            path.targetInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID &&
            path.targetInfo.modeInfoIdx < modeCount) {
            const auto& mode = modes[path.targetInfo.modeInfoIdx];
            if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
                refresh = mode.targetMode.targetVideoSignalInfo.vSyncFreq;
            }
        }
        if (refresh.Numerator != 0 && refresh.Denominator != 0) {
            numerator = refresh.Numerator;
            denominator = refresh.Denominator;
            return;
        }
    }

    DEVMODEW mode {};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsExW(gdiName, ENUM_CURRENT_SETTINGS, &mode, 0) && mode.dmDisplayFrequency > 1) {
        numerator = mode.dmDisplayFrequency;
        denominator = 1;
    }
}

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

std::string monitorKey(IDXGIAdapter1* adapter, const DXGI_OUTPUT_DESC& outputDesc) {
    DXGI_ADAPTER_DESC1 adapterDesc {};
    if (!adapter || FAILED(adapter->GetDesc1(&adapterDesc))) return narrow(outputDesc.DeviceName);
    std::ostringstream key;
    key << std::hex << static_cast<uint32_t>(adapterDesc.AdapterLuid.HighPart)
        << ':' << adapterDesc.AdapterLuid.LowPart << ':' << narrow(outputDesc.DeviceName);
    return key.str();
}

}  // namespace

std::string narrow(const wchar_t* value) {
    if (!value) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string result(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), needed, nullptr, nullptr);
    result.pop_back();
    return result;
}

std::string hresultHex(HRESULT value) {
    std::ostringstream message;
    message << "0x" << std::hex << std::uppercase << static_cast<uint32_t>(value);
    return message.str();
}

float monitorSdrWhiteLevel(HMONITOR monitor) {
    constexpr float fallbackWhiteLevel = 4.5f;
    MONITORINFOEXW monitorInfo {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) return fallbackWhiteLevel;

    DISPLAYCONFIG_PATH_INFO path {};
    if (!activeDisplayPathForGdiName(monitorInfo.szDevice, path)) return fallbackWhiteLevel;

    DISPLAYCONFIG_SDR_WHITE_LEVEL whiteLevel {};
    whiteLevel.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
    whiteLevel.header.size = sizeof(whiteLevel);
    whiteLevel.header.adapterId = path.targetInfo.adapterId;
    whiteLevel.header.id = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&whiteLevel.header) != ERROR_SUCCESS ||
        whiteLevel.SDRWhiteLevel == 0) {
        return fallbackWhiteLevel;
    }
    return whiteLevel.SDRWhiteLevel / 1000.0f;
}

bool selectOutput(const std::string& requestedId, SelectedOutput& selected) {
    Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;

    SelectedOutput firstHardwareOutput;
    SelectedOutput primaryOutput;
    bool hasFirstHardwareOutput = false;
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
            candidate.monitorKey = monitorKey(adapter.Get(), desc);
            candidate.hdrEnabled = monitorHdrEnabled(desc.DeviceName);
            monitorRefreshRate(desc.DeviceName, candidate.refreshNumerator, candidate.refreshDenominator);

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

HRESULT createD3dDeviceForOutput(
    IDXGIAdapter1* adapter,
    Microsoft::WRL::ComPtr<ID3D11Device>& device,
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>& context) {
    D3D_FEATURE_LEVEL featureLevels[] { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D_FEATURE_LEVEL selectedLevel {};
    const HRESULT hr = D3D11CreateDevice(
        adapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        featureLevels,
        ARRAYSIZE(featureLevels),
        D3D11_SDK_VERSION,
        &device,
        &selectedLevel,
        &context);
    if (FAILED(hr)) return hr;

    Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
    if (SUCCEEDED(context.As(&multithread)) && multithread) multithread->SetMultithreadProtected(TRUE);
    return S_OK;
}

void CaptureSharedState::resetForStart(
    FrameQueue* queue,
    const SelectedOutput& output,
    CaptureBackendPreference preference) {
    frameQueue.store(queue, std::memory_order_release);
    if (queue) queue->clear();
    lastFramePts100ns.store(0);
    lastFrameInterval100ns.store(0);
    maximumFrameInterval100ns.store(0);
    lastPublishedSteady100ns.store(0);
    hdrTonemappingActive.store(false);
    running.store(false);
    acquireWaitLatency.clear();
    framePreparationLatency.clear();
    cursorCompositeLatency.clear();
    frameProcessingLatency.clear();
    {
        std::lock_guard samplerLock(samplerMutex);
        sampler.reset();
    }
    ++captureEpoch;
    setSelectedOutput(output);
    {
        std::lock_guard lock(stateMutex);
        requestedBackend = std::string(captureBackendPreferenceName(preference));
        activeBackend = "none";
        fallbackReason.clear();
        activeBackendStarted100ns = 0;
        activeBackendPresentBaseline = desktopPresents.load(std::memory_order_relaxed);
        activeBackendPublishedBaseline = publishedFrames.load(std::memory_order_relaxed);
    }
}

void CaptureSharedState::beginEpoch(bool clearQueue) {
    if (clearQueue) {
        if (auto* queue = frameQueue.load(std::memory_order_acquire)) queue->clear();
    }
    lastFramePts100ns.store(0, std::memory_order_relaxed);
    lastFrameInterval100ns.store(0, std::memory_order_relaxed);
    lastPublishedSteady100ns.store(0, std::memory_order_relaxed);
    acquireWaitLatency.clear();
    framePreparationLatency.clear();
    cursorCompositeLatency.clear();
    frameProcessingLatency.clear();
    {
        std::lock_guard samplerLock(samplerMutex);
        sampler.reset();
    }
    ++captureEpoch;
}

void CaptureSharedState::setStatus(std::string nextStatus) {
    std::lock_guard lock(stateMutex);
    status = std::move(nextStatus);
}

void CaptureSharedState::setFallbackReason(std::string reason) {
    std::lock_guard lock(stateMutex);
    fallbackReason = std::move(reason);
}

void CaptureSharedState::setSelectedOutput(const SelectedOutput& output) {
    const int width = output.desc.DesktopCoordinates.right - output.desc.DesktopCoordinates.left;
    const int height = output.desc.DesktopCoordinates.bottom - output.desc.DesktopCoordinates.top;
    {
        std::lock_guard lock(stateMutex);
        resolution = std::to_string(width) + "x" + std::to_string(height);
        displayName = output.displayName;
        refreshNumerator = output.refreshNumerator;
        refreshDenominator = std::max<uint32_t>(1, output.refreshDenominator);
    }
    activeMonitor.store(output.desc.Monitor, std::memory_order_release);
}

void CaptureSharedState::setActiveBackend(CaptureBackendKind kind) {
    std::lock_guard lock(stateMutex);
    activeBackend = std::string(captureBackendKindName(kind));
    activeBackendStarted100ns = monotonicNow100ns();
    activeBackendPresentBaseline = desktopPresents.load(std::memory_order_relaxed);
    activeBackendPublishedBaseline = publishedFrames.load(std::memory_order_relaxed);
}

bool CaptureSharedState::selectFrameTimestamp(
    int64_t sourceTimestamp100ns,
    int64_t& outputTimestamp100ns) {
    const int64_t previousTimestamp100ns = lastFramePts100ns.load(std::memory_order_relaxed);
    if (!captureTimestampIsStrictlyNew(previousTimestamp100ns, sourceTimestamp100ns)) {
        ++nonMonotonicTimestamps;
        return false;
    }
    lastFramePts100ns.store(sourceTimestamp100ns, std::memory_order_relaxed);
    if (previousTimestamp100ns > 0) {
        const int64_t interval100ns = sourceTimestamp100ns - previousTimestamp100ns;
        lastFrameInterval100ns.store(interval100ns, std::memory_order_relaxed);
        int64_t previousMaximum = maximumFrameInterval100ns.load(std::memory_order_relaxed);
        while (interval100ns > previousMaximum &&
               !maximumFrameInterval100ns.compare_exchange_weak(previousMaximum, interval100ns)) {
        }
    }

    std::lock_guard samplerLock(samplerMutex);
    if (!sampler.shouldSample(sourceTimestamp100ns, targetFps.load(std::memory_order_relaxed))) {
        ++samplerRejections;
        ++sourceFramesSuperseded;
        return false;
    }
    outputTimestamp100ns = sampler.selectedPts100ns();
    return true;
}

void CaptureSharedState::publish(
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture,
    std::shared_ptr<void> textureLease,
    int64_t timestamp100ns,
    int width,
    int height) {
    auto* queue = frameQueue.load(std::memory_order_acquire);
    if (!queue || !texture || !textureLease) return;
    const int64_t publishedAtSteady100ns = monotonicNow100ns();
    ++capturedFrames;
    ++publishedFrames;
    lastPublishedSteady100ns.store(publishedAtSteady100ns, std::memory_order_release);
    queue->push(CapturedFrame {
        std::move(texture),
        std::move(textureLease),
        timestamp100ns,
        width,
        height,
        publishedAtSteady100ns,
        captureEpoch.load(std::memory_order_relaxed),
        ++frameSequence,
    });
}

CaptureRuntimeStats CaptureSharedState::snapshot() const {
    CaptureRuntimeStats result;
    result.acquiredUpdates = acquiredUpdates.load(std::memory_order_relaxed);
    result.desktopPresents = desktopPresents.load(std::memory_order_relaxed);
    result.pointerUpdates = pointerUpdates.load(std::memory_order_relaxed);
    result.publishedFrames = publishedFrames.load(std::memory_order_relaxed);
    result.accumulatedFrames = accumulatedFrames.load(std::memory_order_relaxed);
    result.accumulationEvents = accumulationEvents.load(std::memory_order_relaxed);
    result.samplerRejections = samplerRejections.load(std::memory_order_relaxed);
    result.nonMonotonicTimestamps = nonMonotonicTimestamps.load(std::memory_order_relaxed);
    result.acquireTimeouts = acquireTimeouts.load(std::memory_order_relaxed);
    result.accessLosses = accessLosses.load(std::memory_order_relaxed);
    result.recreationAttempts = recreationAttempts.load(std::memory_order_relaxed);
    result.recreationSuccesses = recreationSuccesses.load(std::memory_order_relaxed);
    result.fallbackCount = fallbackCount.load(std::memory_order_relaxed);

    int64_t startedAt100ns = 0;
    uint64_t presentBaseline = 0;
    uint64_t publishedBaseline = 0;
    {
        std::lock_guard lock(stateMutex);
        result.requestedBackend = requestedBackend;
        result.activeBackend = activeBackend;
        result.fallbackReason = fallbackReason;
        result.refreshNumerator = refreshNumerator;
        result.refreshDenominator = refreshDenominator;
        result.refreshHz = refreshDenominator > 0
            ? static_cast<double>(refreshNumerator) / refreshDenominator
            : 0.0;
        startedAt100ns = activeBackendStarted100ns;
        presentBaseline = activeBackendPresentBaseline;
        publishedBaseline = activeBackendPublishedBaseline;
    }
    const int64_t now100ns = monotonicNow100ns();
    const int64_t elapsed100ns = startedAt100ns > 0 ? now100ns - startedAt100ns : 0;
    if (elapsed100ns > 0) {
        result.desktopPresentFps = static_cast<double>(result.desktopPresents - presentBaseline) * 10'000'000.0 /
            static_cast<double>(elapsed100ns);
        result.publishedFreshFps = static_cast<double>(result.publishedFrames - publishedBaseline) * 10'000'000.0 /
            static_cast<double>(elapsed100ns);
    }
    result.acquireWaitLatency = acquireWaitLatency.snapshot(now100ns);
    result.framePreparationLatency = framePreparationLatency.snapshot(now100ns);
    result.cursorCompositeLatency = cursorCompositeLatency.snapshot(now100ns);
    result.frameProcessingLatency = frameProcessingLatency.snapshot(now100ns);
    const int64_t recentWindow100ns = std::min<int64_t>(elapsed100ns, 50'000'000LL);
    if (recentWindow100ns > 0) {
        result.recentPublishedFreshFps =
            static_cast<double>(result.frameProcessingLatency.samples) * 10'000'000.0 /
            static_cast<double>(recentWindow100ns);
    }
    return result;
}

struct CaptureTexturePool::Slot {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> renderTargetView;
    std::atomic<bool> leased = false;
};

CaptureTexturePool::CaptureTexturePool(std::shared_ptr<CaptureSharedState> shared)
    : shared_(std::move(shared)) {}

CaptureTexture CaptureTexturePool::acquire(
    ID3D11Device* device,
    UINT width,
    UINT height,
    bool needsUnorderedAccess,
    std::string& error) {
    if (!device || width == 0 || height == 0) return {};
    std::lock_guard lock(mutex_);
    const bool needsNewGeneration = slots_.empty() || device_.Get() != device ||
        desc_.Width != width || desc_.Height != height ||
        (((desc_.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0) != needsUnorderedAccess);
    if (needsNewGeneration) {
        slots_.clear();
        device_ = device;
        hdrInputTextures_.clear();
        nextHdrInputTexture_ = 0;
        desc_ = {};
        desc_.Width = width;
        desc_.Height = height;
        desc_.MipLevels = 1;
        desc_.ArraySize = 1;
        desc_.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc_.SampleDesc.Count = 1;
        desc_.Usage = D3D11_USAGE_DEFAULT;
        desc_.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (needsUnorderedAccess) desc_.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;

        constexpr std::size_t slotCount = 12;
        slots_.reserve(slotCount);
        for (std::size_t index = 0; index < slotCount; ++index) {
            auto slot = std::make_shared<Slot>();
            HRESULT hr = device->CreateTexture2D(&desc_, nullptr, &slot->texture);
            if (FAILED(hr) || !slot->texture) {
                error = "CreateTexture2D for capture slot failed: " + hresultHex(hr);
                slots_.clear();
                return {};
            }
            hr = device->CreateRenderTargetView(slot->texture.Get(), nullptr, &slot->renderTargetView);
            if (FAILED(hr) || !slot->renderTargetView) {
                error = "CreateRenderTargetView for capture slot failed: " + hresultHex(hr);
                slots_.clear();
                return {};
            }
            slots_.push_back(std::move(slot));
        }
        if (needsUnorderedAccess) {
            D3D11_TEXTURE2D_DESC hdrDesc = desc_;
            hdrDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            hdrDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            constexpr std::size_t hdrInputCount = 3;
            hdrInputTextures_.reserve(hdrInputCount);
            for (std::size_t index = 0; index < hdrInputCount; ++index) {
                Microsoft::WRL::ComPtr<ID3D11Texture2D> hdrInputTexture;
                const HRESULT hr = device->CreateTexture2D(&hdrDesc, nullptr, &hdrInputTexture);
                if (FAILED(hr) || !hdrInputTexture) {
                    error = "CreateTexture2D for HDR capture staging failed: " + hresultHex(hr);
                    slots_.clear();
                    hdrInputTextures_.clear();
                    return {};
                }
                hdrInputTextures_.push_back(std::move(hdrInputTexture));
            }
        }
    }

    for (const auto& slot : slots_) {
        bool expected = false;
        if (!slot->leased.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) continue;
        auto lease = std::shared_ptr<void>(slot.get(), [slot](void*) {
            slot->leased.store(false, std::memory_order_release);
        });
        Microsoft::WRL::ComPtr<ID3D11Texture2D> hdrInputTexture;
        if (!hdrInputTextures_.empty()) {
            hdrInputTexture = hdrInputTextures_[nextHdrInputTexture_];
            nextHdrInputTexture_ = (nextHdrInputTexture_ + 1) % hdrInputTextures_.size();
        }
        return { slot->texture, std::move(hdrInputTexture), slot->renderTargetView, std::move(lease) };
    }
    ++shared_->ownedSlotDrops;
    return {};
}

void CaptureTexturePool::reset() {
    std::lock_guard lock(mutex_);
    slots_.clear();
    device_.Reset();
    hdrInputTextures_.clear();
    nextHdrInputTexture_ = 0;
    desc_ = {};
}

}  // namespace clipture::capture
