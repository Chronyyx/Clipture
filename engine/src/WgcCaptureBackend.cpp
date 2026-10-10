#include "WgcCaptureBackend.hpp"
#include "GameFrameProbe.hpp"
#include "GameWindowCoverage.hpp"
#include "OverlayWindowCapture.hpp"
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

    // Hybrid capture: a second WGC source on the detected game window. While
    // the game covers the monitor unobstructed its frames stand in for the
    // desktop, which the compositor stops refreshing under full GPU load
    // (CS2 smokes). Anything else on screen switches back to the desktop.
    struct GameSource {
        HWND window = nullptr;
        GraphicsCaptureItem item { nullptr };
        Direct3D11CaptureFramePool framePool { nullptr };
        GraphicsCaptureSession session { nullptr };
        winrt::event_token frameArrivedToken {};
        winrt::Windows::Graphics::SizeInt32 size {};
        bool verified = false;  // a captured frame showed content (never black-only)
        int64_t lastProbe100ns = 0;
    };
    GameSource game;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> probeStaging;
    static constexpr int64_t kGameProbeInterval100ns = 2'500'000;  // 250 ms
    HWND failedGameWindow = nullptr;
    std::atomic<bool> recordGame = false;
    int64_t coveredSince100ns = 0;
    std::string lastCoverageReason;
    std::shared_ptr<OverlayCompositor> overlayCompositor;
    std::unique_ptr<OverlayWindowCapture> overlayCapture;
    static constexpr int64_t kGameSwitchDelay100ns = 2'000'000;  // 200 ms of steady coverage

    static void drain(Direct3D11CaptureFramePool const& pool) {
        while (auto frame = pool.TryGetNextFrame()) {
            try { frame.Close(); } catch (...) {}
        }
    }

    void onFrames(Direct3D11CaptureFramePool const& sender, bool fromGame) {
        try {
            std::lock_guard callbackLock(callbackMutex);
            if (!started.load(std::memory_order_acquire)) return;
            if (fromGame && !game.verified) {
                verifyGameFrames(sender);
                return;
            }
            if (fromGame != recordGame.load(std::memory_order_acquire)) {
                drain(sender);
                return;
            }

            while (auto frame = sender.TryGetNextFrame()) {
                ++shared->acquiredUpdates;

                const auto size = frame.ContentSize();
                if (size.Width <= 0 || size.Height <= 0) {
                    try { frame.Close(); } catch (...) {}
                    continue;
                }
                if (fromGame) {
                    if (size.Width != game.size.Width || size.Height != game.size.Height) {
                        try { frame.Close(); } catch (...) {}
                        game.size = size;
                        game.verified = false;
                        sender.Recreate(direct3DDevice, framePoolPixelFormat, 4, size);
                        return;
                    }
                    if (size.Width != framePoolSize.Width || size.Height != framePoolSize.Height) {
                        try { frame.Close(); } catch (...) {}
                        continue;
                    }
                } else if (size.Width != framePoolSize.Width || size.Height != framePoolSize.Height) {
                    try { frame.Close(); } catch (...) {}
                    frame = nullptr;
                    framePoolSize = size;
                    // Queued selected frames may still use the tone mapper.
                    // Its bounded view cache owns old resources until retired.
                    texturePool.reset();
                    sourceClock.reset();
                    sourceClockRejectionLogged = false;
                    shared->beginEpoch();
                    {
                        std::lock_guard stateLock(shared->stateMutex);
                        shared->resolution =
                            std::to_string(size.Width) + "x" + std::to_string(size.Height);
                    }
                    sender.Recreate(direct3DDevice, framePoolPixelFormat, 4, size);
                    return;
                }

                const int64_t sourceRelative100ns = frame.SystemRelativeTime().count();
                if (!sourceClock.accept(sourceRelative100ns)) {
                    ++shared->nonMonotonicTimestamps;
                    if (!sourceClockRejectionLogged) {
                        sourceClockRejectionLogged = true;
                        std::cerr << "[capture] WGC rejected stale source timestamp=" << sourceRelative100ns
                                  << " last=" << sourceClock.last() << '\n';
                    }
                    try { frame.Close(); } catch (...) {}
                    continue;
                }
                ++shared->desktopPresents;
                const int64_t sourceTimestamp100ns = mediaTimeFromSystemRelative100ns(sourceRelative100ns);
                int64_t outputTimestamp100ns = 0;
                if (!shared->selectFrameTimestamp(sourceTimestamp100ns, outputTimestamp100ns)) {
                    try { frame.Close(); } catch (...) {}
                    continue;
                }
                const int64_t frameProcessingStarted100ns = monotonicNow100ns();

                auto access = frame.Surface().template as<
                    ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                Microsoft::WRL::ComPtr<ID3D11Texture2D> sourceTexture;
                if (FAILED(access->GetInterface(IID_PPV_ARGS(&sourceTexture))) || !sourceTexture) {
                    ++shared->callbackErrors;
                    try { frame.Close(); } catch (...) {}
                    continue;
                }

                std::string slotError;
                WgcOverlays overlays;
                if (fromGame && overlayCapture) overlays = {overlayCompositor, overlayCapture->snapshot()};
                auto prepared = prepareWgcFrame(sourceTexture.Get(), size.Width, size.Height,
                    texturePool, tonemapper, shared, d3dDevice.Get(), d3dContext, copyProbe,
                    capturePipelinePolicy(), slotError, std::move(overlays));
                if (!prepared.owned.lease) {
                    if (!slotError.empty()) {
                        ++shared->callbackErrors;
                        fail(slotError);
                    }
                    try { frame.Close(); } catch (...) {}
                    continue;
                }

                sourceTexture.Reset();
                try { frame.Close(); } catch (...) {}
                frame = nullptr;
                shared->publish(
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
                shared->frameProcessingLatency.record(
                    frameProcessed100ns,
                    frameProcessed100ns - frameProcessingStarted100ns);
            }
        } catch (const winrt::hresult_error& error) {
            ++shared->callbackErrors;
            fail("WGC frame callback failed: " + narrow(error.message().c_str()));
        } catch (const std::exception& error) {
            ++shared->callbackErrors;
            fail("WGC frame callback failed: " + std::string(error.what()));
        } catch (...) {
            ++shared->callbackErrors;
            fail("WGC frame callback failed with an unknown error.");
        }
    }

    // Game frames are recorded only after one is seen with content; a window
    // that captures black-only (hidden from capture, exclusive fullscreen)
    // keeps the desktop. Called with callbackMutex held.
    void verifyGameFrames(Direct3D11CaptureFramePool const& sender) {
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame newest { nullptr };
        while (auto frame = sender.TryGetNextFrame()) {
            if (newest) { try { newest.Close(); } catch (...) {} }
            newest = frame;
        }
        if (!newest) return;
        const auto size = newest.ContentSize();
        const int64_t now = monotonicNow100ns();
        if (size.Width == game.size.Width && size.Height == game.size.Height &&
            now - game.lastProbe100ns >= kGameProbeInterval100ns) {
            game.lastProbe100ns = now;
            auto access = newest.Surface().template as<
                ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
            if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(&texture))) && texture &&
                gameFrameHasContent(d3dDevice.Get(), d3dContext.Get(), texture.Get(),
                    static_cast<UINT>(size.Width), static_cast<UINT>(size.Height), probeStaging)) {
                game.verified = true;
                std::cerr << "[capture] Hybrid capture verified game-window frames.\n";
            }
        }
        try { newest.Close(); } catch (...) {}
    }

    void openGame(HWND gameWindow) {
        try {
            auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            winrt::com_ptr<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem> abiItem;
            winrt::check_hresult(interop->CreateForWindow(
                gameWindow,
                __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem),
                abiItem.put_void()));
            game.item = abiItem.as<GraphicsCaptureItem>();
            game.size = game.item.Size();
            game.framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
                direct3DDevice, framePoolPixelFormat, 4, game.size);
            game.frameArrivedToken = game.framePool.FrameArrived([this](auto const& sender, auto const&) {
                onFrames(sender, true);
            });
            game.session = game.framePool.CreateCaptureSession(game.item);
            game.session.IsCursorCaptureEnabled(true);
            if (auto cadence = game.session.try_as<winrt::Windows::Graphics::Capture::IGraphicsCaptureSession5>()) {
                try { cadence.MinUpdateInterval(winrt::Windows::Foundation::TimeSpan{0}); } catch (...) {}
            }
            try { game.session.IsBorderRequired(false); } catch (...) {}
            game.window = gameWindow;
            if (!overlayCompositor) {
                auto compositor = std::make_shared<OverlayCompositor>(d3dDevice, d3dContext);
                std::string error;
                if (compositor->initialize(error)) overlayCompositor = std::move(compositor);
                else std::cerr << "[capture] Overlay compositor unavailable: " << error << '\n';
            }
            if (!overlayCapture) {
                overlayCapture = std::make_unique<OverlayWindowCapture>(
                    d3dDevice, d3dContext, static_cast<IUnknown*>(winrt::get_abi(direct3DDevice)));
            }
            game.session.StartCapture();
            std::cerr << "[capture] Hybrid capture watching game window " << game.size.Width << 'x'
                      << game.size.Height << ".\n";
        } catch (const winrt::hresult_error& error) {
            closeGame();
            failedGameWindow = gameWindow;
            std::cerr << "[capture] Hybrid capture cannot capture the game window: "
                      << hresultHex(error.code()) << '\n';
        }
    }

    void closeGame() {
        recordGame.store(false, std::memory_order_release);
        if (game.framePool) {
            try { game.framePool.FrameArrived(game.frameArrivedToken); } catch (...) {}
        }
        std::lock_guard callbackLock(callbackMutex);
        if (game.session) {
            try { game.session.Close(); } catch (...) {}
        }
        if (game.framePool) {
            try { game.framePool.Close(); } catch (...) {}
        }
        game = {};
        if (overlayCapture) overlayCapture->clear();
        coveredSince100ns = 0;
        lastCoverageReason.clear();
    }

    void setRecordGame(bool next, const std::string& reason) {
        if (recordGame.exchange(next, std::memory_order_acq_rel) == next) return;
        std::cerr << "[capture] Hybrid capture records "
                  << (next ? "the game window" : "the desktop: " + reason) << ".\n";
    }

    // Runs on the backend thread every poll.
    void updateHybrid() {
        if (window || !capturePipelinePolicy().hybridGameCapture) return;
        auto wanted = static_cast<HWND>(shared->hybridGameWindow.load(std::memory_order_acquire));
        if (wanted && !IsWindow(wanted)) wanted = nullptr;
        if (wanted != game.window) {
            closeGame();
            if (wanted && wanted != failedGameWindow) openGame(wanted);
        }
        if (!game.session) return;

        auto coverage = gameWindowCoverage(game.window, output.desc.Monitor);
        if (coverage.covers && !coverage.overlays.empty() && !overlayCompositor) {
            coverage = { false, "overlays above game cannot be composited" };
        }
        if (coverage.covers && overlayCapture) {
            MONITORINFO info { sizeof(info) };
            GetMonitorInfoW(output.desc.Monitor, &info);
            if (!overlayCapture->sync(coverage.overlays, info.rcMonitor)) {
                coverage = { false, "overlay above game not captured yet" };
            }
        }
        if (coverage.covers) {
            std::lock_guard callbackLock(callbackMutex);
            if (!game.verified) {
                coverage = { false, "waiting for game-window frames with content" };
            } else if (game.size.Width != framePoolSize.Width || game.size.Height != framePoolSize.Height) {
                coverage = { false, "game window is " + std::to_string(game.size.Width) + "x" +
                    std::to_string(game.size.Height) + ", monitor is " +
                    std::to_string(framePoolSize.Width) + "x" + std::to_string(framePoolSize.Height) };
            }
        }
        if (!coverage.covers) {
            coveredSince100ns = 0;
            if (coverage.reason != lastCoverageReason) {
                lastCoverageReason = coverage.reason;
                if (!recordGame.load(std::memory_order_acquire)) {
                    std::cerr << "[capture] Hybrid capture stays on the desktop: " << coverage.reason << ".\n";
                }
            }
            setRecordGame(false, coverage.reason);
            return;
        }
        lastCoverageReason.clear();
        const int64_t now = monotonicNow100ns();
        if (!coveredSince100ns) coveredSince100ns = now;
        if (now - coveredSince100ns >= kGameSwitchDelay100ns) setRecordGame(true, {});
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
            impl_->onFrames(sender, false);
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
        impl_->updateHybrid();
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
    state.closeGame();
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
