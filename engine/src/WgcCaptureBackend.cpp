#include "WgcCaptureBackend.hpp"

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
    std::unique_ptr<Tonemapper> tonemapper;
    std::mutex callbackMutex;
    std::mutex failureMutex;
    std::string failureReason;
    std::atomic<bool> failed = false;
    std::atomic<bool> started = false;

    Impl(std::shared_ptr<CaptureSharedState> nextShared, SelectedOutput nextOutput)
        : shared(std::move(nextShared)), output(std::move(nextOutput)), texturePool(shared) {}

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

WgcCaptureBackend::WgcCaptureBackend(std::shared_ptr<CaptureSharedState> shared, SelectedOutput output)
    : impl_(std::make_unique<Impl>(std::move(shared), std::move(output))) {}

WgcCaptureBackend::~WgcCaptureBackend() {
    stop();
}

BackendStartResult WgcCaptureBackend::start() {
    auto& state = *impl_;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        HRESULT hr = createD3dDeviceForOutput(state.output.adapter.Get(), state.d3dDevice, state.d3dContext);
        if (FAILED(hr)) return { false, "D3D11 device creation for WGC failed: " + hresultHex(hr) };

        Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
        hr = state.d3dDevice.As(&dxgiDevice);
        if (FAILED(hr)) return { false, "Could not query IDXGIDevice for WGC: " + hresultHex(hr) };

        winrt::com_ptr<::IInspectable> inspectableDevice;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectableDevice.put());
        if (FAILED(hr)) return { false, "CreateDirect3D11DeviceFromDXGIDevice failed: " + hresultHex(hr) };
        state.direct3DDevice = inspectableDevice.as<IDirect3DDevice>();

        auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::com_ptr<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem> abiItem;
        hr = interop->CreateForMonitor(
            state.output.desc.Monitor,
            __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem),
            abiItem.put_void());
        if (FAILED(hr)) return { false, "CreateForMonitor failed: " + hresultHex(hr) };
        state.item = abiItem.as<GraphicsCaptureItem>();

        state.itemClosedToken = state.item.Closed([this](auto const&, auto const&) {
            impl_->fail("Windows.Graphics.Capture item closed.");
        });

        DirectXPixelFormat pixelFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;
        if (state.output.hdrEnabled) {
            pixelFormat = DirectXPixelFormat::R16G16B16A16Float;
            state.tonemapper = std::make_unique<Tonemapper>(state.d3dDevice);
            std::string tonemapperError;
            if (!state.tonemapper->Initialize(
                    tonemapperError,
                    monitorSdrWhiteLevel(state.output.desc.Monitor))) {
                return { false, "WGC HDR tonemapper initialization failed: " + tonemapperError };
            }
            state.shared->hdrTonemappingActive.store(true, std::memory_order_relaxed);
        } else {
            state.shared->hdrTonemappingActive.store(false, std::memory_order_relaxed);
        }

        state.framePoolPixelFormat = pixelFormat;
        state.framePoolSize = state.item.Size();
        state.framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
            state.direct3DDevice,
            pixelFormat,
            3,
            state.framePoolSize);

        state.frameArrivedToken = state.framePool.FrameArrived([this](auto const& sender, auto const&) {
            auto& callbackState = *impl_;
            try {
                std::lock_guard callbackLock(callbackState.callbackMutex);
                if (!callbackState.started.load(std::memory_order_acquire)) return;

                auto frame = sender.TryGetNextFrame();
                if (!frame) return;
                ++callbackState.shared->acquiredUpdates;
                ++callbackState.shared->desktopPresents;
                while (auto newerFrame = sender.TryGetNextFrame()) {
                    frame = std::move(newerFrame);
                    ++callbackState.shared->acquiredUpdates;
                    ++callbackState.shared->desktopPresents;
                    ++callbackState.shared->sourceFramesSuperseded;
                }

                const auto size = frame.ContentSize();
                if (size.Width <= 0 || size.Height <= 0) return;
                if (size.Width != callbackState.framePoolSize.Width ||
                    size.Height != callbackState.framePoolSize.Height) {
                    frame = nullptr;
                    callbackState.framePoolSize = size;
                    if (callbackState.tonemapper) callbackState.tonemapper->ResetViewCache();
                    callbackState.texturePool.reset();
                    callbackState.shared->beginEpoch();
                    {
                        std::lock_guard stateLock(callbackState.shared->stateMutex);
                        callbackState.shared->resolution =
                            std::to_string(size.Width) + "x" + std::to_string(size.Height);
                    }
                    sender.Recreate(
                        callbackState.direct3DDevice,
                        callbackState.framePoolPixelFormat,
                        3,
                        size);
                    return;
                }

                const int64_t sourceTimestamp100ns = mediaTimeFromSystemRelative100ns(
                    frame.SystemRelativeTime().count());
                int64_t outputTimestamp100ns = 0;
                if (!callbackState.shared->selectFrameTimestamp(
                        sourceTimestamp100ns, outputTimestamp100ns)) return;

                auto access = frame.Surface().template as<
                    ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                Microsoft::WRL::ComPtr<ID3D11Texture2D> sourceTexture;
                if (FAILED(access->GetInterface(IID_PPV_ARGS(&sourceTexture))) || !sourceTexture) {
                    ++callbackState.shared->callbackErrors;
                    return;
                }

                D3D11_TEXTURE2D_DESC sourceDesc {};
                sourceTexture->GetDesc(&sourceDesc);
                const bool needsTonemapping = callbackState.tonemapper &&
                    sourceDesc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
                std::string slotError;
                auto owned = callbackState.texturePool.acquire(
                    callbackState.d3dDevice.Get(),
                    static_cast<UINT>(size.Width),
                    static_cast<UINT>(size.Height),
                    needsTonemapping,
                    slotError);
                if (!owned.texture || !owned.lease) {
                    if (!slotError.empty()) callbackState.fail(slotError);
                    return;
                }

                D3D11_BOX sourceBox {
                    0,
                    0,
                    0,
                    static_cast<UINT>(size.Width),
                    static_cast<UINT>(size.Height),
                    1,
                };
                if (needsTonemapping) {
                    if (!owned.hdrInputTexture) return;
                    callbackState.d3dContext->CopySubresourceRegion(
                        owned.hdrInputTexture.Get(), 0, 0, 0, 0, sourceTexture.Get(), 0, &sourceBox);
                    std::string error;
                    if (!callbackState.tonemapper->Process(owned.hdrInputTexture, owned.texture, error)) {
                        ++callbackState.shared->callbackErrors;
                        callbackState.fail("WGC HDR tonemapping failed: " + error);
                        return;
                    }
                } else {
                    callbackState.d3dContext->CopySubresourceRegion(
                        owned.texture.Get(), 0, 0, 0, 0, sourceTexture.Get(), 0, &sourceBox);
                }

                callbackState.shared->publish(
                    std::move(owned.texture),
                    std::move(owned.lease),
                    outputTimestamp100ns,
                    size.Width,
                    size.Height);
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
        try {
            state.session.IsBorderRequired(false);
        } catch (...) {
        }
        state.started.store(true, std::memory_order_release);
        state.session.StartCapture();
        state.shared->setActiveBackend(CaptureBackendKind::Wgc);
        state.shared->running.store(true, std::memory_order_release);
        state.shared->setStatus("Windows.Graphics.Capture is running on " + state.output.displayName + ".");
        std::cerr << "[capture] Windows.Graphics.Capture started on "
                  << state.output.displayName << ".\n";
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
}

}  // namespace clipture::capture
