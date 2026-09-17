#include "WgcCaptureBackend.hpp"
#include "WgcFramePreparation.hpp"
#include "clipture/SourceTimestampGate.hpp"

#include "clipture/MediaClock.hpp"
#include "clipture/Tonemapper.hpp"

#include <d3d11_4.h>
#include <inspectable.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/base.h>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

namespace clipture::capture {
namespace {

using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;

}  // namespace

struct WgcCaptureBackend::Impl {
    std::shared_ptr<CaptureSharedState> shared;
    SelectedOutput output;
    HWND window = nullptr;
    std::string targetName;
    CaptureTexturePool texturePool;
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDevice;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dContext;
    IDirect3DDevice direct3DDevice { nullptr };
    GraphicsCaptureItem item { nullptr };
    Direct3D11CaptureFramePool framePool { nullptr };
    GraphicsCaptureSession session { nullptr };
    winrt::event_token frameArrivedToken {};
    winrt::event_token itemClosedToken {};
    DirectXPixelFormat framePoolPixelFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;
    winrt::Windows::Graphics::SizeInt32 framePoolSize {};
    std::shared_ptr<Tonemapper> tonemapper;
    GpuStageProbe copyProbe {"wgc-copy"};
    SourceTimestampGate sourceClock;
    bool sourceClockRejectionLogged = false;
    std::mutex callbackMutex;
    std::mutex failureMutex;
    std::string failureReason;
    std::atomic<bool> failed = false;
    std::atomic<bool> started = false;
    bool apartmentInitialized = false;
    DWORD apartmentThreadId = 0;

    Impl(
        std::shared_ptr<CaptureSharedState> nextShared,
        SelectedOutput nextOutput,
        HWND nextWindow,
        std::string nextTargetName)
        : shared(std::move(nextShared)),
          output(std::move(nextOutput)),
          window(nextWindow),
          targetName(std::move(nextTargetName)),
          texturePool(shared) {}

    void fail(std::string reason) {
        {
            std::lock_guard lock(failureMutex);
            failureReason = std::move(reason);
        }
        failed.store(true, std::memory_order_release);
    }

    std::string failure() {
        std::lock_guard lock(failureMutex);
        return failureReason;
    }
};

WgcCaptureBackend::WgcCaptureBackend(
    std::shared_ptr<CaptureSharedState> shared,
    SelectedOutput output,
    void* window,
    std::string targetName)
    : impl_(std::make_unique<Impl>(
          std::move(shared),
          std::move(output),
          static_cast<HWND>(window),
          std::move(targetName))) {}

WgcCaptureBackend::~WgcCaptureBackend() {
    stop();
}

BackendStartResult WgcCaptureBackend::start() {
    auto& state = *impl_;
    if (auto* tickGate = state.shared->captureTickGate.load(std::memory_order_acquire)) {
        tickGate->deactivate();
    }
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        state.apartmentInitialized = true;
        state.apartmentThreadId = GetCurrentThreadId();
        HRESULT hr = createD3dDeviceForOutput(state.output.adapter.Get(), state.d3dDevice, state.d3dContext);
        if (FAILED(hr)) return { false, "D3D11 device creation for WGC failed: " + hresultHex(hr) };

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        hr = state.d3dDevice.As(&dxgiDevice);
        if (FAILED(hr)) return { false, "Could not query IDXGIDevice for WGC: " + hresultHex(hr) };

        winrt::com_ptr<::IInspectable> inspectableDevice;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectableDevice.put());
        if (FAILED(hr)) return { false, "CreateDirect3D11DeviceFromDXGIDevice failed: " + hresultHex(hr) };
        state.direct3DDevice = inspectableDevice.as<IDirect3DDevice>();

        winrt::hresult_error factoryError;
        auto interop = winrt::try_get_activation_factory<
            GraphicsCaptureItem,
            IGraphicsCaptureItemInterop>(factoryError);
        if (!interop) {
            return {
                false,
                "Could not activate Windows.Graphics.Capture interop: " +
                    hresultHex(factoryError.code())
            };
        }
        winrt::com_ptr<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem> abiItem;
        if (state.window) {
            if (!IsWindow(state.window)) {
                return { false, "The selected game window is no longer available." };
            }
            hr = interop->CreateForWindow(
                state.window,
                __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem),
                abiItem.put_void());
            if (FAILED(hr)) return { false, "CreateForWindow failed: " + hresultHex(hr) };
        } else {
            hr = interop->CreateForMonitor(
                state.output.desc.Monitor,
                __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem),
                abiItem.put_void());
            if (FAILED(hr)) return { false, "CreateForMonitor failed: " + hresultHex(hr) };
        }
        state.item = abiItem.as<GraphicsCaptureItem>();

        state.itemClosedToken = state.item.Closed([this](auto const&, auto const&) {
            impl_->fail("Windows.Graphics.Capture item closed.");
        });

        DirectXPixelFormat pixelFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;
        state.shared->hdrTonemappingActive.store(false, std::memory_order_relaxed);
        if (state.output.hdrEnabled) {
            float sdrWhiteLevel = monitorSdrWhiteLevel(state.output.desc.Monitor);
            state.tonemapper = std::make_shared<Tonemapper>(state.d3dDevice);
            std::string tonemapperError;
            if (state.tonemapper->Initialize(tonemapperError, sdrWhiteLevel)) {
                pixelFormat = DirectXPixelFormat::R16G16B16A16Float;
                state.shared->hdrTonemappingActive.store(true, std::memory_order_relaxed);
            } else {
                state.tonemapper.reset();
            }
        }

        state.framePoolPixelFormat = pixelFormat;
        state.framePoolSize = state.item.Size();
        state.framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            state.direct3DDevice,
            pixelFormat,
            4,
            state.framePoolSize);

        state.frameArrivedToken = state.framePool.FrameArrived([this](auto const& sender, auto const&) {
            auto& callbackState = *impl_;
            try {
                std::lock_guard callbackLock(callbackState.callbackMutex);
                if (!callbackState.started.load(std::memory_order_acquire)) return;

                while (auto frame = sender.TryGetNextFrame()) {
                    ++callbackState.shared->acquiredUpdates;

                    const auto size = frame.ContentSize();
                    if (size.Width <= 0 || size.Height <= 0) {
                        try { frame.Close(); } catch (...) {}
                        continue;
                    }
                    if (size.Width != callbackState.framePoolSize.Width ||
                        size.Height != callbackState.framePoolSize.Height) {
                        try { frame.Close(); } catch (...) {}
                        frame = nullptr;
                        callbackState.framePoolSize = size;
                        // Queued selected frames may still use the tone mapper.
                        // Its bounded view cache owns old resources until retired.
                        callbackState.texturePool.reset();
                        callbackState.sourceClock.reset();
                        callbackState.sourceClockRejectionLogged = false;
                        callbackState.shared->beginEpoch();
                        {
                            std::lock_guard stateLock(callbackState.shared->stateMutex);
                            callbackState.shared->resolution =
                                std::to_string(size.Width) + "x" + std::to_string(size.Height);
                        }
                        sender.Recreate(
                            callbackState.direct3DDevice,
                            callbackState.framePoolPixelFormat,
                            4,
                            size);
                        return;
                    }

                    const int64_t sourceRelative100ns = frame.SystemRelativeTime().count();
                    if (!callbackState.sourceClock.accept(sourceRelative100ns)) {
                        ++callbackState.shared->nonMonotonicTimestamps;
                        if (!callbackState.sourceClockRejectionLogged) {
                            callbackState.sourceClockRejectionLogged = true;
                            std::cerr << "[capture] WGC rejected stale source timestamp=" << sourceRelative100ns
                                      << " last=" << callbackState.sourceClock.last() << '\n';
                        }
                        try { frame.Close(); } catch (...) {}
                        continue;
                    }
                    ++callbackState.shared->desktopPresents;
                    const int64_t sourceTimestamp100ns = mediaTimeFromSystemRelative100ns(sourceRelative100ns);
                    int64_t outputTimestamp100ns = 0;
                    if (!callbackState.shared->selectFrameTimestamp(
                            sourceTimestamp100ns, outputTimestamp100ns)) {
                        try { frame.Close(); } catch (...) {}
                        continue;
                    }
                    const int64_t frameProcessingStarted100ns = monotonicNow100ns();

                    auto access = frame.Surface().template as<
                        ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                    Microsoft::WRL::ComPtr<ID3D11Texture2D> sourceTexture;
                    if (FAILED(access->GetInterface(IID_PPV_ARGS(&sourceTexture))) || !sourceTexture) {
                        ++callbackState.shared->callbackErrors;
                        try { frame.Close(); } catch (...) {}
                        continue;
                    }

                    std::string slotError;
                    auto prepared = prepareWgcFrame(sourceTexture.Get(), size.Width, size.Height,
                        callbackState.texturePool, callbackState.tonemapper, callbackState.shared,
                        callbackState.d3dDevice.Get(), callbackState.d3dContext, callbackState.copyProbe,
                        capturePipelinePolicy(), slotError);
                    if (!prepared.owned.lease) {
                        if (!slotError.empty()) {
                            ++callbackState.shared->callbackErrors;
                            callbackState.fail(slotError);
                        }
                        try { frame.Close(); } catch (...) {}
                        continue;
                    }

                    sourceTexture.Reset();
                    try { frame.Close(); } catch (...) {}
                    frame = nullptr;
                    callbackState.shared->publish(
                        std::move(prepared.owned.texture),
                        std::move(prepared.owned.lease),
                        outputTimestamp100ns,
                        size.Width,
                        size.Height,
                        true,
                        false,
                        std::move(prepared.preparation),
                        std::move(prepared.owned.gpuReadState));
                    const int64_t frameProcessed100ns = monotonicNow100ns();
                    callbackState.shared->frameProcessingLatency.record(
                        frameProcessed100ns,
                        frameProcessed100ns - frameProcessingStarted100ns);
                }
            } catch (const winrt::hresult_error& error) {
                ++callbackState.shared->callbackErrors;
                callbackState.fail("WGC frame callback failed: " + narrow(error.message().c_str()));
            } catch (const std::exception& error) {
                ++callbackState.shared->callbackErrors;
                callbackState.fail("WGC frame callback failed: " + std::string(error.what()));
            } catch (...) {
                ++callbackState.shared->callbackErrors;
                callbackState.fail("WGC frame callback failed with an unknown error.");
            }
        });

        state.session = state.framePool.CreateCaptureSession(state.item);
        state.session.IsCursorCaptureEnabled(true);
        // Capability-query the optional API; old Windows keeps its behavior.
        // Zero removes an API-side rate floor, not a promise of delivered FPS.
        if (auto cadence = state.session.try_as<winrt::Windows::Graphics::Capture::IGraphicsCaptureSession5>()) {
            try {
                cadence.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{0});
                std::cerr << "[capture-pipeline] WGC minUpdateInterval100ns="
                          << cadence.MinUpdateInterval().count() << '\n';
            } catch (const winrt::hresult_error& error) {
                std::cerr << "[capture-pipeline] WGC interval request unavailable: "
                          << hresultHex(error.code()) << '\n';
            }
        }
        try {
            state.session.IsBorderRequired(false);
        } catch (...) {
        }
        state.started.store(true, std::memory_order_release);
        state.session.StartCapture();
        const bool capturesWindow = state.window != nullptr;
        const auto backendKind = capturesWindow
            ? CaptureBackendKind::WgcWindow
            : CaptureBackendKind::Wgc;
        const std::string captureName = capturesWindow && !state.targetName.empty()
            ? state.targetName
            : state.output.displayName;
        state.shared->setActiveBackend(backendKind);
        state.shared->running.store(true, std::memory_order_release);
        state.shared->setStatus(
            std::string("Windows.Graphics.Capture is running on ") + captureName + ".");
        std::cerr << "[capture] Windows.Graphics.Capture started on "
                  << captureName
                  << " target=" << (capturesWindow ? "game-window" : "monitor")
                  << ".\n";
        return { true, {} };
    } catch (const winrt::hresult_error& error) {
        return { false, "WGC start failed: " + narrow(error.message().c_str()) };
    } catch (const std::exception& error) {
        return { false, "WGC start failed: " + std::string(error.what()) };
    }
}

BackendOutcome WgcCaptureBackend::run(std::stop_token stopToken) {
    while (!stopToken.stop_requested() && !impl_->failed.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (stopToken.stop_requested()) return BackendOutcome::Stopped;
    const std::string reason = impl_->failure();
    impl_->shared->setStatus(reason.empty() ? "Windows.Graphics.Capture stopped unexpectedly." : reason);
    return BackendOutcome::Failed;
}

void WgcCaptureBackend::stop() {
    auto& state = *impl_;
    state.started.store(false, std::memory_order_release);
    if (state.framePool) {
        try {
            state.framePool.FrameArrived(state.frameArrivedToken);
        } catch (...) {
        }
    }
    if (state.item) {
        try {
            state.item.Closed(state.itemClosedToken);
        } catch (...) {
        }
    }
    {
        std::lock_guard callbackLock(state.callbackMutex);
        if (state.session) {
            try {
                state.session.Close();
            } catch (...) {
            }
            state.session = nullptr;
        }
        if (state.framePool) {
            try {
                state.framePool.Close();
            } catch (...) {
            }
            state.framePool = nullptr;
        }
        state.item = nullptr;
        state.direct3DDevice = nullptr;
        state.tonemapper.reset();
        state.texturePool.reset();
        state.d3dContext.Reset();
        state.d3dDevice.Reset();
    }
    state.shared->hdrTonemappingActive.store(false, std::memory_order_relaxed);
    if (state.apartmentInitialized && state.apartmentThreadId == GetCurrentThreadId()) {
        winrt::uninit_apartment();
        state.apartmentInitialized = false;
        state.apartmentThreadId = 0;
    }
}

}  // namespace clipture::capture
