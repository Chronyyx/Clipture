#include "WgcFramePreparation.hpp"

namespace clipture::capture {
PreparedWgcFrame prepareWgcFrame(ID3D11Texture2D* source, UINT width, UINT height,
    CaptureTexturePool& pool, std::shared_ptr<Tonemapper> tone,
    std::shared_ptr<CaptureSharedState> stats, ID3D11Device* device,
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, GpuStageProbe& copyProbe,
    const CapturePipelinePolicy& policy, std::string& error, WgcOverlays overlays) {
    if (!source || !width || !height) { error = "WGC source is empty."; return {}; }
    D3D11_TEXTURE2D_DESC desc {};
    source->GetDesc(&desc);
    const bool hdr = desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    if ((hdr && !tone) || (!hdr && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) ||
        desc.Width < width || desc.Height < height || desc.SampleDesc.Count != 1) {
        error = "WGC source format or content size is unsupported.";
        return {}; // Never publish an uninitialized or partly copied texture.
    }
    // Tone mapping reads the WGC surface in place; WGC creates it as a shader
    // resource, and the pass is queued before the frame returns to its pool.
    const bool directTone = hdr && policy.wgcDirectTonemap && !policy.deferPreparation;
    // Slot layout follows the session, not the frame: an 8-bit game window on an
    // HDR desktop must not regenerate the pool at every recorded-source switch.
    const bool hdrSession = tone != nullptr;
    auto owned = pool.acquire(device, width, height, hdrSession, error, hdrSession && policy.deferPreparation,
        policy.directTextureRead && policy.isolatedDevice && policy.gpuHandoff,
        hdrSession && policy.wgcDirectTonemap && !policy.deferPreparation);
    if (!owned.lease) return {}; // Pool counts GPU-pressure drops itself.
    std::shared_ptr<DeferredFramePreparation> preparation;
    if (directTone) {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sourceView;
        if (FAILED(device->CreateShaderResourceView(source, nullptr, &sourceView)) || !sourceView) {
            error = "WGC surface shader view creation failed.";
            return {};
        }
        Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
        context.As(&multithread);
        if (multithread) multithread->Enter();
        const auto start = monotonicNow100ns();
        const bool toned = tone->Process(sourceView.Get(), owned.toneOutputView.Get(), width, height, error);
        const auto finished = monotonicNow100ns();
        if (multithread) multithread->Leave();
        if (!toned) return {};
        stats->framePreparationLatency.record(finished, finished - start);
    } else {
        const D3D11_BOX box {0, 0, 0, width, height, 1};
        auto sample = copyProbe.scope(context.Get());
        context->CopySubresourceRegion(hdr ? owned.hdrInputTexture.Get() : owned.texture.Get(),
            0, 0, 0, 0, source, 0, &box);
    }
    if (hdr && !directTone) {
        preparation = deferDesktopPreparation(owned, std::move(tone), nullptr, {},
            std::move(stats), context, width, height);
    }
    if (overlays.compositor && !overlays.layers.empty()) {
        // Overlays are SDR UI; draw them after tone mapping, as on screen.
        preparation = std::make_shared<DeferredFramePreparation>(
            [base = std::move(preparation), compositor = std::move(overlays.compositor),
             layers = std::move(overlays.layers), output = owned.renderTargetView,
             context, width, height](std::string& failure) {
                if (base && !base->prepare(failure)) return false;
                Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
                context.As(&multithread);
                if (multithread) multithread->Enter();
                const bool ok = compositor->composite(layers, output.Get(), width, height, failure);
                if (multithread) multithread->Leave();
                return ok;
            });
    }
    if (preparation && !policy.deferPreparation && !preparation->prepare(error)) return {};
    return {std::move(owned), std::move(preparation)};
}
}
