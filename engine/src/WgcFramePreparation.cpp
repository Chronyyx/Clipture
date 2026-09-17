#include "WgcFramePreparation.hpp"

namespace clipture::capture {
PreparedWgcFrame prepareWgcFrame(ID3D11Texture2D* source, UINT width, UINT height,
    CaptureTexturePool& pool, std::shared_ptr<Tonemapper> tone,
    std::shared_ptr<CaptureSharedState> stats, ID3D11Device* device,
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, GpuStageProbe& copyProbe,
    const CapturePipelinePolicy& policy, std::string& error) {
    if (!source || !width || !height) { error = "WGC source is empty."; return {}; }
    D3D11_TEXTURE2D_DESC desc {};
    source->GetDesc(&desc);
    const bool hdr = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if ((hdr && !tone) || (!hdr && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) ||
        desc.Width < width || desc.Height < height || desc.SampleDesc.Count != 1) {
        error = "WGC source format or content size is unsupported.";
        return {}; // Never publish an uninitialized or partly copied texture.
    }
    auto owned = pool.acquire(device, width, height, hdr, error, hdr && policy.deferPreparation,
        policy.directTextureRead && policy.isolatedDevice && policy.gpuHandoff);
    if (!owned.lease) return {}; // Pool counts GPU-pressure drops itself.
    const D3D11_BOX box {0, 0, 0, width, height, 1};
    {
        auto sample = copyProbe.scope(context.Get());
        context->CopySubresourceRegion(hdr ? owned.hdrInputTexture.Get() : owned.texture.Get(),
            0, 0, 0, 0, source, 0, &box);
    }
    std::shared_ptr<DeferredFramePreparation> preparation;
    if (hdr) {
        preparation = deferDesktopPreparation(owned, std::move(tone), nullptr, {},
            std::move(stats), context, width, height);
        if (!policy.deferPreparation && !preparation->prepare(error)) return {};
    }
    return {std::move(owned), std::move(preparation)};
}
}
