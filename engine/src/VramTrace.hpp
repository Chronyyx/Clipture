#pragma once

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace clipture {

// Engineering trace for VRAM attribution runs: CLIPTURE_VRAM_TRACE=1 logs this
// process's local video memory after each named allocation stage.
inline void traceVram(ID3D11Device* device, const char* stage) {
    static const bool enabled = [] {
        const char* value = std::getenv("CLIPTURE_VRAM_TRACE");
        return value && value[0] == '1';
    }();
    if (!enabled || !device) return;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
        FAILED(adapter.As(&adapter3))) return;
    DXGI_QUERY_VIDEO_MEMORY_INFO local {};
    DXGI_QUERY_VIDEO_MEMORY_INFO nonLocal {};
    adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
    adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonLocal);
    std::cerr << "[vram] " << stage << " localMiB=" << local.CurrentUsage / (1024 * 1024)
              << " nonLocalMiB=" << nonLocal.CurrentUsage / (1024 * 1024) << '\n';
}

}  // namespace clipture
