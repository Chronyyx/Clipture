#include "EncoderSourceSnapshot.hpp"
#include <array>
#include <chrono>
#include <cstdlib>
#include <thread>
using namespace clipture;
using Microsoft::WRL::ComPtr;
static void require(bool ok) { if (!ok) std::abort(); }
static bool reusable(const std::shared_ptr<GpuTextureReadState>& state) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!state->reusable() && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return state->reusable();
}
static void trial(bool early) {
    ComPtr<ID3D11Device> producer, consumer;
    ComPtr<ID3D11DeviceContext> pc, cc;
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &producer, nullptr, &pc)));
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &consumer, nullptr, &cc)));
    ComPtr<ID3D11Device5> c5, p5;
    ComPtr<ID3D11DeviceContext4> cc4, pc4;
    require(SUCCEEDED(consumer.As(&c5)) && SUCCEEDED(cc.As(&cc4)) &&
        SUCCEEDED(producer.As(&p5)) && SUCCEEDED(pc.As(&pc4)));
    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = desc.Height = 16; desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    ComPtr<ID3D11Texture2D> original, output, staging;
    require(SUCCEEDED(producer->CreateTexture2D(&desc, nullptr, &original)));
    desc.MiscFlags = 0;
    require(SUCCEEDED(consumer->CreateTexture2D(&desc, nullptr, &output)));
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    require(SUCCEEDED(consumer->CreateTexture2D(&desc, nullptr, &staging)));
    ComPtr<ID3D11Fence> gate, gateProducer;
    require(SUCCEEDED(c5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gate))));
    HANDLE handle = nullptr;
    require(SUCCEEDED(gate->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle)));
    const auto opened = p5->OpenSharedFence(handle, IID_PPV_ARGS(&gateProducer));
    CloseHandle(handle);
    require(SUCCEEDED(opened));
    auto state = std::make_shared<GpuTextureReadState>();
    SharedTextureReader reader;
    EncoderSourceSnapshot snapshot;
    require(reader.initialize(producer.Get(), consumer.Get()));
    std::string error;
    auto alias = reader.open(original.Get(), state, error);
    require(alias != nullptr);
    std::array<uint32_t, 256> pixels;
    pixels.fill(0xff123456);
    pc->UpdateSubresource(original.Get(), 0, nullptr, pixels.data(), 64, 0);
    pc->Flush();
    // Reject invalid ownership before arming a completion guard.
    require(!snapshot.copyAndRetire(consumer.Get(), cc.Get(), reader, original.Get(), state, error));
    require(state->reusable());
    error.clear();
    ComPtr<ID3D11Texture2D> selected;
    if (early) {
        selected = snapshot.copyAndRetire(consumer.Get(), cc.Get(), reader, alias.Get(), state, error);
        require(selected && selected.Get() != alias.Get());
    } else {
        require(reader.begin(state, error));
        selected = alias;
    }
    // Stand in for blocked downstream conversion, AFTER the candidate's copy.
    require(SUCCEEDED(cc4->Wait(gate.Get(), 1)));
    cc->CopyResource(output.Get(), selected.Get());
    if (!early) require(reader.end(error));
    cc->Flush();
    if (early) {
        require(reusable(state));
        pixels.fill(0xffabcdef);
        pc->UpdateSubresource(original.Get(), 0, nullptr, pixels.data(), 64, 0);
        pc->Flush(); // Capture may safely overwrite while downstream is parked.
    } else {
        require(!state->reusable());
    }
    // Test-only independent producer signal releases the deliberate GPU stall.
    require(SUCCEEDED(pc4->Signal(gateProducer.Get(), 1)));
    pc->Flush();
    cc->CopyResource(staging.Get(), output.Get());
    D3D11_MAPPED_SUBRESOURCE mapped {};
    require(SUCCEEDED(cc->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
    for (UINT y = 0; y < 16; ++y) {
        auto row = reinterpret_cast<const uint32_t*>(static_cast<const char*>(mapped.pData) + mapped.RowPitch * y);
        for (UINT x = 0; x < 16; ++x) require(row[x] == 0xff123456);
    }
    cc->Unmap(staging.Get(), 0);
    require(reusable(state));
    if (early) {
        auto again = snapshot.copyAndRetire(consumer.Get(), cc.Get(), reader, alias.Get(), state, error);
        require(again.Get() == selected.Get()); // One scratch, no per-frame growth.
        require(reusable(state));
        snapshot = {}; reader = {};
        require(state->reusable());
    }
}
int main() {
    trial(false);
    trial(true);
    std::cout << "Early retirement frees the source before downstream completion without changing pixels.\n";
}
