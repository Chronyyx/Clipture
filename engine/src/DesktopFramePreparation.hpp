#pragma once
#include "CaptureBackend.hpp"
#include "DesktopPointerCompositor.hpp"
#include "clipture/DeferredFramePreparation.hpp"
#include "clipture/Tonemapper.hpp"

namespace clipture::capture {
std::shared_ptr<DeferredFramePreparation> deferDesktopPreparation(
    CaptureTexture texture, std::shared_ptr<Tonemapper> tonemapper,
    std::shared_ptr<DesktopPointerCompositor> compositor,
    DesktopPointerCompositor::Snapshot pointer, std::shared_ptr<CaptureSharedState> stats,
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, UINT width, UINT height);
}
