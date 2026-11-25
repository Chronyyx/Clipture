#include "clipture/platform/windows/GpuAdapterFacts.hpp"

#include <Windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdlib>
#include <iostream>

namespace {
using clipture::platform::busiestEngineSharePercent;
using clipture::platform::ProcessGpuTime;

void require(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAILED: " << message << '\n'; std::exit(1); }
}

void shareMath() {
    // 2 s apart: the 3D engine ran 1.5 s, the copy engine 0.1 s.
    const auto share = busiestEngineSharePercent({100, 50, 0}, {100 + 15'000'000, 50 + 1'000'000, 0}, 20'000'000);
    require(share && *share == 75.0, "the busiest engine's share is reported");
    require(busiestEngineSharePercent({0}, {0}, 20'000'000) == 0.0, "an idle process is 0%");
    require(busiestEngineSharePercent({0}, {30'000'000}, 20'000'000) == 100.0, "counter overlap is clamped to 100%");
    require(!busiestEngineSharePercent({0, 0}, {0}, 20'000'000), "engine lists that differ give nothing");
    require(!busiestEngineSharePercent({0}, {0}, 0), "no time between reads gives nothing");
}

// Reads this process on the first hardware adapter: proves the Windows call
// works here. The first read is a baseline; the second has a share.
void liveRead() {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapters1(0, &adapter))) {
        std::cout << "no adapter: live read skipped\n";
        return;
    }
    // Windows keeps GPU statistics only for processes that used the adapter,
    // as a game always has: clear a texture so this process has some.
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, &device, nullptr, &context))) {
        std::cout << "no D3D11 device: live read skipped\n";
        return;
    }
    const auto draw = [&] {
        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = desc.Height = 2048;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> view;
        device->CreateTexture2D(&desc, nullptr, &texture);
        device->CreateRenderTargetView(texture.Get(), nullptr, &view);
        const float colour[4] {0.2f, 0.4f, 0.6f, 1.0f};
        for (int i = 0; i < 200; ++i) context->ClearRenderTargetView(view.Get(), colour);
        context->Flush();
    };
    draw();
    ProcessGpuTime reader(adapter.Get());
    const auto self = GetCurrentProcessId();
    require(!reader.percent(self, 1'000'000), "the first read of a process is a baseline");
    draw();
    Sleep(50);
    const auto share = reader.percent(self, 21'000'000);
    require(share.has_value(), "the second read of the same process has a share");
    require(*share >= 0.0 && *share <= 100.0, "the share is a percentage");
    require(!reader.percent(4, 41'000'000), "a process that cannot be queried (System) gives nothing");
    std::cout << "live read: this process used " << *share << "% of its busiest GPU engine\n";
}
}  // namespace

int main() {
    shareMath();
    liveRead();
    std::cout << "process GPU time tests passed\n";
    return 0;
}
