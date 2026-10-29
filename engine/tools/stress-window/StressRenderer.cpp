#include "StressRenderer.hpp"

#include "FrameStamp.hpp"

#include <d3dcompiler.h>

#include <cstring>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace clipture::stress {

namespace {

// ALU iterations per pixel per load layer; layers scale the total cost.
constexpr uint32_t kLoadIterations = 48;

struct FrameConstants {
    float width;
    float height;
    float seconds;
    uint32_t payload;
    uint32_t iterations;
    float highlight;
    float stampWhite;
    float patches;
};
static_assert(sizeof(FrameConstants) % 16 == 0);

constexpr const char* kShaderSource = R"(
cbuffer Frame : register(b0) {
    float2 size;
    float seconds;
    uint payload;
    uint iterations;
    float highlight;   // 1 in SDR; scRGB multiplier for HDR highlights
    float stampWhite;
    float patches;     // 1 draws the known colour patches
};

float4 fullscreen(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float hash(float2 p) { return frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453); }

// Fine scrolling detail keeps the encoder busy like game motion does.
float4 scene(float4 position : SV_Position) : SV_Target {
    float2 p = position.xy;
    float2 q = p + float2(seconds * 900.0, seconds * 170.0);
    float n = hash(floor(q / 6.0));
    float3 color = float3(0.12, 0.17, 0.26) + 0.4 * n * float3(0.8, 0.9, 1.0);
    float barX = frac(seconds * 0.5) * size.x;
    if (n > 0.97) color *= highlight;
    if (abs(p.x - barX) < 40.0) color = float3(1.0, 0.6, 0.1) * highlight;
    // Known 8-bit colours for capture colour checks: red, green, blue, grey 128,
    // a skin tone, white; 240 px squares from (0, 200).
    if (patches > 0.5 && p.y >= 200.0 && p.y < 440.0 && p.x < 1440.0) {
        const float3 known[6] = { float3(255, 0, 0), float3(0, 255, 0), float3(0, 0, 255),
                                  float3(128, 128, 128), float3(200, 150, 100), float3(255, 255, 255) };
        return float4(known[uint(p.x / 240.0)] / 255.0, 1);
    }
    return float4(color, 1);
}

// Volumetric-smoke-like cost: dependent ALU per pixel, blended over the scene.
float4 load(float4 position : SV_Position) : SV_Target {
    float2 z = position.xy / size * 3.0 + seconds * 0.1;
    float acc = 0;
    [loop] for (uint i = 0; i < iterations; ++i) {
        z = float2(sin(z.y * 1.7 + acc), cos(z.x * 1.3 - acc));
        acc += dot(z, z) * 0.01;
    }
    return float4(0.62, 0.62, 0.64, 0.012 + 0.01 * frac(acc));
}

// Compute variant: one full-screen dispatch per layer accumulating density,
// like a volumetric smoke pass. Long dispatches are coarser to preempt than draws.
RWTexture2D<float> densityOut : register(u0);
[numthreads(16, 16, 1)]
void loadCompute(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= uint2(size))) return;
    float2 z = id.xy / size * 3.0 + seconds * 0.1;
    float acc = 0;
    [loop] for (uint i = 0; i < iterations; ++i) {
        z = float2(sin(z.y * 1.7 + acc), cos(z.x * 1.3 - acc));
        acc += dot(z, z) * 0.01;
    }
    densityOut[id.xy] = saturate(densityOut[id.xy] * 0.9 + 0.012 + 0.01 * frac(acc));
}

Texture2D<float> densityIn : register(t0);
float4 composite(float4 position : SV_Position) : SV_Target {
    return float4(0.62, 0.62, 0.64, densityIn.Load(int3(position.xy, 0)));
}

float4 stamp(float4 position : SV_Position) : SV_Target {
    int2 p = int2(position.xy) - int2(STAMP_X, STAMP_Y);
    int2 extent = int2(STAMP_CELL_W * STAMP_CELLS, STAMP_CELL_H);
    if (any(p < -STAMP_MARGIN) || any(p >= extent + STAMP_MARGIN)) discard;
    if (any(p < 0) || any(p >= extent)) return float4(0, 0, 0, 1);
    uint cell = uint(p.x) / STAMP_CELL_W;
    uint bit = cell == 0 ? 1 : cell == 1 ? 0 : (payload >> (31 - (cell - 2))) & 1;
    return float4(bit * stampWhite.xxx, 1);
}
)";

}  // namespace

bool StressRenderer::initialize(HWND window, int width, int height, int maxFrameLatency, bool hdr,
                                bool computeLoad, std::string& error) {
    width_ = width;
    hdr_ = hdr;
    computeLoad_ = computeLoad;
    height_ = height;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1,
                                 D3D11_SDK_VERSION, &device_, nullptr, &context_))) {
        error = "D3D11CreateDevice failed";
        return false;
    }
    ComPtr<IDXGIDevice1> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory5> factory;
    device_.As(&dxgiDevice);
    dxgiDevice->SetMaximumFrameLatency(static_cast<UINT>(maxFrameLatency));
    dxgiDevice->GetAdapter(&adapter);
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        error = "DXGI 1.5 factory unavailable";
        return false;
    }
    BOOL tearing = FALSE;
    factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
    tearing_ = tearing == TRUE;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.Format = hdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    // A buffer smaller than the window is stretched by composition, like a game
    // running a 4:3 resolution borderless on a 16:9 monitor.
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(factory->CreateSwapChainForHwnd(device_.Get(), window, &desc, nullptr, nullptr, &swapChain_))) {
        error = "CreateSwapChainForHwnd failed";
        return false;
    }
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    if (hdr) {
        // Linear scRGB: 1.0 is 80 nits, so highlights well above SDR white reach
        // the capture tonemapper the way an HDR game's do.
        ComPtr<IDXGISwapChain3> swapChain3;
        if (FAILED(swapChain_.As(&swapChain3)) ||
            FAILED(swapChain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709))) {
            error = "scRGB color space unavailable";
            return false;
        }
    }
    ComPtr<ID3D11Texture2D> backBuffer;
    swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &target_);

    D3D11_BUFFER_DESC constants{sizeof(FrameConstants), D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER,
                                D3D11_CPU_ACCESS_WRITE, 0, 0};
    device_->CreateBuffer(&constants, nullptr, &constants_);

    D3D11_BLEND_DESC blend{};
    auto& rt = blend.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device_->CreateBlendState(&blend, &alphaBlend_);

    D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    device_->CreateQuery(&query, &disjoint_);
    query.Query = D3D11_QUERY_TIMESTAMP;
    device_->CreateQuery(&query, &loadBegin_);
    device_->CreateQuery(&query, &loadEnd_);

    D3D11_TEXTURE2D_DESC density{};
    density.Width = desc.Width;
    density.Height = desc.Height;
    density.MipLevels = density.ArraySize = 1;
    density.Format = DXGI_FORMAT_R32_FLOAT;  // typed UAV loads are guaranteed for R32
    density.SampleDesc.Count = 1;
    density.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device_->CreateTexture2D(&density, nullptr, &density_)) ||
        FAILED(device_->CreateUnorderedAccessView(density_.Get(), nullptr, &densityUav_)) ||
        FAILED(device_->CreateShaderResourceView(density_.Get(), nullptr, &densitySrv_))) {
        error = "Density texture creation failed";
        return false;
    }
    return compileShaders(error);
}

bool StressRenderer::compileShaders(std::string& error) {
    const std::string x = std::to_string(kStampOriginX), y = std::to_string(kStampOriginY);
    const std::string w = std::to_string(kStampCellWidth), h = std::to_string(kStampCellHeight);
    const std::string cells = std::to_string(kStampCells), margin = std::to_string(kStampMargin);
    const D3D_SHADER_MACRO macros[] = {{"STAMP_X", x.c_str()}, {"STAMP_Y", y.c_str()},
                                       {"STAMP_CELL_W", w.c_str()}, {"STAMP_CELL_H", h.c_str()},
                                       {"STAMP_CELLS", cells.c_str()}, {"STAMP_MARGIN", margin.c_str()},
                                       {nullptr, nullptr}};
    auto compile = [&](const char* entry, const char* target, ComPtr<ID3DBlob>& blob) {
        ComPtr<ID3DBlob> errors;
        if (SUCCEEDED(D3DCompile(kShaderSource, std::strlen(kShaderSource), "stress", macros, nullptr,
                                 entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errors))) {
            return true;
        }
        error = std::string(entry) + ": " +
                (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed");
        return false;
    };
    ComPtr<ID3DBlob> vs, scene, load, stamp, loadCompute, composite;
    if (!compile("fullscreen", "vs_5_0", vs) || !compile("scene", "ps_5_0", scene) ||
        !compile("load", "ps_5_0", load) || !compile("stamp", "ps_5_0", stamp) ||
        !compile("loadCompute", "cs_5_0", loadCompute) || !compile("composite", "ps_5_0", composite)) {
        return false;
    }
    device_->CreateComputeShader(loadCompute->GetBufferPointer(), loadCompute->GetBufferSize(), nullptr,
                                 &loadComputeShader_);
    device_->CreatePixelShader(composite->GetBufferPointer(), composite->GetBufferSize(), nullptr,
                               &compositeShader_);
    device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vertexShader_);
    device_->CreatePixelShader(scene->GetBufferPointer(), scene->GetBufferSize(), nullptr, &sceneShader_);
    device_->CreatePixelShader(load->GetBufferPointer(), load->GetBufferSize(), nullptr, &loadShader_);
    device_->CreatePixelShader(stamp->GetBufferPointer(), stamp->GetBufferSize(), nullptr, &stampShader_);
    return true;
}

int StressRenderer::setGpuPriority(int priority) {
    ComPtr<IDXGIDevice> dxgiDevice;
    INT active = 0;
    if (SUCCEEDED(device_.As(&dxgiDevice))) {
        dxgiDevice->SetGPUThreadPriority(priority);
        dxgiDevice->GetGPUThreadPriority(&active);
    }
    return active;
}

RenderedFrame StressRenderer::render(uint32_t counter, double seconds, int loadLayers, bool measureLoad) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        *static_cast<FrameConstants*>(mapped.pData) = {static_cast<float>(width_), static_cast<float>(height_),
                                                       static_cast<float>(seconds), stampPayload(counter),
                                                       kLoadIterations, hdr_ ? 8.0f : 1.0f,
                                                       hdr_ ? 2.5f : 1.0f, patches_ ? 1.0f : 0.0f};
        context_->Unmap(constants_.Get(), 0);
    }
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
    context_->OMSetRenderTargets(1, target_.GetAddressOf(), nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());

    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context_->PSSetShader(sceneShader_.Get(), nullptr, 0);
    context_->Draw(3, 0);

    if (measureLoad) {
        context_->Begin(disjoint_.Get());
        context_->End(loadBegin_.Get());
    }
    context_->OMSetBlendState(alphaBlend_.Get(), nullptr, 0xFFFFFFFF);
    if (computeLoad_ && loadLayers > 0) {
        const float clear[4] = {};
        context_->ClearUnorderedAccessViewFloat(densityUav_.Get(), clear);
        context_->CSSetShader(loadComputeShader_.Get(), nullptr, 0);
        context_->CSSetConstantBuffers(0, 1, constants_.GetAddressOf());
        context_->CSSetUnorderedAccessViews(0, 1, densityUav_.GetAddressOf(), nullptr);
        const UINT groupsX = (static_cast<UINT>(width_) + 15) / 16, groupsY = (static_cast<UINT>(height_) + 15) / 16;
        for (int layer = 0; layer < loadLayers; ++layer) context_->Dispatch(groupsX, groupsY, 1);
        ID3D11UnorderedAccessView* noUav = nullptr;
        context_->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
        context_->PSSetShaderResources(0, 1, densitySrv_.GetAddressOf());
        context_->PSSetShader(compositeShader_.Get(), nullptr, 0);
        context_->Draw(3, 0);
        ID3D11ShaderResourceView* noSrv = nullptr;
        context_->PSSetShaderResources(0, 1, &noSrv);
    } else {
        // One draw per layer, like separate smoke volumes, so other GPU work can
        // be scheduled between them rather than behind a single huge draw.
        context_->PSSetShader(loadShader_.Get(), nullptr, 0);
        for (int layer = 0; layer < loadLayers; ++layer) context_->Draw(3, 0);
    }
    if (measureLoad) {
        context_->End(loadEnd_.Get());
        context_->End(disjoint_.Get());
    }

    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    context_->PSSetShader(stampShader_.Get(), nullptr, 0);
    context_->Draw(3, 0);

    RenderedFrame frame;
    frame.presented = SUCCEEDED(swapChain_->Present(0, tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0));
    // Whether DWM composes this window or it flips independently: a capture
    // session can force composition, which costs the game latency and GPU time.
    ComPtr<IDXGISwapChainMedia> media;
    DXGI_FRAME_STATISTICS_MEDIA statistics{};
    if (SUCCEEDED(swapChain_.As(&media)) && SUCCEEDED(media->GetFrameStatisticsMedia(&statistics))) {
        frame.presentationMode = static_cast<int>(statistics.CompositionMode);
    }
    if (measureLoad) frame.loadGpuMs = readLoadTimestamps();
    return frame;
}

double StressRenderer::readLoadTimestamps() {
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    while (context_->GetData(disjoint_.Get(), &disjoint, sizeof(disjoint), 0) == S_FALSE) {
        std::this_thread::yield();
    }
    UINT64 begin = 0, end = 0;
    while (context_->GetData(loadBegin_.Get(), &begin, sizeof(begin), 0) == S_FALSE) std::this_thread::yield();
    while (context_->GetData(loadEnd_.Get(), &end, sizeof(end), 0) == S_FALSE) std::this_thread::yield();
    if (disjoint.Disjoint || disjoint.Frequency == 0) return -1;
    return static_cast<double>(end - begin) * 1000.0 / static_cast<double>(disjoint.Frequency);
}

double StressRenderer::calibrateLayerMs(int layers, int frames) {
    double total = 0;
    int measured = 0;
    for (int i = 0; i < frames; ++i) {
        const auto frame = render(0, i / 120.0, layers, true);
        if (frame.loadGpuMs > 0) {
            total += frame.loadGpuMs;
            ++measured;
        }
    }
    return measured == 0 ? -1 : total / measured / layers;
}

}  // namespace clipture::stress
