#include "DesktopFramePreparation.hpp"
#include "WgcFramePreparation.hpp"
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

using namespace clipture;
using namespace clipture::capture;
using Microsoft::WRL::ComPtr;
static void require(bool value) { if (!value) std::abort(); }

// WARP exercises the real shaders without capturing the user's desktop or
// requiring NVIDIA hardware. Readback is test-only, never a production wait.
static std::vector<uint32_t> pixels(ID3D11Device* device, ID3D11DeviceContext* context,
                                   ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)));
    context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    require(SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
    std::vector<uint32_t> result(desc.Width * desc.Height);
    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(result.data() + y * desc.Width,
            static_cast<const char*>(mapped.pData) + y * mapped.RowPitch, desc.Width * 4);
    context->Unmap(staging.Get(), 0);
    return result;
}

int main(int argc, char**) {
    const bool shared = argc > 1;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context)));
    ComPtr<ID3D11Multithread> multithread;
    require(SUCCEEDED(context.As(&multithread)));
    multithread->SetMultithreadProtected(TRUE);
    std::string error;
    auto tone = std::make_shared<Tonemapper>(device);
    require(tone->Initialize(error, 1.0f));
    auto cursor = std::make_shared<DesktopPointerCompositor>(device, context);
    require(cursor->initialize(error));
    auto stats = std::make_shared<CaptureSharedState>();
    CaptureTexturePool pool(stats);
    auto early = pool.acquire(device.Get(), 32, 32, true, error, true, shared);
    auto deferred = pool.acquire(device.Get(), 32, 32, true, error, true, shared);
    require(early.lease && deferred.lease);
    require(!shared || (early.gpuReadState && deferred.gpuReadState));
    require(early.hdrInputTexture.Get() != deferred.hdrInputTexture.Get());

    // Half-float red, green, blue, white and HDR highlights. The exact same
    // existing tone curve must run before and after moving frame selection.
    std::vector<uint16_t> hdr(32 * 32 * 4);
    constexpr std::array<uint16_t, 4> values { 0, 0x3400, 0x3c00, 0x4400 };
    for (size_t i = 0; i < 32 * 32; ++i) {
        hdr[i * 4] = values[i % 4];
        hdr[i * 4 + 1] = values[(i / 4) % 4];
        hdr[i * 4 + 2] = values[(i / 16) % 4];
        hdr[i * 4 + 3] = 0x3c00;
    }
    context->UpdateSubresource(early.hdrInputTexture.Get(), 0, nullptr, hdr.data(), 32 * 8, 0);
    context->UpdateSubresource(deferred.hdrInputTexture.Get(), 0, nullptr, hdr.data(), 32 * 8, 0);

    // WGC includes its cursor in the source. Check deferred HDR without a
    // separate compositor, checked-out source reuse, cropped SDR and failures.
    {
        CaptureTexturePool wgcPool(stats);
        GpuStageProbe copyProbe("wgc-test-copy");
        CapturePipelinePolicy policy;
        require(tone->Process(early.hdrInputTexture, early.texture, error));
        const auto plainReference = pixels(device.Get(), context.Get(), early.texture.Get());
        auto wgc = prepareWgcFrame(early.hdrInputTexture.Get(), 32, 32, wgcPool, tone,
            stats, device.Get(), context, copyProbe, policy, error);
        require(wgc.owned.lease && wgc.owned.gpuReadState && wgc.preparation);
        // WGC can recycle its source after copy; deferred work owns a raw copy.
        std::vector<uint16_t> blank(hdr.size());
        context->UpdateSubresource(early.hdrInputTexture.Get(), 0, nullptr, blank.data(), 32 * 8, 0);
        wgcPool.reset();
        require(wgc.preparation->prepare(error));
        require(pixels(device.Get(), context.Get(), wgc.owned.texture.Get()) == plainReference);
        require(wgc.preparation->prepare(error));
        require(pixels(device.Get(), context.Get(), wgc.owned.texture.Get()) == plainReference);
        auto sdr = prepareWgcFrame(wgc.owned.texture.Get(), 16, 16, wgcPool, nullptr,
            stats, device.Get(), context, copyProbe, policy, error);
        require(sdr.owned.lease && sdr.owned.gpuReadState && !sdr.preparation);
        const auto cropped = pixels(device.Get(), context.Get(), sdr.owned.texture.Get());
        for (size_t y = 0; y < 16; ++y) for (size_t x = 0; x < 16; ++x)
            require(cropped[y * 16 + x] == plainReference[y * 32 + x]);
        auto invalid = prepareWgcFrame(wgc.owned.texture.Get(), 33, 32, wgcPool, nullptr,
            stats, device.Get(), context, copyProbe, policy, error);
        require(!invalid.owned.lease && !error.empty());
        error.clear();
        invalid = prepareWgcFrame(early.hdrInputTexture.Get(), 32, 32, wgcPool, nullptr,
            stats, device.Get(), context, copyProbe, policy, error);
        require(!invalid.owned.lease && !error.empty());
        error.clear();
        context->UpdateSubresource(early.hdrInputTexture.Get(), 0, nullptr, hdr.data(), 32 * 8, 0);
        policy.deferPreparation = false;
        auto eager = prepareWgcFrame(early.hdrInputTexture.Get(), 32, 32, wgcPool, tone,
            stats, device.Get(), context, copyProbe, policy, error);
        require(static_cast<bool>(eager.owned.lease));
        require(pixels(device.Get(), context.Get(), eager.owned.texture.Get()) == plainReference);
    }

    D3D11_TEXTURE2D_DESC pointerDesc {};
    pointerDesc.Width = pointerDesc.Height = pointerDesc.MipLevels = pointerDesc.ArraySize = 1;
    pointerDesc.SampleDesc.Count = 1;
    pointerDesc.Format = DXGI_FORMAT_R16G16B16A16_UINT;
    pointerDesc.Usage = D3D11_USAGE_IMMUTABLE;
    pointerDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const uint16_t operation[] { 255, 0, 0, 0x2ff }; // Opaque red cursor.
    D3D11_SUBRESOURCE_DATA initial { operation, sizeof(operation), 0 };
    ComPtr<ID3D11Texture2D> pointerTexture;
    require(SUCCEEDED(device->CreateTexture2D(&pointerDesc, &initial, &pointerTexture)));
    DesktopPointerCompositor::Snapshot pointer;
    require(SUCCEEDED(device->CreateShaderResourceView(pointerTexture.Get(), nullptr, &pointer.view)));
    pointer.width = pointer.height = 1;
    pointer.position = { 3, 5 };
    pointer.visible = true;
    require(tone->Process(early.hdrInputTexture, early.texture, error));
    require(cursor->compositeSnapshot(pointer, early.texture.Get(), early.renderTargetView.Get(), 32, 32, error));
    const auto reference = pixels(device.Get(), context.Get(), early.texture.Get());
    require((reference[5 * 32 + 3] & 0xffffff) == 0xff0000);

    auto work = deferDesktopPreparation(deferred, tone, cursor, pointer, stats, context, 32, 32);
    pointer.position = { 20, 20 }; // Later cursor motion cannot change the saved snapshot.
    auto output = deferred.texture;
    deferred = {};
    pool.reset(); // Epoch reset cannot recycle or destroy leased inputs.
    tone.reset();
    cursor.reset();
    require(work->prepare(error));
    require(pixels(device.Get(), context.Get(), output.Get()) == reference);
    require(work->prepare(error)); // Repeats must not tone-map/composite twice.
    require(pixels(device.Get(), context.Get(), output.Get()) == reference);
    std::cout << "WARP: DXGI/WGC deferred HDR, crop, source reuse, cursor, epoch and repeat pixels match.\n";
}
