#pragma once
#include "DesktopFramePreparation.hpp"
#include "GpuStageProbe.hpp"
#include "OverlayCompositor.hpp"
#include "clipture/CapturePipelinePolicy.hpp"

namespace clipture::capture {
struct PreparedWgcFrame {
    CaptureTexture owned;
    std::shared_ptr<DeferredFramePreparation> preparation;
};
// Overlay windows drawn over a game-window frame once it is SDR.
struct WgcOverlays {
    std::shared_ptr<OverlayCompositor> compositor;
    std::vector<OverlayLayer> layers;
};
// Copies the checked-out WGC surface; never retains it after the callback.
// WGC already includes the cursor, so deferred preparation does HDR and
// overlays only.
PreparedWgcFrame prepareWgcFrame(ID3D11Texture2D* source, UINT width, UINT height,
    CaptureTexturePool& pool, std::shared_ptr<Tonemapper> tone,
    std::shared_ptr<CaptureSharedState> stats, ID3D11Device* device,
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, GpuStageProbe& copyProbe,
    const CapturePipelinePolicy& policy, std::string& error, WgcOverlays overlays = {});
}
