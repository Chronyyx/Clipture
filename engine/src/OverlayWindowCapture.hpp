#pragma once

#include "OverlayCompositor.hpp"

#include <Windows.h>

#include <memory>
#include <vector>

namespace clipture::capture {

// Window captures of the click-through overlays stacked above a game (Discord
// overlay, FPS counters). Each keeps its newest image as an immutable texture
// that game frames composite at publish time. Not thread-safe except snapshot().
class OverlayWindowCapture {
public:
    // `direct3DDevice` is the WinRT IDirect3DDevice wrapping `device`, as IInspectable.
    OverlayWindowCapture(Microsoft::WRL::ComPtr<ID3D11Device> device,
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, IUnknown* direct3DDevice);
    ~OverlayWindowCapture();

    // Captures exactly `windows` (bottom to top). Returns false while any of
    // them cannot be captured or has not delivered its first image.
    bool sync(const std::vector<HWND>& windows, const RECT& monitor);
    std::vector<OverlayLayer> snapshot() const;
    void clear();

private:
    struct Source;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace clipture::capture
