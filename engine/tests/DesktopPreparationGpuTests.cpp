#include "DesktopFramePreparation.hpp"
#include "WgcFramePreparation.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>
#include <wrl/implements.h>

using namespace clipture;
using namespace clipture::capture;
using Microsoft::WRL::ComPtr;
static void require(bool value) { if (!value) std::abort(); }

struct __declspec(uuid("6f1c2b8e-3d4a-4c1e-9b7f-2a5d8e0c4f13")) DestructionSentinel
    : Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IUnknown> {
    explicit DestructionSentinel(bool& destroyed) : destroyed_(destroyed) {}
    ~DestructionSentinel() override { destroyed_ = true; }
    bool& destroyed_;
};

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

static uint16_t half(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t sign = (bits >> 16) & 0x8000;
    const int exponent = static_cast<int>((bits >> 23) & 0xff) - 127 + 15;
    uint32_t mantissa = bits & 0x7fffff;
    if (exponent <= 0) {  // subnormal half
        if (exponent < -10) return static_cast<uint16_t>(sign);
        mantissa |= 0x800000;
        const int shift = 14 - exponent;
        return static_cast<uint16_t>(sign | ((mantissa + (1u << (shift - 1))) >> shift));
    }
    const uint32_t rounded = ((static_cast<uint32_t>(exponent) << 10) | (mantissa >> 13)) + ((mantissa >> 12) & 1);
    return static_cast<uint16_t>(sign | rounded);
}

// Windows composes SDR pixels into scRGB with the sRGB curve times SDR white.
static float scRgbFromSdr(int value, float white) {
    const float c = value / 255.0f;
    return white * (c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f));
}

static ComPtr<ID3D11Texture2D> texture(ID3D11Device* device, DXGI_FORMAT format, UINT bind, const void* data, UINT pitch) {
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = desc.Height = 16;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA initial { data, pitch, 0 };
    ComPtr<ID3D11Texture2D> result;
    require(SUCCEEDED(device->CreateTexture2D(&desc, data ? &initial : nullptr, &result)));
    return result;
}

// SDR content on an HDR desktop maps back to its own 8-bit values; a highlight
// above SDR white switches to the roll-off, which then compresses SDR white.
static void toneCurve(ID3D11Device* device, ID3D11DeviceContext* context) {
    constexpr float white = 2.5f;  // 200 nits
    std::vector<uint16_t> sdr(16 * 16 * 4), bright(16 * 16 * 4);
    for (int i = 0; i < 256; ++i) {
        for (int c = 0; c < 3; ++c) {
            sdr[i * 4 + c] = half(scRgbFromSdr(i, white));
            bright[i * 4 + c] = half(i == 0 ? 4.0f * white : scRgbFromSdr(255, white));
        }
        sdr[i * 4 + 3] = bright[i * 4 + 3] = half(1.0f);
    }
    const auto sdrInput = texture(device, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, sdr.data(), 16 * 8);
    const auto brightInput = texture(device, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_SHADER_RESOURCE, bright.data(), 16 * 8);
    const auto output = texture(device, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_UNORDERED_ACCESS, nullptr, 0);
    std::string error;
    Tonemapper tone(device);
    require(tone.Initialize(error, white));
    for (int frame = 0; frame < 8; ++frame) {
        require(tone.Process(sdrInput, output, error));
        context->Flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    require(!tone.hdrContent());
    const auto exact = pixels(device, context, output.Get());
    for (int i = 0; i < 256; ++i) require((exact[i] & 0xffffff) == static_cast<uint32_t>(i * 0x010101));

    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        require(tone.Process(brightInput, output, error));
        context->Flush();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (tone.hdrContent() && std::chrono::steady_clock::now() - start > std::chrono::milliseconds(400)) break;
    }
    require(tone.hdrContent());
    const auto rolled = pixels(device, context, output.Get());
    require((rolled[1] & 0xff) < 250);  // SDR white now below the highlight
    require((rolled[0] & 0xff) == 255);
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
    // Pixel comparisons across calls need one curve, not the measured one.
    tone->holdKnee(ToneCurveControl::kHdrKnee);
    toneCurve(device.Get(), context.Get());
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
        policy.wgcDirectTonemap = false;  // the copy path first; direct below
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
        policy.wgcDirectTonemap = true;  // tone-mapped from the source in place: same pixels
        auto eager = prepareWgcFrame(early.hdrInputTexture.Get(), 32, 32, wgcPool, tone,
            stats, device.Get(), context, copyProbe, policy, error);
        require(static_cast<bool>(eager.owned.lease));
        require(pixels(device.Get(), context.Get(), eager.owned.texture.Get()) == plainReference);
    }

    // A slot grown under load and later freed for idling must release its HDR
    // input even after tone mapping used it. NVIDIA backs each such texture
    // with system memory, so a view held elsewhere is RAM held during gaming.
    {
        CaptureTexturePool growPool(stats);
        std::vector<CaptureTexture> held;
        for (int i = 0; i < 5; ++i) held.push_back(growPool.acquire(device.Get(), 32, 32, true, error, true, shared));
        require(held.back().hdrInputView && held.back().toneOutputView);
        // Views keep their texture alive without showing in its public
        // refcount, so watch for destruction through attached private data.
        bool grownInputDestroyed = false;
        auto sentinel = Microsoft::WRL::Make<DestructionSentinel>(grownInputDestroyed);
        require(SUCCEEDED(held.back().hdrInputTexture->SetPrivateDataInterface(
            __uuidof(DestructionSentinel), sentinel.Get())));
        sentinel.Reset();
        for (const auto& texture : held)
            require(deferDesktopPreparation(texture, tone, nullptr, {}, stats, context, 32, 32)->prepare(error));
        held.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        require(static_cast<bool>(growPool.acquire(device.Get(), 32, 32, true, error, true, shared).lease));
        context->Flush(); // D3D defers destroying resources recent commands used.
        require(grownInputDestroyed);
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
    std::cout << "WARP: exact SDR tone curve, HDR roll-off, DXGI/WGC deferred HDR, crop, source reuse, cursor, epoch and repeat pixels match.\n";
}
