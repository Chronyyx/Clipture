#include "DesktopDuplicationBackend.hpp"

#include "DesktopPointerCompositor.hpp"
#include "clipture/CaptureBackendPolicy.hpp"
#include "clipture/DesktopDuplicationHelpers.hpp"
#include "clipture/MediaClock.hpp"
#include "clipture/Tonemapper.hpp"

#include <avrt.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

namespace clipture::capture {
namespace {

class MmcssCaptureRegistration {
public:
    explicit MmcssCaptureRegistration(bool enabled) {
        if (!enabled) return;
        handle_ = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex_);
        // NVENC uses the same D3D device at high MMCSS priority. Keeping
        // acquisition at normal Capture priority prevents priority inversion.
        if (handle_) AvSetMmThreadPriority(handle_, AVRT_PRIORITY_NORMAL);
    }
    ~MmcssCaptureRegistration() {
        if (handle_) AvRevertMmThreadCharacteristics(handle_);
    }

private:
    DWORD taskIndex_ = 0;
    HANDLE handle_ = nullptr;
};

class AcquiredDesktopFrame {
public:
    explicit AcquiredDesktopFrame(IDXGIOutputDuplication* duplication)
        : duplication_(duplication) {}
    ~AcquiredDesktopFrame() {
        release();
    }
    void markAcquired() { acquired_ = true; }
    void release() {
        if (!acquired_ || !duplication_) return;
        duplication_->ReleaseFrame();
        acquired_ = false;
    }

private:
    IDXGIOutputDuplication* duplication_ = nullptr;
    bool acquired_ = false;
};

class CaptureTickCompletion {
public:
    CaptureTickCompletion(CaptureTickGate* gate, uint64_t generation)
        : gate_(gate), generation_(generation) {}
    ~CaptureTickCompletion() { finish(); }

    void finish() {
        if (!gate_ || generation_ == 0) return;
        gate_->complete(generation_);
        generation_ = 0;
    }

private:
    CaptureTickGate* gate_ = nullptr;
    uint64_t generation_ = 0;
};

bool stopAwareSleep(std::stop_token stopToken, int milliseconds) {
    constexpr int quantumMs = 10;
    for (int elapsed = 0; elapsed < milliseconds; elapsed += quantumMs) {
        if (stopToken.stop_requested()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(quantumMs, milliseconds - elapsed)));
    }
    return !stopToken.stop_requested();
}

bool isRecoverableDuplicationError(HRESULT hr) {
    return hr == DXGI_ERROR_ACCESS_LOST ||
        hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_DEVICE_RESET ||
        hr == DXGI_ERROR_SESSION_DISCONNECTED ||
        hr == E_ACCESSDENIED;
}

}  // namespace

struct DesktopDuplicationBackend::Impl {
    std::shared_ptr<CaptureSharedState> shared;
    SelectedOutput output;
    std::string monitorId;
    CaptureTexturePool texturePool;
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication;
    std::unique_ptr<DesktopPointerCompositor> pointerCompositor;
    std::unique_ptr<Tonemapper> tonemapper;
    DXGI_OUTDUPL_DESC duplicationDesc {};
    DXGI_FORMAT captureFormat = DXGI_FORMAT_UNKNOWN;
    int64_t qpcFrequency = 0;
    UINT width = 0;
    UINT height = 0;
    bool hdrCapture = false;
    float sdrWhiteLevel = 0.0f;
    bool started = false;

    Impl(
        std::shared_ptr<CaptureSharedState> nextShared,
        SelectedOutput nextOutput,
        std::string nextMonitorId)
        : shared(std::move(nextShared)),
          output(std::move(nextOutput)),
          monitorId(std::move(nextMonitorId)),
          texturePool(shared) {
        LARGE_INTEGER frequency {};
        if (QueryPerformanceFrequency(&frequency)) qpcFrequency = frequency.QuadPart;
    }

    void releaseResources() {
        pointerCompositor.reset();
        tonemapper.reset();
        duplication.Reset();
        texturePool.reset();
        d3dContext.Reset();
        d3dDevice.Reset();
        duplicationDesc = {};
        captureFormat = DXGI_FORMAT_UNKNOWN;
        width = 0;
        height = 0;
        hdrCapture = false;
        sdrWhiteLevel = 0.0f;
        shared->hdrTonemappingActive.store(false, std::memory_order_relaxed);
    }

    BackendStartResult initialize();
    bool recover(std::stop_token stopToken);
};

BackendStartResult DesktopDuplicationBackend::Impl::initialize() {
    releaseResources();
    if (output.desc.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
        output.desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
        return { false, "DXGI capture was refused for a rotated output." };
    }
    hdrCapture = output.hdrEnabled;

    HRESULT hr = createD3dDeviceForOutput(output.adapter.Get(), d3dDevice, d3dContext);
    if (FAILED(hr)) return { false, "D3D11 device creation for Desktop Duplication failed: " + hresultHex(hr) };

    Microsoft::WRL::ComPtr<IDXGIOutput5> output5;
    HRESULT duplicateHr = output.output.As(&output5);
    if (SUCCEEDED(duplicateHr) && output5) {
        constexpr DXGI_FORMAT hdrFormats[] {
            DXGI_FORMAT_R16G16B16A16_FLOAT,
            DXGI_FORMAT_B8G8R8A8_UNORM,
        };
        constexpr DXGI_FORMAT sdrFormats[] { DXGI_FORMAT_B8G8R8A8_UNORM };
        const DXGI_FORMAT* formats = hdrCapture ? hdrFormats : sdrFormats;
        const UINT formatCount = hdrCapture
            ? static_cast<UINT>(std::size(hdrFormats))
            : static_cast<UINT>(std::size(sdrFormats));
        duplicateHr = output5->DuplicateOutput1(
            d3dDevice.Get(),
            0,
            formatCount,
            formats,
            &duplication);
    }

    if (!hdrCapture && !duplication &&
        (FAILED(duplicateHr) || duplicateHr == DXGI_ERROR_UNSUPPORTED || duplicateHr == E_INVALIDARG)) {
        Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
        hr = output.output.As(&output1);
        if (FAILED(hr) || !output1) {
            return { false, "The selected output does not expose IDXGIOutput1: " + hresultHex(hr) };
        }
        duplicateHr = output1->DuplicateOutput(d3dDevice.Get(), &duplication);
    }
    if (FAILED(duplicateHr) || !duplication) {
        return {
            false,
            std::string(hdrCapture ? "HDR DuplicateOutput1 failed: " : "DuplicateOutput failed: ") +
                hresultHex(duplicateHr),
        };
    }

    duplication->GetDesc(&duplicationDesc);
    if (duplicationDesc.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
        duplicationDesc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
        return { false, "Desktop Duplication returned a rotated surface; WGC is required." };
    }
    captureFormat = duplicationDesc.ModeDesc.Format;
    if (!dxgiCaptureFormatSupported(captureFormat, hdrCapture)) {
        return {
            false,
            "Desktop Duplication returned unsupported " +
                std::string(hdrCapture ? "HDR" : "SDR") +
                " format " + std::to_string(static_cast<unsigned>(captureFormat)) + ".",
        };
    }
    width = duplicationDesc.ModeDesc.Width;
    height = duplicationDesc.ModeDesc.Height;
    if (width == 0 || height == 0) {
        width = static_cast<UINT>(output.desc.DesktopCoordinates.right - output.desc.DesktopCoordinates.left);
        height = static_cast<UINT>(output.desc.DesktopCoordinates.bottom - output.desc.DesktopCoordinates.top);
    }
    if (width == 0 || height == 0) return { false, "Desktop Duplication returned an empty output mode." };

    if (hdrCapture) {
        sdrWhiteLevel = monitorSdrWhiteLevel(output.desc.Monitor);
        tonemapper = std::make_unique<Tonemapper>(d3dDevice);
        std::string tonemapperError;
        if (!tonemapper->Initialize(tonemapperError, sdrWhiteLevel)) {
            return { false, "DXGI HDR tonemapper initialization failed: " + tonemapperError };
        }
    }

    pointerCompositor = std::make_unique<DesktopPointerCompositor>(d3dDevice, d3dContext);
    std::string pointerError;
    if (!pointerCompositor->initialize(pointerError)) {
        return { false, "Desktop pointer compositor initialization failed: " + pointerError };
    }
    shared->hdrTonemappingActive.store(hdrCapture, std::memory_order_relaxed);
    shared->setSelectedOutput(output);
    return { true, {} };
}

bool DesktopDuplicationBackend::Impl::recover(std::stop_token stopToken) {
    shared->beginEpoch();
    shared->setStatus("DXGI Desktop Duplication is recovering after output loss.");
    releaseResources();

    std::string lastError = "Desktop Duplication recovery failed.";
    for (const int delayMs : kDxgiRecoveryDelaysMs) {
        if (!stopAwareSleep(stopToken, delayMs)) return false;
        ++shared->recreationAttempts;

        SelectedOutput nextOutput;
        if (!selectOutput(monitorId, nextOutput)) {
            lastError = "The selected monitor was not available during DXGI recovery.";
            continue;
        }
        output = std::move(nextOutput);
        const auto startResult = initialize();
        if (startResult.ok) {
            ++shared->recreationSuccesses;
            shared->setStatus("DXGI Desktop Duplication recovered on " + output.displayName + ".");
            std::cerr << "[capture] DXGI Desktop Duplication recovered after output loss.\n";
            return true;
        }
        lastError = startResult.message;
    }

    shared->setFallbackReason(lastError);
    shared->setStatus(lastError + " Automatic WGC fallback is pending.");
    return false;
}

DesktopDuplicationBackend::DesktopDuplicationBackend(
    std::shared_ptr<CaptureSharedState> shared,
    SelectedOutput output,
    std::string monitorId)
    : impl_(std::make_unique<Impl>(std::move(shared), std::move(output), std::move(monitorId))) {}

DesktopDuplicationBackend::~DesktopDuplicationBackend() {
    stop();
}

BackendStartResult DesktopDuplicationBackend::start() {
    const auto result = impl_->initialize();
    if (!result.ok) return result;
    impl_->started = true;
    if constexpr (kEnableEncoderDrivenDxgiCapture) {
        if (auto* tickGate = impl_->shared->captureTickGate.load(std::memory_order_acquire)) {
            tickGate->activate();
        }
    }
    impl_->shared->hdrTonemappingActive.store(impl_->hdrCapture, std::memory_order_relaxed);
    impl_->shared->setActiveBackend(CaptureBackendKind::Dxgi);
    impl_->shared->running.store(true, std::memory_order_release);
    impl_->shared->setStatus(
        "DXGI Desktop Duplication is running on " + impl_->output.displayName +
        (impl_->hdrCapture ? " with HDR tonemapping." : "."));
    std::cerr << "[capture] DXGI Desktop Duplication started on "
              << impl_->output.displayName
              << " format=" << static_cast<unsigned>(impl_->captureFormat)
              << " hdr=" << (impl_->hdrCapture ? "true" : "false")
              << " encoderDrivenPollTimeoutMs=" << kEncoderDrivenDxgiPollTimeoutMs
              << " graceTimeoutMs=" << kEncoderDrivenDxgiGraceTimeoutMs
              << " prearm=" << (kEnableEncoderDrivenDxgiPrearm ? "true" : "false")
              << " prearmLeadMs=" << kEncoderDrivenCapturePrearmLead.count()
              << " completionWaitMs=" << kEncoderDrivenCaptureCompletionWait.count();
    if (impl_->hdrCapture) std::cerr << " sdrWhiteLevel=" << impl_->sdrWhiteLevel;
    std::cerr << ".\n";
    return { true, {} };
}

BackendOutcome DesktopDuplicationBackend::run(std::stop_token stopToken) {
    auto& state = *impl_;
    // HDR still has to publish on cadence while its full-frame compute pass is
    // active. Matching the encoder's Capture scheduling class avoids starving
    // acquisition whenever a game or a save briefly loads the machine.
    MmcssCaptureRegistration mmcss(true);
    bool bootstrapComplete = false;

    while (!stopToken.stop_requested()) {
        auto* tickGate = state.shared->captureTickGate.load(std::memory_order_acquire);
        const bool encoderDriven =
            kEnableEncoderDrivenDxgiCapture && tickGate && tickGate->active();
        uint64_t captureGeneration = 0;
        if (encoderDriven && bootstrapComplete) {
            const auto request = tickGate->wait(stopToken);
            if (!request) break;
            captureGeneration = *request;
        }
        CaptureTickCompletion tickCompletion(tickGate, captureGeneration);

        DXGI_OUTDUPL_FRAME_INFO frameInfo {};
        Microsoft::WRL::ComPtr<IDXGIResource> desktopResource;
        AcquiredDesktopFrame acquiredFrame(state.duplication.Get());
        const int64_t acquireStarted100ns = monotonicNow100ns();
        // OBS can use a zero-timeout poll because capture and rendering share
        // one graphics thread. Preserve that fast path, then give Clipture's
        // separate capture thread one short grace wait only after a miss.
        const UINT acquireTimeoutMs = encoderDriven && bootstrapComplete
            ? kEncoderDrivenDxgiPollTimeoutMs
            : 8U;
        HRESULT acquireHr = state.duplication->AcquireNextFrame(
            acquireTimeoutMs,
            &frameInfo,
            &desktopResource);
        const bool initialPollTimedOut = acquireHr == DXGI_ERROR_WAIT_TIMEOUT;
        if (encoderDriven && bootstrapComplete && initialPollTimedOut) {
            ++state.shared->acquireImmediateMisses;
        }
        if (shouldUseEncoderDrivenDxgiGrace(
                encoderDriven, bootstrapComplete, initialPollTimedOut)) {
            frameInfo = {};
            desktopResource.Reset();
            acquireHr = state.duplication->AcquireNextFrame(
                kEncoderDrivenDxgiGraceTimeoutMs,
                &frameInfo,
                &desktopResource);
            if (SUCCEEDED(acquireHr)) {
                ++state.shared->acquireGraceHits;
            } else if (acquireHr == DXGI_ERROR_WAIT_TIMEOUT) {
                ++state.shared->acquireGraceTimeouts;
            }
        }
        if (acquireHr == DXGI_ERROR_WAIT_TIMEOUT) {
            ++state.shared->acquireTimeouts;
            continue;
        }
        if (FAILED(acquireHr)) {
            if (isRecoverableDuplicationError(acquireHr)) {
                ++state.shared->accessLosses;
                std::cerr << "[capture] DXGI output was lost (" << hresultHex(acquireHr)
                          << "); attempting bounded recovery.\n";
                if (state.recover(stopToken)) continue;
                return stopToken.stop_requested()
                    ? BackendOutcome::Stopped
                    : BackendOutcome::RequestFallback;
            }
            state.shared->setFallbackReason("AcquireNextFrame failed: " + hresultHex(acquireHr));
            return BackendOutcome::RequestFallback;
        }
        acquiredFrame.markAcquired();
        const int64_t acquired100ns = monotonicNow100ns();
        state.shared->acquireWaitLatency.record(acquired100ns, acquired100ns - acquireStarted100ns);

        ++state.shared->acquiredUpdates;
        const uint64_t accumulatedBeyondFirst = dxgiAccumulatedFramesBeyondFirst(frameInfo.AccumulatedFrames);
        if (accumulatedBeyondFirst > 0) {
            ++state.shared->accumulationEvents;
            state.shared->accumulatedFrames.fetch_add(accumulatedBeyondFirst, std::memory_order_relaxed);
            state.shared->sourceFramesSuperseded.fetch_add(accumulatedBeyondFirst, std::memory_order_relaxed);
        }
        if (frameInfo.LastPresentTime.QuadPart != 0) ++state.shared->desktopPresents;
        if (frameInfo.LastMouseUpdateTime.QuadPart != 0) ++state.shared->pointerUpdates;

        std::string pointerError;
        if (!state.pointerCompositor->update(state.duplication.Get(), frameInfo, pointerError)) {
            ++state.shared->callbackErrors;
            state.shared->setFallbackReason(pointerError);
            ++state.shared->accessLosses;
            std::cerr << "[capture] DXGI pointer update failed; rebuilding duplication: "
                      << pointerError << '\n';
            acquiredFrame.release();
            if (state.recover(stopToken)) continue;
            return stopToken.stop_requested()
                ? BackendOutcome::Stopped
                : BackendOutcome::RequestFallback;
        }

        const int64_t timestampTicks = dxgiEffectiveTimestampTicks(frameInfo);
        const int64_t sourceTimestamp100ns = (timestampTicks > 0 && state.qpcFrequency > 0)
            ? mediaTimeFromSystemRelative100ns(detail::qpcTicksTo100ns(timestampTicks, state.qpcFrequency))
            : monotonicNow100ns();
        int64_t outputTimestamp100ns = 0;
        if (!state.shared->selectFrameTimestamp(
                sourceTimestamp100ns,
                outputTimestamp100ns,
                !encoderDriven)) {
            continue;
        }
        const int64_t frameProcessingStarted100ns = monotonicNow100ns();

        Microsoft::WRL::ComPtr<ID3D11Texture2D> desktopTexture;
        HRESULT hr = desktopResource.As(&desktopTexture);
        if (FAILED(hr) || !desktopTexture) {
            ++state.shared->callbackErrors;
            continue;
        }
        D3D11_TEXTURE2D_DESC desktopDesc {};
        desktopTexture->GetDesc(&desktopDesc);
        if (desktopDesc.Width != state.width || desktopDesc.Height != state.height ||
            desktopDesc.Format != state.captureFormat) {
            ++state.shared->accessLosses;
            std::cerr << "[capture] DXGI desktop surface changed; rebuilding duplication"
                      << " expected=" << state.width << 'x' << state.height
                      << "/" << static_cast<unsigned>(state.captureFormat)
                      << " actual=" << desktopDesc.Width << 'x' << desktopDesc.Height
                      << "/" << static_cast<unsigned>(desktopDesc.Format) << ".\n";
            acquiredFrame.release();
            if (state.recover(stopToken)) continue;
            return stopToken.stop_requested()
                ? BackendOutcome::Stopped
                : BackendOutcome::RequestFallback;
        }

        std::string slotError;
        auto owned = state.texturePool.acquire(
            state.d3dDevice.Get(), state.width, state.height, state.hdrCapture, slotError);
        if (!owned.texture || !owned.lease || !owned.renderTargetView) {
            if (!slotError.empty()) {
                ++state.shared->callbackErrors;
                state.shared->setFallbackReason(slotError);
                return BackendOutcome::RequestFallback;
            }
            continue;
        }

        const int64_t framePreparationStarted100ns = monotonicNow100ns();
        if (state.hdrCapture) {
            if (!owned.hdrInputTexture || !state.tonemapper) {
                ++state.shared->callbackErrors;
                state.shared->setFallbackReason("DXGI HDR capture resources were unavailable.");
                return BackendOutcome::RequestFallback;
            }
            state.d3dContext->CopyResource(owned.hdrInputTexture.Get(), desktopTexture.Get());
            acquiredFrame.release();
            std::string tonemapperError;
            if (!state.tonemapper->Process(
                    owned.hdrInputTexture, owned.texture, tonemapperError)) {
                ++state.shared->callbackErrors;
                state.shared->setFallbackReason(
                    "DXGI HDR tonemapping failed: " + tonemapperError);
                return BackendOutcome::RequestFallback;
            }
        } else {
            state.d3dContext->CopyResource(owned.texture.Get(), desktopTexture.Get());
            acquiredFrame.release();
        }
        const int64_t framePrepared100ns = monotonicNow100ns();
        state.shared->framePreparationLatency.record(
            framePrepared100ns,
            framePrepared100ns - framePreparationStarted100ns);
        const int64_t cursorCompositeStarted100ns = monotonicNow100ns();
        if (!state.pointerCompositor->composite(
                owned.texture.Get(),
                owned.renderTargetView.Get(),
                state.width,
                state.height,
                pointerError)) {
            ++state.shared->callbackErrors;
            state.shared->setFallbackReason(pointerError);
            return BackendOutcome::RequestFallback;
        }
        const int64_t cursorComposited100ns = monotonicNow100ns();
        state.shared->cursorCompositeLatency.record(
            cursorComposited100ns,
            cursorComposited100ns - cursorCompositeStarted100ns);

        state.shared->publish(
            std::move(owned.texture),
            std::move(owned.lease),
            outputTimestamp100ns,
            static_cast<int>(state.width),
            static_cast<int>(state.height),
            frameInfo.LastPresentTime.QuadPart != 0,
            frameInfo.LastMouseUpdateTime.QuadPart != 0);
        bootstrapComplete = true;
        const int64_t frameProcessed100ns = monotonicNow100ns();
        state.shared->frameProcessingLatency.record(
            frameProcessed100ns,
            frameProcessed100ns - frameProcessingStarted100ns);
        tickCompletion.finish();
        if (auto* queue = state.shared->frameQueue.load(std::memory_order_acquire);
            queue && queue->size() >= 2) {
            SwitchToThread();
        }
    }
    return BackendOutcome::Stopped;
}

void DesktopDuplicationBackend::stop() {
    if (!impl_) return;
    if (auto* tickGate = impl_->shared->captureTickGate.load(std::memory_order_acquire)) {
        tickGate->deactivate();
    }
    impl_->started = false;
    impl_->releaseResources();
}

}  // namespace clipture::capture
