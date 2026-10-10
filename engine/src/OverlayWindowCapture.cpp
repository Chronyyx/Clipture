#include "OverlayWindowCapture.hpp"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <algorithm>
#include <iostream>
#include <mutex>
#include <set>

namespace clipture::capture {
namespace {
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;
}  // namespace

struct OverlayWindowCapture::Source {
    HWND window = nullptr;
    GraphicsCaptureItem item { nullptr };
    Direct3D11CaptureFramePool pool { nullptr };
    GraphicsCaptureSession session { nullptr };
    winrt::event_token token {};
    winrt::Windows::Graphics::SizeInt32 size {};
    mutable std::mutex mutex;
    OverlayLayer latest;
    int x = 0, y = 0;

    void close() {
        if (pool) { try { pool.FrameArrived(token); } catch (...) {} }
        if (session) { try { session.Close(); } catch (...) {} }
        if (pool) { try { pool.Close(); } catch (...) {} }
        session = nullptr;
        pool = nullptr;
        item = nullptr;
    }
};

struct OverlayWindowCapture::Impl {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    IDirect3DDevice direct3D { nullptr };
    std::vector<std::shared_ptr<Source>> sources;  // bottom to top
    mutable std::mutex sourcesMutex;
    std::set<HWND> failed;

    void onFrame(Source& source, Direct3D11CaptureFramePool const& sender) {
        try {
            while (auto frame = sender.TryGetNextFrame()) {
                const auto size = frame.ContentSize();
                if (size.Width != source.size.Width || size.Height != source.size.Height) {
                    frame.Close();
                    source.size = size;
                    sender.Recreate(direct3D, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
                    return;
                }
                auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                Microsoft::WRL::ComPtr<ID3D11Texture2D> surface;
                if (FAILED(access->GetInterface(IID_PPV_ARGS(&surface))) || !surface) { frame.Close(); continue; }

                D3D11_TEXTURE2D_DESC desc {};
                surface->GetDesc(&desc);
                desc.Width = static_cast<UINT>(size.Width);
                desc.Height = static_cast<UINT>(size.Height);
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                desc.CPUAccessFlags = 0;
                desc.MiscFlags = 0;
                Microsoft::WRL::ComPtr<ID3D11Texture2D> copy;
                Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
                if (FAILED(device->CreateTexture2D(&desc, nullptr, &copy)) ||
                    FAILED(device->CreateShaderResourceView(copy.Get(), nullptr, &view))) {
                    frame.Close();
                    continue;
                }
                const D3D11_BOX box {0, 0, 0, desc.Width, desc.Height, 1};
                context->CopySubresourceRegion(copy.Get(), 0, 0, 0, 0, surface.Get(), 0, &box);
                frame.Close();
                std::lock_guard lock(source.mutex);
                source.latest = {std::move(view), source.x, source.y, desc.Width, desc.Height};
            }
        } catch (...) {
        }
    }

    std::shared_ptr<Source> open(HWND window) {
        auto source = std::make_shared<Source>();
        try {
            auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            winrt::com_ptr<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem> abiItem;
            winrt::check_hresult(interop->CreateForWindow(window,
                __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureItem), abiItem.put_void()));
            source->window = window;
            source->item = abiItem.as<GraphicsCaptureItem>();
            source->size = source->item.Size();
            source->pool = Direct3D11CaptureFramePool::CreateFreeThreaded(
                direct3D, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, source->size);
            // A callback in flight keeps its source alive past close().
            source->token = source->pool.FrameArrived(
                [this, weak = std::weak_ptr<Source>(source)](auto const& sender, auto const&) {
                    if (auto alive = weak.lock()) onFrame(*alive, sender);
                });
            source->session = source->pool.CreateCaptureSession(source->item);
            source->session.IsCursorCaptureEnabled(false);
            try { source->session.IsBorderRequired(false); } catch (...) {}
            source->session.StartCapture();
            return source;
        } catch (const winrt::hresult_error& error) {
            std::cerr << "[capture] Cannot capture overlay window: 0x" << std::hex
                      << static_cast<uint32_t>(error.code()) << std::dec << '\n';
            source->close();
            return nullptr;
        }
    }
};

OverlayWindowCapture::OverlayWindowCapture(Microsoft::WRL::ComPtr<ID3D11Device> device,
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, IUnknown* direct3DDevice)
    : impl_(std::make_unique<Impl>()) {
    impl_->device = std::move(device);
    impl_->context = std::move(context);
    winrt::copy_from_abi(impl_->direct3D, direct3DDevice);
}

OverlayWindowCapture::~OverlayWindowCapture() {
    clear();
}

bool OverlayWindowCapture::sync(const std::vector<HWND>& windows, const RECT& monitor) {
    auto& state = *impl_;
    std::vector<std::shared_ptr<Source>> next;
    bool ready = true;
    {
        std::lock_guard lock(state.sourcesMutex);
        for (HWND window : windows) {
            auto existing = std::find_if(state.sources.begin(), state.sources.end(),
                [window](const auto& source) { return source && source->window == window; });
            std::shared_ptr<Source> source;
            if (existing != state.sources.end()) source = std::move(*existing);
            else if (!state.failed.contains(window)) source = state.open(window);
            if (!source) {
                state.failed.insert(window);
                ready = false;
                continue;
            }
            RECT rect {};
            GetWindowRect(window, &rect);
            std::lock_guard sourceLock(source->mutex);
            source->x = rect.left - monitor.left;
            source->y = rect.top - monitor.top;
            source->latest.x = source->x;
            source->latest.y = source->y;
            if (!source->latest.view) ready = false;
            next.push_back(std::move(source));
        }
        for (auto& stale : state.sources) {
            if (stale) stale->close();
        }
        state.sources = std::move(next);
    }
    return ready;
}

std::vector<OverlayLayer> OverlayWindowCapture::snapshot() const {
    std::vector<OverlayLayer> layers;
    std::lock_guard lock(impl_->sourcesMutex);
    layers.reserve(impl_->sources.size());
    for (const auto& source : impl_->sources) {
        std::lock_guard sourceLock(source->mutex);
        if (source->latest.view) layers.push_back(source->latest);
    }
    return layers;
}

void OverlayWindowCapture::clear() {
    std::lock_guard lock(impl_->sourcesMutex);
    for (auto& source : impl_->sources) source->close();
    impl_->sources.clear();
    impl_->failed.clear();
}

}  // namespace clipture::capture
