#pragma once
#include "DesktopFramePreparation.hpp"
#include "GpuStageProbe.hpp"
#include "clipture/CapturePipelinePolicy.hpp"

namespace clipture::capture {
struct PreparedWgcFrame {
    CaptureTexture owned;
    std::shared_ptr<DeferredFramePreparation> preparation;
};
// Copies the checked-out WGC surface; never retains it after the callback.
// WGC already includes the cursor, so deferred preparation does HDR only.
PreparedWgcFrame prepareWgcFrame(ID3D11Texture2D* source, UINT width, UINT height,
    CaptureTexturePool& pool, std::shared_ptr<Tonemapper> tone,
    std::shared_ptr<CaptureSharedState> stats, ID3D11Device* device,
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, GpuStageProbe& copyProbe,
    const CapturePipelinePolicy& policy, std::string& error);
}
