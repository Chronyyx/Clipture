#include "DesktopFramePreparation.hpp"
#include "clipture/MediaClock.hpp"

namespace clipture::capture {
std::shared_ptr<DeferredFramePreparation> deferDesktopPreparation(
    CaptureTexture texture, std::shared_ptr<Tonemapper> tonemapper,
    std::shared_ptr<DesktopPointerCompositor> compositor,
    DesktopPointerCompositor::Snapshot pointer, std::shared_ptr<CaptureSharedState> stats,
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context, UINT width, UINT height) {
    return std::make_shared<DeferredFramePreparation>(
        [texture = std::move(texture), tonemapper = std::move(tonemapper),
         compositor = std::move(compositor), pointer = std::move(pointer), stats = std::move(stats),
         context = std::move(context), width, height](std::string& error) {
            // Protect the complete state-setting/draw sequence, not just each
            // individual D3D call. No NVENC calls or CPU GPU waits inside this scope.
            Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
            context.As(&multithread);
            struct Scope {
                ID3D11Multithread* api;
                explicit Scope(ID3D11Multithread* value) : api(value) { if (api) api->Enter(); }
                ~Scope() { if (api) api->Leave(); }
            } scope(multithread.Get());
            const auto start = monotonicNow100ns();
            if (tonemapper && !tonemapper->Process(texture.hdrInputTexture, texture.texture, error)) return false;
            const auto mapped = monotonicNow100ns();
            stats->framePreparationLatency.record(mapped, mapped - start);
            if (compositor && !compositor->compositeSnapshot(pointer, texture.texture.Get(), texture.renderTargetView.Get(),
                    width, height, error)) return false;
            const auto finished = monotonicNow100ns();
            stats->cursorCompositeLatency.record(finished, finished - mapped);
            return true;
        });
}
}
