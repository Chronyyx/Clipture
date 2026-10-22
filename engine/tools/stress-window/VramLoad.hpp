#pragma once

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace clipture::stress {

// Holds `megabytes` of VRAM and references every block each frame, standing in
// for a game that fills the GPU (Deadlock, CS2 at high texture settings). Kept
// in the foreground window's process because Windows keeps the foreground
// process resident and demotes background ones, such as the capture engine,
// into system memory. Capped at the process's local budget less `headroomMb`.
class VramLoad {
public:
    VramLoad(int megabytes, int headroomMb = 512) {
        if (megabytes <= 0) return;
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter1;
        if (FAILED(factory->EnumAdapters1(0, &adapter1)) || FAILED(adapter1.As(&adapter_))) return;
        if (FAILED(D3D11CreateDevice(adapter1.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                D3D11_SDK_VERSION, &device_, nullptr, &context_))) return;
        DXGI_QUERY_VIDEO_MEMORY_INFO local {};
        adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
        const uint64_t cap = local.Budget > (uint64_t(headroomMb) << 20) ? local.Budget - (uint64_t(headroomMb) << 20) : 0;
        const uint64_t target = std::min<uint64_t>(uint64_t(megabytes) << 20, cap);

        D3D11_TEXTURE2D_DESC desc {};
        desc.Width = desc.Height = 4096;  // 64 MiB blocks
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        const float colour[4] { 0.2f, 0.2f, 0.2f, 1.0f };
        while ((blocks_.size() + 1) * kBlockBytes <= target) {
            Microsoft::WRL::ComPtr<ID3D11Texture2D> block;
            Microsoft::WRL::ComPtr<ID3D11RenderTargetView> view;
            if (FAILED(device_->CreateTexture2D(&desc, nullptr, &block)) ||
                FAILED(device_->CreateRenderTargetView(block.Get(), nullptr, &view))) break;
            context_->ClearRenderTargetView(view.Get(), colour);  // commit real pages
            blocks_.push_back(std::move(block));
        }
        desc.Width = desc.Height = 1;
        desc.BindFlags = 0;
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &sink_))) blocks_.clear();
        context_->Flush();
    }

    int heldMegabytes() const { return static_cast<int>(blocks_.size() * kBlockBytes >> 20); }

    // One-texel copies are enough to keep each block referenced, and so resident.
    void touch() {
        if (blocks_.empty()) return;
        const D3D11_BOX texel {0, 0, 0, 1, 1, 1};
        for (const auto& block : blocks_) context_->CopySubresourceRegion(sink_.Get(), 0, 0, 0, 0, block.Get(), 0, &texel);
        context_->Flush();
    }

    // Megabytes of this process currently resident in local VRAM.
    int residentMegabytes() const {
        if (!adapter_) return 0;
        DXGI_QUERY_VIDEO_MEMORY_INFO local {};
        adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
        return static_cast<int>(local.CurrentUsage >> 20);
    }

private:
    static constexpr uint64_t kBlockBytes = 64ull << 20;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> sink_;
    std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> blocks_;
};

}  // namespace clipture::stress
