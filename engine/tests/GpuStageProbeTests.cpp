#include "GpuStageProbe.hpp"
#include <cstdlib>
#include <thread>
static void require(bool value) { if (!value) std::abort(); }
int main() {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
    clipture::GpuStageProbe probe("test-stage");
    for (int i = 0; i < 150; ++i) {
        { auto scope = probe.scope(context.Get()); }
        context->Flush(); // Test-only submission; the probe never flushes.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(clipture::pipelineTimingEnabled() ? probe.completed() >= 5 : probe.completed() == 0);
    require(probe.skipped() == 0);
    std::cout << "GPU timing probe enabled=" << clipture::pipelineTimingEnabled()
              << " completed=" << probe.completed() << '\n';
}
