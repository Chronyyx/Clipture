#include "GpuFrameHandoff.hpp"
#include <dxgi.h>
#include <array>
#include <cstdlib>
#include <iostream>

using Microsoft::WRL::ComPtr;
static void require(bool value) { if (!value) std::abort(); }
int main() {
    ComPtr<ID3D11Device> producer, consumer;
    ComPtr<ID3D11DeviceContext> pc, cc;
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        0, nullptr, 0, D3D11_SDK_VERSION, &producer, nullptr, &pc)));
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        0, nullptr, 0, D3D11_SDK_VERSION, &consumer, nullptr, &cc)));
    clipture::GpuFrameHandoff handoff;
    if (!handoff.initialize(producer.Get(), consumer.Get())) {
        std::cout << "WARP shared fences unavailable on this Windows runtime.\n";
        return 77;
    }
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = desc.Height = 16;
    desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> source, shared, opened;
    require(SUCCEEDED(producer->CreateTexture2D(&desc, nullptr, &source)));
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    require(SUCCEEDED(producer->CreateTexture2D(&desc, nullptr, &shared)));
    ComPtr<IDXGIResource> resource;
    require(SUCCEEDED(shared.As(&resource)));
    HANDLE handle = nullptr;
    require(SUCCEEDED(resource->GetSharedHandle(&handle)));
    require(SUCCEEDED(consumer->OpenSharedResource(handle, IID_PPV_ARGS(&opened))));
    desc.MiscFlags = 0;
    std::array<ComPtr<ID3D11Texture2D>, 64> outputs;
    std::string error;
    for (size_t i = 0; i < outputs.size(); ++i) {
        require(SUCCEEDED(consumer->CreateTexture2D(&desc, nullptr, &outputs[i])));
        std::array<uint32_t, 16 * 16> data;
        data.fill(0xff000000u | static_cast<uint32_t>(i * 0x030201));
        pc->UpdateSubresource(source.Get(), 0, nullptr, data.data(), 16 * 4, 0);
        require(handoff.begin(shared.Get(), source.Get(), error));
        cc->CopyResource(outputs[i].Get(), opened.Get());
        require(handoff.end(error));
    }
    // Read back AFTER queuing every transfer: catches premature shared-surface
    // reuse, missing producer flush, and missing consumer completion dependency.
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    require(SUCCEEDED(consumer->CreateTexture2D(&desc, nullptr, &staging)));
    for (size_t i = 0; i < outputs.size(); ++i) {
        cc->CopyResource(staging.Get(), outputs[i].Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        require(SUCCEEDED(cc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
        const auto expected = 0xff000000u | static_cast<uint32_t>(i * 0x030201);
        for (UINT y = 0; y < 16; ++y) {
            const auto* row = reinterpret_cast<const uint32_t*>(
                static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
            for (UINT x = 0; x < 16; ++x) require(row[x] == expected);
        }
        cc->Unmap(staging.Get(), 0);
    }
    require(handoff.initialize(producer.Get(), consumer.Get()));
    std::cout << "64 asynchronous cross-device transfers preserve every image; reset passed.\n";
}
