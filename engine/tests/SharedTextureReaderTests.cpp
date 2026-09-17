#include "SharedTextureReader.hpp"
#include "CaptureBackend.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <thread>

using namespace clipture;
using namespace clipture::capture;
using Microsoft::WRL::ComPtr;
static void require(bool value) { if (!value) std::abort(); }
static bool completed(ID3D11Fence* fence, UINT64 value) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (fence->GetCompletedValue() < value && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Test-only wait.
    return fence->GetCompletedValue() >= value;
}
int main() {
    ComPtr<ID3D11Device> producer, consumer;
    ComPtr<ID3D11DeviceContext> pc, cc;
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &producer, nullptr, &pc)));
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &consumer, nullptr, &cc)));
    ComPtr<ID3D11Device5> p5, c5;
    ComPtr<ID3D11DeviceContext4> pc4, cc4;
    require(SUCCEEDED(producer.As(&p5)) && SUCCEEDED(consumer.As(&c5)));
    require(SUCCEEDED(pc.As(&pc4)) && SUCCEEDED(cc.As(&cc4)));
    auto stats = std::make_shared<CaptureSharedState>();
    CaptureTexturePool pool(stats);
    SharedTextureReader reader;
    require(reader.initialize(producer.Get(), consumer.Get()));
    ComPtr<ID3D11Fence> gate, gateProducer, progress;
    require(SUCCEEDED(c5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gate))));
    HANDLE gateHandle = nullptr;
    require(SUCCEEDED(gate->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &gateHandle)));
    const auto gateOpened = p5->OpenSharedFence(gateHandle, IID_PPV_ARGS(&gateProducer));
    CloseHandle(gateHandle);
    require(SUCCEEDED(gateOpened));
    require(SUCCEEDED(p5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&progress))));
    std::array<ComPtr<ID3D11Texture2D>, 4> outputs, originals, opened;
    std::array<CaptureTexture, 4> frames;
    std::string error;
    for (size_t i = 0; i < outputs.size(); ++i) {
        auto& frame = frames[i];
        frame = pool.acquire(producer.Get(), 16, 16, false, error, false, true);
        require(frame.texture && frame.lease && frame.gpuReadState);
        originals[i] = frame.texture;
        std::array<uint32_t, 256> pixels;
        pixels.fill(0xff000000u | static_cast<uint32_t>((i + 1) * 0x030201));
        pc->UpdateSubresource(frame.texture.Get(), 0, nullptr, pixels.data(), 64, 0);
        opened[i] = reader.open(frame.texture.Get(), frame.gpuReadState, error);
        require(opened[i] != nullptr);
        D3D11_TEXTURE2D_DESC desc {};
        opened[i]->GetDesc(&desc);
        desc.MiscFlags = 0;
        require(SUCCEEDED(consumer->CreateTexture2D(&desc, nullptr, &outputs[i])));
    }
    pc->Flush();
    std::cout << "Resources initialized before parking consumer.\n" << std::flush;
    require(SUCCEEDED(cc4->Wait(gate.Get(), 1))); // Deliberately park the consumer.
    cc->Flush();
    for (size_t i = 0; i < outputs.size(); ++i) {
        auto& frame = frames[i];
        require(reader.begin(frame.gpuReadState, error));
        cc->CopyResource(outputs[i].Get(), opened[i].Get());
        require(reader.end(error));
        require(!frame.gpuReadState->reusable());
        frame = {}; // GPU completion must still guard the expired CPU lease.
    }
    std::cout << "Reads queued while consumer parked.\n" << std::flush;
    auto blocked = pool.acquire(producer.Get(), 16, 16, false, error, false, true);
    require(!blocked.texture && error.empty() && stats->ownedSlotDrops == 1);
    require(SUCCEEDED(pc4->Signal(progress.Get(), 1)));
    pc->Flush();
    const bool captureAdvanced = completed(progress.Get(), 1);
    std::cout << "Independent capture progress=" << captureAdvanced << '\n' << std::flush;
    // Guard ownership must outlive the reader/encoder session.
    reader = {};
    auto stillBlocked = pool.acquire(producer.Get(), 16, 16, false, error, false, true);
    require(!stillBlocked.texture && stats->ownedSlotDrops == 2);
    require(SUCCEEDED(pc4->Signal(gateProducer.Get(), 1)));
    pc->Flush(); // Release the intentional test stall from the independent producer.
    require(captureAdvanced);
    for (size_t i = 0; i < outputs.size(); ++i) {
        D3D11_TEXTURE2D_DESC desc {};
        outputs[i]->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        require(SUCCEEDED(consumer->CreateTexture2D(&desc, nullptr, &staging)));
        cc->CopyResource(staging.Get(), outputs[i].Get());
        D3D11_MAPPED_SUBRESOURCE mapped {};
        require(SUCCEEDED(cc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
        const auto expected = 0xff000000u | static_cast<uint32_t>((i + 1) * 0x030201);
        for (UINT y = 0; y < 16; ++y) {
            const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
            for (UINT x = 0; x < 16; ++x) require(row[x] == expected);
        }
        cc->Unmap(staging.Get(), 0);
    }
    auto reused = pool.acquire(producer.Get(), 16, 16, false, error, false, true);
    require(reused.texture && reused.gpuReadState->reusable());
    require(std::any_of(originals.begin(), originals.end(), [&](auto& p) { return p.Get() == reused.texture.Get(); }));
    require(reader.initialize(producer.Get(), consumer.Get()));
    require(reader.open(reused.texture.Get(), reused.gpuReadState, error));
    pool.reset(); // Old leased frame remains usable after capture epoch reset.
    require(reader.begin(reused.gpuReadState, error) && reader.end(error));
    std::cout << "Consumer stall cannot block capture; GPU-safe reuse, exact pixels, session and epoch lifetime passed.\n";
}
